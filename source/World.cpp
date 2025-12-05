#include "SpiderManTool.h"
#include <fstream>
#include <cstring>
#include <cmath>
#include <set>

// NOTE: After extensive reverse engineering of the PCPACK format, we discovered that
// Ultimate Spider-Man does NOT use instance-based rendering for props like streetlamps.
//
// The scene graph (Type 10) contains references to prop types (STREETLAMPA, TRAFFICLIGHTA, etc.)
// for purposes like collision detection and lighting attachment points, but the actual
// visual geometry is BAKED DIRECTLY into the tile meshes (IJC, IHA, etc.) with world
// coordinates already included.
//
// The position data found in the scene graph (at node+0xAC) is shared reference data,
// not unique instance positions - all instances of a prop type have the SAME position
// value, which proves it's not per-instance placement data.
//
// Therefore, we simply load the tile PCM files which already contain the prop geometry
// at correct world positions.

void SpiderManTool::LoadAllWorldGeometries() {
    previewMeshes.clear();
    worldPcmQueue.clear();
    isWorldMode = true;

    selectedMeshIndex = -1;
    selectedMeshPcmData.clear();
    showWorldMeshHexEditor = false;

    // Set initial camera position
    camPos[0] = 0.0f; camPos[1] = 200.0f; camPos[2] = -600.0f;
    camFront[0] = 0.0f; camFront[1] = -0.3f; camFront[2] = -1.0f;
    camUp[0] = 0.0f; camUp[1] = 1.0f; camUp[2] = 0.0f;
    camYaw = -90.0f;
    camPitch = -15.0f;
    camSpeed = 500.0f;

    Log("Building world PCM queue...");

    int tileCount = 0;

    // Process all world packs (2-letter names like IJ, IH, IG, etc.)
    for (const auto& path : foundPacks) {
        std::string stem = StrToLower(path.stem().string());

        // Only process standard world packs (2-letter names)
        // Skip interiors (*_INT) and other special packs
        if (!IsWorldPack(stem)) continue;

        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) continue;

        size_t fileSize = file.tellg();
        if (fileSize < 32) { file.close(); continue; }

        file.seekg(24);
        uint32_t headerSize, dataOffset;
        file.read((char*)&headerSize, 4);
        file.read((char*)&dataOffset, 4);
        if (!file.good()) { file.close(); continue; }

        // Find entry table start (after 0xE3E3E3E3 markers)
        size_t start = 0;
        const uint32_t magic = 0xE3E3E3E3;
        size_t headerReadSize = std::min((size_t)500000, fileSize);
        std::vector<uint8_t> tempHeader(headerReadSize);
        file.seekg(0);
        file.read((char*)tempHeader.data(), headerReadSize);

        for (size_t i = 0; i + 4 <= tempHeader.size(); i++) {
            if (*(uint32_t*)&tempHeader[i] == magic) {
                for (size_t j = i + 4; j < i + 1000 && j + 4 <= tempHeader.size(); j++) {
                    if (*(uint32_t*)&tempHeader[j] == magic) {
                        start = j + 4;
                        break;
                    }
                }
                if (start != 0) break;
            }
        }

        if (start == 0) { file.close(); continue; }

        file.clear();
        file.seekg(start);

        // Collect all PCM entries
        std::vector<std::tuple<uint32_t, uint32_t, uint32_t>> pcmEntries;

        while (file.good()) {
            uint32_t hash, type, offset, size;
            file.read((char*)&hash, 4);
            file.read((char*)&type, 4);
            file.read((char*)&offset, 4);
            file.read((char*)&size, 4);
            if (!file.good()) break;
            if (type >= 0x1000 || type == 0x0000) break;

            uint32_t absOffset = (uint32_t)(dataOffset + offset);

            if (type == 21 && size > 4) {
                pcmEntries.push_back({hash, absOffset, size});
            }
        }

        // Add tile's PCMs (the main tile geometry like IJC, IHA, etc.)
        // Main tile meshes have names like "igc", "ijc", "iha" - they're 3 chars: 2-letter prefix + 1 suffix
        // These contain world-coordinate geometry including props baked in.
        // Skip prop template meshes like "ig_cp_gate", "ij_fence" which have underscores and local coords.
        for (const auto& [hash, absOffset, size] : pcmEntries) {
            file.clear();
            file.seekg(absOffset);
            uint32_t sig = 0;
            file.read((char*)&sig, 4);

            if (file.good() && sig == 0x204D4350) { // "PCM "
                std::string entryName = dictionary.count(hash) ? StrToLower(dictionary[hash]) : "";

                // Skip if name doesn't start with tile prefix
                if (entryName.empty() || entryName.find(stem) != 0) {
                    continue;
                }

                // Only load 3-character tile meshes (e.g., "igc", "ija", "ihb")
                // Skip anything with underscores (prop templates like "ig_cp_gate")
                if (entryName.length() == 3 && entryName.find('_') == std::string::npos) {
                    WorldPCMItem item;
                    item.packPath = path.string();
                    item.offset = absOffset;
                    item.size = size;
                    worldPcmQueue.push_back(item);
                    tileCount++;
                }
            }
        }

        file.close();
    }

    // Add sky_day from city_arena
    for (const auto& path : foundPacks) {
        std::string stem = StrToLower(path.stem().string());
        if (stem != "city_arena") continue;

        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) continue;

        size_t fileSize = file.tellg();
        if (fileSize < 32) { file.close(); continue; }

        file.seekg(24);
        uint32_t headerSize, dataOffset;
        file.read((char*)&headerSize, 4);
        file.read((char*)&dataOffset, 4);
        if (!file.good()) { file.close(); continue; }

        size_t start = 0;
        const uint32_t magic = 0xE3E3E3E3;
        size_t headerReadSize = std::min((size_t)500000, fileSize);
        std::vector<uint8_t> tempHeader(headerReadSize);
        file.seekg(0);
        file.read((char*)tempHeader.data(), headerReadSize);

        for (size_t i = 0; i + 4 <= tempHeader.size(); i++) {
            if (*(uint32_t*)&tempHeader[i] == magic) {
                for (size_t j = i + 4; j < i + 1000 && j + 4 <= tempHeader.size(); j++) {
                    if (*(uint32_t*)&tempHeader[j] == magic) {
                        start = j + 4;
                        break;
                    }
                }
                if (start != 0) break;
            }
        }

        if (start == 0) { file.close(); continue; }

        file.clear();
        file.seekg(start);

        while (file.good()) {
            uint32_t hash, type, offset, size;
            file.read((char*)&hash, 4);
            file.read((char*)&type, 4);
            file.read((char*)&offset, 4);
            file.read((char*)&size, 4);
            if (!file.good()) break;
            if (type >= 0x1000 || type == 0x0000) break;

            if (size > 4) {
                size_t filePos = file.tellg();
                uint32_t absOffset = (uint32_t)(dataOffset + offset);
                if (absOffset + 4 > fileSize) { file.seekg(filePos); continue; }

                file.seekg(absOffset);
                uint32_t sig = 0;
                file.read((char*)&sig, 4);

                if (file.good() && sig == 0x204D4350) {
                    std::string entryName = dictionary.count(hash) ? StrToLower(dictionary[hash]) : "";
                    if (entryName == "sky_day") {
                        WorldPCMItem item;
                        item.packPath = path.string();
                        item.offset = absOffset;
                        item.size = size;
                        worldPcmQueue.push_back(item);
                    }
                }
                file.clear();
                file.seekg(filePos);
            }
        }
        file.close();
        break;
    }

    Log("Tile models: " + std::to_string(tileCount));
    Log("Total models queued: " + std::to_string(worldPcmQueue.size()));

    if (worldPcmQueue.empty()) {
        Log("No world models found!");
        isLoadingWorld = false;
        currentState = STATE_BROWSER;
        return;
    }

    isLoadingWorld = true;
    worldLoadProgress = 0;
    worldLoadTotal = (int)worldPcmQueue.size();
    currentState = STATE_LOADING_WORLD;
}

