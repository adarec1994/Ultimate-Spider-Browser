#include "SpiderManTool.h"
#include "NalIntegration.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cmath>

void SpiderManTool::ShowNotification(const std::string& msg) {
    notificationMsg = msg;
    notificationTimer = NOTIFICATION_DURATION;
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

                fs::path targetDict = "string_hash_dictionary.txt";
                fs::path p1 = fs::path(searchPath) / targetDict;
                if (fs::exists(p1)) dictPath = p1.string();
                if (dictPath.empty() && fs::exists(targetDict)) dictPath = targetDict.string();
                if (!dictPath.empty()) LoadDictionary(dictPath);

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

    uint32_t mashSize = 0;
    file.seekg(0x1C);
    file.read((char*)&mashSize, 4);
    if (!file.good() || mashSize < 0x40 || mashSize > fileSize) {
        file.close();
        return;
    }

    std::vector<uint8_t> dirBlob(mashSize);
    file.seekg(0);
    file.read((char*)dirBlob.data(), mashSize);
    if (!file.good()) {
        file.close();
        return;
    }

    PackDirectory dir = PackDirectory::Parse(dirBlob);
    if (!dir.valid) {
        file.close();
        return;
    }

    for (const auto& t : dir.textures) {
        if (t.size <= 4 || (size_t)t.offset + 4 > fileSize) continue;

        file.seekg(t.offset);
        uint32_t sig = 0;
        file.read((char*)&sig, 4);
        if (!file.good() || sig != 0x20534444) { file.clear(); continue; }

        TextureLocation loc;
        loc.packPath = packPath.string();
        loc.offset = t.offset;
        loc.size = t.size;

        globalTextureIndex[t.nameHash] = loc;

        if (dictionary.count(t.nameHash)) {
            std::string name = StrToLower(dictionary[t.nameHash]);
            globalTextureNameIndex[name] = loc;
            if (name.size() > 4 && name.substr(name.size() - 4) == ".dds") {
                globalTextureNameIndex[name.substr(0, name.size() - 4)] = loc;
            }
        }
    }

    file.close();
}

