#include "SpiderManTool.h"
#include "NalIntegration.h"
#include <fstream>
#include <sstream>

void SpiderManTool::OpenPCPack(const std::string& path) {
    if (loadedPCPackPath == path) return;

    // Selecting a pack is a browser operation.  Once preview tabs exist, it
    // must not tear down or mutate the active tab merely because the user is
    // looking for another asset.
    const bool preservePreviewTab = activePreviewTab >= 0 && isModelLoaded;
    if (!preservePreviewTab) {
        isModelLoaded = false;
        isModelPreview = false;
    }

    // If the user is browsing the world (isWorldMode), keep the world rendered
    // -- clicking another pack should just switch the file browser, not blow
    // the loaded city away. LoadModelToGL handles the actual teardown when
    // the user opens a single-mesh preview.
    const bool preserveWorld = isWorldMode || preservePreviewTab;

    if (!preserveWorld) {
        for (auto& m : previewMeshes) {
            if (m.vao) glDeleteVertexArrays(1, &m.vao);
            if (m.vbo) glDeleteBuffers(1, &m.vbo);
            if (m.ebo) glDeleteBuffers(1, &m.ebo);
            if (m.instanceVbo) glDeleteBuffers(1, &m.instanceVbo);
        }
        previewMeshes.clear();

        for (auto& t : textureCache) {
            if (t.second != 0) glDeleteTextures(1, &t.second);
        }
        textureCache.clear();

        // Clear name-based texture cache too
        for (auto& t : textureNameCache) {
            if (t.second != 0) glDeleteTextures(1, &t.second);
        }
        textureNameCache.clear();

        // World mesh selection only matters when world geometry is loaded.
        selectedMeshIndex = -1;
        selectedMeshInstanceIndex = -1;
        selectedMeshPcmData.clear();
        showWorldMeshDetails = false;

        // materialMap is per-PCM-being-parsed; ParseMaterialEntries rebuilds
        // it on every AddMeshFromData call. Clearing here is the safe default
        // for non-world flows.
        materialMap.clear();
    }

    if (ddsTextureId != 0) {
        glDeleteTextures(1, &ddsTextureId);
        ddsTextureId = 0;
    }
    showDdsPopup = false;

    selectedFileIndex = -1;
    currentPcmInfos.clear();
    currentPcmIndex = -1;

    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) { Log("Failed to open " + path); return; }

    size_t size = file.tellg();
    file.seekg(0);
    pcPackData.resize(size);
    file.read((char*)pcPackData.data(), size);
    loadedPCPackPath = path;

    // Byte-exact directory parse (replays OpenUSM's resource_directory un-mash;
    // see PackDirectory.h). Replaces the old 0xE3E3E3E3 filler scan.
    currentDir = PackDirectory::Parse(pcPackData);

    entries.clear();
    if (!currentDir.valid) {
        Log("Invalid PCPACK directory: " + currentDir.error);
        return;
    }
    dataOffset = currentDir.dataBase;

    std::map<std::string, int> nameCounts;

    for (const auto& r : currentDir.resources) {
        FileEntry e;
        e.hash = r.hash; e.type = r.type; e.offset = r.offset; e.size = r.size;
        e.isPcm = false;
        e.isDds = false;

        if (dictionary.count(e.hash)) e.name = dictionary[e.hash];
        else { std::stringstream ss; ss << "Unknown_" << std::hex << e.hash; e.name = ss.str(); }

        // Extension: content signature first (drives viewer behavior), then the
        // directory's authoritative resource_key_type.
        const char* sigExt = nullptr;
        if (e.size > 4 && (size_t)e.offset + 4 <= pcPackData.size()) {
            const char* magicSig = (const char*)&pcPackData[e.offset];
            if (strncmp(magicSig, "PCM ", 4) == 0) { sigExt = ".pcm"; e.isPcm = true; }
            else if (strncmp(magicSig, "DDS ", 4) == 0) { sigExt = ".dds"; e.isDds = true; }
        }
        if (sigExt) e.name += sigExt;
        else switch (r.type) {
            case RES_KEY_ANIMATION:   e.name += ".pcanim"; break;
            case RES_KEY_NAL_SKL:     e.name += ".pcskel"; break;
            case RES_KEY_ALS_FILE:    e.name += ".als"; break;
            case RES_KEY_ENTITY:
            case RES_KEY_EXTERNAL_ENT: e.name += ".ent"; break;
            case RES_KEY_SCN_ENTITY:  e.name += ".scn"; break;
            case RES_KEY_IFL:         e.name += ".ifl"; break;
            case RES_KEY_SCRIPT:      e.name += ".script"; break;
            default:                  e.name += ".dat"; break;
        }

        std::string originalName = e.name;
        if (nameCounts.count(originalName)) {
            int idx = nameCounts[originalName];
            fs::path p(originalName);
            std::string stem = p.stem().string();
            std::string ext = p.extension().string();
            e.name = stem + "_" + std::to_string(idx) + ext;
            nameCounts[originalName]++;
        } else {
            nameCounts[originalName] = 0;
        }

        entries.push_back(e);
    }

    Log("Directory: " + std::to_string(currentDir.resources.size()) + " resources, " +
        std::to_string(currentDir.skeletons.size()) + " skeleton(s), " +
        std::to_string(currentDir.animFiles.size()) + " anim file(s), " +
        std::to_string(currentDir.anims.size()) + " named anim(s)");

    std::sort(entries.begin(), entries.end(), [](const FileEntry& a, const FileEntry& b) {
        return a.name < b.name;
    });

    Log("Opened " + fs::path(path).filename().string());

    // Active preview tabs own their skeleton/animation state.  Loading the
    // newly browsed pack here would replace that state and corrupt the active
    // model.  A newly opened PCM tab performs this work after caching the old
    // tab; headless/export and the first-tab path retain the eager behavior.
    if (!preservePreviewTab) {
        LoadSkeletonForCurrentPack();
        LoadAnimationForCurrentPack();
    }
}

