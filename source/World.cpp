#include "SpiderManTool.h"
#include <fstream>
#include <cstring>
#include <set>

// ── Helper: 4×4 row-major matrix multiply  C = A * B ──────────────────────
static void MultiplyMatrix4x4(const float* A, const float* B, float* C) {
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            C[i * 4 + j] = 0;
            for (int k = 0; k < 4; k++) {
                C[i * 4 + j] += A[i * 4 + k] * B[k * 4 + j];
            }
        }
    }
}

// ── Helper: strip trailing digits  "EE_STORESIGN03" → "EE_STORESIGN" ──────
static std::string StripNumericSuffix(const std::string& name) {
    size_t end = name.size();
    while (end > 0 && std::isdigit((unsigned char)name[end - 1])) end--;
    if (end == 0) return name;  // all digits – keep as-is
    // Also strip a trailing underscore that preceded the digits (e.g. "name_01" → "name")
    if (end < name.size() && end > 0 && name[end - 1] == '_') end--;
    if (end == 0) return name;
    return name.substr(0, end);
}

// ── Helper: strip 2-letter zone prefix  "EE_PARK" → "PARK" ────────────────
static std::string StripZonePrefix(const std::string& name) {
    if (name.size() > 3 && std::isalpha((unsigned char)name[0]) &&
        std::isalpha((unsigned char)name[1]) && name[2] == '_') {
        return name.substr(3);
    }
    return name;
}

// ── Helper: expand known instance-name abbreviations to PCM names ─────────
//    The game uses shortened names in instance data that don't match the
//    actual PCM model names.  This map covers all known cases.
static std::string ExpandAbbreviations(const std::string& name) {
    std::string result = name;

    // strtlampa → streetlampa,  strtlampb → streetlampb
    if (result.find("strtlamp") != std::string::npos) {
        size_t pos = result.find("strtlamp");
        result.replace(pos, 8, "streetlamp");
    }
    // bilboard → billboard  (typo in some instance names)
    if (result.find("bilboard") != std::string::npos) {
        size_t pos = result.find("bilboard");
        result.replace(pos, 8, "billboard");
    }
    // glsidewalk → sidewalk (doubled prefix artifact)
    if (result.find("glsidewalk") != std::string::npos) {
        size_t pos = result.find("glsidewalk");
        result.replace(pos, 10, "sidewalk");
    }
    // sidewalkcorn → sidewalk_corner (abbreviated form)
    if (result.find("sidewalkcorn") != std::string::npos) {
        size_t pos = result.find("sidewalkcorn");
        result.replace(pos, 12, "sidewalk_corner");
    }
    // sidewalkcorner → sidewalk_corner (missing underscore)
    if (result.find("sidewalkcorner") != std::string::npos) {
        size_t pos = result.find("sidewalkcorner");
        result.replace(pos, 14, "sidewalk_corner");
    }
    // cornerbar_clean → cornerbar (strip _clean suffix)
    if (result.find("cornerbar_clean") != std::string::npos) {
        size_t pos = result.find("cornerbar_clean");
        result.replace(pos, 15, "cornerbar");
    }
    // Strip ent_ prefix: ent_con_barrier → con_barrier
    if (result.find("ent_") == 0) {
        result = result.substr(4);
    }
    // cos_XX_ prefix (zone leftover): cos_cg_cable → cable
    if (result.size() > 7 && result.substr(0, 4) == "cos_" && result[6] == '_') {
        result = result.substr(7);
    }
    // Strip _n suffix (night variant): smokestacka_n → smokestacka
    if (result.size() > 2 && result.substr(result.size() - 2) == "_n") {
        result = result.substr(0, result.size() - 2);
    }
    // Double letter typos: streetlampaa → streetlampa
    if (result.find("streetlampaa") != std::string::npos) {
        size_t pos = result.find("streetlampaa");
        result.replace(pos, 12, "streetlampa");
    }
    // stor_comk_5_5m → stor_comk_5_5 (strip trailing m from dimension)
    if (result.find("stor_comk_5_5m") != std::string::npos) {
        size_t pos = result.find("stor_comk_5_5m");
        result.replace(pos, 14, "stor_comk_5_5");
    }
    return result;
}

