#include "SpiderManTool.h"
#include <algorithm>
#include <fstream>
#include <cstring>
#include <set>
#include <sstream>
#include <array>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

// â”€â”€ Helper: 4Ã—4 row-major matrix multiply  C = A * B â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
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

// â”€â”€ Helper: strip trailing digits  "EE_STORESIGN03" â†’ "EE_STORESIGN" â”€â”€â”€â”€â”€â”€
static std::string StripNumericSuffix(const std::string& name) {
    size_t end = name.size();
    while (end > 0 && std::isdigit((unsigned char)name[end - 1])) end--;
    if (end == 0) return name;  // all digits â€“ keep as-is
    // Also strip a trailing underscore that preceded the digits (e.g. "name_01" â†’ "name")
    if (end < name.size() && end > 0 && name[end - 1] == '_') end--;
    if (end == 0) return name;
    return name.substr(0, end);
}

// â”€â”€ Helper: strip 2-letter zone prefix  "EE_PARK" â†’ "PARK" â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
static std::string StripZonePrefix(const std::string& name) {
    if (name.size() > 3 && std::isalpha((unsigned char)name[0]) &&
        std::isalpha((unsigned char)name[1]) && name[2] == '_') {
        return name.substr(3);
    }
    return name;
}

// â”€â”€ Helper: expand known instance-name abbreviations to PCM names â”€â”€â”€â”€â”€â”€â”€â”€â”€
//    The game uses shortened names in instance data that don't match the
//    actual PCM model names.  This map covers all known cases.
static std::string ExpandAbbreviations(const std::string& name) {
    std::string result = name;

    // strtlampa â†’ streetlampa,  strtlampb â†’ streetlampb
    if (result.find("strtlamp") != std::string::npos) {
        size_t pos = result.find("strtlamp");
        result.replace(pos, 8, "streetlamp");
    }
    // bilboard â†’ billboard  (typo in some instance names)
    if (result.find("bilboard") != std::string::npos) {
        size_t pos = result.find("bilboard");
        result.replace(pos, 8, "billboard");
    }
    // glsidewalk â†’ sidewalk (doubled prefix artifact)
    if (result.find("glsidewalk") != std::string::npos) {
        size_t pos = result.find("glsidewalk");
        result.replace(pos, 10, "sidewalk");
    }
    // sidewalkcorn â†’ sidewalk_corner (abbreviated form)
    if (result.find("sidewalkcorn") != std::string::npos) {
        size_t pos = result.find("sidewalkcorn");
        result.replace(pos, 12, "sidewalk_corner");
    }
    // sidewalkcorner â†’ sidewalk_corner (missing underscore)
    if (result.find("sidewalkcorner") != std::string::npos) {
        size_t pos = result.find("sidewalkcorner");
        result.replace(pos, 14, "sidewalk_corner");
    }
    // cornerbar_clean â†’ cornerbar (strip _clean suffix)
    if (result.find("cornerbar_clean") != std::string::npos) {
        size_t pos = result.find("cornerbar_clean");
        result.replace(pos, 15, "cornerbar");
    }
    // streetlampaa â†’ streetlampa (typo - double 'a')
    if (result.find("streetlampaa") != std::string::npos) {
        size_t pos = result.find("streetlampaa");
        result.replace(pos, 12, "streetlampa");
    }
    // Strip _n suffix (night variant): smokestacka_n â†’ smokestacka
    if (result.size() > 2 && result.substr(result.size() - 2) == "_n") {
        result = result.substr(0, result.size() - 2);
    }
    // drum_cone instances - these are zone-specific (hi_cone, hj_cone)
    // No universal mapping available
    // con_barrier â†’ barriera (construction barrier maps to barrier PCM)
    if (result == "con_barrier" || result == "ent_con_barrier") {
        result = "barriera";
    }
    // con_dmpstr â†’ dumpstera (construction dumpster)
    if (result == "con_dmpstr" || result == "ent_con_dmpstr" || result == "constr_dmpstr") {
        result = "dumpstera";
    }
    // barrier_plstc â†’ barriera (plastic barrier)
    if (result == "barrier_plstc" || result == "ent_barrier_plstc") {
        result = "barriera";
    }
    // warn_tape â†’ skip (non-renderable marker)
    if (result.find("warn_tape") != std::string::npos) {
        result = "";
    }
    // rf_penthouse* â†’ ref_penthouse* (rf_ is abbreviation for ref_)
    if (result.find("rf_penthouse") != std::string::npos) {
        size_t pos = result.find("rf_penthouse");
        result.replace(pos, 12, "ref_penthouse");
    }
    // forklft â†’ forklift (missing 'i')
    if (result.find("forklft") != std::string::npos) {
        size_t pos = result.find("forklft");
        result.replace(pos, 7, "forklift");
    }
    // stor abbreviations: "storcome" â†’ "stor_come", "storcomb" â†’ "stor_comb"
    if (result.find("stor") == 0 && result.size() > 4 && result[4] != '_') {
        result.insert(4, "_");
    }
    return result;
}

