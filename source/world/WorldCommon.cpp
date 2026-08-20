#include "SpiderManTool.h"
#include <algorithm>
#include <fstream>
#include <cstring>
#include <set>
#include <sstream>
#include <array>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

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

static std::string StripNumericSuffix(const std::string& name) {
    size_t end = name.size();
    while (end > 0 && std::isdigit((unsigned char)name[end - 1])) end--;
    if (end == 0) return name;

    if (end < name.size() && end > 0 && name[end - 1] == '_') end--;
    if (end == 0) return name;
    return name.substr(0, end);
}

static std::string StripZonePrefix(const std::string& name) {
    if (name.size() > 3 && std::isalpha((unsigned char)name[0]) &&
        std::isalpha((unsigned char)name[1]) && name[2] == '_') {
        return name.substr(3);
    }
    return name;
}

static std::string ExpandAbbreviations(const std::string& name) {
    std::string result = name;

    if (result.find("strtlamp") != std::string::npos) {
        size_t pos = result.find("strtlamp");
        result.replace(pos, 8, "streetlamp");
    }

    if (result.find("bilboard") != std::string::npos) {
        size_t pos = result.find("bilboard");
        result.replace(pos, 8, "billboard");
    }

    if (result.find("glsidewalk") != std::string::npos) {
        size_t pos = result.find("glsidewalk");
        result.replace(pos, 10, "sidewalk");
    }

    if (result.find("sidewalkcorn") != std::string::npos) {
        size_t pos = result.find("sidewalkcorn");
        result.replace(pos, 12, "sidewalk_corner");
    }

    if (result.find("sidewalkcorner") != std::string::npos) {
        size_t pos = result.find("sidewalkcorner");
        result.replace(pos, 14, "sidewalk_corner");
    }

    if (result.find("cornerbar_clean") != std::string::npos) {
        size_t pos = result.find("cornerbar_clean");
        result.replace(pos, 15, "cornerbar");
    }

    if (result.find("streetlampaa") != std::string::npos) {
        size_t pos = result.find("streetlampaa");
        result.replace(pos, 12, "streetlampa");
    }

    if (result.size() > 2 && result.substr(result.size() - 2) == "_n") {
        result = result.substr(0, result.size() - 2);
    }

    if (result == "con_barrier" || result == "ent_con_barrier") {
        result = "barriera";
    }

    if (result == "con_dmpstr" || result == "ent_con_dmpstr" || result == "constr_dmpstr") {
        result = "dumpstera";
    }

    if (result == "barrier_plstc" || result == "ent_barrier_plstc") {
        result = "barriera";
    }

    if (result.find("warn_tape") != std::string::npos) {
        result = "";
    }

    if (result.find("rf_penthouse") != std::string::npos) {
        size_t pos = result.find("rf_penthouse");
        result.replace(pos, 12, "ref_penthouse");
    }

    if (result.find("forklft") != std::string::npos) {
        size_t pos = result.find("forklft");
        result.replace(pos, 7, "forklift");
    }

    if (result.find("stor") == 0 && result.size() > 4 && result[4] != '_') {
        result.insert(4, "_");
    }
    return result;
}

static bool IsValidTransformMatrix(const float* m) {
    if (std::fabs(m[15] - 1.0f) > 0.01f) return false;
    if (std::fabs(m[12]) > 100000.0f || std::fabs(m[13]) > 100000.0f ||
        std::fabs(m[14]) > 100000.0f) return false;
    return true;
}

static bool IsNonRenderableName(const std::string& nameLower) {
    return IsNonRenderableMeshName(nameLower);
}

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

static uint32_t TryResolveName(const std::string& base,
                               const std::map<uint32_t, PCMModelRef>& pcmIndex,
                               const std::map<std::string, uint32_t>& pcmNameToHash) {
    if (base.empty()) return 0;

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