// ── Helper: quick check if a 4×4 matrix looks valid (row3[3] ≈ 1.0) ──────
static bool IsValidTransformMatrix(const float* m) {
    if (std::fabs(m[15] - 1.0f) > 0.01f) return false;
    if (std::fabs(m[12]) > 100000.0f || std::fabs(m[13]) > 100000.0f ||
        std::fabs(m[14]) > 100000.0f) return false;
    return true;
}

// ── Helper: check if a name looks like a non-renderable instance ──────────
static bool IsNonRenderableName(const std::string& nameLower) {
    // Check both the full name and the zone-stripped name
    // This catches "jh_omni01", "ee_light01" etc. that have zone prefixes
    std::string stripped = nameLower;
    if (stripped.size() > 3 && std::isalpha((unsigned char)stripped[0]) &&
        std::isalpha((unsigned char)stripped[1]) && stripped[2] == '_') {
        stripped = stripped.substr(3);
    }

    // Substring checks (work on full name)
    if (nameLower.find("fspot") != std::string::npos) return true;
    if (nameLower.find("_colvol") != std::string::npos) return true;
    if (nameLower.find("colvol") != std::string::npos) return true;
    if (nameLower.find("_marker") != std::string::npos) return true;
    if (nameLower.find("marker_") != std::string::npos) return true;
    if (nameLower.find("light_ground") != std::string::npos) return true;
    if (nameLower.find("skidmark") != std::string::npos) return true;
    if (nameLower.find("spawn_pt") != std::string::npos) return true;
    if (nameLower.find("_spawn") != std::string::npos) return true;
    if (nameLower.find("elec_marker") != std::string::npos) return true;
    if (nameLower.find("fx_shoreline") != std::string::npos) return true;
    if (nameLower.find("fx_dcl_") != std::string::npos) return true;

    // Start-of-string checks (check both full name AND zone-stripped name)
    auto startsCheck = [](const std::string& s) {
        if (s.find("swing") == 0) return true;
        if (s.find("light") == 0) return true;
        if (s.find("omni") == 0) return true;
        if (s.find("pedlight") == 0) return true;
        if (s.find("spot") == 0 && s.find("fspot") != 0) return true;
        if (s.find("fspot") == 0) return true;
        // Car spawn points (zz_car01 etc.) are entity spawns, not PCM models
        if (s.size() >= 4 && s.find("car") == 0 && std::isdigit((unsigned char)s[3])) return true;
        return false;
    };

    if (startsCheck(nameLower)) return true;
    if (startsCheck(stripped)) return true;
    return false;
}

// ── Internal: location of a PCM model in a pack file ──────────────────────
struct PCMModelRef {
    std::string packPath;
    uint32_t absOffset;
    uint32_t size;
};

// ── Helper: try all name variants to find a PCM match ─────────────────────
//    Given a base name, tries: as-is, abbreviation-expanded, zone-stripped,
//    zone-stripped+expanded — against both the hash index and the name index.
static uint32_t TryResolveName(const std::string& base,
                               const std::map<uint32_t, PCMModelRef>& pcmIndex,
                               const std::map<std::string, uint32_t>& pcmNameToHash) {
    if (base.empty()) return 0;

    // Build list of candidate names to try
    std::string expanded = ExpandAbbreviations(base);
    std::string noZone   = StripZonePrefix(base);
    std::string noZoneEx = ExpandAbbreviations(noZone);

    const std::string* candidates[] = { &base, &expanded, &noZone, &noZoneEx };
    for (auto* cand : candidates) {
        if (cand->empty()) continue;
        uint32_t h = CalculateCRC32(*cand);
        if (pcmIndex.count(h)) return h;
        if (pcmNameToHash.count(*cand)) return pcmNameToHash.at(*cand);
    }
    return 0;
}

// ── Internal: TOC helper – find TOC start (after double E3E3E3E3) ─────────
static size_t FindTocStart(const std::vector<uint8_t>& header, size_t headerSize) {
    const uint32_t magic = 0xE3E3E3E3;
    for (size_t i = 0; i + 4 <= headerSize; i++) {
        if (*(const uint32_t*)&header[i] == magic) {
            for (size_t j = i + 4; j < i + 1000 && j + 4 <= headerSize; j++) {
                if (*(const uint32_t*)&header[j] == magic) {
                    return j + 4;
                }
            }
            break;
        }
    }
    return 0;
}