void SpiderManTool::ScanDirectory() {
    foundPacks.clear();

    try {
        if (!fs::exists(searchPath)) {

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

        if (!foundPacks.empty()) {
            BuildGlobalTextureIndex();
        } else {
            currentState = STATE_SPLASH;
        }
    } catch (const std::exception&) {

        currentState = STATE_SPLASH;
    }
}

void SpiderManTool::LoadDictionary(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) return;
    std::string line;
    std::getline(file, line); std::getline(file, line);
    while (std::getline(file, line)) {

        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::stringstream ss(line);
        std::string hashStr, name;
        ss >> hashStr;
        std::getline(ss, name);

        if (!name.empty() && name.back() == '\r') name.pop_back();
        size_t first = name.find_first_not_of(" \t");
        if (first != std::string::npos) name = name.substr(first);
        try { dictionary[std::stoul(hashStr, nullptr, 16)] = name; } catch (...) {}
    }

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

    if (data[0] != 's' || data[1] != 'h' || data[2] != 'd' || data[3] != 0) {

        return;
    }

    const uint32_t MARKER = 0x15BADBAD;
    int count = 0;

    for (size_t pos = 0x10; pos + 4 < fileSize; pos++) {
        uint32_t val;
        memcpy(&val, &data[pos], 4);
        if (val != MARKER) continue;

        if (pos < 12) continue;

        uint32_t hash, recSize, strLen;
        memcpy(&hash, &data[pos - 12], 4);
        memcpy(&recSize, &data[pos - 8], 4);
        memcpy(&strLen, &data[pos - 4], 4);

        if (strLen == 0 || strLen > 255) continue;

        size_t strStart = pos + 8;
        if (strStart + strLen > fileSize) continue;

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

        uint32_t mashSize = 0;
        file.seekg(0x1C);
        file.read((char*)&mashSize, 4);
        if (!file.good() || mashSize < 0x40 || mashSize > fileSize) {
            file.close();
            continue;
        }

        std::vector<uint8_t> dirBlob(mashSize);
        file.seekg(0);
        file.read((char*)dirBlob.data(), mashSize);
        if (!file.good()) {
            file.close();
            continue;
        }

        PackDirectory dir = PackDirectory::Parse(dirBlob);
        if (!dir.valid) {
            file.close();
            continue;
        }

        for (const auto& r : dir.resources) {
            std::string fileName;
            if (dictionary.count(r.hash)) {
                fileName = dictionary[r.hash];
            } else {
                std::stringstream ss;
                ss << "Unknown_" << std::hex << r.hash;
                fileName = ss.str();
            }

            bool isPcm = false;
            bool isDds = false;

            if (r.size > 4 && (size_t)r.offset + 4 <= fileSize) {
                file.seekg(r.offset);
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
            }

            std::string fileNameLower = StrToLower(fileName);
            if (fileNameLower.find(queryLower) != std::string::npos) {
                GlobalSearchResult result;
                result.packIndex = packIdx;
                result.packName = packPath.filename().string();
                result.fileName = fileName;
                result.hash = r.hash;
                result.offset = r.offset;
                result.size = r.size;
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

int SpiderManTool::FindEntryBySignature(uint32_t sig) const {
    for (int i = 0; i < (int)entries.size(); i++) {
        const auto& e = entries[i];
        if (e.offset + 4 > pcPackData.size()) continue;
        uint32_t fileSig;
        memcpy(&fileSig, &pcPackData[e.offset], 4);
        if (fileSig == sig) return i;
    }
    return -1;
}

void SpiderManTool::LoadSkeletonForCurrentPack() {
    loadedSkeleton.reset();
    loadedSkeletonName.clear();
    skeletonCandidates.clear();
    activeSkeletonCandidate = -1;
    if (pcPackData.empty()) return;

    struct SkelSource { uint32_t hash; uint32_t offset; uint32_t size; int entryIndex; };
    std::vector<SkelSource> sources;

    if (currentDir.valid) {
        for (const auto& s : currentDir.skeletons) {
            if ((size_t)s.offset + s.size > pcPackData.size() || s.size < 80) continue;
            int entryIndex = -1;
            for (int i = 0; i < (int)entries.size(); i++) {
                if (entries[i].offset == s.offset) { entryIndex = i; break; }
            }
            sources.push_back({s.nameHash, s.offset, s.size, entryIndex});
        }
    }

    for (const auto& src : sources) {
        std::string tempPath = "temp_skel.pcskel";
        {
            std::ofstream tmp(tempPath, std::ios::binary);
            if (!tmp.is_open()) continue;
            tmp.write((const char*)&pcPackData[src.offset], src.size);
        }

        auto skel = std::make_shared<NalSkeletonData>();
        try {
            *skel = ParseNalSkeleton(tempPath);
        } catch (const std::exception&) {

            std::remove(tempPath.c_str());
            continue;
        } catch (...) {

            std::remove(tempPath.c_str());
            continue;
        }
        std::remove(tempPath.c_str());

        SkeletonCandidate cand;
        cand.data = skel;
        cand.name = !skel->name.empty() ? skel->name
                  : (dictionary.count(src.hash) ? dictionary[src.hash] : "");
        cand.hash = src.hash;
        cand.entryIndex = src.entryIndex;
        skeletonCandidates.push_back(cand);
    }

    for (size_t k = 0; k < skeletonCandidates.size(); ++k) {
        const auto& c = skeletonCandidates[k];

    }

    if (!skeletonCandidates.empty()) {

        ActivateSkeletonCandidate(0);
    }
}

void SpiderManTool::ActivateSkeletonCandidate(int candidateIndex) {
    if (candidateIndex < 0 || candidateIndex >= (int)skeletonCandidates.size()) {
        loadedSkeleton.reset();
        loadedSkeletonName.clear();
        activeSkeletonCandidate = -1;
        return;
    }
    if (activeSkeletonCandidate == candidateIndex && loadedSkeleton) return;

    activeSkeletonCandidate = candidateIndex;
    const auto& cand = skeletonCandidates[candidateIndex];
    loadedSkeleton = cand.data;
    loadedSkeletonName = cand.name;
    selectedVisemeIndex = -1;
    std::fill(morphTargetWeights.begin(), morphTargetWeights.end(), 0.0f);

    if (loadedAnimFile && !loadedAnimFile->animations.empty()) {
        bool selectedMatches = selectedAnimIndex >= 0 &&
            selectedAnimIndex < (int)loadedAnimFile->animations.size();
        if (selectedMatches) {
            const auto& selected = loadedAnimFile->animations[selectedAnimIndex];
            selectedMatches = !selected.skeleton ||
                nal_skeleton_pose_inheritable(selected.skeleton.get(), loadedSkeleton.get());
        }
        if (!selectedMatches) {
            selectedAnimIndex = -1;
            for (int i = 0; i < (int)loadedAnimFile->animations.size(); ++i) {
                const auto& candidate = loadedAnimFile->animations[i];
                if (!candidate.skeleton ||
                    nal_skeleton_pose_inheritable(candidate.skeleton.get(), loadedSkeleton.get())) {
                    selectedAnimIndex = i;
                    break;
                }
            }
            currentAnimFrame = 0;
            animFrameFraction = 0.0f;
            animPlaybackTime = 0.0f;
            isAnimPlaying = selectedAnimIndex >= 0;
        }
    }

}

void SpiderManTool::SelectSkeletonForMesh(const std::string& meshName, uint32_t meshHash) {
    SelectBoneMappingForMesh(meshHash);
    if (skeletonCandidates.empty()) return;
    if (meshName.empty()) {
        ActivateSkeletonCandidate(0);
        return;
    }

    std::string meshLower = StrToLower(meshName);

    auto stripExt = [](std::string s) {
        size_t dot = s.find_last_of('.');
        if (dot != std::string::npos) s.resize(dot);
        return s;
    };
    meshLower = stripExt(meshLower);

    int bestIdx = 0;
    int bestScore = -1;
    for (size_t k = 0; k < skeletonCandidates.size(); ++k) {
        const std::string skelLower = StrToLower(skeletonCandidates[k].name);
        if (skelLower.empty()) continue;
        int score = 0;
        if (skelLower == meshLower) {
            score = 1000;
        } else if (meshLower.find(skelLower) != std::string::npos ||
                   skelLower.find(meshLower) != std::string::npos) {

            size_t a = meshLower.find(skelLower);
            size_t b = skelLower.find(meshLower);
            score = 500 - (int)std::min(a == std::string::npos ? 999 : a,
                                        b == std::string::npos ? 999 : b);
        } else {

            int prefix = 0;
            int lim = (int)std::min(meshLower.size(), skelLower.size());
            while (prefix < lim && meshLower[prefix] == skelLower[prefix]) ++prefix;
            score = prefix;
        }
        if (score > bestScore) {
            bestScore = score;
            bestIdx = (int)k;
        }
    }

    if (bestScore <= 0) {

        if (activeSkeletonCandidate < 0) ActivateSkeletonCandidate(0);
        return;
    }
    ActivateSkeletonCandidate(bestIdx);
}

void SpiderManTool::BuildGlobalSkeletonIndex() {
    if (globalSkeletonIndexBuilt) return;
    globalSkeletonIndexBuilt = true;
    globalSkeletonIndex.clear();

    for (const auto& packPath : foundPacks) {
        std::ifstream f(packPath, std::ios::binary | std::ios::ate);
        if (!f.is_open()) continue;
        size_t fileSize = (size_t)f.tellg();
        if (fileSize < 0x40) continue;

        uint32_t mashSize = 0;
        f.seekg(0x1C);
        f.read((char*)&mashSize, 4);
        if (mashSize < 0x40 || mashSize > fileSize) continue;

        std::vector<uint8_t> dirBlob(mashSize);
        f.seekg(0);
        f.read((char*)dirBlob.data(), mashSize);
        if (!f.good()) continue;

        PackDirectory dir = PackDirectory::Parse(dirBlob);
        if (!dir.valid) continue;
        for (const auto& s : dir.skeletons) {
            if (!globalSkeletonIndex.count(s.nameHash)) {
                globalSkeletonIndex[s.nameHash] = {packPath.string(), s.offset, s.size};
            }
        }
    }

}

void SpiderManTool::BuildGlobalMorphIndex() {
    if (globalMorphIndexBuilt) return;
    globalMorphIndexBuilt = true;
    globalMorphIndex.clear();

    for (const auto& packPath : foundPacks) {
        std::ifstream f(packPath, std::ios::binary | std::ios::ate);
        if (!f.is_open()) continue;
        size_t fileSize = (size_t)f.tellg();
        if (fileSize < 0x40) continue;

        uint32_t mashSize = 0;
        f.seekg(0x1C);
        f.read((char*)&mashSize, 4);
        if (mashSize < 0x40 || mashSize > fileSize) continue;

        std::vector<uint8_t> dirBlob(mashSize);
        f.seekg(0);
        f.read((char*)dirBlob.data(), mashSize);
        if (!f.good()) continue;

        PackDirectory dir = PackDirectory::Parse(dirBlob);
        if (!dir.valid) continue;
        for (const auto& r : dir.resources) {
            if (r.type == RES_KEY_MORPH && r.size != 0 && !globalMorphIndex.count(r.hash)) {
                globalMorphIndex[r.hash] = {packPath.string(), r.offset, r.size};
            }
        }
    }
}

void SpiderManTool::BuildGlobalAnimIndex() {
    if (globalAnimIndexBuilt) return;
    globalAnimIndexBuilt = true;
    globalAnimIndex.clear();

    for (const auto& packPath : foundPacks) {
        std::ifstream f(packPath, std::ios::binary | std::ios::ate);
        if (!f.is_open()) continue;
        size_t fileSize = (size_t)f.tellg();
        if (fileSize < 0x40) continue;

        uint32_t mashSize = 0;
        f.seekg(0x1C);
        f.read((char*)&mashSize, 4);
        if (mashSize < 0x40 || mashSize > fileSize) continue;

        std::vector<uint8_t> dirBlob(mashSize);
        f.seekg(0);
        f.read((char*)dirBlob.data(), mashSize);
        if (!f.good()) continue;

        PackDirectory dir = PackDirectory::Parse(dirBlob);
        if (!dir.valid) continue;

        std::vector<AnimFileLocation> animLocs;
        for (const auto& a : dir.animFiles) {
            if (a.size >= 64 && (uint64_t)a.offset + a.size <= fileSize)
                animLocs.push_back({packPath.string(), a.offset, a.size});
        }
        if (animLocs.empty()) continue;

        // Index each anim file in this pack under every skeleton hash present in the same pack,
        // so a model can find its clips by its skeleton hash.
        std::vector<uint32_t> skelHashes;
        for (const auto& s : dir.skeletons) skelHashes.push_back(s.nameHash);
        for (const auto& r : dir.resources)
            if (r.type == RES_KEY_NAL_SKL) skelHashes.push_back(r.hash);

        for (uint32_t h : skelHashes)
            for (const auto& loc : animLocs)
                globalAnimIndex[h].push_back(loc);
    }
}

std::shared_ptr<NalSkeletonData> SpiderManTool::LoadSkeletonFromLocation(const SkeletonLocation& loc) {
    std::ifstream f(loc.packPath, std::ios::binary);
    if (!f.is_open()) return nullptr;
    std::vector<uint8_t> data(loc.size);
    f.seekg(loc.offset);
    f.read((char*)data.data(), loc.size);
    if (!f.good()) return nullptr;

    std::string tempPath = "temp_skel.pcskel";
    {
        std::ofstream tmp(tempPath, std::ios::binary);
        if (!tmp.is_open()) return nullptr;
        tmp.write((const char*)data.data(), data.size());
    }
    auto skel = std::make_shared<NalSkeletonData>();
    try {
        *skel = ParseNalSkeleton(tempPath);
    } catch (...) {
        std::remove(tempPath.c_str());
        return nullptr;
    }
    std::remove(tempPath.c_str());
    return skel;
}

void SpiderManTool::LoadVisemeStreamsForCurrentPack() {
    loadedVisemeStreams.clear();
    selectedVisemeIndex = -1;
    if (pcPackData.empty() || !currentDir.valid) return;

    for (const auto& resource : currentDir.resources) {
        if (resource.type != RES_KEY_VISEME_STREAM || resource.size == 0 ||
            resource.offset > pcPackData.size() ||
            resource.size > pcPackData.size() - resource.offset) continue;

        std::vector<uint8_t> bytes(pcPackData.begin() + resource.offset,
                                   pcPackData.begin() + resource.offset + resource.size);
        UsmViseme::Stream stream = UsmViseme::Parse(bytes);
        stream.resource_hash = resource.hash;
        const auto name = dictionary.find(resource.hash);
        if (name != dictionary.end()) stream.name = name->second;
        else {
            std::ostringstream fallback;
            fallback << "Unknown_" << std::hex << resource.hash;
            stream.name = fallback.str();
        }
        if (!stream.valid) {
            continue;
        }
        loadedVisemeStreams.push_back(std::move(stream));
    }
    std::sort(loadedVisemeStreams.begin(), loadedVisemeStreams.end(),
        [](const UsmViseme::Stream& a, const UsmViseme::Stream& b) {
            return a.name < b.name;
        });
    if (!loadedVisemeStreams.empty()) {

    }
}

void SpiderManTool::LoadAnimationForCurrentPack() {
    loadedAnimFile.reset();
    loadedAnimName.clear();
    selectedAnimIndex = -1;
    currentAnimFrame = 0;
    isAnimPlaying = false;
    LoadVisemeStreamsForCurrentPack();
    if (pcPackData.empty()) return;

    std::vector<NalSkeletonRef> skelRefs;
    for (const auto& cand : skeletonCandidates) {
        skelRefs.push_back({cand.hash, cand.name, cand.data});

        if (cand.data && cand.data->name_hash != 0 && cand.data->name_hash != cand.hash)
            skelRefs.push_back({cand.data->name_hash, cand.name, cand.data});
    }

    struct AnimSource { uint32_t offset; uint32_t size; std::vector<uint8_t> ownedBytes; };
    std::vector<AnimSource> sources;
    auto addAnimSource = [&](uint32_t offset, uint32_t size) {
        for (const auto& existing : sources)
            if (existing.ownedBytes.empty() && existing.offset == offset) return;
        sources.push_back({offset, size, {}});
    };
    // A source's bytes come from the current pack (pcPackData at offset) unless it was resolved
    // cross-pack, in which case it owns its bytes.
    auto sourceData = [&](const AnimSource& s) -> const uint8_t* {
        if (!s.ownedBytes.empty()) return s.ownedBytes.data();
        return pcPackData.empty() ? nullptr : &pcPackData[s.offset];
    };
    auto sourceSize = [&](const AnimSource& s) -> size_t {
        return s.ownedBytes.empty() ? s.size : s.ownedBytes.size();
    };
    if (currentDir.valid) {
        for (const auto& a : currentDir.animFiles) {
            if ((size_t)a.offset + a.size > pcPackData.size() || a.size < 64) continue;
            addAnimSource(a.offset, a.size);
        }
    }

    for (const auto& e : entries) {
        if (e.isPcm || e.isDds) continue;
        if (e.size < 64 || (size_t)e.offset + e.size > pcPackData.size()) continue;
        uint32_t sig = 0;
        memcpy(&sig, &pcPackData[e.offset], 4);
        if (sig == NAL_ANIM_CONTAINER) addAnimSource(e.offset, e.size);
    }

    // Scan an anim source's referenced skeletons, resolve any missing ones cross-pack (adding
    // them as candidates), then parse and merge its clips into `merged`. Shared by the current
    // pack pass and the cross-pack fallback below.
    std::shared_ptr<NalAnimFile> merged;
    auto processSources = [&](const std::vector<AnimSource>& srcs) {
        for (const auto& src : srcs) {
            const uint8_t* d = sourceData(src);
            const size_t n = sourceSize(src);
            if (!d || n < 64) continue;
            int32_t numSkels = 0;
            memcpy(&numSkels, d + 12, 4);
            for (int i = 0; i < numSkels; i++) {
                size_t entryOff = 64 + (size_t)i * 32;
                if (entryOff + 32 > n) break;
                uint32_t skelHash;
                memcpy(&skelHash, d + entryOff + 8, 4);

                bool haveIt = false;
                for (const auto& ref : skelRefs)
                    if (ref.hash == skelHash) { haveIt = true; break; }
                if (haveIt) continue;

                BuildGlobalSkeletonIndex();
                auto it = globalSkeletonIndex.find(skelHash);
                if (it == globalSkeletonIndex.end()) continue;
                auto skel = LoadSkeletonFromLocation(it->second);
                if (!skel) continue;
                skelRefs.push_back({skelHash, skel->name, skel});

                SkeletonCandidate cand;
                cand.data = skel;
                cand.name = skel->name;
                cand.hash = skelHash;
                cand.entryIndex = -1;
                skeletonCandidates.push_back(cand);
                if (!loadedSkeleton)
                    ActivateSkeletonCandidate((int)skeletonCandidates.size() - 1);
            }
        }

        for (const auto& src : srcs) {
            const uint8_t* d = sourceData(src);
            const size_t n = sourceSize(src);
            if (!d || n == 0) continue;
            std::string tempPath = "temp_anim.pcanim";
            {
                std::ofstream tmp(tempPath, std::ios::binary);
                if (!tmp.is_open()) continue;
                tmp.write((const char*)d, n);
            }

            NalSkeletonData* fallbackSkel = loadedSkeleton ? loadedSkeleton.get() : nullptr;
            NalAnimFile animFile = ParseNalAnimation(tempPath, skelRefs, fallbackSkel, true);
            std::remove(tempPath.c_str());

            animFile.animations.erase(
                std::remove_if(animFile.animations.begin(), animFile.animations.end(),
                    [](const NalAnimEntry& anim) { return anim.frame_count <= 0; }),
                animFile.animations.end());

            if (animFile.animations.empty()) continue;
            if (!merged) {
                merged = std::make_shared<NalAnimFile>(std::move(animFile));
            } else {
                merged->animations.insert(merged->animations.end(),
                                          animFile.animations.begin(), animFile.animations.end());
                merged->skeletons.insert(merged->skeletons.end(),
                                         animFile.skeletons.begin(), animFile.skeletons.end());
                merged->num_anims += animFile.num_anims;
            }
        }
    };

    auto hasCompatibleAnim = [&]() -> bool {
        if (!merged) return false;
        for (const auto& a : merged->animations)
            if (!a.skeleton || !loadedSkeleton ||
                nal_skeleton_pose_inheritable(a.skeleton.get(), loadedSkeleton.get()))
                return true;
        return false;
    };

    processSources(sources);

    // Cross-pack fallback: if the current pack produced no animation compatible with the loaded
    // skeleton (e.g. a character opened from a cutscene pack that only has a scene anim, or no
    // anim file at all), pull its clips from another pack that contains its skeleton — its
    // CH_VWR viewer/costume pack. Mirrors the cross-pack morph resolution.
    if (!hasCompatibleAnim() && !skelRefs.empty()) {
        BuildGlobalAnimIndex();
        std::vector<AnimSource> crossSources;
        std::vector<std::string> seen;
        for (const auto& ref : skelRefs) {
            auto it = globalAnimIndex.find(ref.hash);
            if (it == globalAnimIndex.end()) continue;
            for (const auto& loc : it->second) {
                if (loc.size < 64) continue;
                std::string key = loc.packPath + "#" + std::to_string(loc.offset);
                if (std::find(seen.begin(), seen.end(), key) != seen.end()) continue;
                seen.push_back(key);
                std::ifstream af(loc.packPath, std::ios::binary);
                if (!af.is_open()) continue;
                std::vector<uint8_t> bytes(loc.size);
                af.seekg(loc.offset);
                af.read(reinterpret_cast<char*>(bytes.data()), loc.size);
                if (af.gcount() != (std::streamsize)loc.size) continue;
                crossSources.push_back(AnimSource{0, loc.size, std::move(bytes)});
            }
        }
        processSources(crossSources);
    }

    if (merged) {
        loadedAnimFile = merged;
        loadedAnimName = merged->name;
        selectedAnimIndex = -1;
        for (int i = 0; i < (int)merged->animations.size(); ++i) {
            const auto& candidate = merged->animations[i];
            if (!candidate.skeleton || !loadedSkeleton ||
                nal_skeleton_pose_inheritable(candidate.skeleton.get(), loadedSkeleton.get())) {
                selectedAnimIndex = i;
                break;
            }
        }
        currentAnimFrame = 0;
        animPlaybackTime = 0.0f;
        animFrameFraction = 0.0f;
        isAnimPlaying = selectedAnimIndex >= 0;

        for (size_t a = 0; a < merged->animations.size(); a++) {
            const auto& anim = merged->animations[a];
            std::string info = "  [" + std::to_string(a) + "] " + anim.name;
            info += " frames=" + std::to_string(anim.frame_count);
            info += " dur=" + std::to_string(anim.playback_duration());
            if (!anim.skeleton_name.empty()) info += " skel=" + anim.skeleton_name;
            if (!anim.skeleton) info += " [no skel match]";
            if (anim.is_looping()) info += " [loop]";
            if (anim.is_scene_anim()) info += " [scene]";
            info += " comps=" + std::to_string(anim.components.size());

        }
    }
}

void SpiderManTool::UpdateAnimationPlayback(float deltaTime) {
    if (!isAnimPlaying) return;

    if (selectedVisemeIndex >= 0) {
        if (selectedVisemeIndex >= static_cast<int>(loadedVisemeStreams.size()) ||
            !loadedMorphFile.valid || morphTargetWeights.empty()) {
            isAnimPlaying = false;
            return;
        }
        const auto& stream = loadedVisemeStreams[selectedVisemeIndex];
        animPlaybackTime = std::max(0.0f, animPlaybackTime + deltaTime);
        const uint64_t frame = static_cast<uint64_t>(
            static_cast<double>(stream.sample_rate) * animPlaybackTime);

        if (frame >= stream.frame_count) {
            std::fill(morphTargetWeights.begin(), morphTargetWeights.end(), 0.0f);
            currentAnimFrame = stream.frame_count ? static_cast<int>(stream.frame_count - 1) : 0;
            animFrameFraction = 0.0f;
            isAnimPlaying = false;
            return;
        }

        std::fill(morphTargetWeights.begin(), morphTargetWeights.end(), 0.0f);
        const float* weights = stream.frame(static_cast<uint32_t>(frame));
        if (!weights) {
            isAnimPlaying = false;
            return;
        }
        const size_t channels = std::min<size_t>(
            stream.channel_count,
            loadedMorphFile.sets.size() > 1 ? loadedMorphFile.sets.size() - 1 : 0);
        for (size_t channel = 0; channel < channels; ++channel)
            morphTargetWeights[channel + 1] = weights[channel];
        currentAnimFrame = static_cast<int>(frame);
        animFrameFraction = 0.0f;
        return;
    }

    if (!loadedAnimFile || selectedAnimIndex < 0) return;
    if (selectedAnimIndex >= (int)loadedAnimFile->animations.size()) return;

    const auto& anim = loadedAnimFile->animations[selectedAnimIndex];
    int frameCount = anim.playback_frame_count();
    if (frameCount <= 0) return;

    animPlaybackTime = std::max(0.0f, animPlaybackTime + deltaTime);

    if (anim.is_looping()) {

        float duration = (float)frameCount / NAL_PREVIEW_FPS;
        if (duration <= 0.0f) return;
        animPlaybackTime = fmodf(animPlaybackTime, duration);
        float frameF = animPlaybackTime * NAL_PREVIEW_FPS;
        currentAnimFrame = std::max(0, std::min((int)floorf(frameF), frameCount - 1));
        animFrameFraction = frameF - (float)currentAnimFrame;
    } else {

        float endTime = (float)std::max(0, frameCount - 1) / NAL_PREVIEW_FPS;
        if (animPlaybackTime >= endTime) {
            animPlaybackTime = endTime;
            currentAnimFrame = frameCount - 1;
            animFrameFraction = 0.0f;
            isAnimPlaying = false;
            return;
        }
        float frameF = animPlaybackTime * NAL_PREVIEW_FPS;
        currentAnimFrame = std::max(0, std::min((int)floorf(frameF), frameCount - 1));
        animFrameFraction = frameF - (float)currentAnimFrame;
    }
}

void SpiderManTool::BuildGlobalEntityIndex() {
    if (globalEntityIndexBuilt) return;
    globalEntityIndexBuilt = true;
    globalEntityIndex.clear();

    for (const auto& packPath : foundPacks) {
        std::ifstream f(packPath, std::ios::binary | std::ios::ate);
        if (!f.is_open()) continue;
        size_t fileSize = (size_t)f.tellg();
        if (fileSize < 0x40) continue;

        uint32_t mashSize = 0;
        f.seekg(0x1C);
        f.read((char*)&mashSize, 4);
        if (mashSize < 0x40 || mashSize > fileSize) continue;

        std::vector<uint8_t> dirBlob(mashSize);
        f.seekg(0);
        f.read((char*)dirBlob.data(), mashSize);
        if (!f.good()) continue;

        PackDirectory dir = PackDirectory::Parse(dirBlob);
        if (!dir.valid) continue;
        for (const auto& r : dir.resources) {
            if (r.type != RES_KEY_ENTITY || r.size == 0 ||
                (uint64_t)r.offset + r.size > fileSize) continue;
            globalEntityIndex[r.hash].push_back({packPath.string(), r.offset, r.size});
        }
    }

}

bool SpiderManTool::SelectBoneMappingForMesh(uint32_t meshHash) {
    activeBoneMapping = {};
    activeBoneMappingHash = 0;
    if (meshHash == 0) return false;

    auto accept = [&](const std::vector<uint8_t>& entityData,
                      const std::string& sourceName) -> bool {
        EntityBoneMapping parsed = ParseEntityBoneMapping(entityData);
        if (!parsed.valid) return false;
        activeBoneMapping = std::move(parsed);
        activeBoneMappingHash = meshHash;

        return true;
    };

    if (currentDir.valid) {
        for (const auto& r : currentDir.resources) {
            if (r.type != RES_KEY_ENTITY || r.hash != meshHash ||
                (uint64_t)r.offset + r.size > pcPackData.size()) continue;
            std::vector<uint8_t> entityData(pcPackData.begin() + r.offset,
                                            pcPackData.begin() + r.offset + r.size);
            if (accept(entityData, fs::path(loadedPCPackPath).filename().string())) return true;
        }
    }

    BuildGlobalEntityIndex();
    auto found = globalEntityIndex.find(meshHash);
    if (found == globalEntityIndex.end()) return false;
    for (const auto& loc : found->second) {
        std::ifstream f(loc.packPath, std::ios::binary);
        if (!f.is_open()) continue;
        std::vector<uint8_t> entityData(loc.size);
        f.seekg(loc.offset);
        f.read((char*)entityData.data(), loc.size);
        if (f.gcount() != (std::streamsize)loc.size) continue;
        if (accept(entityData, fs::path(loc.packPath).filename().string())) return true;
    }
    return false;
}