void SpiderManTool::LoadWorldGeometryStep(int index) {
    if (index < 0 || index >= (int)worldPcmQueue.size()) return;

    const auto& item = worldPcmQueue[index];

    // Create transform matrix for X-axis flip (to correct coordinate system)
    float transformMatrix[16];
    memset(transformMatrix, 0, sizeof(float) * 16);
    transformMatrix[0] = -1.0f;  // Flip X axis
    transformMatrix[5] = 1.0f;
    transformMatrix[10] = 1.0f;
    transformMatrix[15] = 1.0f;

    // Capture the count of meshes BEFORE loading this item
    size_t startMeshCount = previewMeshes.size();

    std::ifstream file(item.packPath, std::ios::binary);
    if (file.is_open()) {
        file.seekg(item.offset);
        if (file.good()) {
            std::vector<uint8_t> fileData(item.size);
            file.read((char*)fileData.data(), item.size);
            file.close();

            if (!fileData.empty()) {
                AddMeshFromDataWithTransform(fileData, "", nullptr, item.packPath, item.offset, transformMatrix);
            }
        } else {
            file.close();
        }
    }

    // Apply texture-based visibility overrides to the newly added meshes
    for (size_t i = startMeshCount; i < previewMeshes.size(); i++) {
        auto& m = previewMeshes[i];
        std::string texName = StrToLower(m.textureName);
        std::string meshNameLower = StrToLower(m.meshName);

        // --- GROUP 1: Volume / Ghostly (Alpha 0.3) ---
        if (texName.find("colorvol_1") != std::string::npos ||
            meshNameLower.find("alpha01") != std::string::npos ||
            meshNameLower.find("volumn_light") != std::string::npos) {
            m.isColorVolume = true; // Sets alpha to 0.3 in shader
        }

        // --- GROUP 2: Use Texture Alpha Channel ---
        // These textures have alpha channels that should be used for transparency
        // (e.g., fences, foliage, graffiti decals). The shader should use texColor.a
        // instead of a fixed alpha value.
        else if (texName.find("debris") != std::string::npos ||
                 texName.find("graffiti") != std::string::npos ||
                 texName.find("pothole") != std::string::npos ||
                 texName.find("markings") != std::string::npos ||
                 texName.find("manhole") != std::string::npos ||
                 texName.find("foliage") != std::string::npos ||
                 texName.find("vine") != std::string::npos ||
                 texName.find("fence") != std::string::npos ||
                 texName.find("barbwire") != std::string::npos ||
                 texName.find("fireescape") != std::string::npos ||
                 texName.find("gate") != std::string::npos ||
                 texName.find("tall_grass") != std::string::npos ||
                 texName.find("clouds") != std::string::npos ||
                 texName.find("chain") != std::string::npos ||
                 texName.find("nat_clouds_day") != std::string::npos ||
                 texName.find("water") != std::string::npos ||
                 texName.find("ocean") != std::string::npos ||
                 texName.find("bridge") != std::string::npos ||
                 texName.find("beam") != std::string::npos ||
                 // Special check for "tree": make sure we don't accidentally match "street"
                 (texName.find("tree") != std::string::npos && texName.find("street") == std::string::npos)) {

            m.isTranslucent = true; // Enable alpha blending using texture's alpha channel
            m.isHidden = false;
        }

        // --- GROUP 3: Hidden (Invisible) ---
        else if (meshNameLower.find("material #10") != std::string::npos ||
                 meshNameLower.find("light volume") != std::string::npos ||
                 texName.find("light volume") != std::string::npos ||
                 texName.find("lightvol") != std::string::npos) {
            m.isHidden = true; // Completely invisible
        }
    }
}