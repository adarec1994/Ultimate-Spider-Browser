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

    if (tool.isModelLoaded && tool.viewportTextureId != 0) {
        ImVec2 winSize = ImGui::GetContentRegionAvail();
        ImGui::Image((void*)(intptr_t)tool.viewportTextureId, winSize, ImVec2(0,1), ImVec2(1,0));

        bool uiHovered = ImGui::GetIO().WantCaptureMouse;
        bool isViewportActive = !uiHovered;

        tool.UpdateWorldCamera(isViewportActive);

        ImGui::SetCursorPos(ImVec2(20, 20));
        ImGui::TextColored(ImVec4(1, 1, 1, 0.8f), "Hold RMB + WASD/ZX to Fly | Scroll to Speed Up");

        tool.RenderModelPreview();
    } else {
        ImGui::SetCursorPos(ImVec2(viewport->Size.x * 0.5f - 100, viewport->Size.y * 0.5f));
        ImGui::TextDisabled("Select a 3D Model (.pcm) to preview");
    }

    ImGui::End();
    ImGui::PopStyleVar(3);

    if (tool.showDdsPopup && tool.ddsTextureId != 0) {
        ImGui::SetNextWindowSize(ImVec2((float)tool.ddsWidth + 20, (float)tool.ddsHeight + 40), ImGuiCond_FirstUseEver);

        std::string title = "Texture Preview (" + std::to_string(tool.ddsWidth) + "x" + std::to_string(tool.ddsHeight) + ")###TexturePop";

        if (ImGui::Begin(title.c_str(), &tool.showDdsPopup)) {
            ImVec2 avail = ImGui::GetContentRegionAvail();

            float scale = 1.0f;
            if (avail.x < tool.ddsWidth || avail.y < tool.ddsHeight) {
                float scaleX = avail.x / tool.ddsWidth;
                float scaleY = avail.y / tool.ddsHeight;
                scale = (scaleX < scaleY) ? scaleX : scaleY;
            }

            ImGui::Image((void*)(intptr_t)tool.ddsTextureId,
                         ImVec2(tool.ddsWidth * scale, tool.ddsHeight * scale));
        }
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

            if (tool.entries[tool.selectedFileIndex].isPcm) {
                ImGui::SameLine();
                if (ImGui::Button("Convert to GLB")) tool.ExtractFile(tool.selectedFileIndex, true);
            }

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
            std::string typeStr = "Data";
            if (e.isPcm) typeStr = "PCM";
            else if (e.isDds) typeStr = "DDS";

            std::string title = "Hex View: " + e.name + " (" + typeStr + ")";

            ImGui::SetNextWindowSize(ImVec2(900, 600), ImGuiCond_FirstUseEver);
            if (ImGui::Begin(title.c_str(), &tool.showHexEditor)) {

                bool showSidebar = e.isPcm;

                if (showSidebar) {
                    tool.AnalyzePCM(tool.selectedFileIndex);

                    ImGui::Columns(2, "HexCols");
                    ImGui::SetColumnWidth(0, ImGui::GetWindowWidth() - 320);
                }

                tool.hexEditor.Bytes = &tool.pcPackData[e.offset];
                tool.hexEditor.MaxBytes = e.size;

                ImGui::BeginChild("HexPanel", ImVec2(0,0), false);
                ImGui::BeginHexEditor("##Hex", &tool.hexEditor);
                ImGui::EndHexEditor();
                ImGui::EndChild();

                if (showSidebar) {
                    ImGui::NextColumn();

                    ImGui::BeginChild("SidePanel", ImVec2(0,0), false);

                    ImGui::Text("Global Skeleton");
                    ImGui::Separator();
                    ImGui::Text("Bone Count: %u", tool.currentPcmSkeleton.count);
                    if (tool.currentPcmSkeleton.count > 0) {
                        ImGui::Text("Bone Offset: %u (0x%X)", tool.currentPcmSkeleton.offset, tool.currentPcmSkeleton.offset);
                    } else {
                        ImGui::TextDisabled("No skeleton data");
                    }

                    ImGui::Spacing();
                    ImGui::Text("Mesh Info");
                    ImGui::Separator();

                    for(size_t i=0; i<tool.currentPcmInfos.size(); i++) {
                        const auto& info = tool.currentPcmInfos[i];
                        if (ImGui::CollapsingHeader(("Mesh " + std::to_string(i)).c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                            ImGui::Text("Vertices:   %u", info.vCount);
                            ImGui::Text("V Offset:   0x%X", info.vOffset);
                            ImGui::Text("Faces:      %u", info.iCount);
                            ImGui::Text("F Offset:   0x%X", info.iOffset);
                            ImGui::Text("Stride:     %u", info.stride);
                            ImGui::Text("Prim Type:  %u", info.primitiveType);

                            ImGui::Spacing();
                            ImGui::Text("Attributes:");
                            if (info.hasUV) ImGui::BulletText("UVs Present");
                            else ImGui::TextDisabled("No UVs");

                            if (info.hasBones) ImGui::BulletText("Bone Weights");
                            else ImGui::TextDisabled("No Weights");
                        }
                    }
                    ImGui::EndChild();

                    ImGui::Columns(1);
                }
            }
            ImGui::End();
        }
    }

    if (tool.notificationTimer > 0.0f) {
        tool.notificationTimer -= ImGui::GetIO().DeltaTime;

        float alpha = 1.0f;
        if (tool.notificationTimer < 0.5f) {
            alpha = tool.notificationTimer / 0.5f;
        }

        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.8f * alpha));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, alpha));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.0f, 1.0f, 1.0f, 0.5f * alpha));

        ImGuiViewport* vp = ImGui::GetMainViewport();
        ImVec2 windowPos = ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y - 50.0f);
        ImGui::SetNextWindowPos(windowPos, ImGuiCond_Always, ImVec2(0.5f, 1.0f));

        ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                 ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs;

        if (ImGui::Begin("Toast", nullptr, flags)) {
            ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, alpha), "[SUCCESS]");
            ImGui::SameLine();
            ImGui::TextUnformatted(tool.notificationMsg.c_str());
        }
        ImGui::End();

        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar(2);
    }
}