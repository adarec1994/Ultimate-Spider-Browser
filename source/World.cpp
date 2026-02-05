#include "SpiderManTool.h"
#include <fstream>
#include <cstring>

void SpiderManTool::LoadAllWorldGeometries() {
    previewMeshes.clear();
    isWorldMode = true;
    isModelPreview = true;

    selectedMeshIndex = -1;
    selectedMeshPcmData.clear();
    showWorldMeshHexEditor = false;

    camPos[0] = 0.0f; camPos[1] = 200.0f; camPos[2] = -600.0f;
    camFront[0] = 0.0f; camFront[1] = -0.3f; camFront[2] = -1.0f;
    camUp[0] = 0.0f; camUp[1] = 1.0f; camUp[2] = 0.0f;
    camYaw = -90.0f;
    camPitch = -15.0f;
    camSpeed = 500.0f;

    InitModelPreview();

    float transformMatrix[16] = {0};
    transformMatrix[0] = -1.0f;
    transformMatrix[5] = 1.0f;
    transformMatrix[10] = 1.0f;
    transformMatrix[15] = 1.0f;

    Log("Loading all world geometries...");

    for (const auto& path : foundPacks) {
        std::string stem = StrToLower(path.stem().string());
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

            uint32_t absOffset = (uint32_t)(dataOffset + offset);

            if (size > 4 && absOffset + 4 <= fileSize) {
                size_t filePos = file.tellg();
                file.seekg(absOffset);
                uint32_t sig = 0;
                file.read((char*)&sig, 4);
                file.clear();
                file.seekg(filePos);

                if (sig == 0x204D4350) {
                    std::string entryName = dictionary.count(hash) ? StrToLower(dictionary[hash]) : "";

                    if (!entryName.empty() && entryName.find(stem) == 0 && entryName.find('_') == std::string::npos) {
                        std::vector<uint8_t> pcmData(size);
                        file.seekg(absOffset);
                        file.read((char*)pcmData.data(), size);
                        file.clear();
                        file.seekg(filePos);

                        AddMeshFromDataWithTransform(pcmData, entryName, nullptr, path.string(), absOffset, transformMatrix);
                    }
                }
            }
        }

        file.close();
    }

    LoadSkybox();

    isModelLoaded = true;
    Log("World loaded. Total meshes: " + std::to_string(previewMeshes.size()));
}

void SpiderManTool::LoadSkybox() {
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
                        std::vector<uint8_t> skyData(size);
                        file.seekg(absOffset);
                        file.read((char*)skyData.data(), size);

                        float transformMatrix[16] = {0};
                        transformMatrix[0] = -1.0f;
                        transformMatrix[5] = 1.0f;
                        transformMatrix[10] = 1.0f;
                        transformMatrix[15] = 1.0f;

                        AddMeshFromDataWithTransform(skyData, "sky_day", nullptr, path.string(), absOffset, transformMatrix);
                        file.close();
                        return;
                    }
                }
                file.clear();
                file.seekg(filePos);
            }
        }
        file.close();
        break;
    }
}