// â”€â”€ Helper: quick check if a 4Ã—4 matrix looks valid (row3[3] â‰ˆ 1.0) â”€â”€â”€â”€â”€â”€
static bool IsValidTransformMatrix(const float* m) {
    if (std::fabs(m[15] - 1.0f) > 0.01f) return false;
    if (std::fabs(m[12]) > 100000.0f || std::fabs(m[13]) > 100000.0f ||
        std::fabs(m[14]) > 100000.0f) return false;
    return true;
}

// â”€â”€ Helper: check if a name looks like a non-renderable instance â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// Delegates to the shared helper. Kept as a thin wrapper so existing call
// sites compile; the call sites no longer early-out -- AddMeshFromData* now
// loads these meshes and marks them RenderMesh::isDebugTransparent so they
// show as a translucent ghost overlay instead of opaque white blocks.
static bool IsNonRenderableName(const std::string& nameLower) {
    return IsNonRenderableMeshName(nameLower);
}

// â”€â”€ Internal: location of a PCM model in a pack file â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
struct PCMModelRef {
    std::string packPath;
    uint32_t absOffset;
    uint32_t size;
};

struct PackMeshFileResource {
    uint32_t hash = 0;
    uint32_t absOffset = 0;
    uint32_t size = 0;
};

struct PackTlResourceLocation {
    uint32_t hash = 0;
    uint32_t type = 0;
    uint32_t absOffset = 0;
    uint32_t size = 0;
};

struct MeshLocationBinding {
    uint32_t pcmHash = 0;
    uint32_t meshHash = 0;
    uint32_t meshOffsetInPcm = 0xFFFFFFFFu;
};

static std::map<std::string, std::map<std::string, int>> gWorldMeshDebugCategories;

struct WorldOriginPlacementDebugKey {
    std::string category;
    std::string meshName;
    std::string sourcePack;
    uint32_t sourceOffset = 0;

    bool operator<(const WorldOriginPlacementDebugKey& other) const {
        if (category != other.category) return category < other.category;
        if (meshName != other.meshName) return meshName < other.meshName;
        if (sourcePack != other.sourcePack) return sourcePack < other.sourcePack;
        return sourceOffset < other.sourceOffset;
    }
};

static std::map<WorldOriginPlacementDebugKey, int> gWorldOriginPlacementDebug;

static const std::vector<std::string>& WorldMeshDebugCategoryOrder() {
    static const std::vector<std::string> categories = {
        "lego",
        "streetlights/entities",
        "zone chunks",
        "interiors",
        "conglomerate",
        "unique pcms"
    };
    return categories;
}

static void ResetWorldMeshDebugCategories() {
    gWorldMeshDebugCategories.clear();
    gWorldOriginPlacementDebug.clear();
    for (const auto& category : WorldMeshDebugCategoryOrder()) {
        gWorldMeshDebugCategories[category];
    }
}

static void RecordWorldMeshDebug(const std::string& category, const std::string& meshName) {
    std::string cleanName = meshName.empty() ? "<unnamed>" : meshName;
    gWorldMeshDebugCategories[category][cleanName]++;
}

static bool IsWorldOriginTransform(const float* transform) {
    if (!transform) return false;
    constexpr float kOriginEpsilon = 0.01f;
    return std::fabs(transform[12]) <= kOriginEpsilon &&
           std::fabs(transform[13]) <= kOriginEpsilon &&
           std::fabs(transform[14]) <= kOriginEpsilon;
}

static void RecordWorldOriginPlacementDebug(const std::string& category,
                                            const std::string& meshName,
                                            const std::string& sourcePack,
                                            uint32_t sourceOffset,
                                            const float* transform) {
    // Direct world-space meshes use an identity transform by design; only
    // placement-driven categories are useful for "why did this spawn at 0,0".
    if (category == "zone chunks" || category == "interiors" || category == "unique pcms") return;
    if (!IsWorldOriginTransform(transform)) return;

    WorldOriginPlacementDebugKey key;
    key.category = category.empty() ? "none" : category;
    key.meshName = meshName.empty() ? "<unnamed>" : meshName;
    key.sourcePack = sourcePack.empty() ? "<unknown pack>" : fs::path(sourcePack).filename().string();
    key.sourceOffset = sourceOffset;
    gWorldOriginPlacementDebug[key]++;
}

