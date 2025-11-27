#include "Interface.h"
#include "imgui.h"
#include "ImGuiFileDialog.h"
#include <algorithm>

void RenderUI(SpiderManTool& tool) {
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("USM Tool", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse);

    if (tool.currentState == SpiderManTool::STATE_SPLASH) {
        ImGui::SetCursorPos(ImVec2(ImGui::GetWindowWidth()/2 - 175, ImGui::GetWindowHeight()/2 - 75));
        ImGui::BeginChild("Splash", ImVec2(350, 150), false, ImGuiWindowFlags_NoBackground);

        const char* title = "Ultimate Spider-Man (PC) Tool";
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - ImGui::CalcTextSize(title).x) * 0.5f);
        ImGui::TextUnformatted(title);
        ImGui::Spacing();
        ImGui::Spacing();
        ImGui::Spacing();

        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - 320) * 0.5f);
        if (ImGui::Button("Browse to Ultimate Spiderman Directory", ImVec2(320, 50))) {
            IGFD::FileDialogConfig config;
            config.path = ".";
            IGFD::FileDialog::Instance()->OpenDialog("ChooseDirDlgKey", "Choose Directory", nullptr, config);
        }

        if (IGFD::FileDialog::Instance()->Display("ChooseDirDlgKey")) {
            if (IGFD::FileDialog::Instance()->IsOk()) {
                tool.searchPath = IGFD::FileDialog::Instance()->GetCurrentPath();
                tool.SaveConfig();

                std::string dictPath;
                fs::path targetDict = "string_hash_dictionary.txt";

                fs::path p1 = fs::path(tool.searchPath) / targetDict;
                if (fs::exists(p1)) {
                    dictPath = p1.string();
                }

                if (dictPath.empty() && fs::exists(targetDict)) {
                    dictPath = targetDict.string();
                }

                if (dictPath.empty()) {
                    try {
                        for (const auto& entry : fs::recursive_directory_iterator(tool.searchPath)) {
                            if (entry.is_regular_file() && entry.path().filename() == targetDict) {
                                dictPath = entry.path().string();
                                break;
                            }
                        }
                    } catch (...) {}
                }

                if (!dictPath.empty()) {
                    tool.LoadDictionary(dictPath);
                }

                tool.ScanDirectory();
                std::sort(tool.foundPacks.begin(), tool.foundPacks.end());
            }
            IGFD::FileDialog::Instance()->Close();
        }

        ImGui::EndChild();
    }
    else if (tool.currentState == SpiderManTool::STATE_BROWSER) {

        ImGui::Text("Found %zu Packs in %s", tool.foundPacks.size(), tool.searchPath.c_str());
        ImGui::Separator();

        float width = ImGui::GetWindowWidth();
        float height = ImGui::GetWindowHeight() - 40;

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
            ImGui::SameLine();

            if (tool.selectedFileIndex == -1) ImGui::BeginDisabled();
            if (ImGui::Button("Extract Selected")) {
                tool.ExtractFile(tool.selectedFileIndex);
            }
            if (tool.selectedFileIndex == -1) ImGui::EndDisabled();

            ImGui::SameLine();
            bool canPreview = tool.selectedFileIndex != -1 && (tool.entries[tool.selectedFileIndex].isDds || tool.entries[tool.selectedFileIndex].isPcm);
            if (!canPreview) ImGui::BeginDisabled();
            if (ImGui::Button("Preview")) {
                tool.LoadPreview(tool.selectedFileIndex);
            }
            if (!canPreview) ImGui::EndDisabled();

            ImGui::Separator();

            if (ImGui::BeginTable("FileTable", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY)) {
                ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 120.0f);
                ImGui::TableHeadersRow();

                for (int i = 0; i < tool.entries.size(); i++) {
                    const auto& e = tool.entries[i];
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);

                    if (e.isPcm) {
                        bool isSelected = (tool.selectedFileIndex == i);
                        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanFullWidth;
                        if (isSelected) flags |= ImGuiTreeNodeFlags_Selected;

                        bool open = ImGui::TreeNodeEx((void*)(intptr_t)i, flags, "%s", e.name.c_str());
                        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
                            tool.selectedFileIndex = i;
                        }

                        ImGui::TableSetColumnIndex(1);
                        ImGui::Text("PCM (Model)");

                        if (open) {
                            for(const auto& s : e.subItems) {
                                ImGui::TableNextRow();
                                ImGui::TableSetColumnIndex(0);
                                ImGui::Text("  - %s", s.c_str());
                            }
                            ImGui::TreePop();
                        }
                    } else {
                        bool isSelected = (tool.selectedFileIndex == i);
                        if (ImGui::Selectable(e.name.c_str(), isSelected, ImGuiSelectableFlags_SpanAllColumns)) {
                            tool.selectedFileIndex = i;
                        }

                        ImGui::TableSetColumnIndex(1);
                        if (e.isDds) ImGui::Text("DDS (Texture)");
                        else ImGui::Text("Data");
                    }
                }
                ImGui::EndTable();
            }
        } else {
            ImGui::TextDisabled("Select a pack on the left to view contents.");
        }
        ImGui::EndChild();

        ImGui::Columns(1);

        if (tool.showPreview && tool.previewTextureId != 0) {
            if (tool.isModelPreview) {
                tool.RenderModelPreview();
            }

            ImGui::OpenPopup("Preview");
            if (ImGui::BeginPopupModal("Preview", &tool.showPreview, ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::Text("Size: %dx%d", tool.previewWidth, tool.previewHeight);

                ImVec2 size((float)tool.previewWidth, (float)tool.previewHeight);
                if (size.x > 800) { float r = 800/size.x; size.x *= r; size.y *= r; }
                if (size.y > 600) { float r = 600/size.y; size.x *= r; size.y *= r; }

                ImVec2 p = ImGui::GetCursorScreenPos();
                ImGui::Image((void*)(intptr_t)tool.previewTextureId, size, ImVec2(0,1), ImVec2(1,0)); // Flip Y for OpenGL FBO

                if (tool.isModelPreview) {
                    if (ImGui::IsItemActive() || ImGui::IsItemHovered()) {
                        if (ImGui::IsMouseDragging(0)) {
                            ImVec2 d = ImGui::GetMouseDragDelta(0);
                            tool.modelRotY += d.x * 0.005f;
                            tool.modelRotX += d.y * 0.005f;
                            ImGui::ResetMouseDragDelta(0);
                        }
                        float wheel = ImGui::GetIO().MouseWheel;
                        if (wheel != 0) {
                            tool.modelZoom += wheel * 0.1f;
                            if (tool.modelZoom < 0.1f) tool.modelZoom = 0.1f;
                        }
                    }
                }

                if (ImGui::Button("Close")) {
                    tool.showPreview = false;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
        }
    }

    ImGui::End();
}