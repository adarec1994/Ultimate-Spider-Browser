#include "SpiderManTool.h"
#include "NalIntegration.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cmath>

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

    // Parse only the directory region (everything before res_dir_mash_size);
    // the typed texture_locations table replaces the old 0xE3E3E3E3 scan.
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

        // Confirm it's a DDS (MPAL/IFL textures share the table).
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
        // Strip Windows line endings
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::stringstream ss(line);
        std::string hashStr, name;
        ss >> hashStr;
        std::getline(ss, name);
        // Strip Windows line endings from name too
        if (!name.empty() && name.back() == '\r') name.pop_back();
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

        // Directory-region parse (typed entries), replacing the old filler scan.
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

    // The pack directory lists every skeleton explicitly (skeleton_locations),
    // including its tlresource name hash -- the same string_hash the anim
    // file's skeleton table uses, so anims can be bound per-skeleton exactly
    // like the engine does. No more signature sniffing.
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
        } catch (const std::exception& ex) {
            Log("Skeleton parse error at offset " + std::to_string(src.offset) + ": " + ex.what());
            std::remove(tempPath.c_str());
            continue;
        } catch (...) {
            Log("Skeleton parse error (unknown) at offset " + std::to_string(src.offset));
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

    Log("Found " + std::to_string(skeletonCandidates.size()) + " skeleton(s) in pack");
    for (size_t k = 0; k < skeletonCandidates.size(); ++k) {
        const auto& c = skeletonCandidates[k];
        Log("  [" + std::to_string(k) + "] " + (c.name.empty() ? "<unnamed>" : c.name) +
            " (" + std::to_string(c.data->bone_map.size()) + " bones, " +
            std::to_string(c.data->components.size()) + " components) [" +
            c.data->skeleton_kind + "]");
    }

    if (!skeletonCandidates.empty()) {
        // Default to the first candidate. The mesh-load path will refine this
        // via SelectSkeletonForMesh once it knows the model name.
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

    // A pack can contain animations for several rigs. When mesh selection
    // changes the active skeleton, do not leave an animation from the previous
    // rig selected: its component bone indices are only meaningful for the
    // skeleton it was decoded against.
    if (loadedAnimFile && !loadedAnimFile->animations.empty()) {
        bool selectedMatches = selectedAnimIndex >= 0 &&
            selectedAnimIndex < (int)loadedAnimFile->animations.size();
        if (selectedMatches) {
            const auto& selected = loadedAnimFile->animations[selectedAnimIndex];
            selectedMatches = !selected.skeleton ||
                nal_skeleton_pose_compatible(selected.skeleton.get(), loadedSkeleton.get());
        }
        if (!selectedMatches) {
            selectedAnimIndex = -1;
            for (int i = 0; i < (int)loadedAnimFile->animations.size(); ++i) {
                const auto& candidate = loadedAnimFile->animations[i];
                if (!candidate.skeleton ||
                    nal_skeleton_pose_compatible(candidate.skeleton.get(), loadedSkeleton.get())) {
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

    Log("Active skeleton: " + (cand.name.empty() ? "<unnamed>" : cand.name) +
        " (" + std::to_string(cand.data->bone_map.size()) + " bones)");
}

void SpiderManTool::SelectSkeletonForMesh(const std::string& meshName, uint32_t meshHash) {
    SelectBoneMappingForMesh(meshHash);
    if (skeletonCandidates.empty()) return;
    if (meshName.empty()) {
        ActivateSkeletonCandidate(0);
        return;
    }

    // Score each candidate by how well its name matches the mesh.
    //   - exact case-insensitive match (best)
    //   - prefix match either direction
    //   - substring match either direction
    //   - longest common substring (fallback, weighted by length)
    std::string meshLower = StrToLower(meshName);
    // Strip common suffixes/prefixes that don't help matching
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
            // Substring -- prefer matches near the start.
            size_t a = meshLower.find(skelLower);
            size_t b = skelLower.find(meshLower);
            score = 500 - (int)std::min(a == std::string::npos ? 999 : a,
                                        b == std::string::npos ? 999 : b);
        } else {
            // Cheap longest common prefix in chars (no allocations).
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
        // No usable match; keep whatever's currently active (or default to 0).
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

        // Only the directory region is needed; resource data stays on disk.
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
    Log("Global skeleton index: " + std::to_string(globalSkeletonIndex.size()) + " skeletons across " +
        std::to_string(foundPacks.size()) + " packs");
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

void SpiderManTool::LoadAnimationForCurrentPack() {
    loadedAnimFile.reset();
    loadedAnimName.clear();
    selectedAnimIndex = -1;
    currentAnimFrame = 0;
    isAnimPlaying = false;
    if (pcPackData.empty()) return;

    // Every skeleton in the pack, so each anim can bind to its own skeleton
    // via its skel_index (matched by tlresource hash, then name) -- exactly
    // what the engine's nalLoadAnimFileInternal does. Decoding with the wrong
    // skeleton's component-slot table reads garbage flags/masks, which is why
    // animations used to break on packs with several skeletons.
    std::vector<NalSkeletonRef> skelRefs;
    for (const auto& cand : skeletonCandidates) {
        skelRefs.push_back({cand.hash, cand.name, cand.data});
        // The directory resource hash and the skeleton header's logical-name
        // hash are normally equal, but older 0x10200 generic assets use the
        // latter in PCANIM skeleton tables. Preserve both aliases.
        if (cand.data && cand.data->name_hash != 0 && cand.data->name_hash != cand.hash)
            skelRefs.push_back({cand.data->name_hash, cand.name, cand.data});
    }

    // Anim containers straight from the directory; fall back to a signature
    // scan only if the directory was unparseable.
    struct AnimSource { uint32_t offset; uint32_t size; };
    std::vector<AnimSource> sources;
    auto addAnimSource = [&](uint32_t offset, uint32_t size) {
        for (const auto& existing : sources)
            if (existing.offset == offset) return;
        sources.push_back({offset, size});
    };
    if (currentDir.valid) {
        for (const auto& a : currentDir.animFiles) {
            if ((size_t)a.offset + a.size > pcPackData.size() || a.size < 64) continue;
            addAnimSource(a.offset, a.size);
        }
    }

    // Directory resource typing is advisory, while the container magic is
    // authoritative. Always supplement the typed list with a complete magic
    // scan; otherwise valid PCANIM resources disappear whenever an otherwise
    // valid pack directory omits or misclassifies their type.
    for (const auto& e : entries) {
        if (e.isPcm || e.isDds) continue;
        if (e.size < 64 || (size_t)e.offset + e.size > pcPackData.size()) continue;
        uint32_t sig = 0;
        memcpy(&sig, &pcPackData[e.offset], 4);
        if (sig == NAL_ANIM_CONTAINER) addAnimSource(e.offset, e.size);
    }

    // Pre-scan each container's skeleton table; pull skeletons that live in
    // other packs (engine: global nalSkeletonDirectory) into the ref list.
    for (const auto& src : sources) {
        int32_t numSkels = 0;
        if ((size_t)src.offset + 64 > pcPackData.size()) continue;
        memcpy(&numSkels, &pcPackData[src.offset + 12], 4);
        for (int i = 0; i < numSkels; i++) {
            size_t entryOff = (size_t)src.offset + 64 + (size_t)i * 32;
            if (entryOff + 32 > pcPackData.size()) break;
            uint32_t skelHash;
            memcpy(&skelHash, &pcPackData[entryOff + 8], 4);

            bool haveIt = false;
            for (const auto& ref : skelRefs) {
                if (ref.hash == skelHash) { haveIt = true; break; }
            }
            if (haveIt) continue;

            BuildGlobalSkeletonIndex();
            auto it = globalSkeletonIndex.find(skelHash);
            if (it == globalSkeletonIndex.end()) continue;
            auto skel = LoadSkeletonFromLocation(it->second);
            if (!skel) continue;
            skelRefs.push_back({skelHash, skel->name, skel});

            // External skeletons participate in rendering too, not only in
            // animation decoding.  Packs such as CH_VWR_VENOM_VIEWER contain
            // an animation and PCM but reference the Venom skeleton stored in
            // CITY_ARENA.PCPACK.  Keeping that skeleton only in skelRefs left
            // loadedSkeleton null, so the correctly decoded animation could
            // never reach either the live skinning or GLB export path.
            SkeletonCandidate cand;
            cand.data = skel;
            cand.name = skel->name;
            cand.hash = skelHash;
            cand.entryIndex = -1;
            skeletonCandidates.push_back(cand);
            if (!loadedSkeleton) {
                ActivateSkeletonCandidate((int)skeletonCandidates.size() - 1);
            }
            Log("Loaded external skeleton '" + skel->name + "' from " +
                fs::path(it->second.packPath).filename().string());
        }
    }

    // Parse every container and merge into one list for the UI.
    std::shared_ptr<NalAnimFile> merged;
    for (const auto& src : sources) {
        std::string tempPath = "temp_anim.pcanim";
        {
            std::ofstream tmp(tempPath, std::ios::binary);
            if (!tmp.is_open()) continue;
            tmp.write((const char*)&pcPackData[src.offset], src.size);
        }

        NalSkeletonData* fallbackSkel = loadedSkeleton ? loadedSkeleton.get() : nullptr;
        NalAnimFile animFile = ParseNalAnimation(tempPath, skelRefs, fallbackSkel, true);
        std::remove(tempPath.c_str());

        // Some retail containers retain named directory/list nodes whose
        // animation payload is the engine's 0xFFFFFFFF sentinel.  Preserve
        // those bytes in ParseNalAnimation/source_bytes, but do not expose the
        // node as a playable clip: treating -1 as a frame count produces a
        // one-frame fallback with a nonsensical duration.  Boomerang's
        // gss_webblindloop is one such node.
        const size_t parsedAnimationCount = animFile.animations.size();
        animFile.animations.erase(
            std::remove_if(animFile.animations.begin(), animFile.animations.end(),
                [](const NalAnimEntry& anim) { return anim.frame_count < 0; }),
            animFile.animations.end());
        const size_t sentinelCount = parsedAnimationCount - animFile.animations.size();
        if (sentinelCount > 0) {
            Log("Ignored " + std::to_string(sentinelCount) +
                " empty animation sentinel(s) in " + animFile.name);
        }

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

    if (merged) {
        loadedAnimFile = merged;
        loadedAnimName = merged->name;
        selectedAnimIndex = -1;
        for (int i = 0; i < (int)merged->animations.size(); ++i) {
            const auto& candidate = merged->animations[i];
            if (!candidate.skeleton || !loadedSkeleton ||
                nal_skeleton_pose_compatible(candidate.skeleton.get(), loadedSkeleton.get())) {
                selectedAnimIndex = i;
                break;
            }
        }
        currentAnimFrame = 0;
        animPlaybackTime = 0.0f;
        animFrameFraction = 0.0f;
        isAnimPlaying = selectedAnimIndex >= 0;
        Log("Loaded " + std::to_string(merged->animations.size()) + " animations from " +
            std::to_string(sources.size()) + " container(s): " + merged->name);

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
            Log(info);
        }
    }
}

void SpiderManTool::UpdateAnimationPlayback(float deltaTime) {
    if (!isAnimPlaying || !loadedAnimFile || selectedAnimIndex < 0) return;
    if (selectedAnimIndex >= (int)loadedAnimFile->animations.size()) return;

    const auto& anim = loadedAnimFile->animations[selectedAnimIndex];
    int frameCount = anim.playback_frame_count();
    if (frameCount <= 0) return;

    animPlaybackTime = std::max(0.0f, animPlaybackTime + deltaTime);

    if (anim.is_looping()) {
        // N frames contain N intervals for a looping clip: the last interval
        // interpolates frame N-1 back to frame 0.
        float duration = (float)frameCount / NAL_PREVIEW_FPS;
        if (duration <= 0.0f) return;
        animPlaybackTime = fmodf(animPlaybackTime, duration);
        float frameF = animPlaybackTime * NAL_PREVIEW_FPS;
        currentAnimFrame = std::max(0, std::min((int)floorf(frameF), frameCount - 1));
        animFrameFraction = frameF - (float)currentAnimFrame;
    } else {
        // Non-looping clips stop on their final sample instead of wrapping.
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
    Log("Global entity index: " + std::to_string(globalEntityIndex.size()) +
        " names across " + std::to_string(foundPacks.size()) + " packs");
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
        Log("ENTITY bone map: " + std::to_string(activeBoneMapping.meshPoseCount) +
            " mesh bones from " + sourceName);
        return true;
    };

    // The engine's resource directory resolves the local pack first.
    if (currentDir.valid) {
        for (const auto& r : currentDir.resources) {
            if (r.type != RES_KEY_ENTITY || r.hash != meshHash ||
                (uint64_t)r.offset + r.size > pcPackData.size()) continue;
            std::vector<uint8_t> entityData(pcPackData.begin() + r.offset,
                                            pcPackData.begin() + r.offset + r.size);
            if (accept(entityData, fs::path(loadedPCPackPath).filename().string())) return true;
        }
    }

    // Viewer/cutscene packs often contain only the PCM.  Resolve the character
    // ENTITY globally by the same resource hash, matching the game directory.
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
