#include "Interface.h"
#include "imgui.h"
#include "ImGuiFileDialog.h"
#include <algorithm>
#include <string>
#include <cctype>

std::string ToLower(const std::string& str) {
    std::string lower = str;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    return lower;
}

bool IsWorldPack(const std::string& stemName) {
    return stemName.length() == 2;
}

bool IsWorldInteriorPack(const std::string& stemName) {
    if (stemName.length() < 6) return false;
    std::string lowerName = ToLower(stemName);
    if (lowerName.substr(2, 4) == "_int") {
        return true;
    }
    return false;
}

void RenderUI(SpiderManTool& tool) {
    if (tool.currentState == SpiderManTool::STATE_SPLASH) {
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("SplashBackground", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings);

        ImGui::SetCursorPos(ImVec2(ImGui::GetWindowWidth()/2 - 175, ImGui::GetWindowHeight()/2 - 75));
        ImGui::BeginChild("SplashContent", ImVec2(350, 150), false, ImGuiWindowFlags_NoBackground);

        const char* title = "Ultimate Spider-Man (PC) Tool";
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - ImGui::CalcTextSize(title).x) * 0.5f);
        ImGui::TextUnformatted(title);
        ImGui::Spacing(); ImGui::Spacing(); ImGui::Spacing();

        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - 320) * 0.5f);
        if (ImGui::Button("Browse to Ultimate Spiderman Directory", ImVec2(320, 50))) {
            IGFD::FileDialogConfig config; config.path = ".";
            IGFD::FileDialog::Instance()->OpenDialog("ChooseDirDlgKey", "Choose Directory", nullptr, config);
        }

        if (IGFD::FileDialog::Instance()->Display("ChooseDirDlgKey")) {
            if (IGFD::FileDialog::Instance()->IsOk()) {
                tool.searchPath = IGFD::FileDialog::Instance()->GetCurrentPath();
                tool.SaveConfig();

                std::string dictPath; fs::path targetDict = "string_hash_dictionary.txt";
                fs::path p1 = fs::path(tool.searchPath) / targetDict;
                if (fs::exists(p1)) dictPath = p1.string();
                if (dictPath.empty() && fs::exists(targetDict)) dictPath = targetDict.string();
                if (dictPath.empty()) { try { for (const auto& entry : fs::recursive_directory_iterator(tool.searchPath)) { if (entry.is_regular_file() && entry.path().filename() == targetDict) { dictPath = entry.path().string(); break; } } } catch (...) {} }
                if (!dictPath.empty()) tool.LoadDictionary(dictPath);

                tool.ScanDirectory();
                std::sort(tool.foundPacks.begin(), tool.foundPacks.end());
            }
            IGFD::FileDialog::Instance()->Close();
        }
        ImGui::EndChild();
        ImGui::End();
        return;
    }

    if (tool.previewTextureId != 0 && tool.showPreview) {
        ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->Pos);
        ImGui::SetNextWindowSize(viewport->Size);

        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

        ImGuiWindowFlags bgFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                                   ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
                                   ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

        ImGui::Begin("Viewport", nullptr, bgFlags);

        ImVec2 winSize = ImGui::GetContentRegionAvail();
        ImGui::Image((void*)(intptr_t)tool.previewTextureId, winSize, ImVec2(0,1), ImVec2(1,0));

        bool uiHovered = ImGui::GetIO().WantCaptureMouse;
        bool isViewportActive = !uiHovered;

        if (tool.isModelPreview) {
            // Unified Camera Logic: Always use Fly Camera
            tool.UpdateWorldCamera(isViewportActive);

            // Instructions Overlay
            ImGui::SetCursorPos(ImVec2(20, 20));
            ImGui::TextColored(ImVec4(1, 1, 1, 0.8f), "Hold RMB + WASD/ZX to Fly | Scroll to Speed Up");

            tool.RenderModelPreview();
        }

        ImGui::End();
        ImGui::PopStyleVar(3);
    } else {
        ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->Pos);
        ImGui::SetNextWindowSize(viewport->Size);
        ImGui::Begin("Background", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus);
        ImGui::TextDisabled("Select a file to preview...");
        ImGui::End();
    }

    ImGui::SetNextWindowSize(ImVec2(400, 600), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Asset Browser", nullptr, 0)) {

        ImGui::Text("Found %zu Packs", tool.foundPacks.size());
        ImGui::Separator();

        ImGui::BeginChild("PackList", ImVec2(0, 200), true);

        if (ImGui::TreeNodeEx("World", ImGuiTreeNodeFlags_DefaultOpen)) {
            for (int i = 0; i < tool.foundPacks.size(); i++) {
                std::string stem = tool.foundPacks[i].stem().string();
                if (IsWorldPack(stem)) {
                    bool isSelected = (tool.selectedPackIndex == i);
                    if (ImGui::Selectable(tool.foundPacks[i].filename().string().c_str(), isSelected)) {
                        tool.selectedPackIndex = i;
                        tool.OpenPCPack(tool.foundPacks[i].string());
                    }
                }
            }
            ImGui::TreePop();
        }
        if (ImGui::TreeNodeEx("World Interiors", ImGuiTreeNodeFlags_DefaultOpen)) {
            for (int i = 0; i < tool.foundPacks.size(); i++) {
                std::string stem = tool.foundPacks[i].stem().string();
                if (IsWorldInteriorPack(stem)) {
                    bool isSelected = (tool.selectedPackIndex == i);
                    if (ImGui::Selectable(tool.foundPacks[i].filename().string().c_str(), isSelected)) {
                        tool.selectedPackIndex = i;
                        tool.OpenPCPack(tool.foundPacks[i].string());
                    }
                }
            }
            ImGui::TreePop();
        }
        if (ImGui::TreeNodeEx("Other", ImGuiTreeNodeFlags_DefaultOpen)) {
            for (int i = 0; i < tool.foundPacks.size(); i++) {
                std::string stem = tool.foundPacks[i].stem().string();
                if (!IsWorldPack(stem) && !IsWorldInteriorPack(stem)) {
                    bool isSelected = (tool.selectedPackIndex == i);
                    if (ImGui::Selectable(tool.foundPacks[i].filename().string().c_str(), isSelected)) {
                        tool.selectedPackIndex = i;
                        tool.OpenPCPack(tool.foundPacks[i].string());
                    }
                }
            }
            ImGui::TreePop();
        }
        ImGui::EndChild();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Text("Files");

        ImGui::BeginChild("FileList", ImVec2(0, 0), true);

        if (tool.selectedPackIndex != -1 && !tool.entries.empty()) {
            if (ImGui::Button("Extract Pack")) tool.ExtractPack(tool.loadedPCPackPath, false);
            ImGui::SameLine();
            bool canExtract = tool.selectedFileIndex != -1;
            if (!canExtract) ImGui::BeginDisabled();
            if (ImGui::Button("Extract File")) tool.ExtractFile(tool.selectedFileIndex, false);
            ImGui::SameLine();
            if (ImGui::Button("Preview")) tool.LoadPreview(tool.selectedFileIndex);
            if (!canExtract) ImGui::EndDisabled();

            ImGui::SameLine();
            if (ImGui::Button("Hex")) tool.showHexEditor = true;

            if (ImGui::BeginTable("FileTable", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY)) {
                ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 60.0f);
                ImGui::TableHeadersRow();

                for (int i = 0; i < tool.entries.size(); i++) {
                    const auto& e = tool.entries[i];
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);

                    bool isSelected = (tool.selectedFileIndex == i);
                    if (ImGui::Selectable(e.name.c_str(), isSelected, ImGuiSelectableFlags_SpanAllColumns)) {
                        tool.selectedFileIndex = i;
                    }

                    ImGui::TableSetColumnIndex(1);
                    if (e.isPcm) ImGui::Text("MDL");
                    else if (e.isDds) ImGui::Text("TEX");
                    else ImGui::Text("DAT");
                }
                ImGui::EndTable();
            }
        } else {
            ImGui::TextDisabled("Select a pack to view files.");
        }
        ImGui::EndChild();
    }
    ImGui::End();

    if (tool.showHexEditor && tool.selectedFileIndex != -1 && !tool.pcPackData.empty()) {
        const auto& e = tool.entries[tool.selectedFileIndex];
        if (e.offset + e.size <= tool.pcPackData.size()) {
            ImGui::SetNextWindowSize(ImVec2(600, 400), ImGuiCond_FirstUseEver);
            if (ImGui::Begin("Hex Editor", &tool.showHexEditor)) {
                tool.hexEditor.Bytes = &tool.pcPackData[e.offset];
                tool.hexEditor.MaxBytes = e.size;
                ImGui::BeginHexEditor("##Hex", &tool.hexEditor);
                ImGui::EndHexEditor();
            }
            ImGui::End();
        }
    }
}