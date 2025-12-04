#include "SpiderManTool.h"
#include <fstream>
#include <cstring>
#include <cmath>
#include <set>

// Instance data structure for world placement
struct WorldInstance {
    uint32_t hash;
    std::string name;
    float x, y, z;
};

// Target hashes for CITY_ARENA props we want to instance
static std::set<uint32_t> GetTargetHashes() {
    return {
        0x47FC4522,  // streetlampa
        0x00B4AF58,  // trafficlighta
        0xC626D802,  // fireesca
        0xA63D8D81,  // rooflampa
        0x7944EA50,  // skylighta
        0x7944EA51,  // skylightb
        0x06DCCEC2,  // windowwasher
    };
}

// Extract instances from Type 10 scene graph
static std::vector<WorldInstance> ExtractInstancesFromType10(
    const std::vector<uint8_t>& data,
    const std::map<uint32_t, std::string>& dictionary,
    const std::set<uint32_t>& targetHashes)
{
    std::vector<WorldInstance> instances;
    if (data.size() < 64) return instances;

    const uint32_t NODE_MARKER = 0x7ACE5BAD;

    // Scan for node markers
    for (size_t pos = 0; pos + 20 < data.size(); pos += 4) {
        uint32_t marker;
        memcpy(&marker, &data[pos], 4);

        if (marker != NODE_MARKER) continue;

        // Check hash at marker + 16
        if (pos + 20 > data.size()) continue;

        uint32_t hash;
        memcpy(&hash, &data[pos + 16], 4);

        // Is this a target object?
        if (targetHashes.find(hash) == targetHashes.end()) continue;

        // Search forward for valid world position (XYZ triplet)
        for (size_t searchOff = pos + 20; searchOff + 12 < data.size() && searchOff < pos + 300; searchOff += 4) {
            float x, y, z;
            memcpy(&x, &data[searchOff], 4);
            memcpy(&y, &data[searchOff + 4], 4);
            memcpy(&z, &data[searchOff + 8], 4);

            // Check if in valid world space
            if (x > -1000 && x < 1000 &&
                y > -50 && y < 300 &&
                z > -1000 && z < 0) {

                WorldInstance inst;
                inst.hash = hash;
                inst.name = dictionary.count(hash) ? dictionary.at(hash) : "";
                inst.x = x;
                inst.y = y;
                inst.z = z;
                instances.push_back(inst);
                break;
            }
        }
    }

    return instances;
}

// Structure to hold PCM data with its transform
struct PCMWithTransform {
    std::string packPath;
    uint32_t offset;
    uint32_t size;
    uint32_t hash;
    std::string name;
    float x, y, z;
    bool hasPosition;
};

