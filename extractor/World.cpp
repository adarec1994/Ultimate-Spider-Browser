#include "SpiderManTool.h"
#include <fstream>

void SpiderManTool::LoadAllWorldGeometries() {
    previewMeshes.clear();
    isWorldMode = true;
    globalTextureIndex.clear();

    camPos[0] = 0.0f; camPos[1] = 2000.0f; camPos[2] = 2000.0f;
    camFront[0] = 0.0f; camFront[1] = -0.5f; camFront[2] = -1.0f;
    camUp[0] = 0.0f; camUp[1] = 1.0f; camUp[2] = 0.0f;
    camYaw = -90.0f;
    camPitch = -30.0f;
    camSpeed = 500.0f;

    Log("Loading World Context...");

    std::vector<std::pair<std::string, std::pair<uint32_t, uint32_t>>> pcmQueue;

    for (const auto& path : foundPacks) {
        std::string stem = StrToLower(path.stem().string());
        bool isRelevant = IsWorldPack(stem) || IsWorldInteriorPack(stem);
        if (!isRelevant) continue;

        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) continue;

        file.seekg(24);
        uint32_t headerSize, dataOffset;
        file.read((char*)&headerSize, 4);
        file.read((char*)&dataOffset, 4);

        size_t start = 0;
        const uint32_t magic = 0xE3E3E3E3;
        std::vector<uint8_t> tempHeader(200000);
        file.seekg(0);
        file.read((char*)tempHeader.data(), tempHeader.size());

        for(size_t i=0; i<tempHeader.size()-4; i++) {
            if (*(uint32_t*)&tempHeader[i] == magic) {
                bool confirm = false;
                for(size_t j=i+4; j<i+1000; j++) {
                     if (*(uint32_t*)&tempHeader[j] == magic) {
                         start = j + 4;
                         confirm = true;
                         break;
                     }
                }
                if(confirm) break;
            }
        }

        if (start == 0) continue;

        file.seekg(start);

        while (true) {
            uint32_t hash, type, offset, size;
            file.read((char*)&hash, 4);
            file.read((char*)&type, 4);
            file.read((char*)&offset, 4);
            file.read((char*)&size, 4);

            if (type >= 0x1000 || type == 0x0000) break;

            if (size > 4) {
                size_t filePos = file.tellg();
                uint32_t absOffset = (uint32_t)(dataOffset + offset);

                file.seekg(absOffset);
                uint32_t sig;
                file.read((char*)&sig, 4);

                if (sig == 0x204D4350) {
                    std::string entryName = "";
                    if (dictionary.count(hash)) entryName = StrToLower(dictionary[hash]);
                    if (!entryName.empty() && entryName.find(stem) == 0) {
                        pcmQueue.push_back({path.string(), {absOffset, size}});
                    }
                }
                else if (sig == 0x20534444) {
                    globalTextureIndex[hash] = {path.string(), absOffset, size};
                }

                file.seekg(filePos);
            }
        }
        file.close();
    }

    Log("Index Complete. Textures: " + std::to_string(globalTextureIndex.size()) + ", Models: " + std::to_string(pcmQueue.size()));

    std::sort(pcmQueue.begin(), pcmQueue.end(), [](const auto& a, const auto& b){
        return a.first < b.first;
    });

    std::string currentPath = "";
    std::ifstream currentFile;

    auto globalResolver = [&](uint32_t texHash) -> unsigned int {
        if (textureCache.count(texHash)) return textureCache[texHash];
        if (globalTextureIndex.count(texHash)) {
            auto& loc = globalTextureIndex[texHash];

            std::ifstream texFile(loc.packPath, std::ios::binary);
            if(texFile.is_open()) {
                texFile.seekg(loc.offset);
                std::vector<uint8_t> ddsData(loc.size);
                texFile.read((char*)ddsData.data(), loc.size);
                texFile.close();

                unsigned int tex = LoadTextureFromData(ddsData);
                if (tex != 0) {
                    textureCache[texHash] = tex;
                    return tex;
                }
            }
        }
        return 0;
    };

    for(const auto& item : pcmQueue) {
        if(item.first != currentPath) {
            if(currentFile.is_open()) currentFile.close();
            currentPath = item.first;
            currentFile.open(currentPath, std::ios::binary);
        }

        if(currentFile.is_open()) {
            currentFile.seekg(item.second.first);
            std::vector<uint8_t> fileData(item.second.second);
            currentFile.read((char*)fileData.data(), item.second.second);
            AddMeshFromData(fileData, "", globalResolver);
        }
    }
    if(currentFile.is_open()) currentFile.close();

    Log("World Context Loaded. Total meshes: " + std::to_string(previewMeshes.size()));
}