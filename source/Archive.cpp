#include "SpiderManTool.h"
#include "NalIntegration.h"
#include <cstring>
#include <fstream>
#include <sstream>

void SpiderManTool::OpenPCPack(const std::string& path) {
    if (loadedPCPackPath == path) return;

    const bool preservePreviewTab = activePreviewTab >= 0 && isModelLoaded;
    if (!preservePreviewTab) {
        isModelLoaded = false;
        isModelPreview = false;
    }

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

        for (auto& t : textureNameCache) {
            if (t.second != 0) glDeleteTextures(1, &t.second);
        }
        textureNameCache.clear();

        selectedMeshIndex = -1;
        selectedMeshInstanceIndex = -1;
        selectedMeshPcmData.clear();
        showWorldMeshDetails = false;

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
    if (!file.is_open()) {  return; }

    size_t size = file.tellg();
    file.seekg(0);
    pcPackData.resize(size);
    file.read((char*)pcPackData.data(), size);
    loadedPCPackPath = path;

    currentDir = PackDirectory::Parse(pcPackData);

    entries.clear();
    if (!currentDir.valid) {

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
            case RES_KEY_VISEME_STREAM: e.name += ".viseme"; break;
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

    std::sort(entries.begin(), entries.end(), [](const FileEntry& a, const FileEntry& b) {
        return a.name < b.name;
    });

    LoadAnimationStateMachinesForCurrentPack();
    LoadCollisionMeshesForCurrentPack();

    if (!preservePreviewTab) {
        LoadSkeletonForCurrentPack();
        LoadAnimationForCurrentPack();
    }
}

void SpiderManTool::LoadAnimationStateMachinesForCurrentPack() {
    animationStateMachines.clear();
    if (!currentDir.valid || pcPackData.empty()) return;
    for (const auto& resource : currentDir.resources) {
        if (resource.type != RES_KEY_ALS_FILE || resource.size == 0) continue;
        if (resource.offset > pcPackData.size() ||
            resource.size > pcPackData.size() - resource.offset) continue;
        auto file = UsmAls::Parse(pcPackData.data() + resource.offset,
                                  resource.size, resource.hash);
        if (file.valid) animationStateMachines.push_back(std::move(file));
    }
}

// Parses every COLLISION_MESH ('COLL') resource in the open pack.
//
// Layout, verified against the shipped data (all axis triples come out exactly mutually
// orthogonal, which is what confirms the field boundaries):
//     0x00 'COLL' | 0x04 version | 0x08 total size | 0x0C count | 0x10 obb[]
// and per 0x38-byte OBB:
//     0x00 center | 0x0C bounding radius | 0x10 X axis | 0x1C Y axis | 0x28 Z axis | 0x34 flags
// The axis vectors are already scaled by their half-extent, so the eight corners are simply
// center +/- X +/- Y +/- Z. Note OpenUSM's colmesh_common.h types the Y/Z axis floats as `int`
// (field_1C..field_30) -- that is a decompiler artifact, they are floats.
// World geometry is assembled from every pack in foundPacks (LoadAllWorldGeometries), never through
// OpenPCPack, so currentDir/pcPackData describe an unrelated pack and carry none of the world's
// collision. Scan the packs directly instead.
void SpiderManTool::LoadCollisionMeshesForWorld() {
    collisionGroups.clear();
    collisionVertCount = 0;

    for (const auto& path : foundPacks) {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) continue;
        const std::streamsize size = file.tellg();
        if (size <= 0) continue;
        file.seekg(0, std::ios::beg);
        std::vector<uint8_t> blob(static_cast<size_t>(size));
        if (!file.read(reinterpret_cast<char*>(blob.data()), size)) continue;

        const PackDirectory dir = PackDirectory::Parse(blob);
        if (!dir.valid) continue;
        AppendCollisionGroups(blob, dir);
    }
}

void SpiderManTool::LoadCollisionMeshesForCurrentPack() {
    collisionGroups.clear();
    collisionVertCount = 0;
    if (!currentDir.valid || pcPackData.empty()) return;
    AppendCollisionGroups(pcPackData, currentDir);
}

void SpiderManTool::AppendCollisionGroups(const std::vector<uint8_t>& packData,
                                          const PackDirectory& dir) {
    for (const auto& resource : dir.resources) {
        if (resource.type != RES_KEY_COLLISION_MESH || resource.size < 0x48) continue;
        if (resource.offset > packData.size() ||
            resource.size > packData.size() - resource.offset) continue;

        const uint8_t* d = packData.data() + resource.offset;
        if (std::memcmp(d, "COLL", 4) != 0) continue;

        CollisionGroup group;
        auto nameIt = dictionary.find(resource.hash);
        if (nameIt != dictionary.end()) group.name = StrToLower(nameIt->second);

        const uint32_t count = (resource.size - 0x10) / 0x38;
        for (uint32_t i = 0; i < count; ++i) {
            const uint8_t* o = d + 0x10 + i * 0x38;
            CollisionObb obb;
            std::memcpy(obb.center, o + 0x00, sizeof(float) * 3);
            std::memcpy(&obb.radius, o + 0x0C, sizeof(float));
            std::memcpy(obb.axisX,  o + 0x10, sizeof(float) * 3);
            std::memcpy(obb.axisY,  o + 0x1C, sizeof(float) * 3);
            std::memcpy(obb.axisZ,  o + 0x28, sizeof(float) * 3);

            // Degenerate/placeholder nodes carry a zero frame; skip so they don't draw as dots.
            const float ext = std::fabs(obb.axisX[0]) + std::fabs(obb.axisY[1]) + std::fabs(obb.axisZ[2]);
            if (ext <= 0.0f) continue;

            group.obbs.push_back(obb);
        }

        if (!group.obbs.empty()) collisionGroups.push_back(std::move(group));
    }
}

