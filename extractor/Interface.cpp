#include "Interface.h"

#include "dependencies/imgui/imgui.h"
#include "dependencies/imguifiledialog/ImGuiFileDialog.h"

void RenderUI(SpiderManTool& tool) {
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("USM Tool", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar);

    if (tool.currentState == SpiderManTool::STATE_SPLASH) {
        ImGui::SetCursorPos(ImVec2(ImGui::GetWindowWidth()/2 - 200, ImGui::GetWindowHeight()/2 - 100));
        ImGui::BeginChild("Splash", ImVec2(400, 200), true);
        ImGui::Text("Ultimate Spider-Man (PC) Tool");
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::Text("Enter Directory to Search:");

        char pathBuf[512];
        strncpy(pathBuf, tool.searchPath.c_str(), sizeof(pathBuf));
        pathBuf[sizeof(pathBuf) - 1] = 0;

        if (ImGui::InputText("##Path", pathBuf, sizeof(pathBuf))) {
            tool.searchPath = pathBuf;
        }

        ImGui::SameLine();
        if (ImGui::Button("Browse...")) {
            IGFD::FileDialogConfig config;
            config.path = ".";
            IGFD::FileDialog::Instance()->OpenDialog("ChooseDirDlgKey", "Choose Directory", nullptr, config);
        }

        if (IGFD::FileDialog::Instance()->Display("ChooseDirDlgKey")) {
            if (IGFD::FileDialog::Instance()->IsOk()) {
                tool.searchPath = IGFD::FileDialog::Instance()->GetCurrentPath();
                tool.SaveConfig();
            }
            IGFD::FileDialog::Instance()->Close();
        }

        ImGui::Spacing();
        if (ImGui::Button("SCAN DIRECTORY", ImVec2(385, 40))) {
            tool.ScanDirectory();
        }
        ImGui::EndChild();
    }
    else if (tool.currentState == SpiderManTool::STATE_BROWSER) {

        ImGui::Text("Found %zu Packs in %s", tool.foundPacks.size(), tool.searchPath.c_str());
        ImGui::SameLine();
        if (ImGui::Button("Back to Splash")) tool.currentState = SpiderManTool::STATE_SPLASH;
        ImGui::SameLine();
        if (ImGui::Button("EXTRACT & CONVERT ALL PACKS")) {
            for(const auto& p : tool.foundPacks) {
                tool.ExtractPack(p.string(), true);
            }
        }
        ImGui::Separator();

        float width = ImGui::GetWindowWidth();
        float height = ImGui::GetWindowHeight() - 150;

        ImGui::Columns(2, "MainCols");
        ImGui::SetColumnWidth(0, width * 0.3f);

        ImGui::Text("PCPACK Files");
        ImGui::BeginChild("PackList", ImVec2(0, height), true);
        for (int i = 0; i < tool.foundPacks.size(); i++) {
            bool isSelected = (tool.selectedPackIndex == i);
            if (ImGui::Selectable(tool.foundPacks[i].filename().string().c_str(), isSelected)) {
                tool.selectedPackIndex = i;
                tool.OpenPCPack(tool.foundPacks[i].string());
            }
        }
        ImGui::EndChild();

        ImGui::NextColumn();

        ImGui::Text("Pack Contents");
        ImGui::BeginChild("PackDetails", ImVec2(0, height), true);
        if (tool.selectedPackIndex != -1 && !tool.entries.empty()) {
            if (ImGui::Button("Extract All Files")) tool.ExtractPack(tool.loadedPCPackPath, false);
            ImGui::SameLine();
            if (ImGui::Button("Extract & Convert All Models")) tool.ExtractPack(tool.loadedPCPackPath, true);

            ImGui::Separator();

            ImGui::Columns(3, "FileCols");
            ImGui::Text("Name"); ImGui::NextColumn();
            ImGui::Text("Type"); ImGui::NextColumn();
            ImGui::Text("Action"); ImGui::NextColumn();
            ImGui::Separator();

            for (auto& e : tool.entries) {
                ImGui::TextUnformatted(e.name.c_str()); ImGui::NextColumn();
                ImGui::Text(e.isPcm ? "PCM (Model)" : "Data"); ImGui::NextColumn();
                if (e.isPcm) {
                    if (ImGui::SmallButton(("Convert##" + std::to_string(e.hash)).c_str())) {
                         fs::path p(tool.loadedPCPackPath);
                         fs::path outDir = p.parent_path() / (p.stem().string() + "_extracted");
                         fs::create_directories(outDir);
                         std::vector<uint8_t> data(tool.pcPackData.begin() + e.offset, tool.pcPackData.begin() + e.offset + e.size);
                         tool.ConvertPCM(data, (outDir / e.name).string() + ".glb");
                    }
                }
                ImGui::NextColumn();
            }
            ImGui::Columns(1);
        } else {
            ImGui::TextDisabled("Select a pack on the left to view contents.");
        }
        ImGui::EndChild();

        ImGui::Columns(1);
    }

    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 140);
    ImGui::Separator();
    ImGui::Text("Log:");
    ImGui::BeginChild("Log", ImVec2(0, 0), true);
    ImGui::TextUnformatted(tool.logBuffer.c_str());
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
        ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();

    ImGui::End();
}