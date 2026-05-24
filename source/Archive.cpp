#include "SpiderManTool.h"
#include "NalIntegration.h"
#include <fstream>
#include <sstream>

void SpiderManTool::OpenPCPack(const std::string& path) {
    if (loadedPCPackPath == path) return;

    isModelLoaded = false;
    isModelPreview = false;

    // If the user is browsing the world (isWorldMode), keep the world rendered
    // -- clicking another pack should just switch the file browser, not blow
    // the loaded city away. LoadModelToGL handles the actual teardown when
    // the user opens a single-mesh preview.
    const bool preserveWorld = isWorldMode;

    if (!preserveWorld) {
        for (auto& m : previewMeshes) {
            if (m.vao) glDeleteVertexArrays(1, &m.vao);
            if (m.vbo) glDeleteBuffers(1, &m.vbo);
            if (m.ebo) glDeleteBuffers(1, &m.ebo);
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
        selectedMeshPcmData.clear();
        showWorldMeshHexEditor = false;

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

    BinaryReader br(pcPackData);
    br.Skip(24);
    uint32_t headerSize = br.Read<uint32_t>();
    dataOffset = br.Read<uint32_t>();

    size_t start = 0;
    bool found = false;
    const uint32_t magic = 0xE3E3E3E3;
    for(size_t i=0; i<size-4; i++) {
        if (*(uint32_t*)&pcPackData[i] == magic) {
            for(size_t j=i+4; j<size-4; j++) {
                if (*(uint32_t*)&pcPackData[j] == magic) {
                    start = j + 4;
                    found = true;
                    break;
                }
            }
            break;
        }
    }

    entries.clear();
    if (!found) { Log("Invalid PCPACK header."); return; }

    std::map<std::string, int> nameCounts;

    br.Seek(start);
    int counter = 0;
    while (true) {
        uint32_t hash = br.Read<uint32_t>();
        uint32_t type = br.Read<uint32_t>();
        uint32_t offset = br.Read<uint32_t>();
        uint32_t fsize = br.Read<uint32_t>();

        if (type >= 0x1000 || type == 0x0000) break;

        FileEntry e;
        e.hash = hash; e.type = type; e.offset = offset + dataOffset; e.size = fsize;
        e.isPcm = false;
        e.isDds = false;

        if (dictionary.count(hash)) e.name = dictionary[hash];
        else { std::stringstream ss; ss << "Unknown_" << std::hex << hash; e.name = ss.str(); }

        if (fsize > 4 && e.offset + 4 <= pcPackData.size()) {
            const char* magicSig = (const char*)&pcPackData[e.offset];
            if (strncmp(magicSig, "PCM ", 4) == 0) {
                e.name += ".pcm";
                e.isPcm = true;
            }
            else if (strncmp(magicSig, "DDS ", 4) == 0) {
                e.name += ".dds";
                e.isDds = true;
            }
            else e.name += ".dat";
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
        br.Seek(start + (counter + 1) * 16);
        counter++;
    }

    std::sort(entries.begin(), entries.end(), [](const FileEntry& a, const FileEntry& b) {
        return a.name < b.name;
    });

    Log("Opened " + fs::path(path).filename().string());

    // Auto-detect and load skeleton/animation from pack
    LoadSkeletonForCurrentPack();
    LoadAnimationForCurrentPack();
}

void SpiderManTool::ExtractPack(const std::string& packPath, bool convertAll) {
    OpenPCPack(packPath);
    if (entries.empty()) return;

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
        fs::path glbPath = fullFilePath;
        glbPath.replace_extension(".glb");
        std::vector<uint8_t> pcmData(pcPackData.begin() + e.offset, pcPackData.begin() + e.offset + e.size);
        ConvertPCM(pcmData, glbPath.string());
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