bool SpiderManTool::PcmModelHasSkeleton(int entryIndex) const {
    if (entryIndex < 0 || entryIndex >= static_cast<int>(entries.size())) return false;
    const auto& entry = entries[entryIndex];
    if (!entry.isPcm || entry.offset > pcPackData.size() ||
        entry.size > pcPackData.size() - entry.offset || entry.size < 16) return false;
    const uint8_t* pcm = pcPackData.data() + entry.offset;
    auto read16 = [&](size_t offset) {
        uint16_t value = 0;
        std::memcpy(&value, pcm + offset, sizeof(value));
        return value;
    };
    auto read32 = [&](size_t offset) {
        uint32_t value = 0;
        std::memcpy(&value, pcm + offset, sizeof(value));
        return value;
    };
    const uint32_t entryCount = read32(8);
    const uint32_t tableOffset = read32(12);
    if (entryCount > 1000 || tableOffset > entry.size ||
        static_cast<uint64_t>(entryCount) * 12 > entry.size - tableOffset) return false;
    for (uint32_t index = 0; index < entryCount; ++index) {
        const size_t record = tableOffset + static_cast<size_t>(index) * 12;
        const uint16_t tag = read16(record + 2);
        const uint32_t object = read32(record + 4);
        if (tag == 768 && object <= entry.size - 4 && read32(object) > 0) return true;
        if (tag != 512 || object > entry.size - 16) continue;
        const uint32_t submeshCount = read32(object + 8);
        const uint32_t submeshTable = read32(object + 12);
        if (submeshCount > 256 || submeshTable > entry.size ||
            static_cast<uint64_t>(submeshCount) * 8 > entry.size - submeshTable) continue;
        for (uint32_t submesh = 0; submesh < submeshCount; ++submesh) {
            const size_t item = submeshTable + static_cast<size_t>(submesh) * 8;
            const uint32_t submeshOffset = read32(item + 4);
            if (submeshOffset <= entry.size - 76 && read32(submeshOffset + 72) >= 44) return true;
        }
    }
    return false;
}

bool SpiderManTool::CanViewAnimationStateMachine(int entryIndex) const {
    return !animationStateMachines.empty() && PcmModelHasSkeleton(entryIndex);
}

void SpiderManTool::OpenAnimationStateMachineForModel(int entryIndex) {
    if (!CanViewAnimationStateMachine(entryIndex)) return;
    animationStateMachineModelHash = entries[entryIndex].hash;
    animationStateMachineModelName = entries[entryIndex].name;
    selectedAnimationStateMachine = 0;
    selectedAnimationMachineLayer = 0;
    const auto& machines = animationStateMachines[0].machines;
    for (int index = 0; index < static_cast<int>(machines.size()); ++index) {
        if (machines[index].baseLayer) {
            selectedAnimationMachineLayer = index;
            break;
        }
    }
    showAnimationStateMachine = true;
}

void SpiderManTool::ExtractPack(const std::string& packPath, bool convertAll) {
    OpenPCPack(packPath);
    if (entries.empty()) return;

    const int suspendedPreviewTab = activePreviewTab;
    if (convertAll) {
        // Populate skeleton candidates (and animations) for the pack so per-mesh export can
        // select the right rig AND so LoadAnimationForCurrentPack can resolve clips cross-pack
        // when this pack has no anim file of its own.
        if (suspendedPreviewTab >= 0) StoreActivePreviewTab();
        LoadSkeletonForCurrentPack();
        LoadAnimationForCurrentPack();
    }

    fs::path p(packPath);
    fs::path outDir = fs::current_path() / "extracted" / p.stem();
    fs::create_directories(outDir);

    for(auto& e : entries) {
        fs::path fullFilePath = outDir / e.name;
        if (fullFilePath.has_parent_path()) {
            fs::create_directories(fullFilePath.parent_path());
        }

        if (e.isPcm && convertAll) {
            fs::path glbPath = fullFilePath;
            glbPath.replace_extension(".glb");

            SelectSkeletonForMesh(e.name, e.hash);
            if (!loadedAnimFile) LoadAnimationForCurrentPack();
            std::vector<uint8_t> pcmData(pcPackData.begin() + e.offset, pcPackData.begin() + e.offset + e.size);
            ConvertPCM(pcmData, glbPath.string(), e.hash);
        } else {
            std::ofstream out(fullFilePath, std::ios::binary);
            if (out.is_open()) {
                out.write((char*)&pcPackData[e.offset], e.size);
                out.close();
            } else {

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
        ConvertPCM(pcmData, glbPath.string(), e.hash);
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

        }
    }
}
