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
        f << searchPath << "\n";
        f << (foundPacks.empty() ? "0" : "1");
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
        if (std::getline(f, line) && line == "1") {
            if (fs::exists(searchPath)) {
                std::string dictPath;
                // Try text dictionary first
                fs::path targetDict = "string_hash_dictionary.txt";
                fs::path p1 = fs::path(searchPath) / targetDict;
                if (fs::exists(p1)) dictPath = p1.string();
                if (dictPath.empty() && fs::exists(targetDict)) dictPath = targetDict.string();
                if (!dictPath.empty()) LoadDictionary(dictPath);

                // Also try binary dictionary if text one wasn't found or to supplement
                if (dictionary.empty()) {
                    fs::path binDict = "string_hash_dictionary.bin";
                    fs::path bp1 = fs::path(searchPath) / binDict;
                    std::string binDictPath;
                    if (fs::exists(bp1)) binDictPath = bp1.string();
                    if (binDictPath.empty() && fs::exists(binDict)) binDictPath = binDict.string();
                    if (!binDictPath.empty()) LoadBinaryDictionary(binDictPath);
                }

                ScanDirectory();
                std::sort(foundPacks.begin(), foundPacks.end());
            }
        }
        f.close();
    }
}

void SpiderManTool::BuildGlobalTextureIndex() {
    globalTextureIndex.clear();
    globalTextureNameIndex.clear();

    if (foundPacks.empty()) {
        currentState = STATE_SPLASH;
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
            uint32_t absOffset = dataOffset + offset;


            if (absOffset + 4 > fileSize) {
                file.seekg(filePos);
                continue;
            }


            file.seekg(absOffset);
            uint32_t sig = 0;
            file.read((char*)&sig, 4);

            if (file.good() && sig == 0x20534444) {
                TextureLocation loc;
                loc.packPath = packPath.string();
                loc.offset = absOffset;
                loc.size = size;


                globalTextureIndex[hash] = loc;


                if (dictionary.count(hash)) {
                    std::string name = StrToLower(dictionary[hash]);
                    globalTextureNameIndex[name] = loc;


                    if (name.size() > 4 && name.substr(name.size() - 4) == ".dds") {
                        globalTextureNameIndex[name.substr(0, name.size() - 4)] = loc;
                    }
                }
            }

            file.clear();
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
            BuildGlobalTextureIndex();
        } else {
            currentState = STATE_SPLASH;
        }
    } catch (const std::exception& e) {
        Log(std::string("Error scanning: ") + e.what());
        currentState = STATE_SPLASH;
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

void SpiderManTool::LoadBinaryDictionary(const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) return;

    size_t fileSize = file.tellg();
    if (fileSize < 16) return;

    file.seekg(0);
    std::vector<uint8_t> data(fileSize);
    file.read((char*)data.data(), fileSize);
    file.close();

    // Verify "shd\0" magic
    if (data[0] != 's' || data[1] != 'h' || data[2] != 'd' || data[3] != 0) {
        Log("Binary dictionary: invalid magic");
        return;
    }

    // Scan for entries: each record has marker 0x15BADBAD
    // Record layout: [hash:4] [recsize:4] [strlen:4] [0x15BADBAD:4] [data:4] [string...]
    const uint32_t MARKER = 0x15BADBAD;
    int count = 0;

    for (size_t pos = 0x10; pos + 4 < fileSize; pos++) {
        uint32_t val;
        memcpy(&val, &data[pos], 4);
        if (val != MARKER) continue;

        // Marker found at pos; hash is at pos-12, recsize at pos-8, strlen at pos-4
        if (pos < 12) continue;

        uint32_t hash, recSize, strLen;
        memcpy(&hash, &data[pos - 12], 4);
        memcpy(&recSize, &data[pos - 8], 4);
        memcpy(&strLen, &data[pos - 4], 4);

        if (strLen == 0 || strLen > 255) continue;

        size_t strStart = pos + 8; // skip marker + 4 bytes data
        if (strStart + strLen > fileSize) continue;

        // Read null-terminated string
        std::string name;
        for (size_t i = strStart; i < strStart + strLen && i < fileSize; i++) {
            if (data[i] == 0) break;
            name += (char)data[i];
        }

        if (!name.empty()) {
            dictionary[hash] = name;
            count++;
        }
    }

    Log("Loaded binary dictionary: " + std::to_string(count) + " entries.");
}

bool SpiderManTool::IsWorldPack(const std::string& name) {
    return name.length() == 2;
}

bool SpiderManTool::IsWorldInteriorPack(const std::string& name) {
    std::string lower = StrToLower(name);
    if (lower.length() < 6) return false;
    return lower.substr(2, 4) == "_int";
}

void SpiderManTool::SearchAllPacks(const std::string& query) {
    globalSearchResults.clear();
    selectedGlobalSearchIndex = -1;
    lastGlobalSearchQuery = query;

    if (query.empty()) {
        isGlobalSearchMode = false;
        return;
    }

    isGlobalSearchMode = true;
    std::string queryLower = StrToLower(query);

    for (int packIdx = 0; packIdx < (int)foundPacks.size(); packIdx++) {
        const auto& packPath = foundPacks[packIdx];

        std::ifstream file(packPath, std::ios::binary | std::ios::ate);
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


            std::string fileName;
            if (dictionary.count(hash)) {
                fileName = dictionary[hash];
            } else {
                std::stringstream ss;
                ss << "Unknown_" << std::hex << hash;
                fileName = ss.str();
            }


            bool isPcm = false;
            bool isDds = false;

            if (size > 4) {
                size_t filePos = file.tellg();
                uint32_t absOffset = dataOffset + offset;

                if (absOffset + 4 <= fileSize) {
                    file.seekg(absOffset);
                    uint32_t sig = 0;
                    file.read((char*)&sig, 4);

                    if (file.good()) {
                        if (sig == 0x204D4350) {
                            isPcm = true;
                            fileName += ".pcm";
                        } else if (sig == 0x20534444) {
                            isDds = true;
                            fileName += ".dds";
                        } else {
                            fileName += ".dat";
                        }
                    }

                    file.clear();
                    file.seekg(filePos);
                }
            }


            std::string fileNameLower = StrToLower(fileName);
            if (fileNameLower.find(queryLower) != std::string::npos) {
                GlobalSearchResult result;
                result.packIndex = packIdx;
                result.packName = packPath.filename().string();
                result.fileName = fileName;
                result.hash = hash;
                result.offset = dataOffset + offset;
                result.size = size;
                result.isPcm = isPcm;
                result.isDds = isDds;
                globalSearchResults.push_back(result);
            }
        }

        file.close();
    }


    std::sort(globalSearchResults.begin(), globalSearchResults.end(),
        [](const GlobalSearchResult& a, const GlobalSearchResult& b) {
            return a.fileName < b.fileName;
        });
}

void SpiderManTool::SelectGlobalSearchResult(int index) {
    if (index < 0 || index >= (int)globalSearchResults.size()) return;

    const auto& result = globalSearchResults[index];
    selectedGlobalSearchIndex = index;


    std::string packPath = foundPacks[result.packIndex].string();
    if (loadedPCPackPath != packPath) {
        OpenPCPack(packPath);
        selectedPackIndex = result.packIndex;
    }


    for (int i = 0; i < (int)entries.size(); i++) {
        if (entries[i].hash == result.hash) {
            selectedFileIndex = i;
            break;
        }
    }
}