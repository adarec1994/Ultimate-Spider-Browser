#include "SpiderManTool.h"
#include <fstream>

void SpiderManTool::LoadAllWorldGeometries() {
    previewMeshes.clear();
    worldPcmQueue.clear();
    isWorldMode = true;

    // DON'T clear globalTextureIndex - we already built it during startup!

    camPos[0] = 0.0f; camPos[1] = 2000.0f; camPos[2] = 2000.0f;
    camFront[0] = 0.0f; camFront[1] = -0.5f; camFront[2] = -1.0f;
    camUp[0] = 0.0f; camUp[1] = 1.0f; camUp[2] = 0.0f;
    camYaw = -90.0f;
    camPitch = -30.0f;
    camSpeed = 500.0f;

    Log("Building world PCM queue...");

    // First, find and queue sky_day.pcm from city_arena.pcpack
    for (const auto& path : foundPacks) {
        std::string stem = StrToLower(path.stem().string());
        if (stem != "city_arena") continue;

        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) continue;

        size_t fileSize = file.tellg();
        if (fileSize < 32) {
            file.close();
            continue;
        }

        file.seekg(24);
        uint32_t headerSize, dataOffset;
        file.read((char*)&headerSize, 4);
        file.read((char*)&dataOffset, 4);

        if (!file.good()) {
            file.close();
            continue;
        }

        size_t start = 0;
        const uint32_t magic = 0xE3E3E3E3;
        size_t headerReadSize = std::min((size_t)200000, fileSize);
        std::vector<uint8_t> tempHeader(headerReadSize);
        file.seekg(0);
        file.read((char*)tempHeader.data(), headerReadSize);

        for(size_t i=0; i + 4 <= tempHeader.size(); i++) {
            if (*(uint32_t*)&tempHeader[i] == magic) {
                bool confirm = false;
                for(size_t j=i+4; j<i+1000 && j + 4 <= tempHeader.size(); j++) {
                    if (*(uint32_t*)&tempHeader[j] == magic) {
                        start = j + 4;
                        confirm = true;
                        break;
                    }
                }
                if(confirm) break;
            }
        }

        if (start == 0) {
            file.close();
            continue;
        }

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

                if (absOffset + 4 > fileSize) {
                    file.seekg(filePos);
                    continue;
                }

                file.seekg(absOffset);
                uint32_t sig = 0;
                file.read((char*)&sig, 4);

                if (file.good() && sig == 0x204D4350) { // PCM
                    std::string entryName = "";
                    if (dictionary.count(hash)) entryName = StrToLower(dictionary[hash]);
                    if (entryName == "sky_day") {
                        Log("Found sky_day.pcm in city_arena.pcpack");
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

    // Now queue world pack PCMs
    for (const auto& path : foundPacks) {
        std::string stem = StrToLower(path.stem().string());
        bool isRelevant = IsWorldPack(stem) || IsWorldInteriorPack(stem);
        if (!isRelevant) continue;

        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) continue;

        size_t fileSize = file.tellg();
        if (fileSize < 32) {
            file.close();
            continue;
        }

        file.seekg(24);
        uint32_t headerSize, dataOffset;
        file.read((char*)&headerSize, 4);
        file.read((char*)&dataOffset, 4);

        if (!file.good()) {
            file.close();
            continue;
        }

        size_t start = 0;
        const uint32_t magic = 0xE3E3E3E3;
        size_t headerReadSize = std::min((size_t)200000, fileSize);
        std::vector<uint8_t> tempHeader(headerReadSize);
        file.seekg(0);
        file.read((char*)tempHeader.data(), headerReadSize);

        for(size_t i=0; i + 4 <= tempHeader.size(); i++) {
            if (*(uint32_t*)&tempHeader[i] == magic) {
                bool confirm = false;
                for(size_t j=i+4; j<i+1000 && j + 4 <= tempHeader.size(); j++) {
                     if (*(uint32_t*)&tempHeader[j] == magic) {
                         start = j + 4;
                         confirm = true;
                         break;
                     }
                }
                if(confirm) break;
            }
        }

        if (start == 0) {
            file.close();
            continue;
        }

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

                if (absOffset + 4 > fileSize) {
                    file.seekg(filePos);
                    continue;
                }

                file.seekg(absOffset);
                uint32_t sig = 0;
                file.read((char*)&sig, 4);

                if (file.good() && sig == 0x204D4350) {
                    std::string entryName = "";
                    if (dictionary.count(hash)) entryName = StrToLower(dictionary[hash]);
                    if (!entryName.empty() && entryName.find(stem) == 0) {
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
    }

    Log("Models queued: " + std::to_string(worldPcmQueue.size()) + ", Global textures available: " + std::to_string(globalTextureIndex.size()));

    if (worldPcmQueue.empty()) {
        Log("No world models found!");
        isLoadingWorld = false;
        currentState = STATE_BROWSER;
        return;
    }

    // Sort by pack path for efficient file access
    std::sort(worldPcmQueue.begin(), worldPcmQueue.end(), [](const auto& a, const auto& b){
        return a.packPath < b.packPath;
    });

    // Start incremental loading
    isLoadingWorld = true;
    worldLoadProgress = 0;
    worldLoadTotal = (int)worldPcmQueue.size();
    currentState = STATE_LOADING_WORLD;
}

void SpiderManTool::LoadWorldGeometryStep(int index) {
    if (index < 0 || index >= (int)worldPcmQueue.size()) return;

    const auto& item = worldPcmQueue[index];

    std::ifstream file(item.packPath, std::ios::binary);
    if (file.is_open()) {
        file.seekg(item.offset);
        if (file.good()) {
            std::vector<uint8_t> fileData(item.size);
            file.read((char*)fileData.data(), item.size);
            file.close();

            if (!fileData.empty()) {
                // Pass nullptr for textureResolver - this makes AddMeshFromData use
                // LoadTextureByName() which searches globalTextureNameIndex and
                // globalTextureIndex (built during startup indexing)
                AddMeshFromData(fileData, "", nullptr);
            }
        } else {
            file.close();
        }
    }
}