void SpiderManTool::ExtractPack(const std::string& packPath, bool convertAll) {
    OpenPCPack(packPath);
    if (entries.empty()) return;

    const int suspendedPreviewTab = activePreviewTab;
    if (convertAll && suspendedPreviewTab >= 0) {
        StoreActivePreviewTab();
        LoadSkeletonForCurrentPack();
        LoadAnimationForCurrentPack();
    }

    fs::path p(packPath);
    fs::path outDir = fs::current_path() / "extracted" / p.stem();
    fs::create_directories(outDir);

    Log("Extracting to: " + outDir.string());

    for(auto& e : entries) {
        fs::path fullFilePath = outDir / e.name;
        if (fullFilePath.has_parent_path()) {
            fs::create_directories(fullFilePath.parent_path());
        }

        if (e.isPcm && convertAll) {
            fs::path glbPath = fullFilePath;
            glbPath.replace_extension(".glb");
            // Anims are decoded per-skeleton at pack open; only the skinning
            // skeleton needs to follow the mesh here.
            SelectSkeletonForMesh(e.name, e.hash);
            if (!loadedAnimFile) LoadAnimationForCurrentPack();
            std::vector<uint8_t> pcmData(pcPackData.begin() + e.offset, pcPackData.begin() + e.offset + e.size);
            ConvertPCM(pcmData, glbPath.string());
        } else {
            std::ofstream out(fullFilePath, std::ios::binary);
            if (out.is_open()) {
                out.write((char*)&pcPackData[e.offset], e.size);
                out.close();
            } else {
                Log("Failed to write file: " + fullFilePath.string());
                continue;
            }
        }
    }
    if (suspendedPreviewTab >= 0 && previewTabs[suspendedPreviewTab].hasCachedState) {
        RestorePreviewTab(suspendedPreviewTab);
    }
    ShowNotification("Pack extracted to:\n" + outDir.string());
}

void SpiderManTool::ExtractFile(int index, bool asGlb) {
    if (index < 0 || index >= entries.size()) return;
    if (pcPackData.empty()) return;

    const auto& e = entries[index];
    fs::path p(loadedPCPackPath);
    fs::path outDir = fs::current_path() / "extracted" / p.stem();
    fs::path fullFilePath = outDir / e.name;

    if (fullFilePath.has_parent_path()) {
        fs::create_directories(fullFilePath.parent_path());
    }

    if (e.isPcm && asGlb) {
        const int suspendedPreviewTab = activePreviewTab;
        if (suspendedPreviewTab >= 0) {
            StoreActivePreviewTab();
            LoadSkeletonForCurrentPack();
            LoadAnimationForCurrentPack();
        }
        fs::path glbPath = fullFilePath;
        glbPath.replace_extension(".glb");
        SelectSkeletonForMesh(e.name, e.hash);
        if (!loadedAnimFile) LoadAnimationForCurrentPack();
        std::vector<uint8_t> pcmData(pcPackData.begin() + e.offset, pcPackData.begin() + e.offset + e.size);
        ConvertPCM(pcmData, glbPath.string());
        if (suspendedPreviewTab >= 0 && previewTabs[suspendedPreviewTab].hasCachedState) {
            RestorePreviewTab(suspendedPreviewTab);
        }
        ShowNotification("Saved GLB to:\n" + glbPath.string());
    } else {
        std::ofstream out(fullFilePath, std::ios::binary);
        if (out.is_open()) {
            out.write((char*)&pcPackData[e.offset], e.size);
            out.close();
            ShowNotification("Saved file to:\n" + fullFilePath.string());
        } else {
            Log("Failed to write file: " + fullFilePath.string());
        }
    }
}