// ═══════════════════════════════════════════════════════════════════════════
//  LoadAllWorldGeometries  –  zone meshes + instanced props
// ═══════════════════════════════════════════════════════════════════════════
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

    // Base transform: flip X axis to match rendering convention
    float baseTransform[16] = {0};
    baseTransform[0]  = -1.0f;
    baseTransform[5]  =  1.0f;
    baseTransform[10] =  1.0f;
    baseTransform[15] =  1.0f;

    Log("Loading all world geometries...");

    // Debug tracking for skipped instances
    static std::map<std::string, int> noTransformModels;
    static std::map<std::string, int> unresolvedModels;
    noTransformModels.clear();
    unresolvedModels.clear();

    // ═════════════════════════════════════════════════════════════════════
    //  PASS 1 – Build global PCM model index from ALL packs
    // ═════════════════════════════════════════════════════════════════════
    std::map<uint32_t, PCMModelRef> pcmIndex;       // hash → model location
    std::map<std::string, uint32_t> pcmNameToHash;   // lowercase name → hash

    Log("Building PCM model index...");

    for (const auto& path : foundPacks) {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) continue;

        size_t fileSize = file.tellg();
        if (fileSize < 32) { file.close(); continue; }

        file.seekg(24);
        uint32_t headerSize, packDataOffset;
        file.read((char*)&headerSize, 4);
        file.read((char*)&packDataOffset, 4);
        if (!file.good()) { file.close(); continue; }

        size_t headerReadSize = std::min((size_t)500000, fileSize);
        std::vector<uint8_t> tempHeader(headerReadSize);
        file.seekg(0);
        file.read((char*)tempHeader.data(), headerReadSize);

        size_t tocStart = FindTocStart(tempHeader, headerReadSize);
        if (tocStart == 0) { file.close(); continue; }

        file.clear();
        file.seekg(tocStart);

        while (file.good()) {
            uint32_t hash, type, offset, size;
            file.read((char*)&hash, 4);
            file.read((char*)&type, 4);
            file.read((char*)&offset, 4);
            file.read((char*)&size, 4);
            if (!file.good()) break;
            if (type >= 0x1000 || type == 0x0000) break;

            // Type 0x15 = PCM model (high-res), Type 0x17 = PCM model (LOD)
            if ((type == 0x15 || type == 0x17) && size > 4) {
                uint32_t absOffset = packDataOffset + offset;
                if (absOffset + 4 <= fileSize) {
                    size_t savePos = file.tellg();
                    file.seekg(absOffset);
                    uint32_t sig = 0;
                    file.read((char*)&sig, 4);
                    file.clear();
                    file.seekg(savePos);

                    if (sig == 0x204D4350) {
                        // Prefer type 0x15 over 0x17 (don't overwrite high-res with LOD)
                        if (type == 0x15 || !pcmIndex.count(hash)) {
                            PCMModelRef ref;
                            ref.packPath = path.string();
                            ref.absOffset = absOffset;
                            ref.size = size;
                            pcmIndex[hash] = ref;
                        }

                        if (dictionary.count(hash)) {
                            std::string nameLower = StrToLower(dictionary[hash]);
                            if (type == 0x15 || !pcmNameToHash.count(nameLower)) {
                                pcmNameToHash[nameLower] = hash;
                            }
                        }
                    }
                }
            }
        }
        file.close();
    }

    Log("PCM index: " + std::to_string(pcmIndex.size()) + " models across all packs, "
        + std::to_string(pcmNameToHash.size()) + " named");

    // ═════════════════════════════════════════════════════════════════════
    //  PASS 2 – Load base zone geometry (terrain/buildings)
    // ═════════════════════════════════════════════════════════════════════
    for (const auto& path : foundPacks) {
        std::string stem = StrToLower(path.stem().string());
        if (!IsWorldPack(stem)) continue;

        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) continue;

        size_t fileSize = file.tellg();
        if (fileSize < 32) { file.close(); continue; }

        file.seekg(24);
        uint32_t headerSize, packDataOffset;
        file.read((char*)&headerSize, 4);
        file.read((char*)&packDataOffset, 4);
        if (!file.good()) { file.close(); continue; }

        size_t headerReadSize = std::min((size_t)500000, fileSize);
        std::vector<uint8_t> tempHeader(headerReadSize);
        file.seekg(0);
        file.read((char*)tempHeader.data(), headerReadSize);

        size_t tocStart = FindTocStart(tempHeader, headerReadSize);
        if (tocStart == 0) { file.close(); continue; }

        file.clear();
        file.seekg(tocStart);

        while (file.good()) {
            uint32_t hash, type, offset, size;
            file.read((char*)&hash, 4);
            file.read((char*)&type, 4);
            file.read((char*)&offset, 4);
            file.read((char*)&size, 4);
            if (!file.good()) break;
            if (type >= 0x1000 || type == 0x0000) break;

            uint32_t absOffset = (uint32_t)(packDataOffset + offset);

            if (size > 4 && absOffset + 4 <= fileSize) {
                size_t filePos = file.tellg();
                file.seekg(absOffset);
                uint32_t sig = 0;
                file.read((char*)&sig, 4);
                file.clear();
                file.seekg(filePos);

                if (sig == 0x204D4350) {
                    std::string entryName = dictionary.count(hash) ? StrToLower(dictionary[hash]) : "";

                    // Zone geometry: name matches stem, no underscore (e.g., "ee", "gf")
                    if (!entryName.empty() && entryName.find(stem) == 0 && entryName.find('_') == std::string::npos) {
                        std::vector<uint8_t> pcmData(size);
                        file.seekg(absOffset);
                        file.read((char*)pcmData.data(), size);
                        file.clear();
                        file.seekg(filePos);

                        AddMeshFromDataWithTransform(pcmData, entryName, nullptr, path.string(), absOffset, baseTransform);
                    }
                }
            }
        }
        file.close();
    }

    int zoneGeoCount = (int)previewMeshes.size();
    Log("Zone geometry loaded: " + std::to_string(zoneGeoCount) + " meshes");

    // ═════════════════════════════════════════════════════════════════════
    //  PASS 3 – Load instanced props from type 0x0A data
    // ═════════════════════════════════════════════════════════════════════
    Log("Scanning instance data (type 0x0A)...");

    // Cache PCM data to avoid re-reading the same model from disk
    std::map<uint32_t, std::vector<uint8_t>> pcmDataCache;

    // Track which zone hashes we already loaded as base geometry
    std::set<uint32_t> zoneBaseHashes;
    for (const auto& path : foundPacks) {
        std::string stem = StrToLower(path.stem().string());
        if (!IsWorldPack(stem)) continue;
        uint32_t zoneHash = CalculateCRC32(stem);
        zoneBaseHashes.insert(zoneHash);
    }

    int totalInstances = 0;
    int loadedInstances = 0;
    int skippedNoModel = 0;
    int skippedNonRenderable = 0;
    int skippedNoTransform = 0;

    for (const auto& path : foundPacks) {
        std::string stem = StrToLower(path.stem().string());
        if (!IsWorldPack(stem)) continue;

        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) continue;

        size_t fileSize = file.tellg();
        if (fileSize < 32) { file.close(); continue; }

        file.seekg(24);
        uint32_t headerSize, packDataOffset;
        file.read((char*)&headerSize, 4);
        file.read((char*)&packDataOffset, 4);
        if (!file.good()) { file.close(); continue; }

        size_t headerReadSize = std::min((size_t)500000, fileSize);
        std::vector<uint8_t> tempHeader(headerReadSize);
        file.seekg(0);
        file.read((char*)tempHeader.data(), headerReadSize);

        size_t tocStart = FindTocStart(tempHeader, headerReadSize);
        if (tocStart == 0) { file.close(); continue; }

        // Collect ALL type 0x0A entries from the TOC
        file.clear();
        file.seekg(tocStart);

        std::vector<std::pair<uint32_t, uint32_t>> instanceBlocks; // offset, size

        while (file.good()) {
            uint32_t hash, type, offset, size;
            file.read((char*)&hash, 4);
            file.read((char*)&type, 4);
            file.read((char*)&offset, 4);
            file.read((char*)&size, 4);
            if (!file.good()) break;
            if (type >= 0x1000 || type == 0x0000) break;

            if (type == 0x0A && size > 64) {
                uint32_t absOfs = packDataOffset + offset;
                if (absOfs + size <= fileSize) {
                    instanceBlocks.push_back({absOfs, size});
                }
            }
        }

        // Process each type 0x0A block
        for (const auto& [blockOffset, blockSize] : instanceBlocks) {
            std::vector<uint8_t> blockData(blockSize);
            file.clear();
            file.seekg(blockOffset);
            file.read((char*)blockData.data(), blockSize);
            if (!file.good()) continue;

            // Scan for all marker positions (aligned to 4 bytes)
            const uint32_t MARKER_ACE  = 0x7ACE5BAD;
            const uint32_t MARKER_BAD  = 0x5BADF00D;

            std::vector<size_t> acePositions;
            std::vector<size_t> badPositions;

            for (size_t i = 0; i + 4 <= blockSize; i += 4) {
                uint32_t val;
                memcpy(&val, &blockData[i], 4);
                if (val == MARKER_ACE)      acePositions.push_back(i);
                else if (val == MARKER_BAD) badPositions.push_back(i);
            }

            // Process each instance record
            for (size_t aceIdx = 0; aceIdx < acePositions.size(); aceIdx++) {
                size_t acePos = acePositions[aceIdx];
                totalInstances++;

                // Boundary: the previous 7ACE5BAD (or block start)
                size_t prevAcePos = (aceIdx > 0) ? acePositions[aceIdx - 1] : 0;

                // Instance name hash is at 7ACE5BAD + 16
                if (acePos + 20 > blockSize) continue;
                uint32_t instanceHash;
                memcpy(&instanceHash, &blockData[acePos + 16], 4);

                // Skip null hashes (padding/invalid entries)
                if (instanceHash == 0) continue;

                // Skip if this is a zone base hash (already loaded as terrain)
                if (zoneBaseHashes.count(instanceHash)) continue;

                // Get the instance name for filtering / name resolution
                std::string instanceName;
                if (dictionary.count(instanceHash)) {
                    instanceName = StrToLower(dictionary[instanceHash]);
                }

                // Skip known non-renderable instance types
                if (!instanceName.empty() && IsNonRenderableName(instanceName)) {
                    skippedNonRenderable++;
                    continue;
                }

                // ── Resolve instance hash → PCM model hash ──────────
                uint32_t pcmHash = 0;

                // 1) Direct match
                if (pcmIndex.count(instanceHash)) {
                    pcmHash = instanceHash;
                }

                // 2) Strip numeric suffix: "ee_storesign03" → "ee_storesign"
                if (pcmHash == 0 && !instanceName.empty()) {
                    std::string stripped = StripNumericSuffix(instanceName);
                    if (stripped != instanceName)
                        pcmHash = TryResolveName(stripped, pcmIndex, pcmNameToHash);
                }

                // 3) Strip zone prefix + numeric suffix: "ee_lightsa05" → "lightsa"
                if (pcmHash == 0 && !instanceName.empty()) {
                    std::string noPrefix = StripZonePrefix(instanceName);
                    std::string base = StripNumericSuffix(noPrefix);
                    if (base != instanceName)
                        pcmHash = TryResolveName(base, pcmIndex, pcmNameToHash);
                }

                // 4) Strip only zone prefix: "gf_ironwerk" → "ironwerk"
                if (pcmHash == 0 && !instanceName.empty()) {
                    std::string noPrefix = StripZonePrefix(instanceName);
                    if (noPrefix != instanceName)
                        pcmHash = TryResolveName(noPrefix, pcmIndex, pcmNameToHash);
                }

                // 5) Try "ref_" prefix: "jh_penthoused" → "ref_penthoused"
                if (pcmHash == 0 && !instanceName.empty()) {
                    std::string noPrefix = StripZonePrefix(instanceName);
                    std::string base = StripNumericSuffix(noPrefix);
                    std::string refName = "ref_" + base;
                    uint32_t h = CalculateCRC32(refName);
                    if (pcmIndex.count(h)) pcmHash = h;
                    if (pcmHash == 0 && pcmNameToHash.count(refName))
                        pcmHash = pcmNameToHash[refName];
                }

                // 6) Strip intermediate prefixes (ent_, col_, rf_):
                //    "fg_ent_fg_strtlampa05" → strip zone → "ent_fg_strtlampa05"
                //    → strip ent_ → "fg_strtlampa05" → strip num → "fg_strtlampa"
                //    → TryResolveName also tries zone strip + abbreviation expansion
                //      → "strtlampa" → "streetlampa" ✓
                if (pcmHash == 0 && !instanceName.empty()) {
                    std::string noPrefix = StripZonePrefix(instanceName);
                    const char* midPrefixes[] = {"ent_", "col_", "rf_", nullptr};
                    for (int mp = 0; midPrefixes[mp] && pcmHash == 0; mp++) {
                        size_t mpLen = strlen(midPrefixes[mp]);
                        if (noPrefix.size() > mpLen && noPrefix.substr(0, mpLen) == midPrefixes[mp]) {
                            std::string inner = noPrefix.substr(mpLen);
                            inner = StripZonePrefix(inner);
                            inner = StripNumericSuffix(inner);
                            inner = ExpandAbbreviations(inner);
                            if (!inner.empty())
                                pcmHash = TryResolveName(inner, pcmIndex, pcmNameToHash);
                        }
                    }
                }

                // 7) Zone-prefixed model lookup: for instances in zone ZZ,
                //    try "zz_" + base in case the PCM model is zone-specific
                //    (e.g., fg_penthouseb is a PCM in FG.PCPACK)
                if (pcmHash == 0 && !instanceName.empty()) {
                    std::string noPrefix = StripZonePrefix(instanceName);
                    std::string base = StripNumericSuffix(noPrefix);
                    // Also try after stripping double prefixes
                    for (auto* midP : {"ent_", "col_", "rf_"}) {
                        size_t mpLen = strlen(midP);
                        if (base.size() > mpLen && base.substr(0, mpLen) == midP) {
                            base = StripNumericSuffix(base.substr(mpLen));
                            // Strip embedded zone prefix too
                            base = StripZonePrefix(base);
                            base = StripNumericSuffix(base);
                            break;
                        }
                    }
                    if (!base.empty() && base != instanceName) {
                        // Try with the pack's zone prefix
                        std::string zoneModel = stem + "_" + base;
                        pcmHash = TryResolveName(zoneModel, pcmIndex, pcmNameToHash);
                        // Also try expanded abbreviation with zone prefix
                        if (pcmHash == 0) {
                            std::string zoneModelEx = stem + "_" + ExpandAbbreviations(base);
                            pcmHash = TryResolveName(zoneModelEx, pcmIndex, pcmNameToHash);
                        }
                    }
                }

                // 8) Exact name match in pcmNameToHash (last resort)
                if (pcmHash == 0 && !instanceName.empty()) {
                    if (pcmNameToHash.count(instanceName))
                        pcmHash = pcmNameToHash[instanceName];
                    // Also try with abbreviation expansion on the full name
                    if (pcmHash == 0) {
                        std::string expanded = ExpandAbbreviations(instanceName);
                        if (expanded != instanceName && pcmNameToHash.count(expanded))
                            pcmHash = pcmNameToHash[expanded];
                    }
                }

                // 9) Handle city_* prefix (e.g., city_baxter_spire01 → jh_baxter_spire)
                if (pcmHash == 0 && instanceName.size() > 5 && instanceName.substr(0, 5) == "city_") {
                    std::string noCity = instanceName.substr(5);
                    std::string base = StripNumericSuffix(noCity);
                    if (base.size() > 4 && base.substr(base.size() - 4) == "_new") {
                        base = base.substr(0, base.size() - 4);
                    }
                    pcmHash = TryResolveName(base, pcmIndex, pcmNameToHash);
                    if (pcmHash == 0) {
                        pcmHash = TryResolveName("city_" + base, pcmIndex, pcmNameToHash);
                    }
                    if (pcmHash == 0) {
                        pcmHash = TryResolveName("jh_" + base, pcmIndex, pcmNameToHash);
                    }
                }

                // 10) Double zone prefix: "jj_ent_jj_streetlampa05" → strip both
                if (pcmHash == 0 && !instanceName.empty()) {
                    std::string noPrefix = StripZonePrefix(instanceName);
                    if (noPrefix.size() > 7 && noPrefix.substr(0, 4) == "ent_") {
                        std::string inner = noPrefix.substr(4);
                        inner = StripZonePrefix(inner);
                        inner = StripNumericSuffix(inner);
                        inner = ExpandAbbreviations(inner);
                        if (!inner.empty())
                            pcmHash = TryResolveName(inner, pcmIndex, pcmNameToHash);
                    }
                }

                // 11) sidewalk_corner without size → try both _4m and _5m variants
                if (pcmHash == 0 && !instanceName.empty()) {
                    std::string noPrefix = StripZonePrefix(instanceName);
                    std::string base = StripNumericSuffix(noPrefix);
                    base = ExpandAbbreviations(base);
                    if (base == "sidewalk_corner" || base.find("sidewalk_corner") != std::string::npos) {
                        // Strip any existing size suffix and try both variants
                        std::string core = base;
                        if (core.size() > 3 && core.substr(core.size() - 3) == "_4m")
                            core = core.substr(0, core.size() - 3);
                        else if (core.size() > 3 && core.substr(core.size() - 3) == "_5m")
                            core = core.substr(0, core.size() - 3);
                        // Try _5m first (more common), then _4m
                        pcmHash = TryResolveName(core + "_5m", pcmIndex, pcmNameToHash);
                        if (pcmHash == 0)
                            pcmHash = TryResolveName(core + "_4m", pcmIndex, pcmNameToHash);
                    }
                }

                if (pcmHash == 0) {
                    // Track all unresolved instances
                    if (!instanceName.empty()) {
                        std::string noPrefix = StripZonePrefix(instanceName);
                        std::string base = StripNumericSuffix(noPrefix);
                        std::string expanded = ExpandAbbreviations(base);
                        unresolvedModels[expanded]++;
                    }
                    skippedNoModel++;
                    continue;
                }

                // ── Find Layout B transform ─────────────────────────
                // Layout B: matrix(64) → 5BADF00D → ... → 7ACE5BAD → hash
                //
                // The key constraint: the 5BADF00D must be AFTER the previous
                // 7ACE5BAD. Otherwise it belongs to a different record.
                float instanceMatrix[16];
                bool hasTransform = false;

                // Find all 5BADF00D markers between prevAcePos and acePos
                auto badBegin = std::lower_bound(badPositions.begin(), badPositions.end(), prevAcePos);
                auto badEnd   = std::lower_bound(badPositions.begin(), badPositions.end(), acePos);

                // Try each candidate (prefer the closest to acePos = last one)
                for (auto badIt = badEnd; badIt != badBegin; ) {
                    --badIt;
                    size_t badPos = *badIt;

                    // Must have room for 64-byte matrix before 5BADF00D
                    if (badPos < 64) continue;

                    size_t matrixStart = badPos - 64;
                    // Matrix must also be after prevAcePos (within this record's region)
                    if (matrixStart < prevAcePos) continue;

                    float candidate[16];
                    memcpy(candidate, &blockData[matrixStart], 64);

                    // Validate transform matrix
                    if (std::fabs(candidate[15] - 1.0f) > 0.01f) continue;

                    // Check m[3], m[7], m[11] are ~0 (affine transform)
                    if (std::fabs(candidate[3]) > 0.01f ||
                        std::fabs(candidate[7]) > 0.01f ||
                        std::fabs(candidate[11]) > 0.01f) continue;

                    // Translation should be in world range
                    if (std::fabs(candidate[12]) > 50000.0f ||
                        std::fabs(candidate[13]) > 50000.0f ||
                        std::fabs(candidate[14]) > 50000.0f) continue;

                    // Rotation part should have non-zero magnitude
                    float rotMag = 0;
                    for (int r = 0; r < 3; r++)
                        for (int c = 0; c < 3; c++)
                            rotMag += std::fabs(candidate[r * 4 + c]);
                    if (rotMag < 0.01f) continue;

                    // Rotation rows should be roughly unit-length (orthogonal matrix)
                    bool rowsOk = true;
                    for (int r = 0; r < 3 && rowsOk; r++) {
                        float len = 0;
                        for (int c = 0; c < 3; c++)
                            len += candidate[r * 4 + c] * candidate[r * 4 + c];
                        len = std::sqrt(len);
                        // Allow some tolerance (scale + imprecision)
                        if (len < 0.1f || len > 10.0f) rowsOk = false;
                    }
                    if (!rowsOk) continue;

                    memcpy(instanceMatrix, candidate, 64);
                    hasTransform = true;
                    break;
                }

                if (!hasTransform) {
                    // ── Layout A: forward position scan ──────────────
                    // Layout A: 7ACE5BAD → flags → hash → ... → position(3 floats at +172)
                    // These are "simple" instances with position only (no rotation).
                    if (acePos + 184 <= blockSize) {
                        float px, py, pz;
                        memcpy(&px, &blockData[acePos + 172], 4);
                        memcpy(&py, &blockData[acePos + 176], 4);
                        memcpy(&pz, &blockData[acePos + 180], 4);

                        // Validate: reasonable world coordinates, not all zeros
                        if (std::fabs(px) < 50000.0f && std::fabs(py) < 50000.0f &&
                            std::fabs(pz) < 50000.0f &&
                            (std::fabs(px) > 0.01f || std::fabs(py) > 0.01f || std::fabs(pz) > 0.01f)) {
                            // Build identity rotation + translation matrix
                            memset(instanceMatrix, 0, 64);
                            instanceMatrix[0]  = 1.0f;
                            instanceMatrix[5]  = 1.0f;
                            instanceMatrix[10] = 1.0f;
                            instanceMatrix[12] = px;
                            instanceMatrix[13] = py;
                            instanceMatrix[14] = pz;
                            instanceMatrix[15] = 1.0f;
                            hasTransform = true;
                        }
                    }
                }

                if (!hasTransform) {
                    // Track what models are being skipped due to no transform
                    std::string modelName = dictionary.count(pcmHash) ? StrToLower(dictionary[pcmHash]) : "0x" + std::to_string(pcmHash);
                    noTransformModels[modelName]++;
                    skippedNoTransform++;
                    continue;
                }

                // ── Compose: instance transform × base X-flip ───────
                float combinedTransform[16];
                MultiplyMatrix4x4(instanceMatrix, baseTransform, combinedTransform);

                // ── Load PCM model data (cached) ────────────────────
                if (!pcmDataCache.count(pcmHash)) {
                    const auto& ref = pcmIndex[pcmHash];
                    std::ifstream pcmFile(ref.packPath, std::ios::binary);
                    if (!pcmFile.is_open()) continue;
                    pcmDataCache[pcmHash].resize(ref.size);
                    pcmFile.seekg(ref.absOffset);
                    pcmFile.read((char*)pcmDataCache[pcmHash].data(), ref.size);
                    pcmFile.close();

                    if (pcmDataCache[pcmHash].size() < 16) {
                        pcmDataCache.erase(pcmHash);
                        continue;
                    }
                }

                const auto& pcmData = pcmDataCache[pcmHash];
                const auto& ref = pcmIndex[pcmHash];

                std::string modelName = instanceName.empty()
                    ? (dictionary.count(pcmHash) ? StrToLower(dictionary[pcmHash]) : "unknown")
                    : instanceName;

                AddMeshFromDataWithTransform(pcmData, modelName, nullptr,
                                             ref.packPath, ref.absOffset,
                                             combinedTransform);
                loadedInstances++;
            } // end instance record loop
        } // end instanceBlocks loop
        file.close();
    } // end pack loop

    int instanceMeshCount = (int)previewMeshes.size() - zoneGeoCount;
    Log("Instance scan: " + std::to_string(totalInstances) + " records, "
        + std::to_string(loadedInstances) + " props loaded (" + std::to_string(instanceMeshCount) + " meshes), "
        + std::to_string(skippedNoModel) + " unresolved, "
        + std::to_string(skippedNonRenderable) + " non-renderable, "
        + std::to_string(skippedNoTransform) + " no transform");

    // Log top unresolved models
    Log("=== TOP UNRESOLVED (by count) ===");
    std::vector<std::pair<std::string, int>> unresolvedVec(unresolvedModels.begin(), unresolvedModels.end());
    std::sort(unresolvedVec.begin(), unresolvedVec.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    for (size_t i = 0; i < std::min((size_t)20, unresolvedVec.size()); i++) {
        Log("  " + unresolvedVec[i].first + ": " + std::to_string(unresolvedVec[i].second) + "x");
    }

    // Log top no-transform models
    Log("=== TOP NO-TRANSFORM (by count) ===");
    std::vector<std::pair<std::string, int>> noTransformVec(noTransformModels.begin(), noTransformModels.end());
    std::sort(noTransformVec.begin(), noTransformVec.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    for (size_t i = 0; i < std::min((size_t)20, noTransformVec.size()); i++) {
        Log("  " + noTransformVec[i].first + ": " + std::to_string(noTransformVec[i].second) + "x");
    }

    pcmDataCache.clear();

    LoadSkybox();

    isModelLoaded = true;
    Log("World loaded. Total meshes: " + std::to_string(previewMeshes.size()));
}

// ═══════════════════════════════════════════════════════════════════════════
//  LoadSkybox
// ═══════════════════════════════════════════════════════════════════════════
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