void SpiderManTool::LoadAllWorldGeometries() {
    previewMeshes.clear();
    worldPcmQueue.clear();
    worldInstanceTransforms.clear();
    isWorldMode = true;

    selectedMeshIndex = -1;
    selectedMeshPcmData.clear();
    showWorldMeshHexEditor = false;

    camPos[0] = 0.0f; camPos[1] = 200.0f; camPos[2] = -600.0f;
    camFront[0] = 0.0f; camFront[1] = -0.3f; camFront[2] = -1.0f;
    camUp[0] = 0.0f; camUp[1] = 1.0f; camUp[2] = 0.0f;
    camYaw = -90.0f;
    camPitch = -15.0f;
    camSpeed = 500.0f;

    Log("Building world PCM queue with instance transforms...");

    std::map<uint32_t, PCMWithTransform> templatePcms;
    std::set<uint32_t> targetHashes = GetTargetHashes();
    int instanceCount = 0;
    int tileCount = 0;

    // First pass: Load CITY_ARENA templates for target objects
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

            // Check if this is a target hash and type 21 (PCM)
            if (type == 21 && size > 4 && targetHashes.count(hash)) {
                size_t filePos = file.tellg();
                uint32_t absOffset = (uint32_t)(dataOffset + offset);
                if (absOffset + 4 > fileSize) { file.seekg(filePos); continue; }

                file.seekg(absOffset);
                uint32_t sig = 0;
                file.read((char*)&sig, 4);

                if (file.good() && sig == 0x204D4350) {
                    PCMWithTransform pcm;
                    pcm.packPath = path.string();
                    pcm.offset = absOffset;
                    pcm.size = size;
                    pcm.hash = hash;
                    pcm.name = dictionary.count(hash) ? dictionary[hash] : "";
                    pcm.hasPosition = false;
                    pcm.x = pcm.y = pcm.z = 0;
                    templatePcms[hash] = pcm;
                }
                file.clear();
                file.seekg(filePos);
            }
        }
        file.close();
        Log("Found " + std::to_string(templatePcms.size()) + " prop templates in CITY_ARENA");
        break;
    }

    // Second pass: Process world packs and add geometry + instances
    for (const auto& path : foundPacks) {
        std::string stem = StrToLower(path.stem().string());

        // Include JH and all world packs
        bool isRelevant = IsWorldPack(stem) || IsWorldInteriorPack(stem);
        if (!isRelevant) continue;

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

        std::vector<uint8_t> type10Data;
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

            if (type == 10 && size > 64) {
                size_t filePos = file.tellg();
                file.seekg(absOffset);
                type10Data.resize(size);
                file.read((char*)type10Data.data(), size);
                file.clear();
                file.seekg(filePos);
            }
            else if (type == 21 && size > 4) {
                pcmEntries.push_back({hash, absOffset, size});
            }
        }

        // Add tile's own PCMs (tile geometry)
        for (const auto& [hash, absOffset, size] : pcmEntries) {
            file.clear();
            file.seekg(absOffset);
            uint32_t sig = 0;
            file.read((char*)&sig, 4);

            if (file.good() && sig == 0x204D4350) {
                std::string entryName = dictionary.count(hash) ? StrToLower(dictionary[hash]) : "";

                // Include all tile geometry that starts with tile prefix
                if (!entryName.empty() && entryName.find(stem) == 0) {
                    WorldPCMItem item;
                    item.packPath = path.string();
                    item.offset = absOffset;
                    item.size = size;
                    worldPcmQueue.push_back(item);
                    tileCount++;
                }
            }
        }

        // Extract instances from Type 10 and add them
        if (!type10Data.empty()) {
            std::vector<WorldInstance> instances = ExtractInstancesFromType10(type10Data, dictionary, targetHashes);

            for (const auto& inst : instances) {
                if (templatePcms.count(inst.hash) == 0) continue;

                const auto& tpl = templatePcms[inst.hash];

                WorldPCMItem item;
                item.packPath = tpl.packPath;
                item.offset = tpl.offset;
                item.size = tpl.size;

                // Store index BEFORE adding to queue
                int queueIndex = (int)worldPcmQueue.size();
                worldPcmQueue.push_back(item);

                // Create transform with this index
                std::string key = std::to_string(queueIndex);
                WorldInstanceTransform t;
                memset(t.matrix, 0, sizeof(t.matrix));
                t.matrix[0] = 1.0f;   // Identity rotation
                t.matrix[5] = 1.0f;
                t.matrix[10] = 1.0f;
                t.matrix[15] = 1.0f;
                t.matrix[12] = inst.x;  // Translation
                t.matrix[13] = inst.y;
                t.matrix[14] = inst.z;
                t.hasTransform = true;
                worldInstanceTransforms[key] = t;
                instanceCount++;
            }
        }

        file.close();
    }

    // Add sky_day from city_arena (no transform)
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
    Log("Instance transforms: " + std::to_string(instanceCount));
    Log("Total models queued: " + std::to_string(worldPcmQueue.size()));

    if (worldPcmQueue.empty()) {
        Log("No world models found!");
        isLoadingWorld = false;
        currentState = STATE_BROWSER;
        return;
    }

    // DO NOT sort - we use queue index as key for transforms

    isLoadingWorld = true;
    worldLoadProgress = 0;
    worldLoadTotal = (int)worldPcmQueue.size();
    currentState = STATE_LOADING_WORLD;
}

void SpiderManTool::LoadWorldGeometryStep(int index) {
    if (index < 0 || index >= (int)worldPcmQueue.size()) return;

    const auto& item = worldPcmQueue[index];

    // Check if this PCM has a position transform (key is just the index)
    std::string key = std::to_string(index);
    float* transform = nullptr;
    if (worldInstanceTransforms.count(key)) {
        transform = worldInstanceTransforms[key].matrix;
    }

    std::ifstream file(item.packPath, std::ios::binary);
    if (file.is_open()) {
        file.seekg(item.offset);
        if (file.good()) {
            std::vector<uint8_t> fileData(item.size);
            file.read((char*)fileData.data(), item.size);
            file.close();

            if (!fileData.empty()) {
                AddMeshFromDataWithTransform(fileData, "", nullptr, item.packPath, item.offset, transform);
            }
        } else {
            file.close();
        }
    }
}