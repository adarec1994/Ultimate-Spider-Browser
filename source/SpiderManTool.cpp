#include "SpiderManTool.h"
#include <iostream>
#include <fstream>
#include <sstream>

void SpiderManTool::Log(const std::string& msg) {
    logBuffer += msg + "\n";
    std::cout << msg << std::endl;
}

void SpiderManTool::ShowNotification(const std::string& msg) {
    notificationMsg = msg;
    notificationTimer = NOTIFICATION_DURATION;
    Log(msg);
}

void SpiderManTool::SaveConfig() {
    std::ofstream f("usm_config.txt");
    if (f.is_open()) {
        f << searchPath;
        f.close();
    }
}

void SpiderManTool::LoadConfig() {
    std::ifstream f("usm_config.txt");
    if (f.is_open()) {
        std::string line;
        if (std::getline(f, line) && !line.empty()) {
            if (fs::exists(line)) {
                searchPath = line;
            }
        }
        f.close();
    }
}

void SpiderManTool::BuildGlobalTextureIndex() {
    globalTextureIndex.clear();
    globalTextureNameIndex.clear();

    if (foundPacks.empty()) {
        currentState = STATE_BROWSER;
        return;
    }

    Log("Building global texture index...");

    isIndexing = true;
    indexingProgress = 0;
    indexingTotal = (int)foundPacks.size();
    indexingCurrentPack = "";
    currentState = STATE_LOADING;
}

void SpiderManTool::BuildGlobalTextureIndexStep(int packIndex) {
    if (packIndex < 0 || packIndex >= (int)foundPacks.size()) return;

    const auto& packPath = foundPacks[packIndex];
    indexingCurrentPack = packPath.filename().string();

    std::ifstream file(packPath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) return;

    size_t fileSize = file.tellg();
    if (fileSize < 32) {
        file.close();
        return;
    }

    file.seekg(24);
    uint32_t headerSize, dataOffset;
    file.read((char*)&headerSize, 4);
    file.read((char*)&dataOffset, 4);

    if (!file.good()) {
        file.close();
        return;
    }

    // Find entry table start
    size_t start = 0;
    const uint32_t magic = 0xE3E3E3E3;
    size_t headerReadSize = std::min((size_t)200000, fileSize);
    std::vector<uint8_t> tempHeader(headerReadSize);
    file.seekg(0);
    file.read((char*)tempHeader.data(), headerReadSize);

    if (!file.good() && !file.eof()) {
        file.close();
        return;
    }

    for (size_t i = 0; i + 4 <= tempHeader.size(); i++) {
        if (*(uint32_t*)&tempHeader[i] == magic) {
            for (size_t j = i + 4; j < i + 1000 && j + 4 <= tempHeader.size(); j++) {
                if (*(uint32_t*)&tempHeader[j] == magic) {
                    start = j + 4;
                    break;
                }
            }
            break;
        }
    }

    if (start == 0) {
        file.close();
        return;
    }

    // Parse entries
    file.clear(); // Clear any EOF flags
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
            uint32_t absOffset = dataOffset + offset;

            // Safety check - make sure we're not seeking past end of file
            if (absOffset + 4 > fileSize) {
                file.seekg(filePos);
                continue;
            }

            // Check if it's a DDS file
            file.seekg(absOffset);
            uint32_t sig = 0;
            file.read((char*)&sig, 4);

            if (file.good() && sig == 0x20534444) {  // "DDS "
                TextureLocation loc;
                loc.packPath = packPath.string();
                loc.offset = absOffset;
                loc.size = size;

                // Store by hash
                globalTextureIndex[hash] = loc;

                // Store by name if we have it in dictionary
                if (dictionary.count(hash)) {
                    std::string name = StrToLower(dictionary[hash]);
                    globalTextureNameIndex[name] = loc;

                    // Also store without .dds extension
                    if (name.size() > 4 && name.substr(name.size() - 4) == ".dds") {
                        globalTextureNameIndex[name.substr(0, name.size() - 4)] = loc;
                    }
                }
            }

            file.clear(); // Clear any error flags
            file.seekg(filePos);
        }
    }

    file.close();
}

void SpiderManTool::ScanDirectory() {
    foundPacks.clear();
    Log("Scanning " + searchPath + "...");
    try {
        if (!fs::exists(searchPath)) {
            Log("Path does not exist!");
            return;
        }

        SaveConfig();

        for (auto& p : fs::recursive_directory_iterator(searchPath)) {
            auto ext = p.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext == ".pcpack") {
                foundPacks.push_back(p.path());
            }
        }
        Log("Found " + std::to_string(foundPacks.size()) + " .pcpack files.");

        if (!foundPacks.empty()) {
            // Start the indexing process
            BuildGlobalTextureIndex();
        } else {
            currentState = STATE_BROWSER;
        }
    } catch (const std::exception& e) {
        Log(std::string("Error scanning: ") + e.what());
        currentState = STATE_BROWSER;
    }
}

void SpiderManTool::LoadDictionary(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) return;
    std::string line;
    std::getline(file, line); std::getline(file, line);
    while (std::getline(file, line)) {
        std::stringstream ss(line);
        std::string hashStr, name;
        ss >> hashStr;
        std::getline(ss, name);
        size_t first = name.find_first_not_of(" \t");
        if (first != std::string::npos) name = name.substr(first);
        try { dictionary[std::stoul(hashStr, nullptr, 16)] = name; } catch (...) {}
    }
    Log("Loaded dictionary.");
}

bool SpiderManTool::IsWorldPack(const std::string& name) {
    return name.length() == 2;
}

bool SpiderManTool::IsWorldInteriorPack(const std::string& name) {
    std::string lower = StrToLower(name);
    if (lower.length() < 6) return false;
    return lower.substr(2, 4) == "_int";
}