#include "Interface.h"
#include "NalIntegration.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "ImGuiFileDialog.h"
#include <algorithm>
#include <string>
#include <cctype>
#include <cstdio>
#include <array>

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
    if (tool.currentState == SpiderManTool::STATE_LOADING && tool.isIndexing) {
        const int PACKS_PER_FRAME = 10;
        for (int i = 0; i < PACKS_PER_FRAME && tool.indexingProgress < tool.indexingTotal; i++) {
            tool.BuildGlobalTextureIndexStep(tool.indexingProgress);
            tool.indexingProgress++;
        }

        if (tool.indexingProgress >= tool.indexingTotal) {
            tool.isIndexing = false;
            tool.currentState = SpiderManTool::STATE_BROWSER;
            tool.Log("Indexed " + std::to_string(tool.globalTextureIndex.size()) + " textures from " +
                     std::to_string(tool.foundPacks.size()) + " packs");
        }

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("LoadingBackground", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings);

        ImVec2 center = ImVec2(ImGui::GetWindowWidth() / 2.0f, ImGui::GetWindowHeight() / 2.0f);
        float boxWidth = 400.0f;
        float boxHeight = 100.0f;

        ImGui::SetCursorPos(ImVec2(center.x - boxWidth / 2.0f, center.y - boxHeight / 2.0f));
        ImGui::BeginChild("LoadingContent", ImVec2(boxWidth, boxHeight), true, ImGuiWindowFlags_NoScrollbar);

        ImGui::SetCursorPosX((boxWidth - ImGui::CalcTextSize("Loading PCPacks...").x) / 2.0f);
        ImGui::Text("Loading PCPacks...");

        ImGui::Spacing();

        float progress = (tool.indexingTotal > 0) ? (float)tool.indexingProgress / (float)tool.indexingTotal : 0.0f;
        ImGui::ProgressBar(progress, ImVec2(-1, 20), "");

        ImGui::Spacing();

        char progressText[128];
        snprintf(progressText, sizeof(progressText), "%d / %d", tool.indexingProgress, tool.indexingTotal);
        ImGui::SetCursorPosX((boxWidth - ImGui::CalcTextSize(progressText).x) / 2.0f);
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "%s", progressText);

        ImGui::EndChild();
        ImGui::End();
        return;
    }


    if (tool.currentState == SpiderManTool::STATE_SPLASH) {
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("SplashBackground", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings);

        ImGui::SetCursorPos(ImVec2(ImGui::GetWindowWidth()/2 - 175, ImGui::GetWindowHeight()/2 - 75));
        ImGui::BeginChild("SplashContent", ImVec2(350, 150), false, ImGuiWindowFlags_NoBackground);

        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - 320) * 0.5f);
        if (ImGui::Button("Browse to Ultimate Spiderman Directory", ImVec2(320, 50))) {
            IGFD::FileDialogConfig config; config.path = tool.searchPath.empty() ? "." : tool.searchPath;
            IGFD::FileDialog::Instance()->OpenDialog("ChooseDirDlgKey", "Choose Directory", nullptr, config);
        }

        ImGui::EndChild();
        ImGui::End();

        if (IGFD::FileDialog::Instance()->IsOpened("ChooseDirDlgKey")) {
            ImGui::SetNextWindowFocus();
        }
        if (IGFD::FileDialog::Instance()->Display("ChooseDirDlgKey", ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking, ImVec2(600, 400))) {
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
        return;
    }

    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("View")) {
            ImGui::MenuItem("Asset Browser", nullptr, &tool.showAssetBrowser);
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGuiID dockspace_id = ImGui::GetID("USM_DockSpace");

    static bool dockLayoutInitialized = false;
    if (!dockLayoutInitialized) {
        ImGui::DockBuilderRemoveNode(dockspace_id);
        ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspace_id, viewport->Size);

        ImGuiID dock_main_id = dockspace_id;
        ImGuiID dock_id_left = ImGui::DockBuilderSplitNode(dock_main_id, ImGuiDir_Left, 0.25f, nullptr, &dock_main_id);
        ImGuiID dock_id_right = ImGui::DockBuilderSplitNode(dock_main_id, ImGuiDir_Right, 0.22f, nullptr, &dock_main_id);

        if (ImGuiDockNode* viewportDock = ImGui::DockBuilderGetNode(dock_main_id)) {
            viewportDock->SetLocalFlags(
                viewportDock->LocalFlags | ImGuiDockNodeFlags_NoTabBar);
        }

        ImGui::DockBuilderDockWindow("Asset Browser", dock_id_left);
        ImGui::DockBuilderDockWindow("Animations", dock_id_right);
        ImGui::DockBuilderDockWindow("Viewport", dock_main_id);
        ImGui::DockBuilderFinish(dockspace_id);
        dockLayoutInitialized = true;
    }

    ImGui::DockSpaceOverViewport(dockspace_id, viewport, ImGuiDockNodeFlags_PassthruCentralNode);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

    ImGuiWindowFlags bgFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                               ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
                               ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

    ImGuiWindowClass viewportWindowClass;
    viewportWindowClass.DockNodeFlagsOverrideSet = ImGuiDockNodeFlags_NoTabBar;
    ImGui::SetNextWindowClass(&viewportWindowClass);
    ImGui::Begin("Viewport", nullptr, bgFlags);

    int previewTabToClose = -1;
    if (!tool.previewTabs.empty() && ImGui::BeginTabBar(
            "PreviewTabs", ImGuiTabBarFlags_Reorderable |
                           ImGuiTabBarFlags_AutoSelectNewTabs |
                           ImGuiTabBarFlags_FittingPolicyScroll)) {
        for (int i = 0; i < (int)tool.previewTabs.size(); ++i) {
            auto& tab = tool.previewTabs[i];
            bool open = true;
            ImGuiTabItemFlags flags = tab.requestSelect ? ImGuiTabItemFlags_SetSelected : 0;
            const std::string title = tab.label + "###PreviewTab" + std::to_string(tab.id);
            const bool selected = ImGui::BeginTabItem(title.c_str(), &open, flags);
            tab.requestSelect = false;
            if (selected) {
                if (tool.activePreviewTab != i) tool.ActivatePreviewTab(i);
                ImGui::EndTabItem();
            }
            if (!open) previewTabToClose = i;
        }
        ImGui::EndTabBar();
    }
    if (previewTabToClose >= 0) tool.ClosePreviewTab(previewTabToClose);
    const float viewportOverlayTop = ImGui::GetCursorPosY();

    if (tool.isModelLoaded && tool.viewportTextureId != 0) {
        ImVec2 winPos = ImGui::GetCursorScreenPos();
        ImVec2 winSize = ImGui::GetContentRegionAvail();
        ImGui::Image((void*)(intptr_t)tool.viewportTextureId, winSize, ImVec2(0,1), ImVec2(1,0));
        bool viewportImageHovered = ImGui::IsItemHovered();

        bool uiHovered = ImGui::GetIO().WantCaptureMouse;
        bool isViewportActive = !uiHovered;


        if (tool.isWorldMode && viewportImageHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
            ImVec2 mousePos = ImGui::GetMousePos();
            float localX = mousePos.x - winPos.x;
            float localY = mousePos.y - winPos.y;
            tool.HandleMeshPicking(localX, localY, winSize.x, winSize.y);
        }


        if (tool.isWorldMode && tool.selectedMeshIndex >= 0 && tool.selectedMeshIndex < (int)tool.previewMeshes.size()) {
            if (ImGui::IsKeyPressed(ImGuiKey_Delete)) {
                auto& selectedMesh = tool.previewMeshes[tool.selectedMeshIndex];
                if (tool.selectedMeshInstanceIndex >= 0 &&
                    tool.selectedMeshInstanceIndex < (int)selectedMesh.instances.size()) {
                    auto& instance = selectedMesh.instances[tool.selectedMeshInstanceIndex];
                    instance.isHidden = true;
                    tool.RefreshInstanceBuffer(selectedMesh);
                    tool.Log("Hidden mesh: " + (instance.name.empty() ? selectedMesh.meshName : instance.name));
                } else {
                    selectedMesh.isHidden = true;
                    tool.Log("Hidden mesh: " + selectedMesh.meshName);
                }
                tool.selectedMeshIndex = -1;
                tool.selectedMeshInstanceIndex = -1;
                tool.showWorldMeshDetails = false;
            }
        }

        if (tool.isWorldMode) {
            tool.UpdateWorldCamera(isViewportActive);
        } else {
            tool.UpdateModelOrbitCamera(viewportImageHovered);
        }

        ImGui::SetCursorPos(ImVec2(20, viewportOverlayTop + 20));
        if (tool.isWorldMode) {
            ImGui::TextColored(ImVec4(1, 1, 1, 0.8f), "Hold RMB + WASD/ZX to Fly | Scroll to Speed Up | LMB to Select | DEL to Hide");
            if (tool.selectedMeshIndex >= 0 && tool.selectedMeshIndex < (int)tool.previewMeshes.size()) {
                const auto& m = tool.previewMeshes[tool.selectedMeshIndex];
                const std::string instanceName =
                    (tool.selectedMeshInstanceIndex >= 0 &&
                     tool.selectedMeshInstanceIndex < (int)m.instances.size() &&
                     !m.instances[tool.selectedMeshInstanceIndex].name.empty())
                        ? m.instances[tool.selectedMeshInstanceIndex].name
                        : m.meshName;
                ImGui::SetCursorPos(ImVec2(20, viewportOverlayTop + 45));
                ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.4f, 0.9f), "Selected: %s",
                                   instanceName.empty() ? ("Mesh " + std::to_string(tool.selectedMeshIndex)).c_str() : instanceName.c_str());
            }
        } else {
            ImGui::TextColored(ImVec4(1, 1, 1, 0.8f), "Drag LMB to Rotate | Scroll to Zoom");
            if (tool.skeletonBoneCount > 0) {
                ImGui::SetCursorPos(ImVec2(20, viewportOverlayTop + 45));
                ImGui::Checkbox("Show Skeleton", &tool.showSkeleton);
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 0.8f), "(%d bones)", tool.skeletonBoneCount);

                if (tool.showSkeleton && tool.selectedBoneIndex >= 0) {
                    ImGui::SetCursorPos(ImVec2(20, viewportOverlayTop + 70));
                    int nalIdx = (tool.selectedBoneIndex < (int)tool.nalBoneVboOrder.size())
                        ? tool.nalBoneVboOrder[tool.selectedBoneIndex] : -1;
                    std::string boneName = "Bone " + std::to_string(nalIdx);
                    if (nalIdx >= 0 && tool.loadedSkeleton && tool.loadedSkeleton->bone_map.count(nalIdx)) {
                        boneName = tool.loadedSkeleton->bone_map.at(nalIdx);
                    }
                    std::array<float,3> pos = {0,0,0};
                    if (nalIdx >= 0 && tool.nalBonePositions.count(nalIdx))
                        pos = tool.nalBonePositions.at(nalIdx);
                    ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 0.9f), "[%d] %s  (%.2f, %.2f, %.2f)",
                        nalIdx, boneName.c_str(), pos[0], pos[1], pos[2]);
                    if (nalIdx >= 0 && tool.loadedSkeleton && tool.loadedSkeleton->parent_map.count(nalIdx)) {
                        int parentIdx = tool.loadedSkeleton->parent_map.at(nalIdx);
                        std::string parentName = (parentIdx >= 0 && tool.loadedSkeleton->bone_map.count(parentIdx))
                            ? tool.loadedSkeleton->bone_map.at(parentIdx) : "ROOT";
                        ImGui::SetCursorPos(ImVec2(20, viewportOverlayTop + 90));
                        ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 0.8f), "Parent: [%d] %s", parentIdx, parentName.c_str());
                    }
                    ImGui::SetCursorPos(ImVec2(20, viewportOverlayTop + 110));
                    ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 0.8f), "R = Rotate | X/Y/Z = Axis | Esc = Cancel");
                }

                // Bone selection (click when skeleton is visible)
                if (tool.showSkeleton && !tool.isRotatingBone && viewportImageHovered &&
                    ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
                    !ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
                    ImVec2 mousePos = ImGui::GetMousePos();
                    float localX = mousePos.x - winPos.x;
                    float localY = mousePos.y - winPos.y;
                    int picked = tool.PickBoneAtScreenPos(localX, localY, winSize.x, winSize.y);
                    if (picked >= 0) {
                        tool.selectedBoneIndex = picked;
                        tool.isRotatingBone = false;
                    }
                }

                // R key to start rotation
                if (tool.selectedBoneIndex >= 0 && ImGui::IsKeyPressed(ImGuiKey_R) && !tool.isRotatingBone) {
                    tool.isRotatingBone = true;
                    tool.boneRotationAngle = 0.0f;
                    tool.boneRotationsBeforeEdit = tool.manualBoneRotations;
                }

                // Axis selection during rotation
                if (tool.isRotatingBone) {
                    if (ImGui::IsKeyPressed(ImGuiKey_X)) tool.boneRotationAxis = 0;
                    if (ImGui::IsKeyPressed(ImGuiKey_Y)) tool.boneRotationAxis = 1;
                    if (ImGui::IsKeyPressed(ImGuiKey_Z)) tool.boneRotationAxis = 2;

                    // Mouse movement rotates
                    float delta = ImGui::GetIO().MouseDelta.x * 0.01f;
                    if (delta != 0.0f) {
                        tool.boneRotationAngle += delta;
                        tool.ApplyBoneRotation(tool.selectedBoneIndex, delta, tool.boneRotationAxis);
                    }

                    // Left click confirms, Escape cancels
                    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                        tool.isRotatingBone = false;
                        tool.boneRotationsBeforeEdit.clear();
                    }
                    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                        tool.isRotatingBone = false;
                        tool.manualBoneRotations = tool.boneRotationsBeforeEdit;
                        tool.boneRotationsBeforeEdit.clear();
                        tool.boneRotationAngle = 0.0f;
                    }
                }
            }
        }

        tool.RenderModelPreview();
    } else {
        ImVec2 avail = ImGui::GetContentRegionAvail();
        ImGui::SetCursorPos(ImVec2(avail.x * 0.5f - 100, avail.y * 0.5f));
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
            ImGui::Image((void*)(intptr_t)tool.ddsTextureId, ImVec2(tool.ddsWidth * scale, tool.ddsHeight * scale));
        }
        ImGui::End();
    }

    // Rigged-model side panel.  Keep the presentation intentionally minimal:
    // only animation names are visible.  Selecting a name retains the useful
    // behavior of starting that clip, without exposing codec/debug details,
    // frame counters, timelines, or skeleton internals.
    bool hasCompatibleAnimations = false;
    if (!tool.isWorldMode && tool.activePreviewTab >= 0 && tool.loadedSkeleton &&
        tool.loadedAnimFile) {
        for (const auto& anim : tool.loadedAnimFile->animations) {
            if (!anim.skeleton || nal_skeleton_pose_compatible(
                    anim.skeleton.get(), tool.loadedSkeleton.get())) {
                hasCompatibleAnimations = true;
                break;
            }
        }
    }
    if (hasCompatibleAnimations) {
        if (ImGui::Begin("Animations")) {
            for (int i = 0; i < (int)tool.loadedAnimFile->animations.size(); ++i) {
                const auto& anim = tool.loadedAnimFile->animations[i];
                if (anim.skeleton && !nal_skeleton_pose_compatible(
                        anim.skeleton.get(), tool.loadedSkeleton.get())) {
                    continue;
                }
                const std::string label = anim.name + "###Animation" + std::to_string(i);
                if (ImGui::Selectable(label.c_str(), tool.selectedAnimIndex == i)) {
                    tool.selectedAnimIndex = i;
                    tool.currentAnimFrame = 0;
                    tool.animFrameFraction = 0.0f;
                    tool.animPlaybackTime = 0.0f;
                    tool.isAnimPlaying = true;
                }
            }
        }
        ImGui::End();
    }

    if (tool.showAssetBrowser) {
        if (ImGui::Begin("Asset Browser", &tool.showAssetBrowser)) {
            ImGui::InputTextWithHint("##SearchPacks", "Search packs...", tool.searchBuffer, sizeof(tool.searchBuffer));
            std::string searchLower = ToLower(tool.searchBuffer);
            bool useFilter = !searchLower.empty();

            ImGui::Separator();
            ImGui::Text("Found %zu Packs", tool.foundPacks.size());
            ImGui::Separator();

            if (ImGui::Selectable("Load World", false)) {
                tool.LoadAllWorldGeometries();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Click to load all world areas");
            }

            if (ImGui::Selectable("Extract All World Meshes", false, ImGuiSelectableFlags_AllowDoubleClick)) {
                if (ImGui::IsMouseDoubleClicked(0)) {
                    tool.ExtractAllWorldMeshes();
                }
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Double-click to extract all unique PCM meshes as GLB + DDS");
            }
            ImGui::Separator();

            ImGui::BeginChild("PackList", ImVec2(0, 200), true);
            auto RenderPackNode = [&](const char* label, std::function<bool(const std::string&)> filterFunc) {
                if (ImGui::TreeNodeEx(label, ImGuiTreeNodeFlags_DefaultOpen)) {
                    for (int i = 0; i < (int)tool.foundPacks.size(); i++) {
                        std::string stem = tool.foundPacks[i].stem().string();
                        if (filterFunc(stem)) {
                            if (useFilter && ToLower(stem).find(searchLower) == std::string::npos) continue;
                            bool isSelected = (tool.selectedPackIndex == i);
                            if (ImGui::Selectable(tool.foundPacks[i].filename().string().c_str(), isSelected)) {
                                tool.selectedPackIndex = i;
                                tool.OpenPCPack(tool.foundPacks[i].string());
                            }
                        }
                    }
                    ImGui::TreePop();
                }
            };

            RenderPackNode("World Packs", [](const std::string& s) { return IsWorldPack(s); });
            RenderPackNode("Interior Packs", [](const std::string& s) { return IsWorldInteriorPack(s); });
            RenderPackNode("Asset Packs", [](const std::string& s) { return !IsWorldPack(s) && !IsWorldInteriorPack(s); });
            ImGui::EndChild();

            ImGui::Separator();

            static char fileSearchBuffer[256] = "";
            static bool searchAllPacks = true;

            ImGui::Checkbox("Search all packs", &searchAllPacks);
            ImGui::InputTextWithHint("##SearchFiles", "Search files...", fileSearchBuffer, sizeof(fileSearchBuffer));


            static float searchTimer = 0.0f;
            static std::string pendingSearch = "";
            std::string currentSearch = fileSearchBuffer;

            if (currentSearch != pendingSearch) {
                pendingSearch = currentSearch;
                searchTimer = 0.3f;
            }

            if (searchTimer > 0.0f) {
                searchTimer -= ImGui::GetIO().DeltaTime;
                if (searchTimer <= 0.0f && searchAllPacks) {
                    tool.SearchAllPacks(pendingSearch);
                }
            }

            std::string fileSearchLower = ToLower(fileSearchBuffer);
            bool useFileFilter = !fileSearchLower.empty();


            if (!searchAllPacks && tool.isGlobalSearchMode) {
                tool.isGlobalSearchMode = false;
                tool.globalSearchResults.clear();
            }

            ImGui::BeginChild("FileList", ImVec2(0, 0), true);


            if (searchAllPacks && tool.isGlobalSearchMode && !tool.globalSearchResults.empty()) {
                ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Found %d results across all packs", (int)tool.globalSearchResults.size());
                ImGui::Separator();

                bool hasSelection = tool.selectedGlobalSearchIndex >= 0 && tool.selectedGlobalSearchIndex < (int)tool.globalSearchResults.size();
                float halfWidth = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;

                if (!hasSelection) ImGui::BeginDisabled();
                if (ImGui::Button("Extract", ImVec2(-1.0f, 0))) {
                    tool.SelectGlobalSearchResult(tool.selectedGlobalSearchIndex);
                    tool.ExtractFile(tool.selectedFileIndex);
                }

                bool isPcm = hasSelection && tool.globalSearchResults[tool.selectedGlobalSearchIndex].isPcm;
                if (isPcm) {
                    if (ImGui::Button("Details", ImVec2(halfWidth, 0))) {
                        tool.SelectGlobalSearchResult(tool.selectedGlobalSearchIndex);
                        tool.AnalyzePCM(tool.selectedFileIndex);
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("To GLB", ImVec2(halfWidth, 0))) {
                        tool.SelectGlobalSearchResult(tool.selectedGlobalSearchIndex);
                        tool.ExtractFile(tool.selectedFileIndex, true);
                    }
                }
                if (!hasSelection) ImGui::EndDisabled();

                ImGui::Spacing();
                if (ImGui::BeginTabBar("GlobalFileFilterTabs")) {
                    if (ImGui::BeginTabItem("All")) { tool.currentFileFilter = 0; ImGui::EndTabItem(); }
                    if (ImGui::BeginTabItem("Textures")) { tool.currentFileFilter = 1; ImGui::EndTabItem(); }
                    if (ImGui::BeginTabItem("Models")) { tool.currentFileFilter = 2; ImGui::EndTabItem(); }
                    if (ImGui::BeginTabItem("Data")) { tool.currentFileFilter = 3; ImGui::EndTabItem(); }
                    ImGui::EndTabBar();
                }

                if (ImGui::BeginTable("GlobalFileTable", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY)) {
                    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("Pack", ImGuiTableColumnFlags_WidthFixed, 120.0f);
                    ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 50.0f);
                    ImGui::TableHeadersRow();

                    for (int i = 0; i < (int)tool.globalSearchResults.size(); i++) {
                        const auto& r = tool.globalSearchResults[i];

                        bool showEntry = true;
                        if (tool.currentFileFilter == 1 && !r.isDds) showEntry = false;
                        else if (tool.currentFileFilter == 2 && !r.isPcm) showEntry = false;
                        else if (tool.currentFileFilter == 3 && (r.isPcm || r.isDds)) showEntry = false;
                        if (!showEntry) continue;

                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        bool isSelected = (tool.selectedGlobalSearchIndex == i);
                        if (ImGui::Selectable(r.fileName.c_str(), isSelected, ImGuiSelectableFlags_SpanAllColumns)) {
                            tool.selectedGlobalSearchIndex = i;
                            if (r.isPcm) tool.OpenGlobalSearchPcmTab(i);
                        }
                        if (!r.isPcm && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                            tool.SelectGlobalSearchResult(i);
                            tool.LoadPreview(tool.selectedFileIndex);
                        }
                        ImGui::TableSetColumnIndex(1);
                        ImGui::TextDisabled("%s", r.packName.c_str());
                        ImGui::TableSetColumnIndex(2);
                        if (r.isPcm) ImGui::Text("MDL"); else if (r.isDds) ImGui::Text("TEX"); else ImGui::Text("DAT");
                    }
                    ImGui::EndTable();
                }
            }
            else if (!tool.entries.empty()) {
                bool fileSelected = tool.selectedFileIndex >= 0 && tool.selectedFileIndex < (int)tool.entries.size();
                float halfWidth = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;

                if (!fileSelected) ImGui::BeginDisabled();
                if (ImGui::Button("Extract", ImVec2(-1.0f, 0))) tool.ExtractFile(tool.selectedFileIndex);

                bool isPcm = fileSelected && tool.entries[tool.selectedFileIndex].isPcm;
                if (isPcm) {
                    if (ImGui::Button("Details", ImVec2(halfWidth, 0))) {
                        tool.AnalyzePCM(tool.selectedFileIndex);
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("To GLB", ImVec2(halfWidth, 0))) tool.ExtractFile(tool.selectedFileIndex, true);
                }
                if (!fileSelected) ImGui::EndDisabled();

                ImGui::Spacing();
                if (ImGui::BeginTabBar("FileFilterTabs")) {
                    if (ImGui::BeginTabItem("All")) { tool.currentFileFilter = 0; ImGui::EndTabItem(); }
                    if (ImGui::BeginTabItem("Textures")) { tool.currentFileFilter = 1; ImGui::EndTabItem(); }
                    if (ImGui::BeginTabItem("Models")) { tool.currentFileFilter = 2; ImGui::EndTabItem(); }
                    if (ImGui::BeginTabItem("Data")) { tool.currentFileFilter = 3; ImGui::EndTabItem(); }
                    ImGui::EndTabBar();
                }

                if (ImGui::BeginTable("FileTable", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY)) {
                    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 60.0f);
                    ImGui::TableHeadersRow();

                    for (int i = 0; i < (int)tool.entries.size(); i++) {
                        const auto& e = tool.entries[i];
                        if (useFileFilter && ToLower(e.name).find(fileSearchLower) == std::string::npos) continue;

                        bool showEntry = true;
                        if (tool.currentFileFilter == 1 && !e.isDds) showEntry = false;
                        else if (tool.currentFileFilter == 2 && !e.isPcm) showEntry = false;
                        else if (tool.currentFileFilter == 3 && (e.isPcm || e.isDds)) showEntry = false;
                        if (!showEntry) continue;

                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        bool isSelected = (tool.selectedFileIndex == i);
                        if (ImGui::Selectable(e.name.c_str(), isSelected, ImGuiSelectableFlags_SpanAllColumns)) {
                            tool.selectedFileIndex = i;
                            if (e.isPcm) tool.OpenPcmPreviewTab(i);
                        }
                        if (e.isDds && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                            tool.LoadPreview(i);
                        }
                        ImGui::TableSetColumnIndex(1);
                        if (e.isPcm) ImGui::Text("MDL"); else if (e.isDds) ImGui::Text("TEX"); else ImGui::Text("DAT");
                    }
                    ImGui::EndTable();
                }
            } else if (searchAllPacks && tool.isGlobalSearchMode && tool.globalSearchResults.empty()) {
                ImGui::TextDisabled("No results found for '%s'", fileSearchBuffer);
            } else {
                ImGui::TextDisabled("Select a pack to view files, or search all packs.");
            }
            ImGui::EndChild();
        }
        ImGui::End();
    }

    if (tool.showPcmDetailsPanel) {
        ImGui::SetNextWindowSize(ImVec2(720, 620), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Model Details", &tool.showPcmDetailsPanel,
                         ImGuiWindowFlags_NoDocking)) {
            if (ImGui::BeginTabBar("PCMDetailsTabs")) {

                        if (ImGui::BeginTabItem("Overview")) {
                            const auto& d = tool.currentPcmDetails;

                            ImGui::Text("PCM File Analysis");
                            ImGui::Separator();


                            ImGui::TextColored(ImVec4(0.7f, 0.9f, 1.0f, 1.0f), "File Info");
                            ImGui::Text("  Size: %u bytes (0x%X)", d.fileSize, d.fileSize);
                            ImGui::Text("  Entries: %u", d.numEntries);
                            ImGui::Text("  Entry Table: 0x%X", d.entryTableOffset);

                            ImGui::Spacing();
                            ImGui::TextColored(ImVec4(0.7f, 0.9f, 1.0f, 1.0f), "Contents");
                            ImGui::Text("  Materials: %u", d.materialCount);
                            ImGui::Text("  LODs: %u", d.lodCount);
                            ImGui::Text("  Submeshes: %u", d.totalSubmeshes);
                            ImGui::Text("  Total Vertices: %u", d.totalVertices);
                            ImGui::Text("  Total Indices: %u", d.totalIndices);

                            ImGui::Spacing();
                            ImGui::TextColored(ImVec4(0.7f, 0.9f, 1.0f, 1.0f), "Skeleton");
                            if (d.boneCount > 0) {
                                ImGui::Text("  Bones: %u", d.boneCount);
                                ImGui::Text("  Offset: 0x%X", d.bonesOffset);
                                ImGui::TextDisabled("  (Linear parenting - hierarchy not in file)");
                            } else {
                                ImGui::TextDisabled("  No skeleton");
                            }

                            ImGui::EndTabItem();
                        }


                        if (ImGui::BeginTabItem("LODs")) {
                            const auto& d = tool.currentPcmDetails;

                            for (size_t i = 0; i < d.lods.size(); i++) {
                                const auto& lod = d.lods[i];
                                std::string label = "LOD " + std::to_string(i);
                                if (!lod.name.empty()) label += ": " + lod.name;

                                if (ImGui::CollapsingHeader(label.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                                    ImGui::Indent();
                                    ImGui::Text("Submeshes: %u", lod.submeshCount);
                                    ImGui::Text("Bones: %u", lod.boneCount);
                                    if (lod.bonesOffset > 0) {
                                        ImGui::Text("Bones Offset: 0x%X", lod.bonesOffset);
                                    }
                                    if (lod.lodDistance > 0.0f) {
                                        ImGui::Text("LOD Distance: %.2f", lod.lodDistance);
                                    }
                                    if (lod.nextLodOffset > 0) {
                                        ImGui::Text("Next LOD: 0x%X", lod.nextLodOffset);
                                    } else {
                                        ImGui::TextDisabled("Last LOD");
                                    }
                                    ImGui::Unindent();
                                }
                            }

                            ImGui::EndTabItem();
                        }


                        if (ImGui::BeginTabItem("Materials")) {
                            const auto& d = tool.currentPcmDetails;

                            for (size_t i = 0; i < d.materials.size(); i++) {
                                const auto& mat = d.materials[i];
                                std::string label = "Material " + std::to_string(i);
                                if (!mat.name.empty()) label += ": " + mat.name;

                                if (ImGui::CollapsingHeader(label.c_str())) {
                                    ImGui::Indent();


                                    std::string shaderType;
                                    ImVec4 shaderColor = ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
                                    switch (mat.shaderSize) {
                                        case 80: shaderType = "CHARACTER (80)"; shaderColor = ImVec4(1.0f, 0.8f, 0.4f, 1.0f); break;
                                        case 88: shaderType = "CHARACTER_EXT (88)"; shaderColor = ImVec4(1.0f, 0.9f, 0.5f, 1.0f); break;
                                        case 128: shaderType = "STREET (128)"; shaderColor = ImVec4(0.6f, 0.8f, 1.0f, 1.0f); break;
                                        case 132: shaderType = "WORLD (132)"; shaderColor = ImVec4(0.4f, 1.0f, 0.6f, 1.0f); break;
                                        case 136: shaderType = "TRANSLUCENT (136)"; shaderColor = ImVec4(0.8f, 0.6f, 1.0f, 1.0f); break;
                                        default: shaderType = "UNKNOWN (" + std::to_string(mat.shaderSize) + ")"; break;
                                    }
                                    ImGui::TextColored(shaderColor, "Shader: %s", shaderType.c_str());

                                    if (!mat.meshName.empty()) {
                                        ImGui::Text("Mesh: %s", mat.meshName.c_str());
                                    }
                                    if (!mat.alphaFlag.empty()) {
                                        ImVec4 color = mat.isTranslucent ? ImVec4(0.4f, 0.8f, 1.0f, 1.0f) : ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
                                        ImGui::TextColored(color, "Alpha: %s", mat.alphaFlag.c_str());
                                    }
                                    if (!mat.textureName.empty()) {
                                        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Texture: %s", mat.textureName.c_str());
                                    }

                                    ImGui::Unindent();
                                }
                            }

                            ImGui::EndTabItem();
                        }


                        if (ImGui::BeginTabItem("Submeshes")) {
                            const auto& d = tool.currentPcmDetails;

                            for (size_t i = 0; i < d.submeshes.size(); i++) {
                                const auto& sm = d.submeshes[i];
                                std::string label = "Submesh " + std::to_string(i);
                                if (!sm.name.empty()) label += ": " + sm.name;

                                if (ImGui::CollapsingHeader(label.c_str())) {
                                    ImGui::Indent();


                                    ImGui::TextColored(ImVec4(0.7f, 0.9f, 1.0f, 1.0f), "Geometry");
                                    ImGui::Text("  Vertices: %u @ 0x%X", sm.vertexCount, sm.vertexOffset);
                                    ImGui::Text("  Indices: %u @ 0x%X", sm.indexCount, sm.indexOffset);
                                    ImGui::Text("  Stride: %u bytes", sm.stride);
                                    ImGui::Text("  Primitive: %s", sm.primitiveType == 4 ? "TRIANGLELIST" :
                                                                    sm.primitiveType == 5 ? "TRIANGLESTRIP" : "UNKNOWN");
                                    ImGui::Text("  VBuffer Size: %u bytes", sm.vertexBufferSize);
                                    if (sm.boundingRadius > 0) {
                                        ImGui::Text("  Bounding Radius: %.3f", sm.boundingRadius);
                                    }


                                    ImGui::Spacing();
                                    ImGui::TextColored(ImVec4(0.7f, 0.9f, 1.0f, 1.0f), "Vertex Format");
                                    ImGui::BulletText("Position (float3)");
                                    if (sm.hasNormals) ImGui::BulletText("Normal (float3)");
                                    if (sm.hasUV) ImGui::BulletText("UV (float2)");
                                    if (sm.hasBones) {
                                        ImGui::BulletText("Bone Indices");
                                        ImGui::BulletText("Bone Weights");
                                    }


                                    if (sm.boneMapCount > 0) {
                                        ImGui::Spacing();
                                        ImGui::TextColored(ImVec4(0.7f, 0.9f, 1.0f, 1.0f), "Bone Mapping");
                                        ImGui::Text("  Map Offset: 0x%X", sm.boneMapOffset);
                                        ImGui::Text("  Map Count: %u", sm.boneMapCount);
                                    }


                                    if (!sm.materialTexture.empty() || !sm.shaderType.empty()) {
                                        ImGui::Spacing();
                                        ImGui::TextColored(ImVec4(0.7f, 0.9f, 1.0f, 1.0f), "Material");
                                        if (!sm.shaderType.empty()) {
                                            ImGui::Text("  Shader: %s (%u)", sm.shaderType.c_str(), sm.shaderSize);
                                        }
                                        if (!sm.materialTexture.empty()) {
                                            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "  Texture: %s", sm.materialTexture.c_str());
                                        }
                                        if (sm.isTranslucent) {
                                            ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "  Translucent");
                                        }
                                    }

                                    ImGui::Unindent();
                                }
                            }

                            ImGui::EndTabItem();
                        }


                        if (ImGui::BeginTabItem("Bones")) {
                            const auto& d = tool.currentPcmDetails;

                            if (d.bones.empty()) {
                                ImGui::TextDisabled("No skeleton data");
                            } else {
                                ImGui::Text("Bone Count: %zu", d.bones.size());
                                ImGui::TextDisabled("Note: Parent indices use linear fallback");
                                ImGui::TextDisabled("(Hierarchy not stored in PCM)");
                                ImGui::Separator();


                                if (ImGui::BeginTable("BoneTable", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY, ImVec2(0, 300))) {
                                    ImGui::TableSetupColumn("Idx", ImGuiTableColumnFlags_WidthFixed, 30.0f);
                                    ImGui::TableSetupColumn("Position", ImGuiTableColumnFlags_WidthStretch);
                                    ImGui::TableSetupColumn("Parent", ImGuiTableColumnFlags_WidthFixed, 45.0f);
                                    ImGui::TableSetupColumn("Role", ImGuiTableColumnFlags_WidthStretch);
                                    ImGui::TableHeadersRow();

                                    for (const auto& bone : d.bones) {
                                        ImGui::TableNextRow();
                                        ImGui::TableSetColumnIndex(0);
                                        ImGui::Text("%d", bone.index);
                                        ImGui::TableSetColumnIndex(1);
                                        ImGui::Text("%.2f, %.2f, %.2f", bone.posX, bone.posY, bone.posZ);
                                        ImGui::TableSetColumnIndex(2);
                                        if (bone.parentIndex >= 0) {
                                            ImGui::Text("%d", bone.parentIndex);
                                        } else {
                                            ImGui::TextDisabled("-1");
                                        }
                                        ImGui::TableSetColumnIndex(3);
                                        if (!bone.inferredRole.empty()) {
                                            ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.4f, 1.0f), "%s", bone.inferredRole.c_str());
                                        }
                                    }

                                    ImGui::EndTable();
                                }
                            }

                            ImGui::EndTabItem();
                        }


                        if (ImGui::BeginTabItem("Legacy")) {
                            ImGui::Text("Skeleton (Legacy)");
                            ImGui::Separator();
                            ImGui::Text("Bone Count: %u", tool.currentPcmSkeleton.count);
                            if (tool.currentPcmSkeleton.count > 0) {
                                ImGui::Text("Bone Offset: %u (0x%X)", tool.currentPcmSkeleton.offset, tool.currentPcmSkeleton.offset);
                            }

                            ImGui::Spacing();
                            ImGui::Text("Mesh Info (Legacy)");
                            ImGui::Separator();

                            for(size_t i=0; i<tool.currentPcmInfos.size(); i++) {
                                const auto& info = tool.currentPcmInfos[i];
                                std::string headerLabel = "Mesh " + std::to_string(i);
                                if (!info.name.empty()) headerLabel += ": " + info.name;

                                if (ImGui::CollapsingHeader(headerLabel.c_str())) {
                                    ImGui::Text("Vertices:   %u @ 0x%X", info.vCount, info.vOffset);
                                    ImGui::Text("Faces:      %u @ 0x%X", info.iCount, info.iOffset);
                                    ImGui::Text("Stride:     %u", info.stride);
                                    ImGui::Text("Prim Type:  %u", info.primitiveType);
                                    if (info.hasUV) ImGui::BulletText("Has UVs");
                                    if (info.hasBones) ImGui::BulletText("Has Bones");
                                }
                            }

                            ImGui::EndTabItem();
                        }

                ImGui::EndTabBar();
            }
        }
        ImGui::End();
    }

    if (tool.showWorldMeshDetails && tool.selectedMeshIndex >= 0 &&
        tool.selectedMeshIndex < (int)tool.previewMeshes.size()) {
        const auto& m = tool.previewMeshes[tool.selectedMeshIndex];
        const bool hasSelectedInstance =
            tool.selectedMeshInstanceIndex >= 0 &&
            tool.selectedMeshInstanceIndex < (int)m.instances.size();
        const auto* selectedInstance = hasSelectedInstance
            ? &m.instances[tool.selectedMeshInstanceIndex] : nullptr;
        const std::string visibleName =
            selectedInstance && !selectedInstance->name.empty()
                ? selectedInstance->name
                : (m.meshName.empty() ? ("Mesh " + std::to_string(tool.selectedMeshIndex)) : m.meshName);
        const std::string title = "World Mesh Details: " + visibleName + "###WorldMeshDetails";

        ImGui::SetNextWindowSize(ImVec2(430, 520), ImGuiCond_FirstUseEver);
        if (ImGui::Begin(title.c_str(), &tool.showWorldMeshDetails,
                         ImGuiWindowFlags_NoDocking)) {
            ImGui::Text("Index: %d / %d", tool.selectedMeshIndex,
                        (int)tool.previewMeshes.size());
            if (selectedInstance) {
                ImGui::Text("Instance: %d / %d", tool.selectedMeshInstanceIndex,
                            (int)m.instances.size());
            }
            ImGui::Separator();
            ImGui::Text("Name: %s", visibleName.c_str());
            if (!m.textureName.empty()) {
                ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
                                   "Texture: %s", m.textureName.c_str());
            } else {
                ImGui::TextDisabled("Texture: (none)");
            }

            ImGui::Spacing();
            ImGui::Text("Bounding Box");
            ImGui::Text("  Min: (%.1f, %.1f, %.1f)",
                        selectedInstance ? selectedInstance->bboxMin[0] : m.bboxMin[0],
                        selectedInstance ? selectedInstance->bboxMin[1] : m.bboxMin[1],
                        selectedInstance ? selectedInstance->bboxMin[2] : m.bboxMin[2]);
            ImGui::Text("  Max: (%.1f, %.1f, %.1f)",
                        selectedInstance ? selectedInstance->bboxMax[0] : m.bboxMax[0],
                        selectedInstance ? selectedInstance->bboxMax[1] : m.bboxMax[1],
                        selectedInstance ? selectedInstance->bboxMax[2] : m.bboxMax[2]);

            ImGui::Spacing();
            ImGui::Text("Source");
            if (!m.sourcePack.empty()) {
                const fs::path packPath(m.sourcePack);
                ImGui::Text("  Pack: %s", packPath.filename().string().c_str());
                ImGui::Text("  Offset: 0x%X", m.sourceOffset);
                ImGui::Text("  Size: %u bytes", m.sourceSize);
            } else {
                ImGui::TextDisabled("  (no source information)");
            }

            const char* shaderTypeName = "Unknown";
            switch (m.shaderType) {
                case 2: shaderTypeName = "Character"; break;
                case 7: shaderTypeName = "World"; break;
                case 8: shaderTypeName = "Street"; break;
                case 11: shaderTypeName = "Translucent"; break;
            }
            ImGui::Spacing();
            ImGui::Text("Rendering");
            ImGui::Text("  Indices: %d", m.indexCount);
            ImGui::Text("  Shader: %s (%u)", shaderTypeName, m.shaderType);
            ImGui::Text("  Translucent: %s", m.isTranslucent ? "Yes" : "No");

            ImGui::Spacing();
            if (ImGui::Button("Export PCM", ImVec2(-1, 0))) {
                tool.ExportSelectedWorldMesh(false);
            }
            if (ImGui::Button("Export GLB", ImVec2(-1, 0))) {
                tool.ExportSelectedWorldMesh(true);
            }
        }
        ImGui::End();
    }

    if (tool.notificationTimer > 0.0f) {
        tool.notificationTimer -= ImGui::GetIO().DeltaTime;
        float alpha = 1.0f;
        if (tool.notificationTimer < 0.5f) alpha = tool.notificationTimer / 0.5f;

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