static void RecordWorldMeshPlacementDebug(const std::string& category,
                                          const std::string& meshName,
                                          const std::string& sourcePack,
                                          uint32_t sourceOffset,
                                          const float* transform) {
    RecordWorldMeshDebug(category, meshName);
    RecordWorldOriginPlacementDebug(category, meshName, sourcePack, sourceOffset, transform);
}

static void DumpWorldMeshDebugCategories(SpiderManTool& tool) {
    tool.Log("World mesh categories:");
    for (const auto& category : WorldMeshDebugCategoryOrder()) {
        const auto it = gWorldMeshDebugCategories.find(category);
        const auto& meshes = (it != gWorldMeshDebugCategories.end())
            ? it->second
            : gWorldMeshDebugCategories[category];

        tool.Log(category + ":");
        if (meshes.empty()) {
            tool.Log("  (none)");
            continue;
        }

        for (const auto& [meshName, count] : meshes) {
            std::string line = "  " + meshName;
            if (count > 1) line += " x" + std::to_string(count);
            tool.Log(line);
        }
    }
}

static void DumpWorldOriginPlacementDebug(SpiderManTool& tool) {
    tool.Log("World origin placement debug:");
    if (gWorldOriginPlacementDebug.empty()) {
        tool.Log("  (none)");
        return;
    }

    int totalPlacements = 0;
    for (const auto& [key, count] : gWorldOriginPlacementDebug) {
        totalPlacements += count;
    }
    tool.Log("  placements at/near 0,0,0: " + std::to_string(totalPlacements));

    std::string currentCategory;
    for (const auto& [key, count] : gWorldOriginPlacementDebug) {
        if (key.category != currentCategory) {
            currentCategory = key.category;
            tool.Log(currentCategory + ":");
        }

        std::ostringstream line;
        line << "  " << key.meshName;
        if (count > 1) line << " x" << count;
        line << " [" << key.sourcePack << "+0x"
             << std::hex << key.sourceOffset << std::dec << "]";
        tool.Log(line.str());
    }
}

// The 0x20 records after scene data carry world positions, but their tail
// fields also encode grid/cell bounds. Treating f14 as a PCM index makes
// fences/props resolve to unrelated models such as trees.
static constexpr bool kEnableGuessedOrphanPlacementRecords = false;
static constexpr bool kEnableSceneEntityNameFallback = false;

static uint32_t HashString33(const std::string& str) {
    uint32_t result = 0;
    for (unsigned char c : str) {
        if (std::isalpha(c)) c = (unsigned char)std::tolower(c);
        result = c + 33u * result;
    }
    return result;
}

// â”€â”€ Helper: try all name variants to find a PCM match â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
//    Given a base name, tries: as-is, abbreviation-expanded, zone-stripped,
//    zone-stripped+expanded â€” against both the hash index and the name index.
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
        uint32_t h = HashString33(*cand);
        if (pcmIndex.count(h)) return h;
        if (pcmNameToHash.count(*cand)) return pcmNameToHash.at(*cand);
    }
    return 0;
}

// â”€â”€ Internal: TOC helper â€“ find TOC start (after double E3E3E3E3) â”€â”€â”€â”€â”€â”€â”€â”€â”€
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

static uint16_t ReadU16LE(const std::vector<uint8_t>& data, size_t offset) {
    uint16_t v = 0;
    if (offset + sizeof(v) <= data.size()) memcpy(&v, data.data() + offset, sizeof(v));
    return v;
}

static uint32_t ReadU32LE(const std::vector<uint8_t>& data, size_t offset) {
    uint32_t v = 0;
    if (offset + sizeof(v) <= data.size()) memcpy(&v, data.data() + offset, sizeof(v));
    return v;
}

static float ReadF32LE(const std::vector<uint8_t>& data, size_t offset) {
    float v = 0.0f;
    if (offset + sizeof(v) <= data.size()) memcpy(&v, data.data() + offset, sizeof(v));
    return v;
}

static size_t AlignUp(size_t value, size_t alignment) {
    size_t mask = alignment - 1;
    return (value + mask) & ~mask;
}

