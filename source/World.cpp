#include "SpiderManTool.h"
#include <fstream>
#include <cstring>
#include <set>
#include <sstream>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

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
    // streetlampaa → streetlampa (typo - double 'a')
    if (result.find("streetlampaa") != std::string::npos) {
        size_t pos = result.find("streetlampaa");
        result.replace(pos, 12, "streetlampa");
    }
    // Strip _n suffix (night variant): smokestacka_n → smokestacka
    if (result.size() > 2 && result.substr(result.size() - 2) == "_n") {
        result = result.substr(0, result.size() - 2);
    }
    // drum_cone instances - these are zone-specific (hi_cone, hj_cone)
    // No universal mapping available
    // con_barrier → barriera (construction barrier maps to barrier PCM)
    if (result == "con_barrier" || result == "ent_con_barrier") {
        result = "barriera";
    }
    // con_dmpstr → dumpstera (construction dumpster)
    if (result == "con_dmpstr" || result == "ent_con_dmpstr" || result == "constr_dmpstr") {
        result = "dumpstera";
    }
    // barrier_plstc → barriera (plastic barrier)
    if (result == "barrier_plstc" || result == "ent_barrier_plstc") {
        result = "barriera";
    }
    // warn_tape → skip (non-renderable marker)
    if (result.find("warn_tape") != std::string::npos) {
        result = "";
    }
    // rf_penthouse* → ref_penthouse* (rf_ is abbreviation for ref_)
    if (result.find("rf_penthouse") != std::string::npos) {
        size_t pos = result.find("rf_penthouse");
        result.replace(pos, 12, "ref_penthouse");
    }
    // forklft → forklift (missing 'i')
    if (result.find("forklft") != std::string::npos) {
        size_t pos = result.find("forklft");
        result.replace(pos, 7, "forklift");
    }
    // stor abbreviations: "storcome" → "stor_come", "storcomb" → "stor_comb"
    if (result.find("stor") == 0 && result.size() > 4 && result[4] != '_') {
        result.insert(4, "_");
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
    if (nameLower.find("city_omni") != std::string::npos) return true;
    if (nameLower.find("sml_omni") != std::string::npos) return true;
    if (nameLower.find("small_omni") != std::string::npos) return true;
    if (nameLower.find("tinyomni") != std::string::npos) return true;
    if (nameLower.find("cyclemarker") != std::string::npos) return true;
    if (nameLower.find("cslight") != std::string::npos) return true;

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
        if (s.find("lightcast") == 0) return true;
        if (s.find("litecast") == 0) return true;
        if (s.find("shadow") == 0) return true;
        if (s.find("glow") == 0) return true;
        if (s.find("blink") == 0) return true;
        if (s.find("bubbles") == 0) return true;
        if (s.find("hang") == 0) return true;
        if (s.find("platform") == 0) return true;
        if (s.find("rubble") == 0) return true;
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
                    // These are the base terrain meshes with world-space coordinates.
                    // Other PCMs (fg_penthouseb, etc.) are in model space and need
                    // instance placement from type 0x0A blocks.
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
    //  PASS 3 – Load instanced props from type 0x0A and type 0x04 data
    // ═════════════════════════════════════════════════════════════════════
    Log("Scanning instance data (type 0x0A + 0x04)...");

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

    // Block info: offset, size, tocHash, tocType
    struct InstanceBlockInfo {
        uint32_t offset;
        uint32_t size;
        uint32_t tocHash;   // For type 0x04, this IS the PCM model hash
        uint8_t  tocType;   // 0x0A or 0x04
    };

    for (const auto& path : foundPacks) {
        std::string stem = StrToLower(path.stem().string());

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

        // Collect type 0x0A (instance placement) and type 0x04 (entity defs)
        file.clear();
        file.seekg(tocStart);

        std::vector<InstanceBlockInfo> instanceBlocks;

        while (file.good()) {
            uint32_t hash, type, offset, size;
            file.read((char*)&hash, 4);
            file.read((char*)&type, 4);
            file.read((char*)&offset, 4);
            file.read((char*)&size, 4);
            if (!file.good()) break;
            if (type >= 0x1000 || type == 0x0000) break;

            if (size > 64) {
                uint32_t absOfs = packDataOffset + offset;
                if (absOfs + size <= fileSize) {
                    if (type == 0x0A) {
                        instanceBlocks.push_back({absOfs, size, hash, 0x0A});
                    } else if (type == 0x04 && pcmIndex.count(hash)) {
                        // Type 0x04: entity definition block – the TOC hash IS the PCM
                        // model hash. Only collect if we have a matching PCM model.
                        instanceBlocks.push_back({absOfs, size, hash, 0x04});
                    }
                }
            }
        }

        // Process each instance block
        for (const auto& blockInfo : instanceBlocks) {
            std::vector<uint8_t> blockData(blockInfo.size);
            file.clear();
            file.seekg(blockInfo.offset);
            file.read((char*)blockData.data(), blockInfo.size);
            if (!file.good()) continue;

            const uint32_t blockSize = blockInfo.size;

            // Scan for all marker positions (aligned to 4 bytes)
            const uint32_t MARKER_ACE  = 0x7ACE5BAD;

            std::vector<size_t> acePositions;

            for (size_t i = 0; i + 4 <= blockSize; i += 4) {
                uint32_t val;
                memcpy(&val, &blockData[i], 4);
                if (val == MARKER_ACE)      acePositions.push_back(i);
            }

            // Process each instance record
            for (size_t aceIdx = 0; aceIdx < acePositions.size(); aceIdx++) {
                size_t acePos = acePositions[aceIdx];
                totalInstances++;


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

                // For type 0x04 blocks, the TOC hash IS the PCM model hash
                if (blockInfo.tocType == 0x04) {
                    pcmHash = blockInfo.tocHash;
                }

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
                //    "fg_ent_fg_penthouseb" → strip zone → "ent_fg_penthouseb"
                //    → strip ent_ → "fg_penthouseb" → TryResolveName ✓
                //    Also try after stripping embedded zone: "penthouseb"
                if (pcmHash == 0 && !instanceName.empty()) {
                    std::string noPrefix = StripZonePrefix(instanceName);
                    const char* midPrefixes[] = {"ent_", "col_", "rf_", nullptr};
                    for (int mp = 0; midPrefixes[mp] && pcmHash == 0; mp++) {
                        size_t mpLen = strlen(midPrefixes[mp]);
                        if (noPrefix.size() > mpLen && noPrefix.substr(0, mpLen) == midPrefixes[mp]) {
                            std::string inner = noPrefix.substr(mpLen);
                            // Try BEFORE stripping embedded zone (e.g., "fg_penthouseb")
                            std::string innerNum = StripNumericSuffix(inner);
                            if (!innerNum.empty())
                                pcmHash = TryResolveName(innerNum, pcmIndex, pcmNameToHash);
                            if (pcmHash == 0 && !innerNum.empty())
                                pcmHash = TryResolveName(ExpandAbbreviations(innerNum), pcmIndex, pcmNameToHash);
                            // Try AFTER stripping embedded zone (e.g., "penthouseb")
                            if (pcmHash == 0) {
                                inner = StripZonePrefix(inner);
                                inner = StripNumericSuffix(inner);
                                inner = ExpandAbbreviations(inner);
                                if (!inner.empty())
                                    pcmHash = TryResolveName(inner, pcmIndex, pcmNameToHash);
                            }
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

                // 12) Append variant suffix 'a': "trafficlight" → "trafficlighta"
                if (pcmHash == 0 && !instanceName.empty()) {
                    std::string noPrefix = StripZonePrefix(instanceName);
                    std::string base = StripNumericSuffix(noPrefix);
                    base = ExpandAbbreviations(base);
                    if (!base.empty()) {
                        pcmHash = TryResolveName(base + "a", pcmIndex, pcmNameToHash);
                    }
                }

                // 13) Embedded zone prefix without underscore: "iitablea" → "tablea"
                if (pcmHash == 0 && !instanceName.empty()) {
                    std::string noPrefix = StripZonePrefix(instanceName);
                    std::string base = StripNumericSuffix(noPrefix);
                    if (base.size() > 2 && std::isalpha((unsigned char)base[0]) &&
                        std::isalpha((unsigned char)base[1]) && std::islower((unsigned char)base[2])) {
                        std::string inner = base.substr(2);
                        pcmHash = TryResolveName(inner, pcmIndex, pcmNameToHash);
                    }
                }

                // 14) "rf_" prefix models: "rf_skylightb" → "skylightb"
                if (pcmHash == 0 && !instanceName.empty()) {
                    std::string noPrefix = StripZonePrefix(instanceName);
                    std::string base = StripNumericSuffix(noPrefix);
                    if (base.size() > 3 && base.substr(0, 3) == "rf_") {
                        std::string inner = base.substr(3);
                        pcmHash = TryResolveName(inner, pcmIndex, pcmNameToHash);
                    }
                }

                // 15) Numeric-prefixed PCM models: many PCMs have a digit prefix
                //     like "1stor_comhe01_trm02", "2obj_edge_01", "6rf_watertow_woodb".
                //     Instances reference them without the prefix.
                //     Try prepending "1".."9" to the resolved base name.
                if (pcmHash == 0 && !instanceName.empty()) {
                    std::string noPrefix = StripZonePrefix(instanceName);
                    std::string base = StripNumericSuffix(noPrefix);
                    base = ExpandAbbreviations(base);
                    for (char d = '1'; d <= '9' && pcmHash == 0; d++) {
                        std::string numPrefixed = std::string(1, d) + base;
                        uint32_t h = CalculateCRC32(numPrefixed);
                        if (pcmIndex.count(h)) { pcmHash = h; break; }
                        if (pcmNameToHash.count(numPrefixed)) { pcmHash = pcmNameToHash[numPrefixed]; break; }
                    }
                }

                // 16) "obj_" prefix: instances like "dumpster01" → "obj_dumpster"
                if (pcmHash == 0 && !instanceName.empty()) {
                    std::string noPrefix = StripZonePrefix(instanceName);
                    std::string base = StripNumericSuffix(noPrefix);
                    base = ExpandAbbreviations(base);
                    // Strip ent_ if present
                    if (base.size() > 4 && base.substr(0, 4) == "ent_")
                        base = base.substr(4);
                    base = StripZonePrefix(base);
                    base = StripNumericSuffix(base);
                    if (!base.empty()) {
                        std::string objName = "obj_" + base;
                        pcmHash = TryResolveName(objName, pcmIndex, pcmNameToHash);
                    }
                }

                // 17) Try the instance name as a substring of PCM names
                //     e.g. "drum_cone" matches "rf_cone" won't work, but
                //     "parked_car" might match a longer PCM name
                if (pcmHash == 0 && !instanceName.empty()) {
                    std::string noPrefix = StripZonePrefix(instanceName);
                    std::string base = StripNumericSuffix(noPrefix);
                    // Strip ent_, col_ prefixes
                    for (auto* p : {"ent_", "col_", "rf_"}) {
                        if (base.size() > strlen(p) && base.substr(0, strlen(p)) == p)
                            base = base.substr(strlen(p));
                    }
                    base = StripZonePrefix(base);
                    base = StripNumericSuffix(base);
                    base = ExpandAbbreviations(base);
                    // Search for base as a suffix in PCM names
                    if (base.size() >= 5) {
                        for (const auto& [name, hash] : pcmNameToHash) {
                            if (name.size() >= base.size() &&
                                name.substr(name.size() - base.size()) == base) {
                                pcmHash = hash;
                                break;
                            }
                        }
                    }
                }

                // 18) Prefix matching for parameterized names (e.g., stor_come → stor_come_07_10)
                if (pcmHash == 0 && !instanceName.empty()) {
                    std::string noPrefix = StripZonePrefix(instanceName);
                    std::string base = StripNumericSuffix(noPrefix);
                    base = ExpandAbbreviations(base);
                    if (!base.empty() && base.size() >= 4) {
                        for (const auto& [name, hash] : pcmNameToHash) {
                            if (name.size() > base.size() && name.substr(0, base.size()) == base &&
                                (name[base.size()] == '_' || std::isdigit((unsigned char)name[base.size()]))) {
                                pcmHash = hash;
                                break;
                            }
                        }
                    }
                }

                if (pcmHash == 0) {
                    static std::set<std::string> loggedBases;
                    if (loggedBases.size() < 50 && !instanceName.empty()) {
                        std::string noPrefix = StripZonePrefix(instanceName);
                        std::string base = StripNumericSuffix(noPrefix);
                        std::string expanded = ExpandAbbreviations(base);
                        if (!base.empty() && loggedBases.find(base) == loggedBases.end()) {
                            loggedBases.insert(base);
                            Log("UNRESOLVED: " + instanceName + " -> " + expanded);
                        }
                    }
                    skippedNoModel++;
                    continue;
                }

                // Also filter by resolved model name (for type 0x04 where
                // instanceName is _entid_xx but model might be non-renderable)
                if (pcmHash != 0 && dictionary.count(pcmHash)) {
                    std::string modelName = StrToLower(dictionary[pcmHash]);
                    if (modelName == "nullo" || modelName == "marker" || modelName == "tool_marker" ||
                        modelName == "oceanmesh" || modelName == "_gen_building" ||
                        modelName == "checkercab_phys" || modelName == "tokens" ||
                        modelName.find("_lod") != std::string::npos ||
                        modelName.find("sky_") == 0 ||
                        modelName.find("ped_") == 0 ||
                        modelName.find("fx_") == 0 ||
                        modelName.find("decal") != std::string::npos ||
                        modelName.find("shadow") == 0) {
                        skippedNonRenderable++;
                        continue;
                    }
                }

                // ── Find transform ────────────────────────────────
                // Find transform: scan forward for valid po matrix.
                // Each entity (including conglom members) has a world-space po
                // somewhere after its 7ACE5BAD marker. Scan forward until found.
                float instanceMatrix[16];
                bool hasTransform = false;

                for (size_t scanOfs = acePos + 0x44;
                     scanOfs + 64 <= blockSize && scanOfs < acePos + 0x2000;
                     scanOfs += 4)
                {
                    float candidate[16];
                    memcpy(candidate, &blockData[scanOfs], 64);

                    if (std::fabs(candidate[3]) > 0.01f ||
                        std::fabs(candidate[7]) > 0.01f ||
                        std::fabs(candidate[11]) > 0.01f) continue;
                    if (std::fabs(candidate[15] - 1.0f) > 0.1f) continue;

                    bool rowsOk = true;
                    for (int r = 0; r < 3 && rowsOk; r++) {
                        float len2 = 0;
                        for (int c = 0; c < 3; c++)
                            len2 += candidate[r*4+c] * candidate[r*4+c];
                        float len = std::sqrt(len2);
                        if (len < 0.9f || len > 1.1f) rowsOk = false;
                    }
                    if (!rowsOk) continue;

                    // Skip identity at origin (templates / definitions)
                    if (std::fabs(candidate[12]) < 0.01f &&
                        std::fabs(candidate[13]) < 0.01f &&
                        std::fabs(candidate[14]) < 0.01f)
                        continue;

                    memcpy(instanceMatrix, candidate, 64);
                    hasTransform = true;
                    break;
                }

                if (!hasTransform) {
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

            // ── PASS 2: Placement records for orphan PCMs ────────
            // Some PCMs (bushes, trees, lamps, walls, gates) are placed via
            // stride-0x20 records embedded after the last entity in the 0x0A block.
            // Format per record: pad(4) type(2) angle(2) X(4) Y(4) Z(4) f14(2) f16(2) f18(4) group(4)
            if (blockInfo.tocType == 0x0A) {
                // Find contiguous type=9 records by scanning
                size_t recStart = 0;
                int recCount = 0;
                for (size_t probe = 0; probe + 0x20 <= blockSize; probe += 4) {
                    uint16_t rt;
                    memcpy(&rt, &blockData[probe + 4], 2);
                    if (rt != 9) continue;
                    float px;
                    memcpy(&px, &blockData[probe + 8], 4);
                    if (std::fabs(px) > 10000.0f) continue;

                    // Check next record also type=9 (contiguous block)
                    if (probe + 0x40 <= blockSize) {
                        uint16_t rt2;
                        memcpy(&rt2, &blockData[probe + 0x24], 2);
                        if (rt2 != 9) continue;
                    }

                    // Found start of records - count them
                    recStart = probe;
                    size_t r = probe;
                    while (r + 0x20 <= blockSize) {
                        uint16_t rtt;
                        memcpy(&rtt, &blockData[r + 4], 2);
                        if (rtt != 9) break;
                        float rx;
                        memcpy(&rx, &blockData[r + 8], 4);
                        if (std::fabs(rx) > 10000.0f) break;
                        recCount++;
                        r += 0x20;
                    }
                    break;
                }

                if (recCount > 0) {
                    // f14 is a DIRECT INDEX into the type 0x15 PCM list (sorted by hash)
                    std::vector<uint32_t> pcm15Hashes;
                    {
                        file.clear();
                        file.seekg(tocStart);
                        while (file.good()) {
                            uint32_t th, tt, to2, ts;
                            file.read((char*)&th, 4); file.read((char*)&tt, 4);
                            file.read((char*)&to2, 4); file.read((char*)&ts, 4);
                            if (!file.good() || tt >= 0x1000 || tt == 0) break;
                            if (tt == 0x15) pcm15Hashes.push_back(th);
                        }
                    }
                    int numPcm15 = (int)pcm15Hashes.size();

                    // Runtime mesh ordering differs from TOC hash order

                    // Pre-scan: detect world-space PCMs (vertex bounds > 50 units)
                    std::set<uint32_t> worldSpacePcms;
                    for (int pi = 0; pi < numPcm15; pi++) {
                        uint32_t ph = pcm15Hashes[pi];
                        if (!pcmDataCache.count(ph)) {
                            if (!pcmIndex.count(ph)) continue;
                            const auto& ref = pcmIndex[ph];
                            std::ifstream pcmFile(ref.packPath, std::ios::binary);
                            if (!pcmFile.is_open()) continue;
                            pcmDataCache[ph].resize(ref.size);
                            pcmFile.seekg(ref.absOffset);
                            pcmFile.read((char*)pcmDataCache[ph].data(), ref.size);
                            pcmFile.close();
                        }
                        const auto& pcd = pcmDataCache[ph];
                        if (pcd.size() < 80 || *(uint32_t*)pcd.data() != 0x204D4350) continue;
                        uint32_t num2 = *(uint32_t*)&pcd[8];
                        uint32_t ofs2 = *(uint32_t*)&pcd[12];
                        if (num2 > 500 || ofs2 >= pcd.size()) continue;
                        float mnX=1e30f, mxX=-1e30f, mnZ=1e30f, mxZ=-1e30f;
                        bool chk = false;
                        for (uint32_t ei = 0; ei < num2 && ei < 50; ei++) {
                            uint32_t eoff = ofs2 + ei * 12;
                            if (eoff + 12 > pcd.size()) break;
                            if (*(uint16_t*)&pcd[eoff+2] != 512) continue;
                            uint32_t eofs = *(uint32_t*)&pcd[eoff+4];
                            if (eofs+16 > pcd.size()) continue;
                            uint32_t nSm = *(uint32_t*)&pcd[eofs+8];
                            uint32_t smO = *(uint32_t*)&pcd[eofs+12];
                            if (nSm > 256 || smO >= pcd.size()) continue;
                            for (uint32_t si = 0; si < nSm && si < 4; si++) {
                                uint32_t roff = smO + si*8;
                                if (roff+8 > pcd.size()) break;
                                uint32_t smOff = *(uint32_t*)&pcd[roff+4];
                                if (smOff+80 > pcd.size()) continue;
                                uint32_t vn = *(uint32_t*)&pcd[smOff+56];
                                uint32_t vo = *(uint32_t*)&pcd[smOff+60];
                                uint32_t st = *(uint32_t*)&pcd[smOff+72];
                                if (vn > 100000 || vo >= pcd.size() || st == 0) continue;
                                for (uint32_t vi = 0; vi < std::min(vn,100u); vi++) {
                                    uint32_t voff = vo+vi*st;
                                    if (voff+12 > pcd.size()) break;
                                    float vx = *(float*)&pcd[voff], vz = *(float*)&pcd[voff+8];
                                    mnX=std::min(mnX,vx); mxX=std::max(mxX,vx);
                                    mnZ=std::min(mnZ,vz); mxZ=std::max(mxZ,vz);
                                    chk = true;
                                }
                                break;
                            }
                            break;
                        }
                        if (chk && ((mxX-mnX) > 50.0f || (mxZ-mnZ) > 50.0f))
                            worldSpacePcms.insert(ph);
                    }

                    int placedFromRecords = 0;
                    for (int ri = 0; ri < recCount; ri++) {
                        size_t ro = recStart + ri * 0x20;
                        uint16_t f14;
                        memcpy(&f14, &blockData[ro + 20], 2);

                        int pcmIdx = (int)(f14 & 0xFF); // low byte = mesh index, high byte = variant
                        // f14 in [N, 2N) wraps via 0x33 mirror, skipping zone base
                        if (pcmIdx >= numPcm15 && pcmIdx < 2 * numPcm15)
                            pcmIdx = pcmIdx - numPcm15 + 1; // +1 skips zone base at [0]
                        if (pcmIdx < 0 || pcmIdx >= numPcm15) continue;

                        uint32_t pcmH = pcm15Hashes[pcmIdx];
                        if (zoneBaseHashes.count(pcmH)) continue;
                        if (!pcmIndex.count(pcmH)) continue;

                        std::string pcmName = dictionary.count(pcmH) ? StrToLower(dictionary[pcmH]) : "";
                        if (IsNonRenderableName(pcmName)) continue;
                        {
                            std::string stripped = pcmName;
                            if (stripped.size() > 3 && stripped[2] == '_') stripped = stripped.substr(3);
                            if (stripped.find("col_") == 0) continue;
                        }
                        // World-space PCMs loaded once below, not per-record
                        if (worldSpacePcms.count(pcmH)) continue;

                        if (!pcmDataCache.count(pcmH)) {
                            const auto& ref = pcmIndex[pcmH];
                            std::ifstream pcmFile(ref.packPath, std::ios::binary);
                            if (!pcmFile.is_open()) continue;
                            pcmDataCache[pcmH].resize(ref.size);
                            pcmFile.seekg(ref.absOffset);
                            pcmFile.read((char*)pcmDataCache[pcmH].data(), ref.size);
                            pcmFile.close();
                        }

                        uint16_t angle;
                        memcpy(&angle, &blockData[ro + 6], 2);
                        float x, y, z;
                        memcpy(&x, &blockData[ro + 8], 4);
                        memcpy(&y, &blockData[ro + 12], 4);
                        memcpy(&z, &blockData[ro + 16], 4);

                        float yawRad = angle * 3.14159265f / 180.0f;
                        float cy = std::cos(yawRad), sy = std::sin(yawRad);
                        float mat[16] = {
                            cy,  0, sy, 0,
                            0,   1,  0, 0,
                            -sy, 0, cy, 0,
                            x,   y,  z, 1
                        };
                        float combined[16];
                        MultiplyMatrix4x4(mat, baseTransform, combined);

                        const auto& pRef = pcmIndex[pcmH];
                        AddMeshFromDataWithTransform(pcmDataCache[pcmH], pcmName, nullptr,
                                                     pRef.packPath, pRef.absOffset, combined);
                        placedFromRecords++;
                    }

                    if (placedFromRecords > 0) {
                        Log("  Placement records: " + std::to_string(recCount) + " records, "
                            + std::to_string(placedFromRecords) + " placed");
                    }

                    // Load world-space PCMs once at computed centroid from their placement records
                    for (int pi = 0; pi < numPcm15; pi++) {
                        uint32_t ph = pcm15Hashes[pi];
                        if (zoneBaseHashes.count(ph)) continue;
                        if (!pcmIndex.count(ph)) continue;
                        if (!worldSpacePcms.count(ph)) continue;
                        std::string pn = dictionary.count(ph) ? StrToLower(dictionary[ph]) : "";
                        if (IsNonRenderableName(pn)) continue;
                        {
                            std::string stripped = pn;
                            if (stripped.size() > 3 && stripped[2] == '_') stripped = stripped.substr(3);
                            if (stripped.find("col_") == 0) continue;
                        }
                        // Compute centroid from placement records
                        float cx = 0, cy = 0, cz = 0;
                        int ccount = 0;
                        for (int ri2 = 0; ri2 < recCount; ri2++) {
                            size_t ro2 = recStart + ri2 * 0x20;
                            uint16_t rf14; memcpy(&rf14, &blockData[ro2 + 20], 2);
                            int ridx = (int)(rf14 & 0xFF);
                            if (ridx >= numPcm15 && ridx < 2 * numPcm15) ridx = ridx - numPcm15 + 1;
                            if (ridx < 0 || ridx >= numPcm15) continue;
                            { // use ALL records for zone center
                                float rx, ry, rz;
                                memcpy(&rx, &blockData[ro2 + 8], 4);
                                memcpy(&ry, &blockData[ro2 + 12], 4);
                                memcpy(&rz, &blockData[ro2 + 16], 4);
                                cx += rx; cy += ry; cz += rz;
                                ccount++;
                            }
                        }
                        if (ccount > 0) { cx /= ccount; cz /= ccount; } cy = 0.0f;
                        float mat[16] = {
                            1, 0, 0, 0,
                            0, 1, 0, 0,
                            0, 0, 1, 0,
                            cx, cy, cz, 1
                        };
                        float combined[16];
                        MultiplyMatrix4x4(mat, baseTransform, combined);
                        const auto& pRef = pcmIndex[ph];
                        AddMeshFromDataWithTransform(pcmDataCache[ph], pn, nullptr,
                                                     pRef.packPath, pRef.absOffset, combined);
                    }
                }
            } // end placement records pass
        } // end instanceBlocks loop
        file.close();
    } // end pack loop

    int instanceMeshCount = (int)previewMeshes.size() - zoneGeoCount;
    Log("Instance scan: " + std::to_string(totalInstances) + " records, "
        + std::to_string(loadedInstances) + " props loaded (" + std::to_string(instanceMeshCount) + " meshes), "
        + std::to_string(skippedNoModel) + " unresolved, "
        + std::to_string(skippedNonRenderable) + " non-renderable, "
        + std::to_string(skippedNoTransform) + " no transform");

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

// ═══════════════════════════════════════════════════════════════════════════
//  LoadPackEntities — load all placed props from a single PCPACK
//  Called from LoadPreview when viewing a zone mesh in world mode.
//  Handles: named entity instances (0x0A block) + orphan PCM placement records
// ═══════════════════════════════════════════════════════════════════════════
void SpiderManTool::LoadPackEntities(const std::string& packFilePath, const float* baseTransform) {
    std::ifstream file(packFilePath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) return;

    size_t fileSize = file.tellg();
    if (fileSize < 64) { file.close(); return; }

    file.seekg(24);
    uint32_t headerSize, packDataOffset;
    file.read((char*)&headerSize, 4);
    file.read((char*)&packDataOffset, 4);
    if (!file.good()) { file.close(); return; }

    size_t hdrReadSize = std::min((size_t)500000, fileSize);
    std::vector<uint8_t> tempHeader(hdrReadSize);
    file.seekg(0);
    file.read((char*)tempHeader.data(), hdrReadSize);

    size_t tocStart = FindTocStart(tempHeader, hdrReadSize);
    if (tocStart == 0) { file.close(); return; }

    // ── Read TOC ──
    struct TocEntry { uint32_t hash, type, absOffset, size; };
    std::vector<TocEntry> toc;
    file.clear();
    file.seekg(tocStart);
    while (file.good()) {
        uint32_t h, t, o, s;
        file.read((char*)&h, 4); file.read((char*)&t, 4);
        file.read((char*)&o, 4); file.read((char*)&s, 4);
        if (!file.good() || t >= 0x1000 || t == 0) break;
        toc.push_back({h, t, packDataOffset + o, s});
    }

    // ── Identify zone base hash ──
    // The zone base mesh (e.g., IGC for IG.PCPACK) is the largest type 0x15 entry.
    // Also match by name: stem + "c" (e.g., "igc" for "ig").
    fs::path pp(packFilePath);
    std::string stem = StrToLower(pp.stem().string());
    std::string zoneBaseName = stem + "c";
    uint32_t zoneBaseHash = CalculateCRC32(zoneBaseName);
    // Also find by dictionary name match and by largest size
    uint32_t largestPcmHash = 0;
    uint32_t largestPcmSize = 0;
    for (auto& te : toc) {
        if (te.type == 0x15 && te.size > largestPcmSize) {
            largestPcmSize = te.size;
            largestPcmHash = te.hash;
        }
        if (te.type == 0x15 && dictionary.count(te.hash)) {
            std::string n = StrToLower(dictionary[te.hash]);
            if (n == zoneBaseName) zoneBaseHash = te.hash;
        }
    }

    // ── Build local PCM index ──
    struct LocalPcm { uint32_t hash; uint32_t absOffset; uint32_t size; std::string name; };
    std::vector<LocalPcm> localPcms;
    for (auto& te : toc) {
        if (te.type == 0x15 && te.size > 64 &&
            te.hash != zoneBaseHash && te.hash != largestPcmHash) {
            std::string n = dictionary.count(te.hash) ? StrToLower(dictionary[te.hash]) : "";
            localPcms.push_back({te.hash, te.absOffset, te.size, n});
        }
    }

    if (localPcms.empty()) { file.close(); return; }

    // Textures are resolved automatically by AddMeshFromDataWithTransform
    // via LoadTextureByName when textureResolver is nullptr

    // ── Cache PCM data ──
    std::map<uint32_t, std::vector<uint8_t>> pcmCache;
    for (auto& lp : localPcms) {
        if (lp.absOffset + lp.size > fileSize) continue;
        file.clear(); file.seekg(lp.absOffset);
        pcmCache[lp.hash].resize(lp.size);
        file.read((char*)pcmCache[lp.hash].data(), lp.size);
    }

    // ── Build name→hash lookup ──
    std::map<std::string, uint32_t> nameToHash;
    std::map<uint32_t, PCMModelRef> localPcmIndex;
    for (auto& lp : localPcms) {
        if (!lp.name.empty()) nameToHash[lp.name] = lp.hash;
        PCMModelRef ref;
        ref.packPath = packFilePath;
        ref.absOffset = lp.absOffset;
        ref.size = lp.size;
        localPcmIndex[lp.hash] = ref;
    }

    // ── Load CITY_ARENA PCMs for cross-pack references ──
    {
        fs::path packDir = fs::path(packFilePath).parent_path();
        fs::path caPath;
        for (auto& entry : fs::directory_iterator(packDir)) {
            if (StrToLower(entry.path().filename().string()) == "city_arena.pcpack") {
                caPath = entry.path(); break;
            }
        }
        if (!caPath.empty() && fs::exists(caPath) && caPath != fs::path(packFilePath)) {
            std::ifstream caFile(caPath, std::ios::binary);
            if (caFile.is_open()) {
                caFile.seekg(0, std::ios::end);
                size_t caFileSize = caFile.tellg();
                caFile.seekg(24);
                uint32_t caHS, caDataOff;
                caFile.read((char*)&caHS, 4);
                caFile.read((char*)&caDataOff, 4);

                // Read header to find TOC
                size_t hdrRead = std::min((size_t)caDataOff, std::min(caFileSize, (size_t)0x100000));
                std::vector<uint8_t> caHdr(hdrRead);
                caFile.seekg(0);
                caFile.read((char*)caHdr.data(), hdrRead);

                uint32_t caTocStart = 0;
                for (size_t ci = 0; ci + 8 < hdrRead; ci += 4) {
                    uint32_t v; memcpy(&v, &caHdr[ci], 4);
                    if (v != 0xE3E3E3E3) continue;
                    for (size_t cj = ci+4; cj + 4 < hdrRead; cj++) {
                        uint32_t v2; memcpy(&v2, &caHdr[cj], 4);
                        if (v2 == 0xE3E3E3E3) { caTocStart = cj + 4; break; }
                    }
                    break;
                }

                // Pass 1: collect all type 0x15 TOC entries
                struct CaEntry { uint32_t hash, absOffset, size; };
                std::vector<CaEntry> caEntries;
                if (caTocStart > 0 && caTocStart + 16 <= hdrRead) {
                    size_t cp = caTocStart;
                    while (cp + 16 <= hdrRead) {
                        uint32_t ch, ct, co, cs;
                        memcpy(&ch, &caHdr[cp], 4); memcpy(&ct, &caHdr[cp+4], 4);
                        memcpy(&co, &caHdr[cp+8], 4); memcpy(&cs, &caHdr[cp+12], 4);
                        if (ct >= 0x1000 || ct == 0) break;
                        if (ct == 0x15 && !pcmCache.count(ch)) {
                            caEntries.push_back({ch, caDataOff + co, cs});
                        }
                        cp += 16;
                    }
                }

                // Pass 2: load PCM data
                int caLoaded = 0;
                for (auto& ce : caEntries) {
                    if (ce.absOffset + ce.size > caFileSize) continue;
                    pcmCache[ce.hash].resize(ce.size);
                    caFile.seekg(ce.absOffset);
                    caFile.read((char*)pcmCache[ce.hash].data(), ce.size);
                    std::string cn = dictionary.count(ce.hash) ? StrToLower(dictionary[ce.hash]) : "";
                    if (!cn.empty()) nameToHash[cn] = ce.hash;
                    caLoaded++;
                }
                caFile.close();
                if (caLoaded > 0)
                    Log("  Loaded " + std::to_string(caLoaded) + " PCMs from CITY_ARENA");
            }
        }
    }

    // ── Process 0x0A block: entity instances + orphan placement records ──
    int placedEntities = 0, placedOrphans = 0;

    for (auto& te : toc) {
        if (te.type != 0x0A) continue;
        if (te.absOffset + te.size > fileSize) continue;

        std::vector<uint8_t> blockData(te.size);
        file.clear(); file.seekg(te.absOffset);
        file.read((char*)blockData.data(), te.size);
        if (!file.good()) continue;
        uint32_t blockSize = te.size;

        // Find all 7ACE5BAD markers
        const uint32_t MARKER = 0x7ACE5BAD;
        std::vector<size_t> acePositions;
        for (size_t i = 0; i + 4 <= blockSize; i += 4) {
            uint32_t val; memcpy(&val, &blockData[i], 4);
            if (val == MARKER) acePositions.push_back(i);
        }

        // ── Pass 1: Named entity instances ──
        std::set<uint32_t> matchedPcmHashes;
        int hash0Count = 0, filteredCount = 0, noNameCount = 0;
        for (size_t ai = 0; ai < acePositions.size(); ai++) {
            size_t acePos = acePositions[ai];
            if (acePos + 20 > blockSize) continue;
            uint32_t instanceHash; memcpy(&instanceHash, &blockData[acePos + 16], 4);
            if (instanceHash == 0) { hash0Count++; continue; }
            if (instanceHash == zoneBaseHash) continue;

            std::string instanceName;
            if (dictionary.count(instanceHash))
                instanceName = StrToLower(dictionary[instanceHash]);

            if (instanceName.empty()) {
                noNameCount++;
                continue;
            }
            if (IsNonRenderableName(instanceName)) {
                filteredCount++;
                continue;
            }

            // Resolve to PCM hash using same pipeline as world loader
            uint32_t pcmHash = 0;
            if (pcmCache.count(instanceHash)) pcmHash = instanceHash;

            if (pcmHash == 0 && !instanceName.empty()) {
                std::string s = StripNumericSuffix(instanceName);
                if (s != instanceName) pcmHash = TryResolveName(s, localPcmIndex, nameToHash);
                if (pcmHash == 0) pcmHash = TryResolveName(s, localPcmIndex, nameToHash);
            }
            if (pcmHash == 0 && !instanceName.empty()) {
                std::string np = StripZonePrefix(instanceName);
                std::string b = StripNumericSuffix(np);
                if (b != instanceName) {
                    pcmHash = TryResolveName(b, localPcmIndex, nameToHash);
                    if (pcmHash == 0) pcmHash = TryResolveName(b, localPcmIndex, nameToHash);
                }
            }
            if (pcmHash == 0 && !instanceName.empty()) {
                std::string np = StripZonePrefix(instanceName);
                if (np != instanceName) {
                    pcmHash = TryResolveName(np, localPcmIndex, nameToHash);
                    if (pcmHash == 0) pcmHash = TryResolveName(np, localPcmIndex, nameToHash);
                }
            }
            if (pcmHash == 0 && !instanceName.empty()) {
                std::string np = StripZonePrefix(instanceName);
                const char* mids[] = {"ent_", "col_", "rf_", nullptr};
                for (int mp = 0; mids[mp] && pcmHash == 0; mp++) {
                    size_t ml = strlen(mids[mp]);
                    if (np.size() > ml && np.substr(0, ml) == mids[mp]) {
                        std::string inner = StripNumericSuffix(np.substr(ml));
                        pcmHash = TryResolveName(inner, localPcmIndex, nameToHash);
                        if (pcmHash == 0) pcmHash = TryResolveName(inner, localPcmIndex, nameToHash);
                        if (pcmHash == 0) {
                            inner = StripZonePrefix(inner);
                            inner = StripNumericSuffix(inner);
                            pcmHash = TryResolveName(inner, localPcmIndex, nameToHash);
                            if (pcmHash == 0) pcmHash = TryResolveName(inner, localPcmIndex, nameToHash);
                        }
                    }
                }
            }
            // Zone-prefixed model lookup
            if (pcmHash == 0 && !instanceName.empty()) {
                std::string np = StripZonePrefix(instanceName);
                std::string b = StripNumericSuffix(np);
                for (auto* midP : {"ent_", "col_", "rf_"}) {
                    size_t ml = strlen(midP);
                    if (b.size() > ml && b.substr(0, ml) == midP) {
                        b = StripNumericSuffix(b.substr(ml));
                        b = StripZonePrefix(b);
                        b = StripNumericSuffix(b);
                        break;
                    }
                }
                if (!b.empty() && b != instanceName) {
                    std::string zm = stem + "_" + b;
                    pcmHash = TryResolveName(zm, localPcmIndex, nameToHash);
                    if (pcmHash == 0) pcmHash = TryResolveName(zm, localPcmIndex, nameToHash);
                }
            }

            // Must resolve AND have cached PCM data
            if (pcmHash == 0) {
                if (!instanceName.empty() && !IsNonRenderableName(instanceName))
                    Log("  UNRESOLVED entity: " + instanceName);
                continue;
            }
            if (!pcmCache.count(pcmHash)) {
                Log("  NO DATA for: " + instanceName + " (hash 0x" +
                    ([](uint32_t h){ char buf[16]; snprintf(buf,16,"%08X",h); return std::string(buf); })(pcmHash) + ")");
                continue;
            }
            matchedPcmHashes.insert(pcmHash);

            // Find transform
            float mat[16];
            bool hasTransform = false;
            for (size_t scanOfs = acePos + 0x44;
                 scanOfs + 64 <= blockSize && scanOfs < acePos + 0x2000;
                 scanOfs += 4)
            {
                float c[16]; memcpy(c, &blockData[scanOfs], 64);
                if (std::fabs(c[3]) > 0.01f || std::fabs(c[7]) > 0.01f ||
                    std::fabs(c[11]) > 0.01f) continue;
                if (std::fabs(c[15] - 1.0f) > 0.1f) continue;
                bool ok = true;
                for (int r = 0; r < 3 && ok; r++) {
                    float l2 = c[r*4]*c[r*4]+c[r*4+1]*c[r*4+1]+c[r*4+2]*c[r*4+2];
                    if (std::sqrt(l2) < 0.9f || std::sqrt(l2) > 1.1f) ok = false;
                }
                if (!ok) continue;
                if (std::fabs(c[12]) < 0.01f && std::fabs(c[13]) < 0.01f && std::fabs(c[14]) < 0.01f) continue;
                memcpy(mat, c, 64);
                hasTransform = true;
                break;
            }
            if (!hasTransform) continue;

            float combined[16];
            MultiplyMatrix4x4(mat, baseTransform, combined);
            AddMeshFromDataWithTransform(pcmCache[pcmHash], instanceName, nullptr,
                                         packFilePath, 0, combined);
            placedEntities++;
        }
        Log("  Entities: " + std::to_string(acePositions.size()) + " total, "
            + std::to_string(hash0Count) + " anonymous, "
            + std::to_string(noNameCount) + " unknown hash, "
            + std::to_string(filteredCount) + " filtered, "
            + std::to_string(placedEntities) + " placed");

        // ── Pass 2: Placement records (stride-0x20, type=9) with f14 = PCM index ──
        {
            // Find contiguous type=9 records
            size_t recStart = 0;
            int recCount = 0;
            for (size_t probe = 0; probe + 0x20 <= blockSize; probe += 4) {
                uint16_t rt; memcpy(&rt, &blockData[probe + 4], 2);
                if (rt != 9) continue;
                float px; memcpy(&px, &blockData[probe + 8], 4);
                if (std::fabs(px) > 10000.0f) continue;
                if (probe + 0x40 <= blockSize) {
                    uint16_t rt2; memcpy(&rt2, &blockData[probe + 0x24], 2);
                    if (rt2 != 9) continue;
                }
                recStart = probe;
                size_t r = probe;
                while (r + 0x20 <= blockSize) {
                    uint16_t rtt; memcpy(&rtt, &blockData[r + 4], 2);
                    if (rtt != 9) break;
                    float rx; memcpy(&rx, &blockData[r + 8], 4);
                    if (std::fabs(rx) > 10000.0f) break;
                    recCount++;
                    r += 0x20;
                }
                break;
            }

            if (recCount > 0) {
                // Build sorted type 0x15 hash list for f14 indexing
                std::vector<uint32_t> pcm15Hashes;
                for (auto& t2 : toc) {
                    if (t2.type == 0x15) pcm15Hashes.push_back(t2.hash);
                }
                int numPcm15 = (int)pcm15Hashes.size();

                // Runtime mesh ordering differs from TOC hash order

                Log("  PCM index (" + std::to_string(numPcm15) + " entries):");
                for (int pi = 0; pi < numPcm15; pi++) {
                    std::string pn = dictionary.count(pcm15Hashes[pi]) ? dictionary[pcm15Hashes[pi]] : "???";
                    Log("    [" + std::to_string(pi) + "] " + pn);
                }
                Log("  " + std::to_string(recCount) + " placement records found");

                // Count per-mesh placements
                std::map<int, int> meshPlaceCounts;
                std::map<int, std::string> meshSkipReason;

                // Pre-scan: detect world-space PCMs (vertex bounds > 50 units)
                // These are baked geometry that should load once at identity, not per-record
                std::set<uint32_t> worldSpacePcms;
                for (int pi = 0; pi < numPcm15; pi++) {
                    uint32_t ph = pcm15Hashes[pi];
                    if (!pcmCache.count(ph)) continue;
                    const auto& pcd = pcmCache[ph];
                    if (pcd.size() < 80 || *(uint32_t*)pcd.data() != 0x204D4350) continue;
                    uint32_t num = *(uint32_t*)&pcd[8];
                    uint32_t ofs = *(uint32_t*)&pcd[12];
                    if (num > 500 || ofs >= pcd.size()) continue;
                    float minX=1e30f, maxX=-1e30f, minZ=1e30f, maxZ=-1e30f;
                    bool checked = false;
                    for (uint32_t ei = 0; ei < num && ei < 50; ei++) {
                        uint32_t eoff = ofs + ei * 12;
                        if (eoff + 12 > pcd.size()) break;
                        uint16_t etyp = *(uint16_t*)&pcd[eoff + 2];
                        uint32_t eofs = *(uint32_t*)&pcd[eoff + 4];
                        if (etyp != 512 || eofs + 16 > pcd.size()) continue;
                        uint32_t nSm = *(uint32_t*)&pcd[eofs + 8];
                        uint32_t smO = *(uint32_t*)&pcd[eofs + 12];
                        if (nSm > 256 || smO >= pcd.size()) continue;
                        for (uint32_t si = 0; si < nSm && si < 4; si++) {
                            uint32_t roff = smO + si * 8;
                            if (roff + 8 > pcd.size()) break;
                            uint32_t smOff = *(uint32_t*)&pcd[roff + 4];
                            if (smOff + 80 > pcd.size()) continue;
                            uint32_t vn = *(uint32_t*)&pcd[smOff + 56];
                            uint32_t vo = *(uint32_t*)&pcd[smOff + 60];
                            uint32_t st = *(uint32_t*)&pcd[smOff + 72];
                            if (vn > 100000 || vo >= pcd.size() || st == 0) continue;
                            for (uint32_t vi = 0; vi < std::min(vn, 100u); vi++) {
                                uint32_t voff = vo + vi * st;
                                if (voff + 12 > pcd.size()) break;
                                float vx = *(float*)&pcd[voff], vz = *(float*)&pcd[voff+8];
                                minX = std::min(minX, vx); maxX = std::max(maxX, vx);
                                minZ = std::min(minZ, vz); maxZ = std::max(maxZ, vz);
                                checked = true;
                            }
                            break;
                        }
                        break;
                    }
                    if (checked && ((maxX - minX) > 50.0f || (maxZ - minZ) > 50.0f)) {
                        worldSpacePcms.insert(ph);
                    }
                }

                for (int ri = 0; ri < recCount; ri++) {
                    size_t ro = recStart + ri * 0x20;
                    uint16_t f14; memcpy(&f14, &blockData[ro + 20], 2);

                    int pcmIdx = (int)(f14 & 0xFF); // low byte = mesh index, high byte = variant
                    if (pcmIdx >= numPcm15 && pcmIdx < 2 * numPcm15)
                        pcmIdx = pcmIdx - numPcm15 + 1; // +1 skips zone base at [0]
                    if (pcmIdx < 0 || pcmIdx >= numPcm15) { meshSkipReason[f14] = "out of range"; continue; }

                    uint32_t pcmH = pcm15Hashes[pcmIdx];
                    std::string pcmName = dictionary.count(pcmH) ? StrToLower(dictionary[pcmH]) : "";

                    if (pcmH == zoneBaseHash || pcmH == largestPcmHash) { meshSkipReason[pcmIdx] = "zone base"; continue; }
                    if (!pcmCache.count(pcmH)) { meshSkipReason[pcmIdx] = "no data"; continue; }
                    if (IsNonRenderableName(pcmName)) { meshSkipReason[pcmIdx] = "non-renderable"; continue; }
                    {
                        std::string stripped = pcmName;
                        if (stripped.size() > 3 && stripped[2] == '_') stripped = stripped.substr(3);
                        if (stripped.find("col_") == 0) { meshSkipReason[pcmIdx] = "collision"; continue; }
                    }
                    if (worldSpacePcms.count(pcmH)) { meshSkipReason[pcmIdx] = "world-space (load once)"; continue; }

                    meshPlaceCounts[pcmIdx]++;

                    uint16_t angle; memcpy(&angle, &blockData[ro + 6], 2);
                    float x, y, z;
                    memcpy(&x, &blockData[ro + 8], 4);
                    memcpy(&y, &blockData[ro + 12], 4);
                    memcpy(&z, &blockData[ro + 16], 4);

                    float yawRad = angle * 3.14159265f / 180.0f;
                    float cy = std::cos(yawRad), sy = std::sin(yawRad);
                    float mat[16] = {
                        cy,  0, sy, 0,
                        0,   1,  0, 0,
                        -sy, 0, cy, 0,
                        x,   y,  z, 1
                    };
                    float combined[16];
                    MultiplyMatrix4x4(mat, baseTransform, combined);
                    AddMeshFromDataWithTransform(pcmCache[pcmH], pcmName, nullptr,
                                                 packFilePath, 0, combined);
                    placedOrphans++;
                }

                // Log placement summary
                for (auto& [idx, cnt] : meshPlaceCounts) {
                    std::string pn = dictionary.count(pcm15Hashes[idx]) ? dictionary[pcm15Hashes[idx]] : "???";
                    Log("  PLACED: [" + std::to_string(idx) + "] " + pn + " x" + std::to_string(cnt));
                }
                for (auto& [idx, reason] : meshSkipReason) {
                    if (meshPlaceCounts.count(idx)) continue;
                    std::string pn = (idx < numPcm15 && dictionary.count(pcm15Hashes[idx])) ? dictionary[pcm15Hashes[idx]] : "f14=" + std::to_string(idx);
                    Log("  SKIPPED: " + pn + " (" + reason + ")");
                }

                // Load world-space PCMs once at identity + any unplaced ones
                std::set<uint32_t> placedHashes;
                for (auto& [idx, cnt] : meshPlaceCounts) placedHashes.insert(pcm15Hashes[idx]);
                for (auto& [idx, reason] : meshSkipReason) {
                    if (idx < numPcm15) placedHashes.insert(pcm15Hashes[idx]);
                }
                for (int pi = 0; pi < numPcm15; pi++) {
                    uint32_t ph = pcm15Hashes[pi];
                    if (ph == zoneBaseHash || ph == largestPcmHash) continue;
                    if (!pcmCache.count(ph)) continue;
                    std::string pn = dictionary.count(ph) ? StrToLower(dictionary[ph]) : "";
                    if (IsNonRenderableName(pn)) continue;
                    {
                        std::string stripped = pn;
                        if (stripped.size() > 3 && stripped[2] == '_') stripped = stripped.substr(3);
                        if (stripped.find("col_") == 0) continue;
                    }

                    if (worldSpacePcms.count(ph)) {
                        // Compute centroid of placement records that reference this mesh
                        float cx = 0, cy = 0, cz = 0;
                        int ccount = 0;
                        for (int ri2 = 0; ri2 < recCount; ri2++) {
                            size_t ro2 = recStart + ri2 * 0x20;
                            uint16_t rf14; memcpy(&rf14, &blockData[ro2 + 20], 2);
                            int ridx = (int)(rf14 & 0xFF);
                            if (ridx >= numPcm15 && ridx < 2 * numPcm15) ridx = ridx - numPcm15 + 1;
                            if (ridx < 0 || ridx >= numPcm15) continue;
                            { // use ALL records for zone center
                                float rx, ry, rz;
                                memcpy(&rx, &blockData[ro2 + 8], 4);
                                memcpy(&ry, &blockData[ro2 + 12], 4);
                                memcpy(&rz, &blockData[ro2 + 16], 4);
                                cx += rx; cy += ry; cz += rz;
                                ccount++;
                            }
                        }
                        if (ccount > 0) { cx /= ccount; cz /= ccount; } cy = 0.0f;
                        float mat[16] = {
                            1, 0, 0, 0,
                            0, 1, 0, 0,
                            0, 0, 1, 0,
                            cx, cy, cz, 1
                        };
                        float combined[16];
                        MultiplyMatrix4x4(mat, baseTransform, combined);
                        AddMeshFromDataWithTransform(pcmCache[ph], pn, nullptr,
                                                     packFilePath, 0, combined);
                        placedOrphans++;
                        Log("  LOAD ONCE at (" + std::to_string(cx) + "," + std::to_string(cy) + "," + std::to_string(cz) + "): [" + std::to_string(pi) + "] " + pn);
                    } else if (!placedHashes.count(ph)) {
                        Log("  UNPLACED (skipped): [" + std::to_string(pi) + "] " + pn);
                    }
                }
            }
        }
    }

    file.close();
    Log("Pack entities: " + std::to_string(placedEntities) + " instances, "
        + std::to_string(placedOrphans) + " orphan placements from " + stem);
}

static void DecodeDXT1Block(const uint8_t* src, uint8_t out[4][4][4]) {
    uint16_t c0 = src[0] | (src[1] << 8), c1 = src[2] | (src[3] << 8);
    uint8_t palette[4][4];
    auto unpack565 = [](uint16_t c, uint8_t* r) {
        r[0] = ((c >> 11) & 0x1F) * 255 / 31;
        r[1] = ((c >> 5) & 0x3F) * 255 / 63;
        r[2] = (c & 0x1F) * 255 / 31;
        r[3] = 255;
    };
    unpack565(c0, palette[0]); unpack565(c1, palette[1]);
    if (c0 > c1) {
        for (int i = 0; i < 3; i++) { palette[2][i] = (2*palette[0][i]+palette[1][i])/3; palette[3][i] = (palette[0][i]+2*palette[1][i])/3; }
        palette[2][3] = palette[3][3] = 255;
    } else {
        for (int i = 0; i < 3; i++) palette[2][i] = (palette[0][i]+palette[1][i])/2;
        palette[2][3] = 255; palette[3][0]=palette[3][1]=palette[3][2]=0; palette[3][3]=0;
    }
    uint32_t bits = src[4]|(src[5]<<8)|(src[6]<<16)|(src[7]<<24);
    for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) {
        int idx = (bits >> ((y*4+x)*2)) & 3;
        memcpy(out[y][x], palette[idx], 4);
    }
}

static bool ConvertDDStoPNG(const std::vector<uint8_t>& dds, const fs::path& pngPath) {
    if (dds.size() < 128) return false;
    if (*(uint32_t*)dds.data() != 0x20534444) return false;

    struct DDSHdr { uint32_t sz,fl,h,w,pitch,dep,mip,rsv[11]; struct{uint32_t sz,fl,fourcc,bits,rM,gM,bM,aM;} pf; uint32_t caps[4],rsv2; };
    const DDSHdr* hdr = (const DDSHdr*)(dds.data()+4);
    int w = hdr->w, h = hdr->h;
    if (w < 1 || h < 1 || w > 8192 || h > 8192) return false;
    const uint8_t* px = dds.data() + 128;
    size_t pxSize = dds.size() - 128;

    std::vector<uint8_t> rgba(w * h * 4);

    if (hdr->pf.fl & 0x4) {
        uint32_t fourCC = hdr->pf.fourcc;
        int blockSize = (fourCC == 0x31545844) ? 8 : 16;
        int bw = (w+3)/4, bh = (h+3)/4;
        if ((size_t)bw*bh*blockSize > pxSize) return false;

        for (int by = 0; by < bh; by++) for (int bx = 0; bx < bw; bx++) {
            const uint8_t* block = px + (by*bw+bx)*blockSize;
            uint8_t decoded[4][4][4];

            if (fourCC == 0x31545844) {
                DecodeDXT1Block(block, decoded);
            } else if (fourCC == 0x33545844) {
                uint64_t alpha = 0; memcpy(&alpha, block, 8);
                DecodeDXT1Block(block+8, decoded);
                for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++)
                    decoded[y][x][3] = ((alpha >> ((y*4+x)*4)) & 0xF) * 17;
            } else if (fourCC == 0x35545844) {
                uint8_t a0 = block[0], a1 = block[1];
                uint8_t aPal[8]; aPal[0]=a0; aPal[1]=a1;
                if (a0 > a1) { for (int i=1;i<7;i++) aPal[i+1]=(uint8_t)(((7-i)*a0+i*a1)/7); }
                else { for (int i=1;i<5;i++) aPal[i+1]=(uint8_t)(((5-i)*a0+i*a1)/5); aPal[6]=0; aPal[7]=255; }
                uint64_t aBits = 0; for (int i = 2; i < 8; i++) aBits |= (uint64_t)block[i] << ((i-2)*8);
                DecodeDXT1Block(block+8, decoded);
                for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++)
                    decoded[y][x][3] = aPal[(aBits >> ((y*4+x)*3)) & 7];
            } else return false;

            for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) {
                int px2 = bx*4+x, py = by*4+y;
                if (px2 < w && py < h) memcpy(&rgba[(py*w+px2)*4], decoded[y][x], 4);
            }
        }
    } else if (hdr->pf.fl & 0x40) {
        if (hdr->pf.bits == 32) {
            for (int i = 0; i < w*h && (size_t)i*4+3 < pxSize; i++) {
                rgba[i*4+0]=px[i*4+2]; rgba[i*4+1]=px[i*4+1]; rgba[i*4+2]=px[i*4+0];
                rgba[i*4+3] = (hdr->pf.aM) ? px[i*4+3] : 255;
            }
        } else if (hdr->pf.bits == 24) {
            for (int i = 0; i < w*h && (size_t)i*3+2 < pxSize; i++) {
                rgba[i*4+0]=px[i*3+2]; rgba[i*4+1]=px[i*3+1]; rgba[i*4+2]=px[i*3+0]; rgba[i*4+3]=255;
            }
        } else return false;
    } else if (hdr->pf.fl & 0x20000) {
        if (hdr->pf.bits == 8) {
            for (int i = 0; i < w*h && (size_t)i < pxSize; i++) {
                rgba[i*4+0]=rgba[i*4+1]=rgba[i*4+2]=px[i]; rgba[i*4+3]=255;
            }
        } else return false;
    } else return false;

    return stbi_write_png(pngPath.string().c_str(), w, h, 4, rgba.data(), w*4) != 0;
}

static void WriteGLB(const fs::path& path, const RenderMesh& mesh) {
    std::vector<uint16_t> triangleIndices;
    if (mesh.mode == GL_TRIANGLE_STRIP) {
        for (size_t i = 0; i + 2 < mesh.indices.size(); i++) {
            uint16_t i0 = mesh.indices[i], i1 = mesh.indices[i+1], i2 = mesh.indices[i+2];
            if (i0 == i1 || i1 == i2 || i0 == i2) continue;
            if (i % 2 == 0) { triangleIndices.push_back(i0); triangleIndices.push_back(i1); triangleIndices.push_back(i2); }
            else { triangleIndices.push_back(i0); triangleIndices.push_back(i2); triangleIndices.push_back(i1); }
        }
    } else {
        triangleIndices = mesh.indices;
    }
    if (triangleIndices.empty()) return;

    int vertexCount = (int)mesh.positions.size() / 3;
    float minP[3] = {1e30f,1e30f,1e30f}, maxP[3] = {-1e30f,-1e30f,-1e30f};
    for (int i = 0; i < vertexCount; i++) {
        for (int a = 0; a < 3; a++) {
            float v = mesh.positions[i*3+a];
            if (v < minP[a]) minP[a] = v;
            if (v > maxP[a]) maxP[a] = v;
        }
    }

    std::vector<uint8_t> bin;
    auto align = [&bin]() { while (bin.size() % 4) bin.push_back(0); };
    auto add = [&bin](const void* d, size_t s) -> int {
        int o = (int)bin.size();
        const uint8_t* p = (const uint8_t*)d;
        bin.insert(bin.end(), p, p + s);
        return o;
    };

    align(); int posOff = add(mesh.positions.data(), mesh.positions.size()*4); int posLen = (int)(mesh.positions.size()*4);
    align(); int nrmOff = add(mesh.normals.data(), mesh.normals.size()*4); int nrmLen = (int)(mesh.normals.size()*4);
    align(); int uvOff = add(mesh.uvs.data(), mesh.uvs.size()*4); int uvLen = (int)(mesh.uvs.size()*4);
    align(); int idxOff = add(triangleIndices.data(), triangleIndices.size()*2); int idxLen = (int)(triangleIndices.size()*2);
    align();

    std::string name = mesh.meshName;
    for (char& c : name) if (c == '"' || c == '\\') c = '_';

    std::stringstream j;
    j << "{\"asset\":{\"version\":\"2.0\",\"generator\":\"USM\"},";
    j << "\"bufferViews\":[";
    j << "{\"buffer\":0,\"byteOffset\":" << posOff << ",\"byteLength\":" << posLen << ",\"target\":34962},";
    j << "{\"buffer\":0,\"byteOffset\":" << nrmOff << ",\"byteLength\":" << nrmLen << ",\"target\":34962},";
    j << "{\"buffer\":0,\"byteOffset\":" << uvOff << ",\"byteLength\":" << uvLen << ",\"target\":34962},";
    j << "{\"buffer\":0,\"byteOffset\":" << idxOff << ",\"byteLength\":" << idxLen << ",\"target\":34963}],";
    j << "\"accessors\":[";
    j << "{\"bufferView\":0,\"componentType\":5126,\"count\":" << vertexCount << ",\"type\":\"VEC3\","
      << "\"min\":[" << minP[0] << "," << minP[1] << "," << minP[2] << "],"
      << "\"max\":[" << maxP[0] << "," << maxP[1] << "," << maxP[2] << "]},";
    j << "{\"bufferView\":1,\"componentType\":5126,\"count\":" << vertexCount << ",\"type\":\"VEC3\"},";
    j << "{\"bufferView\":2,\"componentType\":5126,\"count\":" << vertexCount << ",\"type\":\"VEC2\"},";
    j << "{\"bufferView\":3,\"componentType\":5123,\"count\":" << triangleIndices.size() << ",\"type\":\"SCALAR\"}],";
    j << "\"materials\":[{\"name\":\"" << name << "\",\"doubleSided\":true,"
      << "\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.8,0.8,0.8,1],\"metallicFactor\":0,\"roughnessFactor\":1},"
      << "\"alphaMode\":\"" << (mesh.isTranslucent ? "BLEND" : "OPAQUE") << "\"}],";
    j << "\"meshes\":[{\"name\":\"" << name << "\",\"primitives\":[{"
      << "\"attributes\":{\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2},"
      << "\"indices\":3,\"material\":0}]}],";
    j << "\"nodes\":[{\"name\":\"" << name << "\",\"mesh\":0}],";
    j << "\"scenes\":[{\"nodes\":[0]}],\"scene\":0,";
    j << "\"buffers\":[{\"byteLength\":" << bin.size() << "}]}";

    std::string json = j.str();
    while (json.size() % 4) json += " ";

    std::ofstream out(path, std::ios::binary);
    uint32_t magic = 0x46546C67, version = 2;
    uint32_t totalLen = 12 + 8 + (uint32_t)json.size() + 8 + (uint32_t)bin.size();
    out.write((char*)&magic, 4); out.write((char*)&version, 4); out.write((char*)&totalLen, 4);
    uint32_t jLen = (uint32_t)json.size(), jType = 0x4E4F534A;
    out.write((char*)&jLen, 4); out.write((char*)&jType, 4); out.write(json.c_str(), jLen);
    uint32_t bLen = (uint32_t)bin.size(), bType = 0x004E4942;
    out.write((char*)&bLen, 4); out.write((char*)&bType, 4); out.write((char*)bin.data(), bLen);
    out.close();
}

void SpiderManTool::ExtractAllWorldMeshes() {
    if (foundPacks.empty()) { Log("No packs found"); return; }

    fs::path levelsDir = fs::current_path() / "extracted" / "Levels";
    fs::path propsDir = fs::current_path() / "extracted" / "Props";
    fs::create_directories(levelsDir);
    fs::create_directories(propsDir);

    auto savedMeshes = std::move(previewMeshes);
    previewMeshes.clear();

    std::set<uint32_t> exported;
    int levelCount = 0, propCount = 0;

    for (const auto& packPath : foundPacks) {
        std::string stem = packPath.stem().string();
        std::string stemLower = StrToLower(stem);
        bool isCityArena = (stemLower == "city_arena");
        if (stem.length() != 2 && !isCityArena) continue;

        std::ifstream file(packPath, std::ios::binary);
        if (!file.is_open()) continue;

        file.seekg(0, std::ios::end);
        size_t fileSize = file.tellg();
        if (fileSize < 64) { file.close(); continue; }

        file.seekg(24);
        uint32_t headerSize, dataOffset;
        file.read((char*)&headerSize, 4);
        file.read((char*)&dataOffset, 4);

        size_t hdrRead = std::min((size_t)dataOffset, std::min(fileSize, (size_t)0x100000));
        std::vector<uint8_t> hdr(hdrRead);
        file.seekg(0);
        file.read((char*)hdr.data(), hdrRead);

        size_t tocStart = FindTocStart(hdr, hdrRead);
        if (tocStart == 0) { file.close(); continue; }

        struct TocEntry { uint32_t hash, type, absOffset, size; };
        std::vector<TocEntry> toc;
        size_t pos = tocStart;
        while (pos + 16 <= hdrRead) {
            uint32_t h, t, o, s;
            memcpy(&h, &hdr[pos], 4); memcpy(&t, &hdr[pos+4], 4);
            memcpy(&o, &hdr[pos+8], 4); memcpy(&s, &hdr[pos+12], 4);
            if (t >= 0x1000 || t == 0) break;
            toc.push_back({h, t, dataOffset + o, s});
            pos += 16;
        }

        uint32_t zoneBaseHash = 0;
        if (!isCityArena) {
            std::string zoneBaseName = stemLower + "c";
            zoneBaseHash = CalculateCRC32(zoneBaseName);
            uint32_t largestHash = 0, largestSize = 0;
            for (auto& te : toc) {
                if (te.type == 0x15 && te.size > largestSize) {
                    largestSize = te.size; largestHash = te.hash;
                }
            }
            if (largestHash != 0 && largestHash != zoneBaseHash) zoneBaseHash = largestHash;
        }

        for (auto& te : toc) {
            if (te.type != 0x15) continue;
            if (te.size < 64) continue;
            if (exported.count(te.hash)) continue;
            exported.insert(te.hash);

            std::string meshName = dictionary.count(te.hash) ? dictionary[te.hash] : "";
            if (meshName.empty()) {
                char buf[32]; snprintf(buf, 32, "0x%08X", te.hash);
                meshName = buf;
            }

            bool isLevel = (!isCityArena && te.hash == zoneBaseHash);
            fs::path destDir = isLevel ? levelsDir : propsDir;

            if (te.absOffset + te.size > fileSize) continue;
            std::vector<uint8_t> pcmData(te.size);
            file.seekg(te.absOffset);
            file.read((char*)pcmData.data(), te.size);

            previewMeshes.clear();
            AddMeshFromData(pcmData, meshName, nullptr, packPath.string(), te.absOffset);

            for (size_t mi = 0; mi < previewMeshes.size(); mi++) {
                auto& m = previewMeshes[mi];
                if (m.positions.empty() || m.indices.empty()) continue;

                std::string baseName = meshName;
                if (previewMeshes.size() > 1) baseName += "_" + std::to_string(mi);
                for (char& c : baseName) if (c=='/'||c=='\\'||c==':'||c=='*'||c=='?'||c=='"'||c=='<'||c=='>'||c=='|') c = '_';

                WriteGLB(destDir / (baseName + ".glb"), m);

                if (!m.textureName.empty()) {
                    std::string texLower = StrToLower(m.textureName);
                    std::vector<uint8_t> texData;
                    bool hasTex = false;
                    if (globalTextureNameIndex.count(texLower)) {
                        auto& loc = globalTextureNameIndex[texLower];
                        std::ifstream tf(loc.packPath, std::ios::binary);
                        if (tf.is_open()) {
                            tf.seekg(loc.offset); texData.resize(loc.size);
                            tf.read((char*)texData.data(), loc.size); tf.close();
                            hasTex = true;
                        }
                    }
                    if (!hasTex && m.textureHash != 0 && globalTextureIndex.count(m.textureHash)) {
                        auto& loc = globalTextureIndex[m.textureHash];
                        std::ifstream tf(loc.packPath, std::ios::binary);
                        if (tf.is_open()) {
                            tf.seekg(loc.offset); texData.resize(loc.size);
                            tf.read((char*)texData.data(), loc.size); tf.close();
                            hasTex = true;
                        }
                    }
                    if (hasTex) {
                        std::string texBaseName = m.textureName;
                        for (char& c : texBaseName) if (c=='/'||c=='\\'||c==':'||c=='*'||c=='?'||c=='"'||c=='<'||c=='>'||c=='|') c = '_';
                        fs::path ddsPath = destDir / (texBaseName + ".dds");
                        fs::path pngPath = destDir / (texBaseName + ".png");
                        if (!fs::exists(ddsPath)) {
                            std::ofstream dout(ddsPath, std::ios::binary);
                            dout.write((char*)texData.data(), texData.size());
                            dout.close();
                        }
                        if (!fs::exists(pngPath)) {
                            ConvertDDStoPNG(texData, pngPath);
                        }
                    }
                }
            }

            for (auto& m : previewMeshes) {
                if (m.vao) glDeleteVertexArrays(1, &m.vao);
                if (m.vbo) glDeleteBuffers(1, &m.vbo);
                if (m.ebo) glDeleteBuffers(1, &m.ebo);
            }
            previewMeshes.clear();
            if (isLevel) levelCount++; else propCount++;
        }

        file.close();
    }

    previewMeshes = std::move(savedMeshes);
    Log("Extracted " + std::to_string(levelCount) + " levels + " + std::to_string(propCount) + " props");
    ShowNotification("Extracted " + std::to_string(levelCount) + " levels, " + std::to_string(propCount) + " props\n"
        + levelsDir.string() + "\n" + propsDir.string());
}