#include "SpiderManTool.h"
#include "NalIntegration.h"
#include <glad/glad.h>
#include <sstream>
#include <fstream>
#include <cmath>
#include <algorithm>
#include <map>
#include <functional>
#include <cctype>
#include "stb_image_write.h"

static constexpr int PREVIEW_MAX_BONES = 64;

static std::string ReadStringTableEntry(const std::vector<uint8_t>& data, uint32_t offset) {
    if (offset == 0 || offset + 32 > data.size()) return "";
    size_t strStart = offset + 4;
    size_t end = strStart;
    while (end < strStart + 28 && end < data.size() && data[end] != 0) end++;
    return std::string((char*)&data[strStart], end - strStart);
}

static inline void DecodeD3DColor(uint32_t v, float& r, float& g, float& b, float& a) {
    constexpr float k = 1.0f / 255.0f;
    b = (float)((v >>  0) & 0xFF) * k;
    g = (float)((v >>  8) & 0xFF) * k;
    r = (float)((v >> 16) & 0xFF) * k;
    a = (float)((v >> 24) & 0xFF) * k;
}

template <typename VertexT>
static std::vector<uint16_t> ResolveSectionBonePalette(
    std::vector<VertexT>& vertices,
    const std::vector<uint16_t>& filePalette,
    int totalSkeletonBones,
    int maxSlots = 64)
{
    if (vertices.empty()) return filePalette;

    auto rawToGlobal = [&](int raw) -> int {
        if (raw < 0) return -1;
        if (!filePalette.empty()) {
            if (raw < (int)filePalette.size()) return (int)filePalette[raw];
            return -1;
        }
        return raw;
    };

    bool needsSynth = false;
    if (filePalette.empty()) {
        needsSynth = true;
    } else if ((int)filePalette.size() > maxSlots) {
        needsSynth = true;
    } else {
        for (const auto& v : vertices) {
            for (int bi = 0; bi < 4; ++bi) {
                int raw = (int)(v.boneIdx[bi] + 0.5f);
                if (v.boneWgt[bi] <= 0.f) continue;
                if (raw < 0 || raw >= (int)filePalette.size()) {
                    needsSynth = true;
                    break;
                }
            }
            if (needsSynth) break;
        }
    }

    if (!needsSynth) {

        for (auto& v : vertices) {
            for (int bi = 0; bi < 4; ++bi) {
                int raw = (int)(v.boneIdx[bi] + 0.5f);
                if (raw < 0 || raw >= (int)filePalette.size() || v.boneWgt[bi] <= 0.f) {
                    v.boneIdx[bi] = 0.f;
                    v.boneWgt[bi] = 0.f;
                }
            }
        }
        return filePalette;
    }

    std::map<int, int> globalToLocal;
    std::vector<uint16_t> synth;
    synth.reserve(std::min((size_t)maxSlots, vertices.size() * 4));

    for (auto& v : vertices) {
        for (int bi = 0; bi < 4; ++bi) {
            if (v.boneWgt[bi] <= 0.f) {
                v.boneIdx[bi] = 0.f;
                continue;
            }
            int raw = (int)(v.boneIdx[bi] + 0.5f);
            int global = rawToGlobal(raw);
            if (global < 0 ||
                (totalSkeletonBones > 0 && global >= totalSkeletonBones)) {
                v.boneIdx[bi] = 0.f;
                v.boneWgt[bi] = 0.f;
                continue;
            }
            auto it = globalToLocal.find(global);
            int localSlot;
            if (it != globalToLocal.end()) {
                localSlot = it->second;
            } else {
                if ((int)synth.size() >= maxSlots) {

                    v.boneIdx[bi] = 0.f;
                    v.boneWgt[bi] = 0.f;
                    continue;
                }
                localSlot = (int)synth.size();
                globalToLocal[global] = localSlot;
                synth.push_back((uint16_t)global);
            }
            v.boneIdx[bi] = (float)localSlot;
        }

        float wTotal = v.boneWgt[0] + v.boneWgt[1] + v.boneWgt[2] + v.boneWgt[3];
        if (wTotal > 1e-8f) {
            float inv = 1.f / wTotal;
            for (int bi = 0; bi < 4; ++bi) v.boneWgt[bi] *= inv;
        } else {
            for (int bi = 0; bi < 4; ++bi) v.boneWgt[bi] = 0.f;
        }
    }

    return synth;
}

static uint32_t ClassifyByShaderName(const std::string& shaderName) {
    std::string n = shaderName;
    for (auto& c : n) c = (char)tolower((unsigned char)c);
    if (n.empty()) return NGLBM_OPAQUE;
    if (n.find("punchthrough") != std::string::npos)  return NGLBM_PUNCHTHROUGH;
    if (n.find("punch_through") != std::string::npos) return NGLBM_PUNCHTHROUGH;
    if (n.find("alpha_test") != std::string::npos)    return NGLBM_PUNCHTHROUGH;
    if (n.find("additive") != std::string::npos)      return NGLBM_ADDITIVE;
    if (n.find("subtractive") != std::string::npos)   return NGLBM_SUBTRACTIVE;
    if (n.find("translucent") != std::string::npos)   return NGLBM_BLEND;
    if (n.find("transparent") != std::string::npos)   return NGLBM_BLEND;
    if (n.find("glass") != std::string::npos)         return NGLBM_BLEND;
    if (n.find("blend") != std::string::npos)         return NGLBM_BLEND;
    return NGLBM_OPAQUE;
}

static bool IsColorVolumeShader(const std::string& shaderName) {
    std::string n = shaderName;
    for (auto& c : n) c = (char)tolower((unsigned char)c);
    return n.find("colorvol") != std::string::npos ||
           n.find("color_vol") != std::string::npos ||
           n.find("colorvolume") != std::string::npos;
}

static bool IsShadowVolumeShader(const std::string& shaderName) {
    std::string n = shaderName;
    for (auto& c : n) c = (char)tolower((unsigned char)c);

    return n.find("shadowvolume") != std::string::npos ||
           n.find("shadow_volume") != std::string::npos ||
           n.find("shadowvol") != std::string::npos;
}

static uint32_t LocateTextureOffset(uint16_t size) {
    if (size == 48)               return 0x1C;
    if (size == 60)               return 0x1C;
    if (size == 64 || size == 68) return 0x1C;
    if (size == 80 || size == 84 || size == 88) return 0x18;
    if (size >= 116)              return 0x60;
    return 0;
}

void SpiderManTool::ParseMaterialEntries(const std::vector<uint8_t>& pcmData) {
    materialMap.clear();

    BinaryReader br(pcmData);
    if (pcmData.size() < 16) return;

    br.Seek(8);
    uint32_t numEntries = br.Read<uint32_t>();
    uint32_t entryTableOfs = br.Read<uint32_t>();

    if (numEntries > 1000 || entryTableOfs >= pcmData.size()) return;

    br.Seek(entryTableOfs);
    struct EntryInfo { uint16_t size; uint16_t tag; uint32_t dataOffset; uint32_t nameOffset; };
    std::vector<EntryInfo> entries;

    for (uint32_t i = 0; i < numEntries; i++) {
        EntryInfo e;
        e.size = br.Read<uint16_t>();
        e.tag = br.Read<uint16_t>();
        e.dataOffset = br.Read<uint32_t>();
        e.nameOffset = br.Read<uint32_t>();
        entries.push_back(e);
    }

    for (auto& e : entries) {
        if (e.tag != 256) continue;
        if (e.dataOffset + e.size > pcmData.size()) continue;

        br.Seek(e.dataOffset);

        uint32_t meshNameOfs = br.Read<uint32_t>();
        uint32_t alphaFlagOfs = br.Read<uint32_t>();

        br.Seek(e.dataOffset + 0x10);
        uint32_t shaderType = br.Read<uint32_t>();

        uint32_t textureNameOfs = 0;
        uint32_t texOffset = LocateTextureOffset(e.size);
        if (texOffset != 0 && e.dataOffset + texOffset + 4 <= pcmData.size()) {
            br.Seek(e.dataOffset + texOffset);
            textureNameOfs = br.Read<uint32_t>();
        }

        MaterialDef mat;
        mat.meshName = ReadStringTableEntry(pcmData, meshNameOfs);
        mat.alphaFlag = ReadStringTableEntry(pcmData, alphaFlagOfs);
        mat.textureName = ReadStringTableEntry(pcmData, textureNameOfs);
        mat.serializedSize = e.size;
        mat.shaderType = shaderType;

        std::string shaderLower = StrToLower(mat.alphaFlag);
        mat.isPersonMaterial = shaderLower.find("person") != std::string::npos;

        if (e.size == 0x54) mat.isPersonMaterial = true;
        if (mat.isPersonMaterial) {
            if (e.dataOffset + 0x24 <= pcmData.size()) {
                br.Seek(e.dataOffset + 0x20);
                mat.secondaryTextureName = ReadStringTableEntry(pcmData, br.Read<uint32_t>());
            }
            if (e.dataOffset + 0x38 <= pcmData.size()) {
                br.Seek(e.dataOffset + 0x28);
                for (float& channel : mat.personBaseColor) channel = br.Read<float>();
                for (float& channel : mat.personBaseColor) {
                    if (!std::isfinite(channel) || channel < 0.0f || channel > 8.0f) {
                        mat.personBaseColor = {1.0f, 1.0f, 1.0f, 1.0f};
                        break;
                    }
                }
            }
            if (e.dataOffset + 0x4C <= pcmData.size()) {
                br.Seek(e.dataOffset + 0x3C);
                mat.personEnvA = br.Read<uint32_t>();
                mat.personEnvB = br.Read<uint32_t>();
                mat.personLighting = br.Read<uint32_t>();
                mat.personOutline = br.Read<uint32_t>();
            }
        }

        uint32_t blendMode = ClassifyByShaderName(mat.alphaFlag);

        if (mat.isPersonMaterial && e.size >= 0x50 &&
            e.dataOffset + 0x50 <= pcmData.size()) {
            br.Seek(e.dataOffset + 0x4C);
            const uint32_t serializedBlendMode = br.Read<uint32_t>();
            if (serializedBlendMode < NGLBM_MAX_BLEND_MODES_GUARD) {
                blendMode = serializedBlendMode;
            }
        }
        mat.blendMode = blendMode;
        mat.isAlphaTest   = (blendMode == NGLBM_PUNCHTHROUGH);
        mat.isTranslucent = (blendMode >= NGLBM_BLEND);
        mat.isColorVolume  = IsColorVolumeShader(mat.alphaFlag);
        mat.isShadowVolume = IsShadowVolumeShader(mat.alphaFlag);

        if (meshNameOfs != 0) {
            materialMap[meshNameOfs] = mat;
        }
    }
}

MaterialDef SpiderManTool::ResolveMaterialByMeshOffset(uint32_t meshNameOffset) {
    if (materialMap.count(meshNameOffset)) {
        return materialMap[meshNameOffset];
    }
    return MaterialDef();
}

static bool IsColorVolumeMesh(const std::string& meshName) {
    (void)meshName;
    return false;
}

static bool ShouldHideMesh(const std::string& meshName) {
    (void)meshName;
    return false;
}

static void GlbDecodeDXT1Block(const uint8_t* src, uint8_t out[4][4][4]) {
    uint16_t c0 = src[0] | (src[1] << 8);
    uint16_t c1 = src[2] | (src[3] << 8);
    uint8_t palette[4][4];

    auto unpack565 = [](uint16_t c, uint8_t* rgba) {
        rgba[0] = (uint8_t)(((c >> 11) & 0x1F) * 255 / 31);
        rgba[1] = (uint8_t)(((c >> 5) & 0x3F) * 255 / 63);
        rgba[2] = (uint8_t)((c & 0x1F) * 255 / 31);
        rgba[3] = 255;
    };

    unpack565(c0, palette[0]);
    unpack565(c1, palette[1]);
    if (c0 > c1) {
        for (int i = 0; i < 3; ++i) {
            palette[2][i] = (uint8_t)((2 * palette[0][i] + palette[1][i]) / 3);
            palette[3][i] = (uint8_t)((palette[0][i] + 2 * palette[1][i]) / 3);
        }
        palette[2][3] = palette[3][3] = 255;
    } else {
        for (int i = 0; i < 3; ++i) palette[2][i] = (uint8_t)((palette[0][i] + palette[1][i]) / 2);
        palette[2][3] = 255;
        palette[3][0] = palette[3][1] = palette[3][2] = 0;
        palette[3][3] = 0;
    }

    uint32_t bits = src[4] | (src[5] << 8) | (src[6] << 16) | (src[7] << 24);
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            int idx = (bits >> ((y * 4 + x) * 2)) & 3;
            memcpy(out[y][x], palette[idx], 4);
        }
    }
}

static bool GlbDecodeDDSToRGBA(const std::vector<uint8_t>& dds, std::vector<uint8_t>& rgba, int& width, int& height) {
    if (dds.size() < 128) return false;
    uint32_t magic = 0;
    memcpy(&magic, dds.data(), 4);
    if (magic != 0x20534444) return false;

    struct DDSHdr {
        uint32_t sz, fl, h, w, pitch, dep, mip, rsv[11];
        struct { uint32_t sz, fl, fourcc, bits, rM, gM, bM, aM; } pf;
        uint32_t caps[4], rsv2;
    };
    const DDSHdr* hdr = reinterpret_cast<const DDSHdr*>(dds.data() + 4);
    width = (int)hdr->w;
    height = (int)hdr->h;
    if (width < 1 || height < 1 || width > 8192 || height > 8192) return false;

    const uint8_t* px = dds.data() + 128;
    size_t pxSize = dds.size() - 128;
    rgba.assign((size_t)width * (size_t)height * 4, 0);

    if (hdr->pf.fl & 0x4) {
        uint32_t fourCC = hdr->pf.fourcc;
        int blockSize = (fourCC == 0x31545844) ? 8 : 16;
        int bw = (width + 3) / 4;
        int bh = (height + 3) / 4;
        if ((size_t)bw * (size_t)bh * (size_t)blockSize > pxSize) return false;

        for (int by = 0; by < bh; ++by) {
            for (int bx = 0; bx < bw; ++bx) {
                const uint8_t* block = px + ((size_t)by * (size_t)bw + (size_t)bx) * (size_t)blockSize;
                uint8_t decoded[4][4][4];

                if (fourCC == 0x31545844) {
                    GlbDecodeDXT1Block(block, decoded);
                } else if (fourCC == 0x33545844) {
                    uint64_t alpha = 0;
                    memcpy(&alpha, block, 8);
                    GlbDecodeDXT1Block(block + 8, decoded);
                    for (int y = 0; y < 4; ++y) {
                        for (int x = 0; x < 4; ++x) {
                            decoded[y][x][3] = (uint8_t)(((alpha >> ((y * 4 + x) * 4)) & 0xF) * 17);
                        }
                    }
                } else if (fourCC == 0x35545844) {
                    uint8_t a0 = block[0], a1 = block[1];
                    uint8_t aPal[8];
                    aPal[0] = a0;
                    aPal[1] = a1;
                    if (a0 > a1) {
                        for (int i = 1; i < 7; ++i) aPal[i + 1] = (uint8_t)(((7 - i) * a0 + i * a1) / 7);
                    } else {
                        for (int i = 1; i < 5; ++i) aPal[i + 1] = (uint8_t)(((5 - i) * a0 + i * a1) / 5);
                        aPal[6] = 0;
                        aPal[7] = 255;
                    }
                    uint64_t aBits = 0;
                    for (int i = 2; i < 8; ++i) aBits |= (uint64_t)block[i] << ((i - 2) * 8);
                    GlbDecodeDXT1Block(block + 8, decoded);
                    for (int y = 0; y < 4; ++y) {
                        for (int x = 0; x < 4; ++x) {
                            decoded[y][x][3] = aPal[(aBits >> ((y * 4 + x) * 3)) & 7];
                        }
                    }
                } else {
                    return false;
                }

                for (int y = 0; y < 4; ++y) {
                    for (int x = 0; x < 4; ++x) {
                        int dstX = bx * 4 + x;
                        int dstY = by * 4 + y;
                        if (dstX < width && dstY < height) {
                            memcpy(&rgba[((size_t)dstY * (size_t)width + (size_t)dstX) * 4], decoded[y][x], 4);
                        }
                    }
                }
            }
        }
    } else if (hdr->pf.fl & 0x40) {
        if (hdr->pf.bits == 32) {
            for (int i = 0; i < width * height && (size_t)i * 4 + 3 < pxSize; ++i) {
                rgba[i * 4 + 0] = px[i * 4 + 2];
                rgba[i * 4 + 1] = px[i * 4 + 1];
                rgba[i * 4 + 2] = px[i * 4 + 0];
                rgba[i * 4 + 3] = hdr->pf.aM ? px[i * 4 + 3] : 255;
            }
        } else if (hdr->pf.bits == 24) {
            for (int i = 0; i < width * height && (size_t)i * 3 + 2 < pxSize; ++i) {
                rgba[i * 4 + 0] = px[i * 3 + 2];
                rgba[i * 4 + 1] = px[i * 3 + 1];
                rgba[i * 4 + 2] = px[i * 3 + 0];
                rgba[i * 4 + 3] = 255;
            }
        } else {
            return false;
        }
    } else if (hdr->pf.fl & 0x20000) {
        if (hdr->pf.bits == 8) {
            for (int i = 0; i < width * height && (size_t)i < pxSize; ++i) {
                rgba[i * 4 + 0] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = px[i];
                rgba[i * 4 + 3] = 255;
            }
        } else if (hdr->pf.bits == 16) {
            for (int i = 0; i < width * height && (size_t)i * 2 + 1 < pxSize; ++i) {
                rgba[i * 4 + 0] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = px[i * 2];
                rgba[i * 4 + 3] = px[i * 2 + 1];
            }
        } else {
            return false;
        }
    } else {
        return false;
    }

    return true;
}

static void GlbPNGWriteCallback(void* context, void* data, int size) {
    if (!context || !data || size <= 0) return;
    auto* out = reinterpret_cast<std::vector<uint8_t>*>(context);
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(data);
    out->insert(out->end(), bytes, bytes + size);
}

static bool GlbEncodePNG(const std::vector<uint8_t>& rgba, int width, int height, std::vector<uint8_t>& png) {
    if (rgba.empty() || width <= 0 || height <= 0) return false;
    png.clear();
    return stbi_write_png_to_func(GlbPNGWriteCallback, &png, width, height, 4, rgba.data(), width * 4) != 0 && !png.empty();
}

static bool GlbRGBAHasAlpha(const std::vector<uint8_t>& rgba) {
    for (size_t i = 3; i < rgba.size(); i += 4) {
        if (rgba[i] < 250) return true;
    }
    return false;
}

static bool GlbRGBAFullyTransparent(const std::vector<uint8_t>& rgba) {
    if (rgba.empty()) return false;
    for (size_t i = 3; i < rgba.size(); i += 4) {
        if (rgba[i] >= 8) return false;
    }
    return true;
}

static std::string GlbSanitizeTextureKey(std::string name) {
    name = StrToLower(name);
    size_t slash = name.find_last_of("/\\");
    if (slash != std::string::npos) name = name.substr(slash + 1);

    const char* extensions[] = { ".dds", ".tga", ".png", ".bmp", ".dat" };
    bool stripped = true;
    while (stripped) {
        stripped = false;
        for (const char* ext : extensions) {
            size_t extLen = strlen(ext);
            if (name.size() > extLen && name.substr(name.size() - extLen) == ext) {
                name.resize(name.size() - extLen);
                stripped = true;
                break;
            }
        }
    }
    return name;
}

static std::vector<std::string> GlbExtractTextureListNames(const std::vector<uint8_t>& data) {
    std::string text;
    text.reserve(data.size());
    size_t printable = 0;
    for (uint8_t byte : data) {
        if (byte == 0) continue;
        if (byte == '\r' || byte == '\n' || byte == '\t' || byte == ' ') {
            text.push_back(' ');
            printable++;
        } else if (byte >= 32 && byte < 127) {
            text.push_back((char)byte);
            printable++;
        }
    }
    if (data.empty() || printable * 2 < data.size()) return {};

    std::vector<std::string> names;
    std::stringstream ss(text);
    std::string token;
    while (ss >> token) {
        while (!token.empty() && ispunct((unsigned char)token.back()) && token.back() != '.') {
            token.pop_back();
        }

        std::string lower = StrToLower(token);
        if (lower.find(".dds") == std::string::npos &&
            lower.find(".tga") == std::string::npos &&
            lower.find(".png") == std::string::npos &&
            lower.find(".bmp") == std::string::npos) {
            continue;
        }

        std::string key = GlbSanitizeTextureKey(lower);
        if (key.empty()) continue;
        if (std::find(names.begin(), names.end(), key) == names.end()) {
            names.push_back(key);
        }
    }
    return names;
}

struct GlbExportQuat {
    float w = 1.0f;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

static void GlbMat4Identity(float* m) {
    memset(m, 0, sizeof(float) * 16);
    m[0] = 1.0f;
    m[5] = 1.0f;
    m[10] = 1.0f;
    m[15] = 1.0f;
}

static void GlbMat4Multiply(const float* a, const float* b, float* out) {
    float r[16];
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            r[col * 4 + row] =
                a[0 * 4 + row] * b[col * 4 + 0] +
                a[1 * 4 + row] * b[col * 4 + 1] +
                a[2 * 4 + row] * b[col * 4 + 2] +
                a[3 * 4 + row] * b[col * 4 + 3];
        }
    }
    memcpy(out, r, sizeof(r));
}

static GlbExportQuat GlbQuatNormalize(GlbExportQuat q) {
    float len2 = q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z;
    if (len2 <= 1e-20f) return {};
    float inv = 1.0f / sqrtf(len2);
    q.w *= inv;
    q.x *= inv;
    q.y *= inv;
    q.z *= inv;
    return q;
}

static GlbExportQuat GlbQuatFromXYZ(float x, float y, float z) {
    float w2 = 1.0f - (x * x + y * y + z * z);
    return GlbQuatNormalize({sqrtf(fabsf(w2)), x, y, z});
}

static GlbExportQuat GlbQuatFromNal(const NalQuat& q) {
    return GlbQuatNormalize({q.w, q.x, q.y, q.z});
}

static GlbExportQuat GlbQuatInverse(GlbExportQuat q) {
    q = GlbQuatNormalize(q);
    return {q.w, -q.x, -q.y, -q.z};
}

static GlbExportQuat GlbQuatMul(GlbExportQuat a, GlbExportQuat b) {
    a = GlbQuatNormalize(a);
    b = GlbQuatNormalize(b);
    return GlbQuatNormalize({
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w
    });
}

static GlbExportQuat GlbQuatAxisAngle(int axis, float angle) {
    float half = angle * 0.5f;
    float s = sinf(half);
    float c = cosf(half);
    if (axis == 0) return GlbQuatNormalize({c, s, 0.0f, 0.0f});
    if (axis == 1) return GlbQuatNormalize({c, 0.0f, s, 0.0f});
    return GlbQuatNormalize({c, 0.0f, 0.0f, s});
}

static GlbExportQuat GlbQuatFromYZAngles(float yAngle, float zAngle) {
    return GlbQuatMul(GlbQuatAxisAngle(1, yAngle), GlbQuatAxisAngle(2, zAngle));
}

static float GlbFingerTipHingeAngle(float angle, bool isThumb) {
    if (!isThumb) return angle;
    float tip = 2.0f * angle;
    float halfPi = 1.57079632679f;
    if (tip > halfPi) return halfPi;
    if (tip < -halfPi) return -halfPi;
    return tip;
}

static void GlbVec3Sub(const float* a, const float* b, float* out) {
    out[0] = a[0] - b[0];
    out[1] = a[1] - b[1];
    out[2] = a[2] - b[2];
}

static void GlbVec3Scale(const float* a, float s, float* out) {
    out[0] = a[0] * s;
    out[1] = a[1] * s;
    out[2] = a[2] * s;
}

static float GlbVec3Len(const float* v) {
    return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

static void GlbVec3Normalize(float* v) {
    float len = GlbVec3Len(v);
    if (len <= 1e-12f) {
        v[0] = v[1] = v[2] = 0.0f;
        return;
    }
    float inv = 1.0f / len;
    v[0] *= inv;
    v[1] *= inv;
    v[2] *= inv;
}

static float GlbVec3Dot(const float* a, const float* b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void GlbVec3Cross(const float* a, const float* b, float* out) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

static void GlbMat4ComposeBasis(const float* xAxis, const float* yAxis, const float* zAxis,
                                const float* pos, float* out) {
    GlbMat4Identity(out);
    out[0] = xAxis[0]; out[1] = xAxis[1]; out[2] = xAxis[2];
    out[4] = yAxis[0]; out[5] = yAxis[1]; out[6] = yAxis[2];
    out[8] = zAxis[0]; out[9] = zAxis[1]; out[10] = zAxis[2];
    out[12] = pos[0]; out[13] = pos[1]; out[14] = pos[2];
}

static void GlbMat4AxisVec(const float* m, int axis, float* out) {
    const int base = axis * 4;
    out[0] = m[base + 0];
    out[1] = m[base + 1];
    out[2] = m[base + 2];
}

static void GlbMat4ProjectPointOntoXform(const float* point, const float* xform, float* out) {
    out[0] = xform[12] + point[0] * xform[0] + point[1] * xform[4] + point[2] * xform[8];
    out[1] = xform[13] + point[0] * xform[1] + point[1] * xform[5] + point[2] * xform[9];
    out[2] = xform[14] + point[0] * xform[2] + point[1] * xform[6] + point[2] * xform[10];
}

static void GlbMat4BuildTwistLineXform(const std::array<float, 3>& pos, float angle, float* m) {
    float s = sinf(angle);
    float c = cosf(angle);
    GlbMat4Identity(m);

    m[5] = c;
    m[6] = -s;
    m[9] = s;
    m[10] = c;
    m[12] = pos[0];
    m[13] = pos[1];
    m[14] = pos[2];
}

static float GlbWrapPi(float angle) {
    constexpr float kPi = 3.14159265358979323846f;
    constexpr float kTau = 2.0f * kPi;
    while (angle > kPi) angle -= kTau;
    while (angle < -kPi) angle += kTau;
    return angle;
}

static float GlbExtractForearmTwist(GlbExportQuat handQuat, bool left) {

    handQuat = GlbQuatNormalize(handQuat);
    float axial = 2.0f * atan2f(handQuat.x, handQuat.w);
    float reference = left ? -1.57079632679f : 1.57079632679f;
    return GlbWrapPi(axial - reference);
}

static void GlbMat4Fing52HingeLocal(float angle, const std::array<float, 3>& pos, float* m) {
    float s = sinf(angle);
    float c = cosf(angle);
    GlbMat4Identity(m);
    m[0] = c;
    m[2] = s;
    m[8] = -s;
    m[10] = c;
    m[12] = pos[0];
    m[13] = pos[1];
    m[14] = pos[2];
}

static void GlbRemapNalIkRows(float* m) {
    float x[3] = {m[0], m[1], m[2]};
    float y[3] = {m[4], m[5], m[6]};
    float z[3] = {m[8], m[9], m[10]};
    m[0] = -x[0]; m[1] = -x[1]; m[2] = -x[2];
    m[4] = -z[0]; m[5] = -z[1]; m[6] = -z[2];
    m[8] = -y[0]; m[9] = -y[1]; m[10] = -y[2];
}

static void GlbMat4FromNalQuat(GlbExportQuat q, float* m) {
    q = GlbQuatNormalize(q);
    GlbMat4Identity(m);

    float x = -q.x;
    float y = -q.y;
    float z = -q.z;
    float xx = x * x;
    float yy = y * y;
    float zz = z * z;
    float xy = x * y;
    float xz = x * z;
    float yz = y * z;
    float wx = q.w * x;
    float wy = q.w * y;
    float wz = q.w * z;

    m[0]  = 1.0f - 2.0f * (yy + zz);
    m[1]  = 2.0f * (xy + wz);
    m[2]  = 2.0f * (xz - wy);
    m[4]  = 2.0f * (xy - wz);
    m[5]  = 1.0f - 2.0f * (xx + zz);
    m[6]  = 2.0f * (yz + wx);
    m[8]  = 2.0f * (xz + wy);
    m[9]  = 2.0f * (yz - wx);
    m[10] = 1.0f - 2.0f * (xx + yy);
}

static void GlbSolveNalIk(const float* baseModelMatrix,
                          const std::array<float, 3>& baseJoint,
                          const float* targetModelMatrix,
                          const NalIKData& ikData,
                          float spinAngle,
                          bool armPlaneCallback,
                          bool mirrorArm,
                          float* upperOut,
                          float* lowerOut) {
    float baseJointArr[3] = {baseJoint[0], baseJoint[1], baseJoint[2]};
    float modelBaseJoint[3];
    GlbMat4ProjectPointOntoXform(baseJointArr, baseModelMatrix, modelBaseJoint);

    float targetPos[3] = {targetModelMatrix[12], targetModelMatrix[13], targetModelMatrix[14]};
    float targetDir[3];
    GlbVec3Sub(targetPos, modelBaseJoint, targetDir);
    float dist = GlbVec3Len(targetDir);
    if (dist <= 1e-12f) dist = 1e-12f;
    GlbVec3Scale(targetDir, 1.0f / dist, targetDir);

    float cosUpper = dist * ikData.fUpperIKc + (1.0f / dist) * ikData.fUpperIKInvc;
    float cosLower = dist * ikData.fLowerIKc + (1.0f / dist) * ikData.fLowerIKInvc;
    cosUpper = std::max(-1.0f, std::min(1.0f, cosUpper));
    cosLower = std::max(-1.0f, std::min(1.0f, cosLower));
    float sinUpper = sqrtf(std::max(0.0f, 1.0f - cosUpper * cosUpper));
    float sinLower = sqrtf(std::max(0.0f, 1.0f - cosLower * cosLower));

    float midDir[3];
    if (!armPlaneCallback) {
        float targetY[3];
        GlbMat4AxisVec(targetModelMatrix, 1, targetY);
        GlbVec3Cross(targetDir, targetY, midDir);
    } else {

        float row0[3], row1[3], row2[3];
        GlbMat4AxisVec(baseModelMatrix, 0, row0);
        GlbMat4AxisVec(baseModelMatrix, 1, row1);
        GlbMat4AxisVec(baseModelMatrix, 2, row2);
        if (mirrorArm) {
            row1[0] = -row1[0]; row1[1] = -row1[1]; row1[2] = -row1[2];
            GlbVec3Cross(row1, targetDir, midDir);
        } else {
            GlbVec3Cross(targetDir, row1, midDir);
        }
        float sign = GlbVec3Dot(targetDir, row1);
        float adjusted[3];
        if (sign < 0.0f) {
            float negSum[3] = {-row0[0] - row2[0], -row0[1] - row2[1], -row0[2] - row2[2]};
            adjusted[0] = midDir[0] * (sign + 1.0f) + negSum[0] * (-sign);
            adjusted[1] = midDir[1] * (sign + 1.0f) + negSum[1] * (-sign);
            adjusted[2] = midDir[2] * (sign + 1.0f) + negSum[2] * (-sign);
        } else {
            float diff[3] = {row2[0] - row0[0], row2[1] - row0[1], row2[2] - row0[2]};
            adjusted[0] = midDir[0] * (1.0f - sign) + diff[0] * sign;
            adjusted[1] = midDir[1] * (1.0f - sign) + diff[1] * sign;
            adjusted[2] = midDir[2] * (1.0f - sign) + diff[2] * sign;
        }
        memcpy(midDir, adjusted, sizeof(adjusted));
    }

    float zAxis[3], yAxis[3], xAxis[3] = {targetDir[0], targetDir[1], targetDir[2]};
    GlbVec3Cross(targetDir, midDir, zAxis);
    GlbVec3Normalize(zAxis);
    GlbVec3Cross(zAxis, targetDir, yAxis);
    float basis[16];
    GlbMat4ComposeBasis(xAxis, yAxis, zAxis, modelBaseJoint, basis);

    float sinTwist = sinf(spinAngle);
    float cosTwist = cosf(spinAngle);
    float zero[3] = {0.0f, 0.0f, 0.0f};
    float upperX[3] = {cosUpper, sinUpper * cosTwist, sinUpper * sinTwist};
    float upperY[3] = {-sinUpper, cosUpper * cosTwist, cosUpper * sinTwist};
    float upperZ[3] = {0.0f, -sinTwist, cosTwist};
    float upperLocal[16];
    GlbMat4ComposeBasis(upperX, upperY, upperZ, zero, upperLocal);
    GlbMat4Multiply(basis, upperLocal, upperOut);

    float lowerPos[3] = {
        ikData.fUpperArmLength * cosUpper,
        cosTwist * (ikData.fUpperArmLength * sinUpper),
        sinTwist * (ikData.fUpperArmLength * sinUpper)
    };
    float lowerX[3] = {cosLower, -(sinLower * cosTwist), -(sinLower * sinTwist)};
    float lowerY[3] = {sinLower, cosLower * cosTwist, cosLower * sinTwist};
    float lowerZ[3] = {0.0f, -sinTwist, cosTwist};
    float lowerLocal[16];
    GlbMat4ComposeBasis(lowerX, lowerY, lowerZ, lowerPos, lowerLocal);
    GlbMat4Multiply(basis, lowerLocal, lowerOut);

    GlbRemapNalIkRows(upperOut);
    GlbRemapNalIkRows(lowerOut);
}

static GlbExportQuat GlbQuatFromMat4Internal(const float* m, bool nalConjugate) {
    float m00 = m[0],  m01 = m[4],  m02 = m[8];
    float m10 = m[1],  m11 = m[5],  m12 = m[9];
    float m20 = m[2],  m21 = m[6],  m22 = m[10];

    GlbExportQuat q;
    float trace = m00 + m11 + m22;
    if (trace > 0.0f) {
        float s = sqrtf(trace + 1.0f) * 2.0f;
        q.w = 0.25f * s;
        q.x = (m21 - m12) / s;
        q.y = (m02 - m20) / s;
        q.z = (m10 - m01) / s;
    } else if (m00 > m11 && m00 > m22) {
        float s = sqrtf(std::max(1e-20f, 1.0f + m00 - m11 - m22)) * 2.0f;
        q.w = (m21 - m12) / s;
        q.x = 0.25f * s;
        q.y = (m01 + m10) / s;
        q.z = (m02 + m20) / s;
    } else if (m11 > m22) {
        float s = sqrtf(std::max(1e-20f, 1.0f + m11 - m00 - m22)) * 2.0f;
        q.w = (m02 - m20) / s;
        q.x = (m01 + m10) / s;
        q.y = 0.25f * s;
        q.z = (m12 + m21) / s;
    } else {
        float s = sqrtf(std::max(1e-20f, 1.0f + m22 - m00 - m11)) * 2.0f;
        q.w = (m10 - m01) / s;
        q.x = (m02 + m20) / s;
        q.y = (m12 + m21) / s;
        q.z = 0.25f * s;
    }

    if (nalConjugate) {
        q.x = -q.x;
        q.y = -q.y;
        q.z = -q.z;
    }
    return GlbQuatNormalize(q);
}

static GlbExportQuat GlbQuatFromMat4Nal(const float* m) {
    return GlbQuatFromMat4Internal(m, true);
}

static GlbExportQuat GlbQuatFromMat4Gltf(const float* m) {
    return GlbQuatFromMat4Internal(m, false);
}

static void GlbMatrixToTRS(const float* m, float translation[3], float rotation[4]) {
    translation[0] = m[12];
    translation[1] = m[13];
    translation[2] = m[14];
    GlbExportQuat q = GlbQuatFromMat4Gltf(m);
    rotation[0] = q.x;
    rotation[1] = q.y;
    rotation[2] = q.z;
    rotation[3] = q.w;
}

static bool GlbGetDefaultQuat(const NalComponentData* comp, int index, GlbExportQuat& out) {
    if (!comp || index < 0 || index >= (int)comp->default_pose.quats.size()) return false;
    out = GlbQuatFromNal(comp->default_pose.quats[index]);
    return true;
}

static int GlbParentOf(const NalSkeletonData* skel, int idx, int boneCount) {
    if (!skel) return -1;
    auto it = skel->parent_map.find(idx);
    if (it == skel->parent_map.end()) return -1;
    int parent = it->second;
    return (parent >= 0 && parent < boneCount && parent != idx) ? parent : -1;
}

static int GlbFindRootBone(const NalSkeletonData* skel, int boneCount) {
    for (int i = 0; i < boneCount; ++i) {
        if (GlbParentOf(skel, i, boneCount) < 0) return i;
    }
    return boneCount > 0 ? 0 : -1;
}

static void GlbBuildRestLocalMatrices(
    const std::vector<std::array<float, 16>>& bindMatrices,
    const NalSkeletonData* skel,
    std::vector<std::array<float, 16>>& restLocal,
    std::vector<GlbExportQuat>& restLocalQuatNal)
{
    int boneCount = (int)bindMatrices.size();
    restLocal.resize(boneCount);
    restLocalQuatNal.resize(boneCount);

    for (int i = 0; i < boneCount; ++i) {
        int parent = GlbParentOf(skel, i, boneCount);
        if (parent >= 0) {
            float invParent[16];
            if (InvertMatrix(bindMatrices[parent].data(), invParent)) {
                GlbMat4Multiply(invParent, bindMatrices[i].data(), restLocal[i].data());
            } else {
                memcpy(restLocal[i].data(), bindMatrices[i].data(), sizeof(float) * 16);
            }
        } else {
            memcpy(restLocal[i].data(), bindMatrices[i].data(), sizeof(float) * 16);
        }
        restLocalQuatNal[i] = GlbQuatFromMat4Nal(restLocal[i].data());
    }
}

struct GlbBonePoseDelta {
    GlbExportQuat rot;
    bool hasRot = false;
    std::array<float, 3> translationDelta = {0.0f, 0.0f, 0.0f};
    bool hasTranslationDelta = false;
    std::array<float, 3> translationAbs = {0.0f, 0.0f, 0.0f};
    bool hasTranslationAbs = false;
};

static void GlbBuildAnimationLocalMatricesForFrame(
    const NalAnimEntry& anim,
    const NalSkeletonData& skel,
    const std::vector<std::array<float, 16>>& restLocal,
    const std::vector<GlbExportQuat>& restLocalQuatNal,
    int frame,
    std::vector<std::array<float, 16>>& outLocal)
{
    int boneCount = (int)restLocal.size();
    outLocal = restLocal;
    if (boneCount <= 0) return;

    std::vector<GlbBonePoseDelta> poseDeltas(boneCount);
    int rootBone = GlbFindRootBone(&skel, boneCount);

    auto findComponentForAnim = [&](const NalAnimComponent& comp) -> const NalComponentData* {
        if (comp.slot_ix >= 0 && comp.slot_ix < (int)skel.components.size()) {
            const auto& bySlot = skel.components[comp.slot_ix];
            if (nal_type_to_comp_id(bySlot.type_id) == comp.comp_ix) return &bySlot;
        }
        for (const auto& sc : skel.components) {
            if (nal_type_to_comp_id(sc.type_id) == comp.comp_ix) return &sc;
        }
        return nullptr;
    };

    auto setBoneDeltaQuat = [&](int boneIdx, GlbExportQuat delta) {
        if (boneIdx < 0 || boneIdx >= boneCount) return;
        auto& dst = poseDeltas[boneIdx];
        dst.rot = dst.hasRot ? GlbQuatMul(dst.rot, delta) : GlbQuatNormalize(delta);
        dst.hasRot = true;
    };

    auto setAbsoluteTrackQuat = [&](int boneIdx, GlbExportQuat q, bool hasDefault, GlbExportQuat defaultQ) {
        if (boneIdx < 0 || boneIdx >= boneCount) return;
        if (boneIdx < (int)restLocalQuatNal.size()) {
            defaultQ = restLocalQuatNal[boneIdx];
            hasDefault = true;
        }
        GlbExportQuat delta = hasDefault ? GlbQuatMul(GlbQuatInverse(defaultQ), q) : q;
        setBoneDeltaQuat(boneIdx, delta);
    };

    auto setTrackXYZ = [&](int boneIdx, float x, float y, float z, const NalComponentData* sc, int defaultIdx) {
        GlbExportQuat defaultQ;
        bool hasDefault = GlbGetDefaultQuat(sc, defaultIdx, defaultQ);
        setAbsoluteTrackQuat(boneIdx, GlbQuatFromXYZ(x, y, z), hasDefault, defaultQ);
    };

    auto addTranslationDelta = [&](int boneIdx, float x, float y, float z) {
        if (boneIdx < 0 || boneIdx >= boneCount) return;
        auto& dst = poseDeltas[boneIdx];
        dst.translationDelta[0] += x;
        dst.translationDelta[1] += y;
        dst.translationDelta[2] += z;
        dst.hasTranslationDelta = true;
    };

    auto setTranslationAbs = [&](int boneIdx, const std::array<float, 3>& pos) {
        if (boneIdx < 0 || boneIdx >= boneCount) return;
        auto& dst = poseDeltas[boneIdx];
        dst.translationAbs = pos;
        dst.hasTranslationAbs = true;
    };

    auto consumeXYZ = [](const std::vector<float>& values, int& cursor, float out[3]) -> bool {
        if (cursor + 2 >= (int)values.size()) return false;
        out[0] = values[cursor++];
        out[1] = values[cursor++];
        out[2] = values[cursor++];
        return true;
    };

    auto defaultQuatOrIdentity = [&](const NalComponentData* compData, int index) -> GlbExportQuat {
        GlbExportQuat q;
        return GlbGetDefaultQuat(compData, index, q) ? q : GlbExportQuat{};
    };

    bool animHasStdLegs = false;
    bool animHasStdArms = false;
    for (const auto& comp : anim.components) {
        if (!comp.decoded.frames.empty()) {
            animHasStdLegs = animHasStdLegs || comp.comp_ix == NalComp::LEGS;
            animHasStdArms = animHasStdArms || comp.comp_ix == NalComp::ARMS;
        }
    }

    std::vector<const NalAnimComponent*> sortedComponents;
    sortedComponents.reserve(anim.components.size());
    for (const auto& comp : anim.components) sortedComponents.push_back(&comp);
    std::sort(sortedComponents.begin(), sortedComponents.end(),
        [](const NalAnimComponent* a, const NalAnimComponent* b) {
            int as = (a->slot_ix >= 0) ? a->slot_ix : (1 << 30);
            int bs = (b->slot_ix >= 0) ? b->slot_ix : (1 << 30);
            if (as != bs) return as < bs;
            return a->comp_ix < b->comp_ix;
        });

    for (const NalAnimComponent* compPtr : sortedComponents) {
        const NalAnimComponent& comp = *compPtr;
        if (frame < 0 || frame >= (int)comp.decoded.frames.size()) continue;
        const auto& fv = comp.decoded.frames[frame];
        const NalComponentData* sc = findComponentForAnim(comp);
        if (!sc) continue;
        if (comp.comp_ix == NalComp::LEGS_IK && animHasStdLegs) continue;
        if (comp.comp_ix == NalComp::ARMS_IK && animHasStdArms) continue;

        int cursor = 0;
        if (comp.comp_ix == NalComp::TORSO_HEAD || comp.comp_ix == NalComp::TORSO_HEAD_STD) {
            if (sc->bone_indices.size() < TorsoBone::COUNT) continue;
            int roles[] = {TorsoBone::SPINE, TorsoBone::SPINE1, TorsoBone::SPINE2, TorsoBone::NECK, TorsoBone::HEAD};
            for (int bit = 0; bit < 5; ++bit) {
                if ((comp.mask & (1u << bit)) == 0) continue;
                float xyz[3];
                if (!consumeXYZ(fv, cursor, xyz)) break;
                setTrackXYZ(sc->bone_indices[roles[bit]], xyz[0], xyz[1], xyz[2], sc, bit);
            }
            if (comp.mask & 0x20) {
                float xyz[3];
                if (consumeXYZ(fv, cursor, xyz)) {
                    setTrackXYZ(sc->bone_indices[TorsoBone::PELVIS], xyz[0], xyz[1], xyz[2], sc, 5);
                }
                float loc[3];
                if (consumeXYZ(fv, cursor, loc)) {
                    addTranslationDelta(sc->bone_indices[TorsoBone::PELVIS],
                        loc[0] - sc->default_pose.pelvis_pos[0],
                        loc[1] - sc->default_pose.pelvis_pos[1],
                        loc[2] - sc->default_pose.pelvis_pos[2]);
                }
            }
        }
        else if (comp.comp_ix == NalComp::LEGS) {
            if (sc->bone_indices.size() < LegStdBone::COUNT) continue;
            for (int bit = 0; bit < 8; ++bit) {
                if ((comp.mask & (1u << bit)) == 0) continue;
                float xyz[3];
                if (!consumeXYZ(fv, cursor, xyz)) break;
                setTrackXYZ(sc->bone_indices[bit], xyz[0], xyz[1], xyz[2], sc, bit);
            }
        }
        else if (comp.comp_ix == NalComp::ARMS) {
            if (sc->bone_indices.size() < ArmBone::COUNT) continue;
            for (int bit = 0; bit < 8; ++bit) {
                if ((comp.mask & (1u << bit)) == 0) continue;
                float xyz[3];
                if (!consumeXYZ(fv, cursor, xyz)) break;
                setTrackXYZ(sc->bone_indices[bit], xyz[0], xyz[1], xyz[2], sc, bit);
            }
        }
        else if (comp.comp_ix == NalComp::LEGS_IK) {
            if (sc->bone_indices.size() < LegBone::COUNT) continue;
            for (int bit = 0; bit < 2; ++bit) {
                if ((comp.mask & (1u << bit)) == 0) continue;
                float xyz[3];
                if (!consumeXYZ(fv, cursor, xyz)) break;
                setTrackXYZ(sc->bone_indices[bit], xyz[0], xyz[1], xyz[2], sc, bit);
            }
            for (int bit = 2; bit < 4; ++bit) {
                if ((comp.mask & (1u << bit)) == 0) continue;
                float xyz[3];
                if (!consumeXYZ(fv, cursor, xyz)) break;
                setTrackXYZ(sc->bone_indices[bit], xyz[0], xyz[1], xyz[2], sc, bit);
                cursor += 4;
            }
        }
        else if (comp.comp_ix == NalComp::ARMS_IK) {
            if (sc->bone_indices.size() < ArmIKBone::COUNT) continue;
            for (int bit = 0; bit < 2; ++bit) {
                if ((comp.mask & (1u << bit)) == 0) continue;
                float xyz[3];
                if (!consumeXYZ(fv, cursor, xyz)) break;
                setTrackXYZ(sc->bone_indices[bit], xyz[0], xyz[1], xyz[2], sc, bit);
            }
            for (int bit = 2; bit < 4; ++bit) {
                if ((comp.mask & (1u << bit)) == 0) continue;
                float xyz[3];
                if (!consumeXYZ(fv, cursor, xyz)) break;
                int side = bit - 2;
                GlbExportQuat defaultQ = side < (int)sc->default_pose.hand_quats.size()
                    ? GlbQuatFromNal(sc->default_pose.hand_quats[side])
                    : GlbExportQuat{};
                setAbsoluteTrackQuat(sc->bone_indices[bit], GlbQuatFromXYZ(xyz[0], xyz[1], xyz[2]), true, defaultQ);
                cursor += 4;
            }
        }
        else if (comp.comp_ix == NalComp::FAKEROOT_STD) {
            if (rootBone < 0) continue;
            if (comp.mask & 0x1) {
                float qxyz[3] = {0.0f, 0.0f, 0.0f};
                if (cursor + 2 < (int)fv.size()) {
                    qxyz[0] = fv[cursor++];
                    qxyz[1] = fv[cursor++];
                    qxyz[2] = fv[cursor++];
                    setBoneDeltaQuat(rootBone, GlbQuatFromXYZ(qxyz[0], qxyz[1], qxyz[2]));
                }
                if (cursor + 2 < (int)fv.size()) {
                    float dx = fv[cursor++] - sc->default_pose.pelvis_pos[0];
                    float dy = fv[cursor++] - sc->default_pose.pelvis_pos[1];
                    float dz = fv[cursor++] - sc->default_pose.pelvis_pos[2];
                    addTranslationDelta(rootBone, dx, dy, dz);
                }
            }
        }
        else if (comp.comp_ix == NalComp::ARBITRARY_PO) {
            int quatCount = std::max(0, sc->default_pose.quat_count);
            int posCount = std::max(0, sc->default_pose.position_count);
            int channelCount = quatCount + posCount;
            std::vector<GlbExportQuat> arbStateQuats(quatCount, GlbExportQuat{});
            std::vector<std::array<float, 3>> arbStatePositions(posCount, {0.0f, 0.0f, 0.0f});
            for (int i = 0; i < quatCount && i < (int)sc->default_pose.quats.size(); ++i)
                arbStateQuats[i] = GlbQuatFromNal(sc->default_pose.quats[i]);
            for (int i = 0; i < posCount && i < (int)sc->default_pose.positions.size(); ++i)
                arbStatePositions[i] = sc->default_pose.positions[i];

            for (int channel = 0; channel < channelCount; ++channel) {
                if (!comp.channel_enabled(channel)) continue;
                float xyz[3];
                if (!consumeXYZ(fv, cursor, xyz)) break;
                if (channel < quatCount) arbStateQuats[channel] = GlbQuatFromXYZ(xyz[0], xyz[1], xyz[2]);
                else arbStatePositions[channel - quatCount] = {xyz[0], xyz[1], xyz[2]};
            }

            for (const auto& node : sc->arb_nodes) {
                int bid = node.my_matrix_ix;
                if (bid < 0 || bid >= boneCount) continue;
                if (node.is_quat_anim && node.quat_ix < arbStateQuats.size()) {
                    setAbsoluteTrackQuat(bid, arbStateQuats[node.quat_ix], false, GlbExportQuat{});
                } else if (node.quat_ix < sc->arb_skel_quats.size()) {
                    setAbsoluteTrackQuat(bid, GlbQuatFromNal(sc->arb_skel_quats[node.quat_ix]), false, GlbExportQuat{});
                }
                if (node.is_pos_anim && node.pos_ix < arbStatePositions.size()) {
                    setTranslationAbs(bid, arbStatePositions[node.pos_ix]);
                } else if (node.pos_ix < sc->arb_skel_positions.size()) {
                    setTranslationAbs(bid, sc->arb_skel_positions[node.pos_ix]);
                }
            }
        }
        else if (comp.comp_ix == NalComp::FING5) {
            if (sc->bone_indices.size() < 30) continue;
            for (int bit = 0; bit < 30; ++bit) {
                if ((comp.mask & (1u << bit)) == 0) continue;
                float xyz[3];
                if (!consumeXYZ(fv, cursor, xyz)) break;
                setTrackXYZ(sc->bone_indices[bit], xyz[0], xyz[1], xyz[2], sc, bit);
            }
        }
        else if (comp.comp_ix == NalComp::FING5_REDUCED) {
            if (sc->bone_indices.size() < 30) continue;
            float baseY[8] = {};
            float baseZ[8] = {};
            float midHinge[10] = {};
            float tipHinge[10] = {};
            float defaultBaseY[8] = {};
            float defaultBaseZ[8] = {};
            float defaultMidHinge[10] = {};
            float defaultTipHinge[10] = {};
            for (int i = 0; i < 8 && i < (int)sc->default_pose.base_y_tracks.size(); ++i)
                baseY[i] = defaultBaseY[i] = sc->default_pose.base_y_tracks[i];
            for (int i = 0; i < 8 && i < (int)sc->default_pose.base_z_tracks.size(); ++i)
                baseZ[i] = defaultBaseZ[i] = sc->default_pose.base_z_tracks[i];
            for (int i = 0; i < 10 && i < (int)sc->default_pose.hinge_tracks.size(); ++i)
                midHinge[i] = defaultMidHinge[i] = sc->default_pose.hinge_tracks[i];
            for (int i = 0; i < 10 && i < (int)sc->default_pose.other_tracks.size(); ++i)
                tipHinge[i] = defaultTipHinge[i] = sc->default_pose.other_tracks[i];
            GlbExportQuat thumb[2] = {defaultQuatOrIdentity(sc, 0), defaultQuatOrIdentity(sc, 1)};

            for (int bit = 0; bit < 30; ++bit) {
                if ((comp.mask & (1u << bit)) == 0) continue;
                if (bit < 2) {
                    float xyz[3];
                    if (!consumeXYZ(fv, cursor, xyz)) break;
                    thumb[bit] = GlbQuatFromXYZ(xyz[0], xyz[1], xyz[2]);
                } else if (bit < 10) {
                    if (cursor + 1 >= (int)fv.size()) break;
                    baseZ[bit - 2] = fv[cursor++];
                    baseY[bit - 2] = fv[cursor++];
                } else if (bit < 20) {
                    if (cursor >= (int)fv.size()) break;
                    midHinge[bit - 10] = fv[cursor++];
                } else {
                    if (cursor >= (int)fv.size()) break;
                    tipHinge[bit - 20] = fv[cursor++];
                }
            }

            for (int base = 0; base < 10; ++base) {
                if (base < 2) {
                    GlbExportQuat def;
                    bool hasDef = GlbGetDefaultQuat(sc, base, def);
                    setAbsoluteTrackQuat(sc->bone_indices[base], thumb[base], hasDef, def);
                } else {
                    setAbsoluteTrackQuat(sc->bone_indices[base],
                        GlbQuatFromYZAngles(baseY[base - 2], baseZ[base - 2]), true,
                        GlbQuatFromYZAngles(defaultBaseY[base - 2], defaultBaseZ[base - 2]));
                }
                setBoneDeltaQuat(sc->bone_indices[10 + base],
                    GlbQuatAxisAngle(0, midHinge[base] - defaultMidHinge[base]));
                setBoneDeltaQuat(sc->bone_indices[20 + base],
                    GlbQuatAxisAngle(0, tipHinge[base] - defaultTipHinge[base]));
            }
        }
        else if (comp.comp_ix == NalComp::FING5_CURL) {
            if (sc->bone_indices.size() < 30) continue;
            float baseZ[8] = {};
            float hinge[10] = {};
            float defaultHinge[10] = {};
            for (int i = 0; i < 10 && i < (int)sc->default_pose.finger_curl.size(); ++i)
                hinge[i] = defaultHinge[i] = sc->default_pose.finger_curl[i];
            GlbExportQuat thumb[2] = {defaultQuatOrIdentity(sc, 0), defaultQuatOrIdentity(sc, 1)};

            for (int bit = 0; bit < 10; ++bit) {
                if ((comp.mask & (1u << bit)) == 0) continue;
                if (bit < 2) {
                    float xyz[3];
                    if (!consumeXYZ(fv, cursor, xyz)) break;
                    thumb[bit] = GlbQuatFromXYZ(xyz[0], xyz[1], xyz[2]);
                    if (cursor < (int)fv.size()) hinge[bit] = fv[cursor++];
                } else {
                    if (cursor + 1 >= (int)fv.size()) break;
                    baseZ[bit - 2] = fv[cursor++];
                    hinge[bit] = fv[cursor++];
                }
            }

            for (int base = 0; base < 10; ++base) {
                if (base < 2) {
                    GlbExportQuat def;
                    bool hasDef = GlbGetDefaultQuat(sc, base, def);
                    setAbsoluteTrackQuat(sc->bone_indices[base], thumb[base], hasDef, def);
                } else {
                    GlbExportQuat def = GlbQuatFromYZAngles(defaultHinge[base], 0.0f);
                    setAbsoluteTrackQuat(sc->bone_indices[base], GlbQuatFromYZAngles(hinge[base], baseZ[base - 2]), true, def);
                }
                setBoneDeltaQuat(sc->bone_indices[10 + base], GlbQuatAxisAngle(0, hinge[base] - defaultHinge[base]));
                setBoneDeltaQuat(sc->bone_indices[20 + base], GlbQuatAxisAngle(0,
                    GlbFingerTipHingeAngle(hinge[base], base < 2) - GlbFingerTipHingeAngle(defaultHinge[base], base < 2)));
            }
        }
        else if (comp.comp_ix == NalComp::FING52) {
            if (sc->bone_indices.size() < 30) continue;
            GlbExportQuat thumb[2] = {defaultQuatOrIdentity(sc, 0), defaultQuatOrIdentity(sc, 1)};
            float baseY[8] = {};
            float baseZ[8] = {};
            float hinge[10] = {};
            for (int i = 0; i < 8 && i < (int)sc->default_pose.base_y_tracks.size(); ++i)
                baseY[i] = sc->default_pose.base_y_tracks[i];
            for (int i = 0; i < 8 && i < (int)sc->default_pose.base_z_tracks.size(); ++i)
                baseZ[i] = sc->default_pose.base_z_tracks[i];
            for (int i = 0; i < 10 && i < (int)sc->default_pose.hinge_tracks.size(); ++i)
                hinge[i] = sc->default_pose.hinge_tracks[i];

            for (int bit = 0; bit < 20; ++bit) {
                if ((comp.mask & (1u << bit)) == 0) continue;
                if (bit < 2) {
                    float xyz[3];
                    if (!consumeXYZ(fv, cursor, xyz)) break;
                    thumb[bit] = GlbQuatFromXYZ(xyz[0], xyz[1], xyz[2]);
                } else if (bit < 10) {
                    if (cursor + 1 >= (int)fv.size()) break;
                    baseY[bit - 2] = fv[cursor++];
                    baseZ[bit - 2] = fv[cursor++];
                } else {
                    if (cursor >= (int)fv.size()) break;
                    hinge[bit - 10] = fv[cursor++];
                }
            }

            for (int base = 0; base < 10; ++base) {
                if (base < 2) {
                    GlbExportQuat def;
                    bool hasDef = GlbGetDefaultQuat(sc, base, def);
                    setAbsoluteTrackQuat(sc->bone_indices[base], thumb[base], hasDef, def);
                } else {
                    GlbExportQuat def = GlbQuatFromYZAngles(
                        (base - 2 < (int)sc->default_pose.base_y_tracks.size()) ? sc->default_pose.base_y_tracks[base - 2] : 0.0f,
                        (base - 2 < (int)sc->default_pose.base_z_tracks.size()) ? sc->default_pose.base_z_tracks[base - 2] : 0.0f);
                    setAbsoluteTrackQuat(sc->bone_indices[base], GlbQuatFromYZAngles(baseY[base - 2], baseZ[base - 2]), true, def);
                }
                float defHinge = (base < (int)sc->default_pose.hinge_tracks.size()) ? sc->default_pose.hinge_tracks[base] : 0.0f;
                setBoneDeltaQuat(sc->bone_indices[10 + base], GlbQuatAxisAngle(0, hinge[base] - defHinge));
                setBoneDeltaQuat(sc->bone_indices[20 + base], GlbQuatAxisAngle(0,
                    GlbFingerTipHingeAngle(hinge[base], base < 2) - GlbFingerTipHingeAngle(defHinge, base < 2)));
            }
        }
    }

    for (int i = 0; i < boneCount; ++i) {
        if (poseDeltas[i].hasRot) {
            float rot[16];
            GlbMat4FromNalQuat(poseDeltas[i].rot, rot);
            GlbMat4Multiply(restLocal[i].data(), rot, outLocal[i].data());
        }
        if (poseDeltas[i].hasTranslationAbs) {
            outLocal[i][12] = poseDeltas[i].translationAbs[0];
            outLocal[i][13] = poseDeltas[i].translationAbs[1];
            outLocal[i][14] = poseDeltas[i].translationAbs[2];
        }
        if (poseDeltas[i].hasTranslationDelta) {
            outLocal[i][12] += poseDeltas[i].translationDelta[0];
            outLocal[i][13] += poseDeltas[i].translationDelta[1];
            outLocal[i][14] += poseDeltas[i].translationDelta[2];
        }
    }
}

static void GlbBuildAnimationWorldMatricesForFrame(
    const NalAnimEntry& anim,
    const NalSkeletonData& skel,
    const std::vector<std::array<float, 16>>& bindMatrices,
    const std::vector<std::array<float, 16>>& restLocal,
    const std::vector<GlbExportQuat>& restLocalQuatNal,
    int frame,
    std::vector<std::array<float, 16>>& outWorld)
{
    int boneCount = (int)bindMatrices.size();
    outWorld.resize(boneCount);
    if (boneCount <= 0) return;

    std::vector<GlbBonePoseDelta> poseDeltas(boneCount);
    std::vector<std::array<float, 16>> animLocal(boneCount);
    for (int i = 0; i < boneCount; ++i) {
        GlbMat4Identity(animLocal[i].data());
        memcpy(outWorld[i].data(), bindMatrices[i].data(), sizeof(float) * 16);
    }

    float rootOffset[3] = {0.0f, 0.0f, 0.0f};
    float rootRotMat[16];
    GlbMat4Identity(rootRotMat);
    bool hasRootRot = false;
    std::map<int, std::array<float, 16>> runtimeWorld;

    auto parentOf = [&](int idx) -> int {
        return GlbParentOf(&skel, idx, boneCount);
    };

    auto findComponentForAnim = [&](const NalAnimComponent& comp) -> const NalComponentData* {
        if (comp.slot_ix >= 0 && comp.slot_ix < (int)skel.components.size()) {
            const auto& bySlot = skel.components[comp.slot_ix];
            if (nal_type_to_comp_id(bySlot.type_id) == comp.comp_ix) return &bySlot;
        }
        for (const auto& sc : skel.components) {
            if (nal_type_to_comp_id(sc.type_id) == comp.comp_ix) return &sc;
        }
        return nullptr;
    };

    auto setBoneDeltaQuat = [&](int boneIdx, GlbExportQuat delta) {
        if (boneIdx < 0 || boneIdx >= boneCount) return;
        auto& dst = poseDeltas[boneIdx];
        dst.rot = dst.hasRot ? GlbQuatMul(dst.rot, delta) : GlbQuatNormalize(delta);
        dst.hasRot = true;
    };

    auto setAbsoluteTrackQuat = [&](int boneIdx, GlbExportQuat q, bool hasDefault, GlbExportQuat defaultQ) {
        if (boneIdx < 0 || boneIdx >= boneCount) return;
        if (boneIdx < (int)restLocalQuatNal.size()) {
            defaultQ = restLocalQuatNal[boneIdx];
            hasDefault = true;
        }
        GlbExportQuat delta = hasDefault ? GlbQuatMul(GlbQuatInverse(defaultQ), q) : q;
        setBoneDeltaQuat(boneIdx, delta);
    };

    auto setTrackXYZ = [&](int boneIdx, float x, float y, float z, const NalComponentData* sc, int defaultIdx) {
        GlbExportQuat defaultQ;
        bool hasDefault = GlbGetDefaultQuat(sc, defaultIdx, defaultQ);
        setAbsoluteTrackQuat(boneIdx, GlbQuatFromXYZ(x, y, z), hasDefault, defaultQ);
    };

    auto consumeXYZ = [](const std::vector<float>& values, int& cursor, float out[3]) -> bool {
        if (cursor + 2 >= (int)values.size()) return false;
        out[0] = values[cursor++];
        out[1] = values[cursor++];
        out[2] = values[cursor++];
        return true;
    };

    auto defaultQuatOrIdentity = [&](const NalComponentData* compData, int index) -> GlbExportQuat {
        GlbExportQuat q;
        return GlbGetDefaultQuat(compData, index, q) ? q : GlbExportQuat{};
    };

    auto matFromQuatPos = [&](GlbExportQuat q, const std::array<float, 3>& pos, float* out) {
        GlbMat4FromNalQuat(q, out);
        out[12] = pos[0];
        out[13] = pos[1];
        out[14] = pos[2];
    };

    auto storeRuntimeWorld = [&](int boneIdx, const float* m) {
        if (boneIdx < 0 || boneIdx >= boneCount) return;
        if (runtimeWorld.count(boneIdx)) return;
        memcpy(runtimeWorld[boneIdx].data(), m, sizeof(float) * 16);
    };

    auto getRuntimeWorld = [&](int boneIdx, float* out) -> bool {
        auto it = runtimeWorld.find(boneIdx);
        if (it == runtimeWorld.end()) return false;
        memcpy(out, it->second.data(), sizeof(float) * 16);
        return true;
    };

    auto localToRuntimeParent = [&](const float* local, int parentBoneIdx, float* out) {
        float parent[16];
        if (getRuntimeWorld(parentBoneIdx, parent)) {
            GlbMat4Multiply(parent, local, out);
        } else if (parentBoneIdx >= 0 && parentBoneIdx < boneCount) {
            GlbMat4Multiply(bindMatrices[parentBoneIdx].data(), local, out);
        } else {
            memcpy(out, local, sizeof(float) * 16);
        }
    };

    bool animHasStdLegs = false;
    bool animHasStdArms = false;
    for (const auto& comp : anim.components) {
        if (!comp.decoded.frames.empty()) {
            animHasStdLegs = animHasStdLegs || comp.comp_ix == NalComp::LEGS;
            animHasStdArms = animHasStdArms || comp.comp_ix == NalComp::ARMS;
        }
    }
    bool solveLegsIk = !animHasStdLegs;
    bool solveArmsIk = !animHasStdArms;

    std::vector<const NalAnimComponent*> sortedComponents;
    sortedComponents.reserve(anim.components.size());
    for (const auto& comp : anim.components) sortedComponents.push_back(&comp);
    std::sort(sortedComponents.begin(), sortedComponents.end(),
        [](const NalAnimComponent* a, const NalAnimComponent* b) {
            int as = (a->slot_ix >= 0) ? a->slot_ix : (1 << 30);
            int bs = (b->slot_ix >= 0) ? b->slot_ix : (1 << 30);
            if (as != bs) return as < bs;
            return a->comp_ix < b->comp_ix;
        });

    for (const NalAnimComponent* compPtr : sortedComponents) {
        const NalAnimComponent& comp = *compPtr;
        if (frame < 0 || frame >= (int)comp.decoded.frames.size()) continue;
        const auto& fv = comp.decoded.frames[frame];
        const NalComponentData* sc = findComponentForAnim(comp);
        if (!sc) continue;
        if (comp.comp_ix == NalComp::LEGS_IK && animHasStdLegs) continue;
        if (comp.comp_ix == NalComp::ARMS_IK && animHasStdArms) continue;

        int cursor = 0;
        if (comp.comp_ix == NalComp::TORSO_HEAD || comp.comp_ix == NalComp::TORSO_HEAD_STD) {
            if (sc->bone_indices.size() < TorsoBone::COUNT) continue;
            int roles[] = {TorsoBone::SPINE, TorsoBone::SPINE1, TorsoBone::SPINE2, TorsoBone::NECK, TorsoBone::HEAD};
            GlbExportQuat torsoState[6];
            for (int i = 0; i < 6; ++i) torsoState[i] = defaultQuatOrIdentity(sc, i);
            std::array<float, 3> pelvisPos = sc->default_pose.pelvis_pos;

            for (int bit = 0; bit < 5; ++bit) {
                if ((comp.mask & (1u << bit)) == 0) continue;
                float xyz[3];
                if (!consumeXYZ(fv, cursor, xyz)) break;
                torsoState[bit] = GlbQuatFromXYZ(xyz[0], xyz[1], xyz[2]);
                setAbsoluteTrackQuat(sc->bone_indices[roles[bit]], torsoState[bit], true, defaultQuatOrIdentity(sc, bit));
            }
            if (comp.mask & 0x20) {
                float xyz[3];
                if (consumeXYZ(fv, cursor, xyz)) {
                    torsoState[5] = GlbQuatFromXYZ(xyz[0], xyz[1], xyz[2]);
                    setAbsoluteTrackQuat(sc->bone_indices[TorsoBone::PELVIS], torsoState[5], true, defaultQuatOrIdentity(sc, 5));
                }
                float loc[3];
                if (consumeXYZ(fv, cursor, loc)) {
                    pelvisPos = {loc[0], loc[1], loc[2]};
                    rootOffset[0] += loc[0] - sc->default_pose.pelvis_pos[0];
                    rootOffset[1] += loc[1] - sc->default_pose.pelvis_pos[1];
                    rootOffset[2] += loc[2] - sc->default_pose.pelvis_pos[2];
                }
            }

            if (sc->offset_locs.size() >= 5) {
                float local[16];
                float world[16];
                int pelvis = sc->bone_indices[TorsoBone::PELVIS];
                matFromQuatPos(torsoState[5], pelvisPos, world);
                storeRuntimeWorld(pelvis, world);

                for (int i = 0; i < 5; ++i) {
                    int childRole = roles[i];
                    int child = sc->bone_indices[childRole];
                    int parent = sc->bone_indices[childRole - 1];
                    matFromQuatPos(torsoState[i], sc->offset_locs[i], local);
                    localToRuntimeParent(local, parent, world);
                    storeRuntimeWorld(child, world);
                }

                if (sc->type_id == NalCompType::TorsoHead_TwoNeck &&
                    sc->bone_indices.size() > TorsoBone::NECK_AUX) {
                    int neckAux = sc->bone_indices[TorsoBone::NECK_AUX];
                    GlbExportQuat emptyQ = GlbQuatNormalize({
                        sc->empty_neck_orient[3],
                        sc->empty_neck_orient[0],
                        sc->empty_neck_orient[1],
                        sc->empty_neck_orient[2]
                    });
                    matFromQuatPos(emptyQ, sc->empty_neck_pos, local);
                    localToRuntimeParent(local, sc->bone_indices[TorsoBone::SPINE2], world);
                    storeRuntimeWorld(neckAux, world);
                }
            }
        }
        else if (comp.comp_ix == NalComp::LEGS) {
            if (sc->bone_indices.size() < LegStdBone::COUNT) continue;
            GlbExportQuat legState[8];
            for (int i = 0; i < 8; ++i) legState[i] = defaultQuatOrIdentity(sc, i);
            for (int bit = 0; bit < 8; ++bit) {
                if ((comp.mask & (1u << bit)) == 0) continue;
                float xyz[3];
                if (!consumeXYZ(fv, cursor, xyz)) break;
                legState[bit] = GlbQuatFromXYZ(xyz[0], xyz[1], xyz[2]);
                setAbsoluteTrackQuat(sc->bone_indices[bit], legState[bit], true, defaultQuatOrIdentity(sc, bit));
            }

            if (sc->offset_locs.size() >= 8) {
                int parentRole[8] = {-1, LegStdBone::L_THIGH, LegStdBone::L_CALF, LegStdBone::L_FOOT,
                                     -1, LegStdBone::R_THIGH, LegStdBone::R_CALF, LegStdBone::R_FOOT};
                int anchor = sc->bone_indices[LegStdBone::ROOT];
                float local[16];
                float world[16];
                for (int role = 0; role < 8; ++role) {
                    int bone = sc->bone_indices[role];
                    matFromQuatPos(legState[role], sc->offset_locs[role], local);
                    int parent = parentRole[role] >= 0 ? sc->bone_indices[parentRole[role]] : anchor;
                    localToRuntimeParent(local, parent, world);
                    storeRuntimeWorld(bone, world);
                }
            }
        }
        else if (comp.comp_ix == NalComp::ARMS) {
            if (sc->bone_indices.size() < ArmBone::COUNT) continue;
            GlbExportQuat armState[8];
            for (int i = 0; i < 8; ++i) armState[i] = defaultQuatOrIdentity(sc, i);
            for (int bit = 0; bit < 8; ++bit) {
                if ((comp.mask & (1u << bit)) == 0) continue;
                float xyz[3];
                if (!consumeXYZ(fv, cursor, xyz)) break;
                armState[bit] = GlbQuatFromXYZ(xyz[0], xyz[1], xyz[2]);
                setAbsoluteTrackQuat(sc->bone_indices[bit], armState[bit], true, defaultQuatOrIdentity(sc, bit));
            }

            if (sc->offset_locs.size() >= 8) {
                int anchor = sc->bone_indices[ArmBone::NECK_PARENT];
                int order[8] = {ArmBone::L_CLAV, ArmBone::L_UPPER, ArmBone::L_FORE, ArmBone::L_HAND,
                                ArmBone::R_CLAV, ArmBone::R_UPPER, ArmBone::R_FORE, ArmBone::R_HAND};
                float local[16];
                float world[16];
                for (int oi = 0; oi < 8; ++oi) {
                    int role = order[oi];
                    int bone = sc->bone_indices[role];
                    matFromQuatPos(armState[role], sc->offset_locs[role], local);
                    int parent = (role == ArmBone::L_CLAV || role == ArmBone::R_CLAV)
                        ? anchor
                        : sc->bone_indices[role - 1];
                    localToRuntimeParent(local, parent, world);
                    storeRuntimeWorld(bone, world);
                }

                if (sc->fore_twist_locs.size() >= 4 && sc->bone_indices.size() > ArmBone::R_TWIST1) {
                    int twistBones[4] = {ArmBone::L_TWIST0, ArmBone::L_TWIST1, ArmBone::R_TWIST0, ArmBone::R_TWIST1};
                    int twistParents[4] = {ArmBone::L_FORE, ArmBone::L_TWIST0, ArmBone::R_FORE, ArmBone::R_TWIST0};
                    for (int i = 0; i < 4; ++i) {
                        int bone = sc->bone_indices[twistBones[i]];
                        int parent = sc->bone_indices[twistParents[i]];
                        float local[16];
                        float world[16];

                        GlbMat4BuildTwistLineXform(sc->fore_twist_locs[i], 0.0f, local);
                        localToRuntimeParent(local, parent, world);
                        storeRuntimeWorld(bone, world);
                    }
                }
            }
        }
        else if (comp.comp_ix == NalComp::LEGS_IK) {
            if (sc->bone_indices.size() < LegBone::COUNT) continue;
            GlbExportQuat toeState[2] = {defaultQuatOrIdentity(sc, 0), defaultQuatOrIdentity(sc, 1)};
            GlbExportQuat footState[2] = {defaultQuatOrIdentity(sc, 2), defaultQuatOrIdentity(sc, 3)};
            std::array<float, 3> footPos[2] = {sc->default_pose.foot_pos[0], sc->default_pose.foot_pos[1]};
            float kneeSpin[2] = {sc->default_pose.knee_spin[0], sc->default_pose.knee_spin[1]};

            for (int bit = 0; bit < 2; ++bit) {
                if ((comp.mask & (1u << bit)) == 0) continue;
                float xyz[3];
                if (!consumeXYZ(fv, cursor, xyz)) break;
                toeState[bit] = GlbQuatFromXYZ(xyz[0], xyz[1], xyz[2]);
                setAbsoluteTrackQuat(sc->bone_indices[bit], toeState[bit], true, defaultQuatOrIdentity(sc, bit));
            }
            for (int bit = 2; bit < 4; ++bit) {
                if ((comp.mask & (1u << bit)) == 0) continue;
                float xyz[3];
                if (!consumeXYZ(fv, cursor, xyz)) break;
                int side = bit - 2;
                footState[side] = GlbQuatFromXYZ(xyz[0], xyz[1], xyz[2]);
                setAbsoluteTrackQuat(sc->bone_indices[bit], footState[side], true, defaultQuatOrIdentity(sc, bit));

                float pos[3];
                if (consumeXYZ(fv, cursor, pos)) footPos[side] = {pos[0], pos[1], pos[2]};
                if (cursor < (int)fv.size()) kneeSpin[side] = fv[cursor++];
            }

            int anchor = sc->bone_indices[LegBone::PELVIS];
            float identity[16];
            GlbMat4Identity(identity);
            float local[16], world[16];
            for (int side = 0; side < 2; ++side) {
                int footRole = side == 0 ? LegBone::L_FOOT : LegBone::R_FOOT;
                int toeRole = side == 0 ? LegBone::L_TOE : LegBone::R_TOE;
                int thighRole = side == 0 ? LegBone::L_THIGH : LegBone::R_THIGH;
                int calfRole = side == 0 ? LegBone::L_CALF : LegBone::R_CALF;
                int foot = sc->bone_indices[footRole];
                int toe = sc->bone_indices[toeRole];

                matFromQuatPos(footState[side], footPos[side], local);
                if (solveLegsIk && sc->has_ik && sc->offset_locs.size() > (size_t)thighRole) {
                    float upperLocal[16], lowerLocal[16];
                    GlbSolveNalIk(identity, sc->offset_locs[thighRole], local, sc->ik_data[side],
                                  kneeSpin[side], false, false, upperLocal, lowerLocal);
                    localToRuntimeParent(upperLocal, anchor, world);
                    storeRuntimeWorld(sc->bone_indices[thighRole], world);
                    localToRuntimeParent(lowerLocal, anchor, world);
                    storeRuntimeWorld(sc->bone_indices[calfRole], world);
                }

                localToRuntimeParent(local, anchor, world);
                storeRuntimeWorld(foot, world);

                std::array<float, 3> toeOff = (sc->offset_locs.size() > (size_t)toeRole)
                    ? sc->offset_locs[toeRole]
                    : std::array<float, 3>{0.0f, 0.0f, 0.0f};
                matFromQuatPos(toeState[side], toeOff, local);
                localToRuntimeParent(local, foot, world);
                storeRuntimeWorld(toe, world);
            }
        }
        else if (comp.comp_ix == NalComp::ARMS_IK) {
            if (sc->bone_indices.size() < ArmIKBone::COUNT) continue;
            GlbExportQuat clavState[2] = {defaultQuatOrIdentity(sc, 0), defaultQuatOrIdentity(sc, 1)};
            GlbExportQuat handState[2] = {GlbExportQuat{}, GlbExportQuat{}};
            for (int i = 0; i < 2; ++i) {
                if (i < (int)sc->default_pose.hand_quats.size()) handState[i] = GlbQuatFromNal(sc->default_pose.hand_quats[i]);
            }
            std::array<float, 3> handPos[2] = {sc->default_pose.hand_pos[0], sc->default_pose.hand_pos[1]};
            float elbowSpin[2] = {sc->default_pose.elbow_spin[0], sc->default_pose.elbow_spin[1]};

            for (int bit = 0; bit < 2; ++bit) {
                if ((comp.mask & (1u << bit)) == 0) continue;
                float xyz[3];
                if (!consumeXYZ(fv, cursor, xyz)) break;
                clavState[bit] = GlbQuatFromXYZ(xyz[0], xyz[1], xyz[2]);
                setAbsoluteTrackQuat(sc->bone_indices[bit], clavState[bit], true, defaultQuatOrIdentity(sc, bit));
            }
            for (int bit = 2; bit < 4; ++bit) {
                if ((comp.mask & (1u << bit)) == 0) continue;
                float xyz[3];
                if (!consumeXYZ(fv, cursor, xyz)) break;
                int side = bit - 2;
                handState[side] = GlbQuatFromXYZ(xyz[0], xyz[1], xyz[2]);
                GlbExportQuat defaultQ = side < (int)sc->default_pose.hand_quats.size()
                    ? GlbQuatFromNal(sc->default_pose.hand_quats[side])
                    : GlbExportQuat{};
                setAbsoluteTrackQuat(sc->bone_indices[bit], handState[side], true, defaultQ);

                float pos[3];
                if (consumeXYZ(fv, cursor, pos)) handPos[side] = {pos[0], pos[1], pos[2]};
                if (cursor < (int)fv.size()) elbowSpin[side] = fv[cursor++];
            }

            if (sc->offset_locs.size() >= 2) {
                int anchor = sc->bone_indices[ArmIKBone::NECK_PARENT];
                int handAnchor = (sc->bone_indices.size() > ArmIKBone::PELVIS && sc->bone_indices[ArmIKBone::PELVIS] >= 0)
                    ? sc->bone_indices[ArmIKBone::PELVIS]
                    : anchor;
                float local[16], world[16];

                int clavRoles[2] = {ArmIKBone::L_CLAV, ArmIKBone::R_CLAV};
                int handRoles[2] = {ArmIKBone::L_HAND, ArmIKBone::R_HAND};
                for (int side = 0; side < 2; ++side) {
                    int clav = sc->bone_indices[clavRoles[side]];
                    matFromQuatPos(clavState[side], sc->offset_locs[side], local);
                    localToRuntimeParent(local, anchor, world);
                    storeRuntimeWorld(clav, world);
                    float clavWorld[16];
                    memcpy(clavWorld, world, sizeof(clavWorld));

                    int hand = sc->bone_indices[handRoles[side]];
                    matFromQuatPos(handState[side], handPos[side], local);
                    localToRuntimeParent(local, handAnchor, world);
                    storeRuntimeWorld(hand, world);
                    float handWorld[16];
                    memcpy(handWorld, world, sizeof(handWorld));

                    int upperRole = side == 0 ? ArmIKBone::L_UPPER : ArmIKBone::R_UPPER;
                    int foreRole = side == 0 ? ArmIKBone::L_FORE : ArmIKBone::R_FORE;
                    if (solveArmsIk && sc->has_ik && sc->offset_locs.size() > (size_t)upperRole) {
                        float upperWorld[16], foreWorld[16];
                        GlbSolveNalIk(clavWorld, sc->offset_locs[upperRole], handWorld, sc->ik_data[side],
                                      elbowSpin[side], true, side == 1, upperWorld, foreWorld);
                        storeRuntimeWorld(sc->bone_indices[upperRole], upperWorld);
                        storeRuntimeWorld(sc->bone_indices[foreRole], foreWorld);
                    }
                }

                if (sc->fore_twist_locs.size() >= 4 &&
                    sc->bone_indices.size() > ArmIKBone::R_TWIST1) {
                    int twistBones[4] = {ArmIKBone::L_TWIST0, ArmIKBone::L_TWIST1,
                                         ArmIKBone::R_TWIST0, ArmIKBone::R_TWIST1};
                    int twistParents[4] = {ArmIKBone::L_FORE, ArmIKBone::L_TWIST0,
                                           ArmIKBone::R_FORE, ArmIKBone::R_TWIST0};
                    for (int i = 0; i < 4; ++i) {
                        GlbMat4BuildTwistLineXform(sc->fore_twist_locs[i], 0.0f, local);
                        localToRuntimeParent(local, sc->bone_indices[twistParents[i]], world);
                        storeRuntimeWorld(sc->bone_indices[twistBones[i]], world);
                    }
                }
            }
        }
        else if (comp.comp_ix == NalComp::FAKEROOT_STD) {
            int fc = 0;
            if (comp.mask & 0x1) {
                float qxyz[3] = {0.0f, 0.0f, 0.0f};
                if (fc + 2 < (int)fv.size()) {
                    qxyz[0] = fv[fc++];
                    qxyz[1] = fv[fc++];
                    qxyz[2] = fv[fc++];
                }
                GlbMat4FromNalQuat(GlbQuatFromXYZ(qxyz[0], qxyz[1], qxyz[2]), rootRotMat);
                hasRootRot = true;

                if (fc + 2 < (int)fv.size()) {
                    rootOffset[0] += fv[fc] - sc->default_pose.pelvis_pos[0]; fc++;
                    rootOffset[1] += fv[fc] - sc->default_pose.pelvis_pos[1]; fc++;
                    rootOffset[2] += fv[fc] - sc->default_pose.pelvis_pos[2]; fc++;
                }
            }
        }
        else if (comp.comp_ix == NalComp::ARBITRARY_PO) {
            int quatCount = std::max(0, sc->default_pose.quat_count);
            int posCount = std::max(0, sc->default_pose.position_count);
            int channelCount = quatCount + posCount;
            std::vector<GlbExportQuat> arbStateQuats(quatCount, GlbExportQuat{});
            std::vector<std::array<float, 3>> arbStatePositions(posCount, {0.0f, 0.0f, 0.0f});
            for (int i = 0; i < quatCount && i < (int)sc->default_pose.quats.size(); ++i)
                arbStateQuats[i] = GlbQuatFromNal(sc->default_pose.quats[i]);
            for (int i = 0; i < posCount && i < (int)sc->default_pose.positions.size(); ++i)
                arbStatePositions[i] = sc->default_pose.positions[i];

            for (int channel = 0; channel < channelCount; ++channel) {
                if (!comp.channel_enabled(channel)) continue;
                float xyz[3];
                if (!consumeXYZ(fv, cursor, xyz)) break;
                if (channel < quatCount) arbStateQuats[channel] = GlbQuatFromXYZ(xyz[0], xyz[1], xyz[2]);
                else arbStatePositions[channel - quatCount] = {xyz[0], xyz[1], xyz[2]};
            }

            for (size_t oi = 0; oi < sc->arb_eval_order.size(); ++oi) {
                uint32_t nodeIndex = sc->arb_eval_order[oi];
                if (nodeIndex >= sc->arb_nodes.size()) continue;
                const auto& node = sc->arb_nodes[nodeIndex];
                    int bid = node.my_matrix_ix;
                    if (bid < 0 || bid >= boneCount) continue;

                    GlbExportQuat quat = GlbExportQuat{};
                    if (node.is_quat_anim && node.quat_ix < arbStateQuats.size()) {
                        quat = arbStateQuats[node.quat_ix];
                    } else if (node.quat_ix < sc->arb_skel_quats.size()) {
                        quat = GlbQuatFromNal(sc->arb_skel_quats[node.quat_ix]);
                    } else if (node.quat_ix < sc->default_pose.quats.size()) {
                        quat = GlbQuatFromNal(sc->default_pose.quats[node.quat_ix]);
                    }

                    std::array<float, 3> pos = {0.0f, 0.0f, 0.0f};
                    if (node.is_pos_anim && node.pos_ix < arbStatePositions.size()) {
                        pos = arbStatePositions[node.pos_ix];
                    } else if (node.pos_ix < sc->arb_skel_positions.size()) {
                        pos = sc->arb_skel_positions[node.pos_ix];
                    } else if (node.pos_ix < sc->default_pose.positions.size()) {
                        pos = sc->default_pose.positions[node.pos_ix];
                    }

                    float local[16];
                    matFromQuatPos(quat, pos, local);

                    int parentId = node.parent_matrix_ix;
                    float world[16];
                    localToRuntimeParent(local, parentId, world);
                    storeRuntimeWorld(bid, world);
            }
        }
        else if (comp.comp_ix == NalComp::FING5) {
            if (sc->bone_indices.size() < 30) continue;
            for (int bit = 0; bit < 30; ++bit) {
                if ((comp.mask & (1u << bit)) == 0) continue;
                float xyz[3];
                if (!consumeXYZ(fv, cursor, xyz)) break;
                setTrackXYZ(sc->bone_indices[bit], xyz[0], xyz[1], xyz[2], sc, bit);
            }
        }
        else if (comp.comp_ix == NalComp::FING5_REDUCED) {
            if (sc->bone_indices.size() < 30) continue;
            float baseY[8] = {};
            float baseZ[8] = {};
            float midHinge[10] = {};
            float tipHinge[10] = {};
            float defaultBaseY[8] = {};
            float defaultBaseZ[8] = {};
            float defaultMidHinge[10] = {};
            float defaultTipHinge[10] = {};
            for (int i = 0; i < 8 && i < (int)sc->default_pose.base_y_tracks.size(); ++i)
                baseY[i] = defaultBaseY[i] = sc->default_pose.base_y_tracks[i];
            for (int i = 0; i < 8 && i < (int)sc->default_pose.base_z_tracks.size(); ++i)
                baseZ[i] = defaultBaseZ[i] = sc->default_pose.base_z_tracks[i];
            for (int i = 0; i < 10 && i < (int)sc->default_pose.hinge_tracks.size(); ++i)
                midHinge[i] = defaultMidHinge[i] = sc->default_pose.hinge_tracks[i];
            for (int i = 0; i < 10 && i < (int)sc->default_pose.other_tracks.size(); ++i)
                tipHinge[i] = defaultTipHinge[i] = sc->default_pose.other_tracks[i];
            GlbExportQuat thumb[2] = {defaultQuatOrIdentity(sc, 0), defaultQuatOrIdentity(sc, 1)};

            for (int bit = 0; bit < 30; ++bit) {
                if ((comp.mask & (1u << bit)) == 0) continue;
                if (bit < 2) {
                    float xyz[3];
                    if (!consumeXYZ(fv, cursor, xyz)) break;
                    thumb[bit] = GlbQuatFromXYZ(xyz[0], xyz[1], xyz[2]);
                } else if (bit < 10) {
                    if (cursor + 1 >= (int)fv.size()) break;
                    baseZ[bit - 2] = fv[cursor++];
                    baseY[bit - 2] = fv[cursor++];
                } else if (bit < 20) {
                    if (cursor >= (int)fv.size()) break;
                    midHinge[bit - 10] = fv[cursor++];
                } else {
                    if (cursor >= (int)fv.size()) break;
                    tipHinge[bit - 20] = fv[cursor++];
                }
            }

            for (int base = 0; base < 10; ++base) {
                if (base < 2) {
                    GlbExportQuat def;
                    bool hasDef = GlbGetDefaultQuat(sc, base, def);
                    setAbsoluteTrackQuat(sc->bone_indices[base], thumb[base], hasDef, def);
                } else {
                    setAbsoluteTrackQuat(sc->bone_indices[base],
                        GlbQuatFromYZAngles(baseY[base - 2], baseZ[base - 2]), true,
                        GlbQuatFromYZAngles(defaultBaseY[base - 2], defaultBaseZ[base - 2]));
                }
                setBoneDeltaQuat(sc->bone_indices[10 + base],
                    GlbQuatAxisAngle(0, midHinge[base] - defaultMidHinge[base]));
                setBoneDeltaQuat(sc->bone_indices[20 + base],
                    GlbQuatAxisAngle(0, tipHinge[base] - defaultTipHinge[base]));
            }
        }
        else if (comp.comp_ix == NalComp::FING5_CURL) {
            if (sc->bone_indices.size() < 30) continue;
            float baseZ[8] = {};
            float hinge[10] = {};
            float defaultHinge[10] = {};
            for (int i = 0; i < 10 && i < (int)sc->default_pose.finger_curl.size(); ++i)
                hinge[i] = defaultHinge[i] = sc->default_pose.finger_curl[i];
            GlbExportQuat thumb[2] = {defaultQuatOrIdentity(sc, 0), defaultQuatOrIdentity(sc, 1)};

            for (int bit = 0; bit < 10; ++bit) {
                if ((comp.mask & (1u << bit)) == 0) continue;
                if (bit < 2) {
                    float xyz[3];
                    if (!consumeXYZ(fv, cursor, xyz)) break;
                    thumb[bit] = GlbQuatFromXYZ(xyz[0], xyz[1], xyz[2]);
                    if (cursor < (int)fv.size()) hinge[bit] = fv[cursor++];
                } else {
                    if (cursor + 1 >= (int)fv.size()) break;
                    baseZ[bit - 2] = fv[cursor++];
                    hinge[bit] = fv[cursor++];
                }
            }

            for (int base = 0; base < 10; ++base) {
                if (base < 2) {
                    GlbExportQuat def;
                    bool hasDef = GlbGetDefaultQuat(sc, base, def);
                    setAbsoluteTrackQuat(sc->bone_indices[base], thumb[base], hasDef, def);
                } else {
                    GlbExportQuat def = GlbQuatFromYZAngles(defaultHinge[base], 0.0f);
                    setAbsoluteTrackQuat(sc->bone_indices[base], GlbQuatFromYZAngles(hinge[base], baseZ[base - 2]), true, def);
                }
                setBoneDeltaQuat(sc->bone_indices[10 + base], GlbQuatAxisAngle(0, hinge[base] - defaultHinge[base]));
                setBoneDeltaQuat(sc->bone_indices[20 + base], GlbQuatAxisAngle(0,
                    GlbFingerTipHingeAngle(hinge[base], base < 2) - GlbFingerTipHingeAngle(defaultHinge[base], base < 2)));
            }
        }
        else if (comp.comp_ix == NalComp::FING52) {
            if (sc->bone_indices.size() < 30) continue;
            GlbExportQuat thumb[2] = {defaultQuatOrIdentity(sc, 0), defaultQuatOrIdentity(sc, 1)};
            float baseY[8] = {};
            float baseZ[8] = {};
            float hinge[10] = {};
            for (int i = 0; i < 8 && i < (int)sc->default_pose.base_y_tracks.size(); ++i)
                baseY[i] = sc->default_pose.base_y_tracks[i];
            for (int i = 0; i < 8 && i < (int)sc->default_pose.base_z_tracks.size(); ++i)
                baseZ[i] = sc->default_pose.base_z_tracks[i];
            for (int i = 0; i < 10 && i < (int)sc->default_pose.hinge_tracks.size(); ++i)
                hinge[i] = sc->default_pose.hinge_tracks[i];

            for (int bit = 0; bit < 20; ++bit) {
                if ((comp.mask & (1u << bit)) == 0) continue;
                if (bit < 2) {
                    float xyz[3];
                    if (!consumeXYZ(fv, cursor, xyz)) break;
                    thumb[bit] = GlbQuatFromXYZ(xyz[0], xyz[1], xyz[2]);
                } else if (bit < 10) {
                    if (cursor + 1 >= (int)fv.size()) break;
                    baseY[bit - 2] = fv[cursor++];
                    baseZ[bit - 2] = fv[cursor++];
                } else {
                    if (cursor >= (int)fv.size()) break;
                    hinge[bit - 10] = fv[cursor++];
                }
            }

            std::map<int, std::array<float, 16>> baseWorldByChain;
            if (sc->offset_locs.size() >= 30) {
                auto anchorIdForChain = [&](int chain) -> int {
                    int slot = (chain == 1 || chain >= 6) ? 1 : 0;
                    int idx = FingerBone::L_HAND_PARENT + slot;
                    return (idx < (int)sc->bone_indices.size()) ? sc->bone_indices[idx] : -1;
                };
                auto loadAnchorWorld = [&](int anchorId, float* out) {
                    if (anchorId >= 0 && getRuntimeWorld(anchorId, out)) return;
                    if (anchorId >= 0 && anchorId < boneCount) {
                        memcpy(out, bindMatrices[anchorId].data(), sizeof(float) * 16);
                        return;
                    }
                    GlbMat4Identity(out);
                };

                float local[16], world[16], anchorWorld[16];
                for (int base = 0; base < 10; ++base) {
                    GlbExportQuat q = (base < 2)
                        ? thumb[base]
                        : GlbQuatFromYZAngles(baseY[base - 2], baseZ[base - 2]);
                    matFromQuatPos(q, sc->offset_locs[base], local);
                    loadAnchorWorld(anchorIdForChain(base), anchorWorld);
                    GlbMat4Multiply(anchorWorld, local, world);

                    std::array<float, 16> stored{};
                    memcpy(stored.data(), world, sizeof(float) * 16);
                    baseWorldByChain[base] = stored;
                    storeRuntimeWorld(sc->bone_indices[base], world);
                }

                for (int chain = 0; chain < 10; ++chain) {
                    auto baseIt = baseWorldByChain.find(chain);
                    if (baseIt == baseWorldByChain.end()) continue;

                    float midLocal[16], tipLocal[16], midWorld[16], tipWorld[16];
                    GlbMat4Fing52HingeLocal(hinge[chain], sc->offset_locs[10 + chain], midLocal);
                    GlbMat4Fing52HingeLocal(GlbFingerTipHingeAngle(hinge[chain], chain < 2), sc->offset_locs[20 + chain], tipLocal);
                    GlbMat4Multiply(baseIt->second.data(), midLocal, midWorld);
                    GlbMat4Multiply(midWorld, tipLocal, tipWorld);
                    storeRuntimeWorld(sc->bone_indices[10 + chain], midWorld);
                    storeRuntimeWorld(sc->bone_indices[20 + chain], tipWorld);
                }
            }

            for (int base = 0; base < 10; ++base) {
                if (base < 2) {
                    GlbExportQuat def;
                    bool hasDef = GlbGetDefaultQuat(sc, base, def);
                    setAbsoluteTrackQuat(sc->bone_indices[base], thumb[base], hasDef, def);
                } else {
                    GlbExportQuat def = GlbQuatFromYZAngles(
                        (base - 2 < (int)sc->default_pose.base_y_tracks.size()) ? sc->default_pose.base_y_tracks[base - 2] : 0.0f,
                        (base - 2 < (int)sc->default_pose.base_z_tracks.size()) ? sc->default_pose.base_z_tracks[base - 2] : 0.0f);
                    setAbsoluteTrackQuat(sc->bone_indices[base], GlbQuatFromYZAngles(baseY[base - 2], baseZ[base - 2]), true, def);
                }
                float defHinge = (base < (int)sc->default_pose.hinge_tracks.size()) ? sc->default_pose.hinge_tracks[base] : 0.0f;
                setBoneDeltaQuat(sc->bone_indices[10 + base], GlbQuatAxisAngle(0, hinge[base] - defHinge));
                setBoneDeltaQuat(sc->bone_indices[20 + base], GlbQuatAxisAngle(0,
                    GlbFingerTipHingeAngle(hinge[base], base < 2) - GlbFingerTipHingeAngle(defHinge, base < 2)));
            }
        }
    }

    for (int i = 0; i < boneCount; ++i) {
        if (poseDeltas[i].hasRot) {
            float rot[16];
            GlbMat4FromNalQuat(poseDeltas[i].rot, rot);
            GlbMat4Multiply(restLocal[i].data(), rot, animLocal[i].data());
        } else if (i < (int)restLocal.size()) {
            memcpy(animLocal[i].data(), restLocal[i].data(), sizeof(float) * 16);
        } else {
            GlbMat4Identity(animLocal[i].data());
        }
    }

    std::vector<uint8_t> visitState(boneCount, 0);
    std::function<void(int)> buildWorld = [&](int boneIdx) {
        if (boneIdx < 0 || boneIdx >= boneCount) return;
        if (visitState[boneIdx] == 2) return;
        if (visitState[boneIdx] == 1) {
            memcpy(outWorld[boneIdx].data(), animLocal[boneIdx].data(), sizeof(float) * 16);
            visitState[boneIdx] = 2;
            return;
        }
        visitState[boneIdx] = 1;
        auto runtimeIt = runtimeWorld.find(boneIdx);
        if (runtimeIt != runtimeWorld.end()) {
            memcpy(outWorld[boneIdx].data(), runtimeIt->second.data(), sizeof(float) * 16);
        } else {
            int parent = parentOf(boneIdx);
            if (parent >= 0) {
                buildWorld(parent);
                GlbMat4Multiply(outWorld[parent].data(), animLocal[boneIdx].data(), outWorld[boneIdx].data());
            } else {
                memcpy(outWorld[boneIdx].data(), animLocal[boneIdx].data(), sizeof(float) * 16);
            }
        }
        visitState[boneIdx] = 2;
    };

    for (int i = 0; i < boneCount; ++i) buildWorld(i);

    if (hasRootRot) {
        for (int i = 0; i < boneCount; ++i) {
            float tmp[16];
            GlbMat4Multiply(rootRotMat, outWorld[i].data(), tmp);
            memcpy(outWorld[i].data(), tmp, sizeof(float) * 16);
        }
    }

    if (runtimeWorld.empty()) {
        for (int i = 0; i < boneCount; ++i) {
            outWorld[i][12] += rootOffset[0];
            outWorld[i][13] += rootOffset[1];
            outWorld[i][14] += rootOffset[2];
        }
    }
}

static float GlbQuatDotXYZZ(const float* a, const float* b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
}

static bool GlbRotationTrackChanged(const std::vector<float>& samples, const float base[4]) {
    if (samples.empty()) return false;
    for (size_t i = 0; i + 3 < samples.size(); i += 4) {
        float q[4] = {samples[i], samples[i + 1], samples[i + 2], samples[i + 3]};
        if (fabsf(1.0f - fabsf(GlbQuatDotXYZZ(q, base))) > 1e-5f) return true;
        if (i >= 4) {
            float prev[4] = {samples[i - 4], samples[i - 3], samples[i - 2], samples[i - 1]};
            if (fabsf(1.0f - fabsf(GlbQuatDotXYZZ(q, prev))) > 1e-5f) return true;
        }
    }
    return false;
}

static bool GlbTranslationTrackChanged(const std::vector<float>& samples, const float base[3]) {
    if (samples.empty()) return false;
    for (size_t i = 0; i + 2 < samples.size(); i += 3) {
        float dx = samples[i] - base[0];
        float dy = samples[i + 1] - base[1];
        float dz = samples[i + 2] - base[2];
        if (dx * dx + dy * dy + dz * dz > 1e-8f) return true;
        if (i >= 3) {
            dx = samples[i] - samples[i - 3];
            dy = samples[i + 1] - samples[i - 2];
            dz = samples[i + 2] - samples[i - 1];
            if (dx * dx + dy * dy + dz * dz > 1e-8f) return true;
        }
    }
    return false;
}

static void GlbBuildWorldFromLocal(
    const std::vector<std::array<float, 16>>& localMatrices,
    const NalSkeletonData& skel,
    std::vector<std::array<float, 16>>& worldMatrices)
{
    int boneCount = (int)localMatrices.size();
    worldMatrices.resize(boneCount);
    std::vector<uint8_t> visitState(boneCount, 0);

    std::function<void(int)> build = [&](int boneIdx) {
        if (boneIdx < 0 || boneIdx >= boneCount) return;
        if (visitState[boneIdx] == 2) return;
        if (visitState[boneIdx] == 1) {
            memcpy(worldMatrices[boneIdx].data(), localMatrices[boneIdx].data(), sizeof(float) * 16);
            visitState[boneIdx] = 2;
            return;
        }

        visitState[boneIdx] = 1;
        int parent = GlbParentOf(&skel, boneIdx, boneCount);
        if (parent >= 0) {
            build(parent);
            GlbMat4Multiply(worldMatrices[parent].data(), localMatrices[boneIdx].data(), worldMatrices[boneIdx].data());
        } else {
            memcpy(worldMatrices[boneIdx].data(), localMatrices[boneIdx].data(), sizeof(float) * 16);
        }
        visitState[boneIdx] = 2;
    };

    for (int i = 0; i < boneCount; ++i) build(i);
}

static void GlbBuildGenericAnimationWorldMatricesForFrame(
    const NalAnimEntry& anim,
    const std::vector<std::array<float, 16>>& bindWorldMatrices,
    int frame,
    std::vector<std::array<float, 16>>& outWorld)
{
    outWorld = bindWorldMatrices;
    if (!anim.generic_decoded.complete ||
        frame < 0 || frame >= (int)anim.generic_decoded.world_frames.size())
        return;

    const auto& genericWorld = anim.generic_decoded.world_frames[frame];
    const size_t count = std::min(outWorld.size(), genericWorld.size());
    for (size_t logical = 0; logical < count; ++logical) {

        memcpy(outWorld[logical].data(), genericWorld[logical].data(),
               sizeof(float) * 16);
    }
}

static int GlbAddAnimationToWriter(
    SkinningGLBWriter& writer,
    const NalAnimEntry& anim,
    int animIndex,
    const NalSkeletonData& skel,
    const std::vector<std::array<float, 16>>& restLocal,
    const std::vector<std::array<float, 16>>& baseNodeMatrices,
    const std::vector<GlbExportQuat>& restLocalQuatNal,
    const std::vector<int>& boneNodeIndices,
    const std::vector<int>& logicalToMesh)
{
    int boneCount = (int)restLocal.size();
    int frameCount = anim.playback_frame_count();
    const bool genericReady = anim.is_gen_anim() && anim.generic_decoded.complete &&
                              !anim.generic_decoded.world_frames.empty();
    if (boneCount <= 0 || frameCount <= 1 ||
        (!genericReady && anim.components.empty())) return 0;
    auto nodeForLogicalBone = [&](int logical) -> int {
        if (logical < 0 || logical >= boneCount) return -1;
        int mesh = (logicalToMesh.size() == (size_t)boneCount)
            ? logicalToMesh[logical] : logical;
        return (mesh >= 0 && mesh < (int)boneNodeIndices.size())
            ? boneNodeIndices[mesh] : -1;
    };

    const int sampleCount = frameCount + (anim.is_looping() ? 1 : 0);

    std::vector<float> times(sampleCount);
    for (int f = 0; f < sampleCount; ++f) {
        times[f] = (float)f / NAL_PREVIEW_FPS;
    }

    std::vector<float> baseTranslations(boneCount * 3, 0.0f);
    std::vector<float> baseRotations(boneCount * 4, 0.0f);
    for (int bi = 0; bi < boneCount; ++bi) {
        const float* baseMatrix = (bi < (int)baseNodeMatrices.size()) ? baseNodeMatrices[bi].data() : restLocal[bi].data();
        GlbMatrixToTRS(baseMatrix, &baseTranslations[bi * 3], &baseRotations[bi * 4]);
    }

    std::vector<std::vector<float>> rotationSamples(boneCount);
    std::vector<std::vector<float>> translationSamples(boneCount);
    for (int bi = 0; bi < boneCount; ++bi) {
        rotationSamples[bi].reserve((size_t)sampleCount * 4);
        translationSamples[bi].reserve((size_t)sampleCount * 3);
    }

    std::vector<std::array<float, 16>> frameWorld;
    for (int sample = 0; sample < sampleCount; ++sample) {
        const int frame = (sample == frameCount) ? 0 : sample;
        if (genericReady) {
            GlbBuildGenericAnimationWorldMatricesForFrame(
                anim, baseNodeMatrices, frame, frameWorld);
        } else {
            GlbBuildAnimationWorldMatricesForFrame(
                anim, skel, baseNodeMatrices, restLocal,
                restLocalQuatNal, frame, frameWorld);
        }
        for (int bi = 0; bi < boneCount; ++bi) {
            if (nodeForLogicalBone(bi) < 0) continue;
            float translation[3];
            float rotation[4];
            const float* sampleMatrix = (bi < (int)frameWorld.size()) ? frameWorld[bi].data() : baseNodeMatrices[bi].data();
            GlbMatrixToTRS(sampleMatrix, translation, rotation);
            auto& rot = rotationSamples[bi];
            if (!rot.empty()) {
                float prev[4] = {rot[rot.size() - 4], rot[rot.size() - 3], rot[rot.size() - 2], rot[rot.size() - 1]};
                if (GlbQuatDotXYZZ(prev, rotation) < 0.0f) {
                    rotation[0] = -rotation[0];
                    rotation[1] = -rotation[1];
                    rotation[2] = -rotation[2];
                    rotation[3] = -rotation[3];
                }
            }
            rot.push_back(rotation[0]);
            rot.push_back(rotation[1]);
            rot.push_back(rotation[2]);
            rot.push_back(rotation[3]);

            auto& trans = translationSamples[bi];
            trans.push_back(translation[0]);
            trans.push_back(translation[1]);
            trans.push_back(translation[2]);
        }
    }

    std::vector<int> animatedRotationBones;
    std::vector<int> animatedTranslationBones;
    for (int bi = 0; bi < boneCount; ++bi) {
        if (nodeForLogicalBone(bi) < 0) continue;
        if (GlbRotationTrackChanged(rotationSamples[bi], &baseRotations[bi * 4])) {
            animatedRotationBones.push_back(bi);
        }
        if (GlbTranslationTrackChanged(translationSamples[bi], &baseTranslations[bi * 3])) {
            animatedTranslationBones.push_back(bi);
        }
    }

    if (animatedRotationBones.empty() && animatedTranslationBones.empty()) return 0;

    std::string name = anim.name.empty() ? ("animation_" + std::to_string(animIndex)) : anim.name;
    int gltfAnim = writer.AddAnimation(name);

    int timeView = writer.AddBufferView(times.data(), times.size() * sizeof(float), 0);
    int timeAccessor = writer.AddAccessorWithBounds(timeView, 5126, sampleCount, "SCALAR", {times.front()}, {times.back()});

    for (int bi : animatedRotationBones) {
        int view = writer.AddBufferView(rotationSamples[bi].data(), rotationSamples[bi].size() * sizeof(float), 0);
        int accessor = writer.AddAccessor(view, 5126, sampleCount, "VEC4");
        int sampler = writer.AddAnimationSampler(gltfAnim, timeAccessor, accessor);
        writer.AddAnimationChannel(gltfAnim, sampler, nodeForLogicalBone(bi), "rotation");
    }

    for (int bi : animatedTranslationBones) {
        int view = writer.AddBufferView(translationSamples[bi].data(), translationSamples[bi].size() * sizeof(float), 0);
        int accessor = writer.AddAccessor(view, 5126, sampleCount, "VEC3");
        int sampler = writer.AddAnimationSampler(gltfAnim, timeAccessor, accessor);
        writer.AddAnimationChannel(gltfAnim, sampler, nodeForLogicalBone(bi), "translation");
    }

    return writer.HasAnimation(gltfAnim) ? 1 : 0;
}

void SpiderManTool::AddMeshFromData(const std::vector<uint8_t>& pcmData, std::string modelName, std::function<unsigned int(uint32_t)> textureResolver, const std::string& sourcePack, uint32_t sourceOffset) {
    ParseMaterialEntries(pcmData);

    BinaryReader br(pcmData);
    if (8 + 4 > pcmData.size()) return;
    br.Seek(8);
    uint32_t num = br.Read<uint32_t>();
    uint32_t ofs = br.Read<uint32_t>();

    if (num > 1000) return;
    if (ofs >= pcmData.size()) return;

    br.Seek(ofs);
    struct Info { uint16_t u1, type; uint32_t offset, u2; };
    std::vector<Info> infos;
    for(uint32_t i=0; i<num; i++) {
        Info inf; inf.u1 = br.Read<uint16_t>(); inf.type = br.Read<uint16_t>(); inf.offset = br.Read<uint32_t>(); inf.u2 = br.Read<uint32_t>(); infos.push_back(inf);
    }

    struct Vertex { float x,y,z; float nx,ny,nz; float u,v; float boneIdx[4]; float boneWgt[4]; float cr,cg,cb,ca; };

    bool loadedFirstLod = false;
    for(auto& inf : infos) {
        if (inf.type != 512) continue;
        if (loadedFirstLod) break;
        loadedFirstLod = true;

        if (inf.offset + 16 > pcmData.size()) continue;

        br.Seek(inf.offset); br.Skip(8);
        uint32_t numSm = br.Read<uint32_t>(); uint32_t infSmOfs = br.Read<uint32_t>();
        if (numSm > 256 || infSmOfs >= pcmData.size()) continue;

        br.Seek(infSmOfs);
        std::vector<std::pair<uint32_t, uint32_t>> smRefs;
        for(uint32_t s=0; s<numSm; s++) {
            uint32_t matRef = br.Read<uint32_t>();
            uint32_t smOfs = br.Read<uint32_t>();
            smRefs.push_back({matRef, smOfs});
        }

        for(size_t sectionIndex = 0; sectionIndex < smRefs.size(); ++sectionIndex) {
            const auto [matRef, smOfs] = smRefs[sectionIndex];
            if (smOfs + 96 > pcmData.size()) continue;

            br.Seek(smOfs);
            uint32_t meshNameRef = br.Read<uint32_t>();
            uint32_t ptrShaderRef = br.Read<uint32_t>();

            std::string meshName;
            if (meshNameRef != 0 && meshNameRef + 32 <= pcmData.size()) {
                size_t strStart = meshNameRef + 4;
                size_t end = strStart;
                while (end < strStart + 28 && end < pcmData.size() && pcmData[end] != 0) end++;
                meshName = std::string((char*)&pcmData[strStart], end - strStart);
            }

            MaterialDef mat = ResolveMaterialByMeshOffset(meshNameRef);
            unsigned int tex = 0;

            if (!mat.textureName.empty()) {
                if (textureResolver) {
                    std::string texNameLower = StrToLower(mat.textureName);
                    uint32_t hash1 = CalculateCRC32(texNameLower + ".dds");
                    tex = textureResolver(hash1);
                    if (tex == 0) {
                        uint32_t hash2 = CalculateCRC32(texNameLower);
                        tex = textureResolver(hash2);
                    }
                } else {
                    tex = LoadTextureByName(mat.textureName);
                }
            }

            if (tex == 0 && !modelName.empty()) {
                std::string cleanName = modelName;
                size_t lastDot = cleanName.find_last_of(".");
                if(lastDot != std::string::npos) cleanName = cleanName.substr(0, lastDot);
                cleanName = StrToLower(cleanName);

                if (textureResolver) {
                    uint32_t diffHash = CalculateCRC32(cleanName + "_d.dds");
                    tex = textureResolver(diffHash);
                } else {
                    tex = LoadTextureByName(cleanName + "_d");
                    if (tex == 0) tex = LoadTextureByName(cleanName);
                }
            }

            if (tex == 0 && !mat.meshName.empty()) {
                if (textureResolver) {
                    std::string meshNameLower = StrToLower(mat.meshName);
                    uint32_t hash4 = CalculateCRC32(meshNameLower + ".dds");
                    tex = textureResolver(hash4);
                } else {
                    tex = LoadTextureByName(mat.meshName);
                }
            }

            br.Seek(smOfs + 40);
            uint32_t itype = br.Read<uint32_t>(); uint32_t inum = br.Read<uint32_t>(); uint32_t iofs = br.Read<uint32_t>();
            br.Skip(4); uint32_t vnum = br.Read<uint32_t>(); uint32_t vofs = br.Read<uint32_t>();
            br.Skip(8); uint32_t stride = br.Read<uint32_t>();

            if (vnum > 100000 || inum > 300000 || vofs >= pcmData.size() || iofs >= pcmData.size() || stride == 0) continue;

            PCMSectionBonePalette bonePalette;
            bonePalette.load(pcmData, smOfs);

            br.Seek(vofs);
            std::vector<Vertex> vertices;
            bool valid = true;

            float bboxMin[3] = {1e30f, 1e30f, 1e30f};
            float bboxMax[3] = {-1e30f, -1e30f, -1e30f};

            for(uint32_t v=0; v<vnum; v++) {
                size_t startV = br.Tell();
                if (startV + stride > pcmData.size()) { valid = false; break; }
                Vertex vert;
                memset(&vert, 0, sizeof(vert));
                vert.x = br.Read<float>(); vert.y = br.Read<float>(); vert.z = br.Read<float>();

                if (vert.x < bboxMin[0]) bboxMin[0] = vert.x;
                if (vert.y < bboxMin[1]) bboxMin[1] = vert.y;
                if (vert.z < bboxMin[2]) bboxMin[2] = vert.z;
                if (vert.x > bboxMax[0]) bboxMax[0] = vert.x;
                if (vert.y > bboxMax[1]) bboxMax[1] = vert.y;
                if (vert.z > bboxMax[2]) bboxMax[2] = vert.z;

                vert.cr = vert.cg = vert.cb = vert.ca = 1.0f;

                if (stride == 64) {
                    if (startV + 24 <= pcmData.size()) {
                        br.Seek(startV + 12);
                        vert.nx = br.Read<float>(); vert.ny = br.Read<float>(); vert.nz = br.Read<float>();
                    }
                    if (startV + 32 <= pcmData.size()) {
                        br.Seek(startV + 24);
                        vert.u = br.Read<float>(); vert.v = br.Read<float>();
                    }

                    if (startV + 64 <= pcmData.size()) {
                        br.Seek(startV + 32);
                        for (int bi = 0; bi < 4; bi++) vert.boneIdx[bi] = br.Read<float>();
                        for (int bi = 0; bi < 4; bi++) vert.boneWgt[bi] = br.Read<float>();

                        float wTotal = vert.boneWgt[0] + vert.boneWgt[1] + vert.boneWgt[2] + vert.boneWgt[3];
                        if (wTotal > 1e-8f) {
                            float inv = 1.f / wTotal;
                            for (int bi = 0; bi < 4; bi++) vert.boneWgt[bi] *= inv;
                        } else {
                            for (int bi = 0; bi < 4; bi++) vert.boneWgt[bi] = 0.f;
                        }
                    }
                } else if (stride == 24) {
                    if (startV + 20 <= pcmData.size()) {
                        br.Seek(startV + 12);
                        vert.u = br.Read<float>(); vert.v = br.Read<float>();
                    }

                    if (startV + 24 <= pcmData.size()) {
                        br.Seek(startV + 20);
                        uint32_t packed = br.Read<uint32_t>();
                        DecodeD3DColor(packed, vert.cr, vert.cg, vert.cb, vert.ca);
                    }
                } else if (stride == 32 || stride == 60) {

                    if (startV + 20 <= pcmData.size()) {
                        br.Seek(startV + 12);
                        vert.u = br.Read<float>(); vert.v = br.Read<float>();
                    }
                    if (startV + 24 <= pcmData.size()) {
                        br.Seek(startV + 20);
                        uint32_t packed = br.Read<uint32_t>();
                        DecodeD3DColor(packed, vert.cr, vert.cg, vert.cb, vert.ca);
                    }
                }
                vertices.push_back(vert);
                br.Seek(startV + stride);
            }
            if (!valid) continue;

            br.Seek(iofs);
            std::vector<uint16_t> indices;
            if (iofs + inum * 2 > pcmData.size()) continue;
            for(uint32_t i=0; i<inum; i++) indices.push_back(br.Read<uint16_t>());
            if (vertices.empty() || indices.empty()) continue;

            if (stride != 64 && !vertices.empty() && !indices.empty()) {

                for (auto& v : vertices) {
                    v.nx = 0; v.ny = 0; v.nz = 0;
                }

                if (itype == 4) {

                    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
                        uint16_t i0 = indices[i], i1 = indices[i+1], i2 = indices[i+2];
                        if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size()) continue;

                        float e1[3] = { vertices[i1].x - vertices[i0].x, vertices[i1].y - vertices[i0].y, vertices[i1].z - vertices[i0].z };
                        float e2[3] = { vertices[i2].x - vertices[i0].x, vertices[i2].y - vertices[i0].y, vertices[i2].z - vertices[i0].z };
                        float n[3];
                        Cross(e1, e2, n);

                        vertices[i0].nx += n[0]; vertices[i0].ny += n[1]; vertices[i0].nz += n[2];
                        vertices[i1].nx += n[0]; vertices[i1].ny += n[1]; vertices[i1].nz += n[2];
                        vertices[i2].nx += n[0]; vertices[i2].ny += n[1]; vertices[i2].nz += n[2];
                    }
                } else {

                    for (size_t i = 0; i + 2 < indices.size(); i++) {
                        uint16_t i0 = indices[i], i1 = indices[i+1], i2 = indices[i+2];
                        if (i0 == i1 || i1 == i2 || i0 == i2) continue;
                        if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size()) continue;

                        float e1[3] = { vertices[i1].x - vertices[i0].x, vertices[i1].y - vertices[i0].y, vertices[i1].z - vertices[i0].z };
                        float e2[3] = { vertices[i2].x - vertices[i0].x, vertices[i2].y - vertices[i0].y, vertices[i2].z - vertices[i0].z };
                        float n[3];
                        Cross(e1, e2, n);

                        if (i % 2 == 1) { n[0] = -n[0]; n[1] = -n[1]; n[2] = -n[2]; }

                        vertices[i0].nx += n[0]; vertices[i0].ny += n[1]; vertices[i0].nz += n[2];
                        vertices[i1].nx += n[0]; vertices[i1].ny += n[1]; vertices[i1].nz += n[2];
                        vertices[i2].nx += n[0]; vertices[i2].ny += n[1]; vertices[i2].nz += n[2];
                    }
                }

                for (auto& v : vertices) {
                    float len = sqrt(v.nx*v.nx + v.ny*v.ny + v.nz*v.nz);
                    if (len > 0.0001f) {
                        v.nx /= len; v.ny /= len; v.nz /= len;
                    } else {
                        v.ny = 1.0f;
                    }
                }
            }

            RenderMesh mesh;
            mesh.sourceSectionIndex = static_cast<uint32_t>(sectionIndex);
            mesh.indexCount = (int)indices.size();
            mesh.mode = (itype == 4) ? GL_TRIANGLES : GL_TRIANGLE_STRIP;
            mesh.textureId = tex;
            mesh.blendMode = mat.blendMode;
            mesh.isTranslucent = mat.isTranslucent;
            mesh.isAlphaTest = mat.isAlphaTest;
            mesh.shaderType = mat.shaderType;
            mesh.isPersonMaterial = mat.isPersonMaterial;
            mesh.personBaseColor = mat.personBaseColor;
            mesh.personLighting = mat.personLighting;
            mesh.personEnvA = mat.personEnvA;
            mesh.personEnvB = mat.personEnvB;
            mesh.personOutline = mat.personOutline;
            mesh.secondaryTextureName = mat.secondaryTextureName;
            mesh.materialShaderName = mat.alphaFlag;
            if (!mat.secondaryTextureName.empty()) {
                mesh.secondaryTextureId = LoadTextureByName(mat.secondaryTextureName);
            }

            if (stride == 64) {
                mesh.bonePalette = ResolveSectionBonePalette(
                    vertices,
                    bonePalette.palette,
                    (int)skeletonBones.size(),
                    PREVIEW_MAX_BONES);
            } else {
                mesh.bonePalette = bonePalette.palette;
            }

            mesh.isFakeShadow = false;

            std::string meshNameLower = StrToLower(meshName.empty() ? modelName : meshName);
            std::string modelNameLower = StrToLower(modelName);

            mesh.isColorVolume  = mat.isColorVolume  || IsColorVolumeMesh(meshNameLower);
            mesh.isShadowVolume = mat.isShadowVolume;

            mesh.isDebugTransparent = mesh.isColorVolume || mesh.isShadowVolume ||
                                      IsNonRenderableMeshName(modelNameLower) ||
                                      IsNonRenderableMeshName(meshNameLower);

            if (IsAdditiveGlowMeshName(modelNameLower) ||
                IsAdditiveGlowMeshName(meshNameLower)) {
                mesh.blendMode     = NGLBM_ADDITIVE;
                mesh.isTranslucent = true;
                mesh.isAlphaTest   = false;
            }

            if (IsWaterMeshName(modelNameLower) ||
                IsWaterMeshName(meshNameLower)) {
                mesh.isWater       = true;
                mesh.isTranslucent = true;
                mesh.isAlphaTest   = false;
                mesh.blendMode     = NGLBM_BLEND;
            }

            mesh.isHidden = ShouldHideMesh(meshNameLower);

            for (int i = 0; i < 3; i++) {
                mesh.bboxMin[i] = bboxMin[i];
                mesh.bboxMax[i] = bboxMax[i];
            }

            mesh.sourcePack = sourcePack;
            mesh.sourceOffset = sourceOffset;
            mesh.sourceSize = (uint32_t)pcmData.size();
            mesh.meshName = meshName.empty() ? modelName : meshName;

            mesh.skipPicking = false;

            mesh.positions.reserve(vertices.size() * 3);
            mesh.normals.reserve(vertices.size() * 3);
            mesh.uvs.reserve(vertices.size() * 2);
            mesh.colors.reserve(vertices.size() * 4);
            for (const auto& v : vertices) {
                mesh.positions.push_back(v.x);
                mesh.positions.push_back(v.y);
                mesh.positions.push_back(v.z);
                mesh.normals.push_back(v.nx);
                mesh.normals.push_back(v.ny);
                mesh.normals.push_back(v.nz);
                mesh.uvs.push_back(v.u);
                mesh.uvs.push_back(v.v);
                mesh.colors.push_back(v.cr);
                mesh.colors.push_back(v.cg);
                mesh.colors.push_back(v.cb);
                mesh.colors.push_back(v.ca);
            }
            mesh.indices = indices;

            if (!isWorldMode && loadedMorphFile.valid) {
                bool hasMorphPosition = false;
                mesh.morphPositionDeltas.resize(loadedMorphFile.sets.size());
                mesh.morphPositionChanged.resize(loadedMorphFile.sets.size());
                for (size_t targetIndex = 0;
                     targetIndex < loadedMorphFile.sets.size(); ++targetIndex) {
                    const auto& set = loadedMorphFile.sets[targetIndex];
                    if (sectionIndex >= set.sections.size()) continue;
                    const auto& target = set.sections[sectionIndex];
                    if (target.vertex_count != vertices.size() || target.position.values.empty())
                        continue;
                    mesh.morphPositionDeltas[targetIndex] = target.position.values;
                    mesh.morphPositionChanged[targetIndex] = target.position.changed;
                    hasMorphPosition = true;
                }
                if (hasMorphPosition) {
                    static_assert(sizeof(Vertex) == sizeof(float) * 20,
                                  "Morph upload assumes the preview Vertex layout");
                    mesh.morphVertexData.resize(vertices.size() * 20);
                    std::memcpy(mesh.morphVertexData.data(), vertices.data(),
                                vertices.size() * sizeof(Vertex));
                } else {
                    mesh.morphPositionDeltas.clear();
                    mesh.morphPositionChanged.clear();
                }
            }

            mesh.textureName = mat.textureName;
            if (!mat.textureName.empty()) {
                auto animationIt = textureAnimationCache.find(StrToLower(mat.textureName));
                if (animationIt != textureAnimationCache.end()) {
                    mesh.textureFrames = animationIt->second;
                }
            }
            if (!mat.textureName.empty()) {
                mesh.textureHash = CalculateCRC32(StrToLower(mat.textureName) + ".dds");
            }

            glGenVertexArrays(1, &mesh.vao); glGenBuffers(1, &mesh.vbo); glGenBuffers(1, &mesh.ebo);
            glBindVertexArray(mesh.vao);
            glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo); glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(Vertex), vertices.data(), GL_STATIC_DRAW);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.ebo); glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(uint16_t), indices.data(), GL_STATIC_DRAW);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)0); glEnableVertexAttribArray(0);
            glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)(3*sizeof(float))); glEnableVertexAttribArray(1);
            glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)(6*sizeof(float))); glEnableVertexAttribArray(2);
            glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)(8*sizeof(float))); glEnableVertexAttribArray(3);
            glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)(12*sizeof(float))); glEnableVertexAttribArray(4);
            glVertexAttribPointer(5, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)(16*sizeof(float))); glEnableVertexAttribArray(5);
            glBindVertexArray(0);
            previewMeshes.push_back(mesh);
        }
    }
}

static void TransformVertex(float& x, float& y, float& z, const float* m) {
    float ox = x, oy = y, oz = z;
    x = m[0]*ox + m[4]*oy + m[8]*oz + m[12];
    y = m[1]*ox + m[5]*oy + m[9]*oz + m[13];
    z = m[2]*ox + m[6]*oy + m[10]*oz + m[14];
}

static void TransformNormal(float& nx, float& ny, float& nz, const float* m) {
    float ox = nx, oy = ny, oz = nz;
    nx = m[0]*ox + m[4]*oy + m[8]*oz;
    ny = m[1]*ox + m[5]*oy + m[9]*oz;
    nz = m[2]*ox + m[6]*oy + m[10]*oz;

    float len = sqrt(nx*nx + ny*ny + nz*nz);
    if (len > 0.0001f) { nx /= len; ny /= len; nz /= len; }
}

bool SpiderManTool::FetchMorphBytesForHash(uint32_t meshHash, std::vector<uint8_t>& out) {
    out.clear();

    // 1) Prefer a morph co-located in the currently open pack.
    if (!pcPackData.empty()) {
        for (const auto& resource : currentDir.resources) {
            if (resource.type == RES_KEY_MORPH && resource.hash == meshHash &&
                resource.size != 0 && resource.offset <= pcPackData.size() &&
                resource.size <= pcPackData.size() - resource.offset) {
                out.assign(pcPackData.begin() + resource.offset,
                           pcPackData.begin() + resource.offset + resource.size);
                return true;
            }
        }
    }

    // 2) Otherwise resolve the same mesh's morph from any other pack. Character-viewer
    //    packs (CH_VWR_*) ship the head mesh but not the morph, which only lives in the
    //    cutscene/IGC packs; the mesh hash is identical there, so the morph still matches.
    BuildGlobalMorphIndex();
    auto it = globalMorphIndex.find(meshHash);
    if (it != globalMorphIndex.end() && it->second.size != 0) {
        std::ifstream f(it->second.packPath, std::ios::binary);
        if (f.is_open()) {
            std::vector<uint8_t> buf(it->second.size);
            f.seekg(it->second.offset);
            f.read(reinterpret_cast<char*>(buf.data()), it->second.size);
            if (f.good()) { out = std::move(buf); return true; }
        }
    }
    return false;
}

void SpiderManTool::LoadMorphForCurrentModel(uint32_t meshHash) {
    loadedMorphFile = {};
    morphTargetWeights.clear();
    focusMorphsTab = false;
    if (isWorldMode) return;

    std::vector<uint8_t> bytes;
    if (!FetchMorphBytesForHash(meshHash, bytes)) return;

    loadedMorphFile = UsmMorph::Parse(bytes);
    if (!loadedMorphFile.valid) {
        loadedMorphFile = {};
        return;
    }

    morphTargetWeights.assign(loadedMorphFile.sets.size(), 0.0f);

    // A model that actually has adjustable morph targets should surface its Morphs tab.
    if (morphTargetWeights.size() > 1) focusMorphsTab = true;
}

static void MultiplyColumnMajor4x4(const float* a, const float* b, float* out) {
    float result[16];
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            result[col * 4 + row] =
                a[0 * 4 + row] * b[col * 4 + 0] +
                a[1 * 4 + row] * b[col * 4 + 1] +
                a[2 * 4 + row] * b[col * 4 + 2] +
                a[3 * 4 + row] * b[col * 4 + 3];
        }
    }
    memcpy(out, result, sizeof(result));
}

static void IdentityColumnMajor4x4(float* out) {
    memset(out, 0, sizeof(float) * 16);
    out[0] = out[5] = out[10] = out[15] = 1.0f;
}

static void TransformBounds(const float localMin[3], const float localMax[3],
                            const float transform[16], float worldMin[3], float worldMax[3]) {
    worldMin[0] = worldMin[1] = worldMin[2] = 1e30f;
    worldMax[0] = worldMax[1] = worldMax[2] = -1e30f;
    for (int corner = 0; corner < 8; ++corner) {
        float x = (corner & 1) ? localMax[0] : localMin[0];
        float y = (corner & 2) ? localMax[1] : localMin[1];
        float z = (corner & 4) ? localMax[2] : localMin[2];
        TransformVertex(x, y, z, transform);
        worldMin[0] = std::min(worldMin[0], x);
        worldMin[1] = std::min(worldMin[1], y);
        worldMin[2] = std::min(worldMin[2], z);
        worldMax[0] = std::max(worldMax[0], x);
        worldMax[1] = std::max(worldMax[1], y);
        worldMax[2] = std::max(worldMax[2], z);
    }
}

void SpiderManTool::RefreshInstanceBuffer(RenderMesh& mesh) {
    struct InstanceGpuData {
        float transform[16];
        float stableIndex;
    };

    std::vector<InstanceGpuData> gpuInstances;
    gpuInstances.reserve(mesh.instances.size());
    for (size_t i = 0; i < mesh.instances.size(); ++i) {
        if (mesh.instances[i].isHidden) continue;
        InstanceGpuData gpu{};
        memcpy(gpu.transform, mesh.instances[i].transform.data(), sizeof(gpu.transform));
        gpu.stableIndex = (float)i;
        gpuInstances.push_back(gpu);
    }
    mesh.instanceDrawCount = (int)gpuInstances.size();

    if (!mesh.instanceVbo) glGenBuffers(1, &mesh.instanceVbo);
    glBindVertexArray(mesh.vao);
    glBindBuffer(GL_ARRAY_BUFFER, mesh.instanceVbo);
    glBufferData(GL_ARRAY_BUFFER,
                 gpuInstances.size() * sizeof(InstanceGpuData),
                 gpuInstances.empty() ? nullptr : gpuInstances.data(),
                 GL_DYNAMIC_DRAW);

    for (int column = 0; column < 4; ++column) {
        const GLuint location = 6u + (GLuint)column;
        glEnableVertexAttribArray(location);
        glVertexAttribPointer(location, 4, GL_FLOAT, GL_FALSE, sizeof(InstanceGpuData),
                              (void*)(sizeof(float) * column * 4));
        glVertexAttribDivisor(location, 1);
    }
    glEnableVertexAttribArray(10);
    glVertexAttribPointer(10, 1, GL_FLOAT, GL_FALSE, sizeof(InstanceGpuData),
                          (void*)(sizeof(float) * 16));
    glVertexAttribDivisor(10, 1);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void SpiderManTool::FlushPendingWholeWorldInstances() {
    collectWholeWorldInstances = false;
    auto pending = std::move(pendingWholeWorldInstances);
    pendingWholeWorldInstances.clear();
    size_t placementCount = 0;
    size_t drawMeshCount = 0;

    for (auto& [key, batch] : pending) {
        (void)key;
        if (batch.transforms.empty() || batch.pcmData.empty()) continue;
        placementCount += batch.transforms.size();
        const size_t firstMesh = previewMeshes.size();
        AddMeshInstancesFromDataBatched(batch.pcmData, batch.modelName, nullptr,
                                        batch.sourcePack, batch.sourceOffset,
                                        batch.transforms, batch.onlyMeshOffset);
        drawMeshCount += previewMeshes.size() - firstMesh;
        for (size_t meshIndex = firstMesh; meshIndex < previewMeshes.size(); ++meshIndex) {
            RenderMesh& mesh = previewMeshes[meshIndex];
            const size_t namedCount = std::min(mesh.instances.size(), batch.instanceNames.size());
            for (size_t instanceIndex = 0; instanceIndex < namedCount; ++instanceIndex) {
                mesh.instances[instanceIndex].name = batch.instanceNames[instanceIndex];
            }
        }
    }

    if (placementCount > 0) {

    }
}

void SpiderManTool::InstanceWholeWorldMeshes() {
    if (!isWorldMode || previewMeshes.empty()) return;

    auto makeKey = [](const RenderMesh& mesh) {
        std::ostringstream key;
        key << mesh.sourcePack << '|'
            << mesh.sourceOffset << '|'
            << mesh.sourceSize << '|'
            << mesh.sourceMeshOffset << '|'
            << mesh.textureId << '|'
            << mesh.shaderType << '|'
            << mesh.blendMode << '|'
            << mesh.mode << '|'
            << mesh.indexCount << '|'
            << mesh.positions.size();
        return key.str();
    };

    std::map<std::string, std::vector<int>> groups;
    for (int i = 0; i < (int)previewMeshes.size(); ++i) {
        const RenderMesh& mesh = previewMeshes[i];
        if (!mesh.hasPlacementTransform || !mesh.instances.empty() || mesh.isHidden ||
            mesh.sourcePack.empty() || mesh.positions.empty() || mesh.indices.empty()) {
            continue;
        }
        groups[makeKey(mesh)].push_back(i);
    }

    std::vector<char> consumed(previewMeshes.size(), 0);
    std::vector<RenderMesh> instanceBatches;
    size_t consolidatedPlacements = 0;

    for (const auto& [key, group] : groups) {
        (void)key;
        if (group.size() < 2) continue;

        const RenderMesh& first = previewMeshes[group.front()];
        float inverseFirst[16];
        if (!InvertMatrix(first.placementTransform.data(), inverseFirst)) continue;

        std::vector<RenderMesh::Instance> instances;
        instances.reserve(group.size());
        float unionMin[3] = {1e30f, 1e30f, 1e30f};
        float unionMax[3] = {-1e30f, -1e30f, -1e30f};

        for (int meshIndex : group) {
            const RenderMesh& source = previewMeshes[meshIndex];
            RenderMesh::Instance instance;
            MultiplyColumnMajor4x4(source.placementTransform.data(), inverseFirst,
                                   instance.transform.data());
            memcpy(instance.bboxMin, source.bboxMin, sizeof(instance.bboxMin));
            memcpy(instance.bboxMax, source.bboxMax, sizeof(instance.bboxMax));
            instance.name = source.placementName.empty() ? source.meshName : source.placementName;
            for (int axis = 0; axis < 3; ++axis) {
                unionMin[axis] = std::min(unionMin[axis], source.bboxMin[axis]);
                unionMax[axis] = std::max(unionMax[axis], source.bboxMax[axis]);
            }
            instances.push_back(std::move(instance));
            consumed[meshIndex] = 1;
        }

        RenderMesh batch = std::move(previewMeshes[group.front()]);

        previewMeshes[group.front()].vao = 0;
        previewMeshes[group.front()].vbo = 0;
        previewMeshes[group.front()].ebo = 0;
        previewMeshes[group.front()].instanceVbo = 0;
        batch.instances = std::move(instances);
        batch.instanceVbo = 0;
        batch.instanceDrawCount = 0;
        batch.hasPlacementTransform = false;
        batch.meshName += " x" + std::to_string(batch.instances.size());
        memcpy(batch.bboxMin, unionMin, sizeof(unionMin));
        memcpy(batch.bboxMax, unionMax, sizeof(unionMax));
        RefreshInstanceBuffer(batch);
        instanceBatches.push_back(std::move(batch));
        consolidatedPlacements += group.size();
    }

    if (instanceBatches.empty()) return;

    std::vector<RenderMesh> rebuilt;
    rebuilt.reserve(previewMeshes.size() - consolidatedPlacements + instanceBatches.size());
    for (size_t i = 0; i < previewMeshes.size(); ++i) {
        if (!consumed[i]) {
            rebuilt.push_back(std::move(previewMeshes[i]));
            continue;
        }
        if (previewMeshes[i].vao) glDeleteVertexArrays(1, &previewMeshes[i].vao);
        if (previewMeshes[i].vbo) glDeleteBuffers(1, &previewMeshes[i].vbo);
        if (previewMeshes[i].ebo) glDeleteBuffers(1, &previewMeshes[i].ebo);
        if (previewMeshes[i].instanceVbo) glDeleteBuffers(1, &previewMeshes[i].instanceVbo);
    }
    for (auto& batch : instanceBatches) rebuilt.push_back(std::move(batch));

    previewMeshes = std::move(rebuilt);
    selectedMeshIndex = -1;
    selectedMeshInstanceIndex = -1;
}

void SpiderManTool::BatchWorldMeshesByType() {
    if (!isWorldMode || previewMeshes.empty()) return;

    struct BatchVertex {
        float x, y, z;
        float nx, ny, nz;
        float u, v;
        float boneIdx[4];
        float boneWgt[4];

        float cr, cg, cb, ca;
    };

    auto canBatch = [](const RenderMesh& m) {
        return !m.isHidden &&
               m.indexCount > 0 &&
               !m.positions.empty() &&
               !m.indices.empty();
    };

    auto makeKey = [](const RenderMesh& m) {
        std::ostringstream ss;
        ss << m.sourcePack << '|'
           << m.sourceOffset << '|'
           << m.sourceSize << '|'
           << m.meshName << '|'
           << m.textureId << '|'
           << m.textureName << '|'
           << m.textureHash << '|'
           << m.shaderType << '|'
           << m.isTranslucent << '|'
           << m.isAlphaTest << '|'
           << m.blendMode;
        return ss.str();
    };

    auto appendTriangles = [](const RenderMesh& m, std::vector<uint16_t>& out) {
        size_t vertexCount = m.positions.size() / 3;
        if (m.mode == GL_TRIANGLES) {
            for (size_t i = 0; i + 2 < m.indices.size(); i += 3) {
                uint16_t i0 = m.indices[i];
                uint16_t i1 = m.indices[i + 1];
                uint16_t i2 = m.indices[i + 2];
                if (i0 == i1 || i1 == i2 || i0 == i2) continue;
                if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount) continue;
                out.push_back(i0);
                out.push_back(i1);
                out.push_back(i2);
            }
        } else if (m.mode == GL_TRIANGLE_STRIP) {
            for (size_t i = 0; i + 2 < m.indices.size(); i++) {
                uint16_t i0 = m.indices[i];
                uint16_t i1 = m.indices[i + 1];
                uint16_t i2 = m.indices[i + 2];
                if (i0 == i1 || i1 == i2 || i0 == i2) continue;
                if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount) continue;
                if ((i & 1) == 0) {
                    out.push_back(i0);
                    out.push_back(i1);
                    out.push_back(i2);
                } else {
                    out.push_back(i1);
                    out.push_back(i0);
                    out.push_back(i2);
                }
            }
        }
    };

    std::map<std::string, std::vector<int>> groups;
    for (int i = 0; i < (int)previewMeshes.size(); i++) {
        if (canBatch(previewMeshes[i])) {
            groups[makeKey(previewMeshes[i])].push_back(i);
        }
    }

    std::vector<char> consumed(previewMeshes.size(), 0);
    std::vector<RenderMesh> batchedMeshes;
    int sourceMeshCount = 0;
    int batchCount = 0;

    auto uploadBatch = [&](const RenderMesh& proto,
                           const std::vector<BatchVertex>& vertices,
                           const std::vector<uint16_t>& indices,
                           const float bboxMin[3],
                           const float bboxMax[3],
                           int mergedCount,
                           int chunkIndex) {
        if (vertices.empty() || indices.empty()) return;

        RenderMesh mesh = proto;
        mesh.vao = 0;
        mesh.vbo = 0;
        mesh.ebo = 0;
        mesh.mode = GL_TRIANGLES;
        mesh.indexCount = (int)indices.size();
        mesh.bonePalette.clear();
        mesh.positions.clear();
        mesh.normals.clear();
        mesh.uvs.clear();
        mesh.indices.clear();
        mesh.meshName = proto.meshName + " x" + std::to_string(mergedCount);
        if (chunkIndex > 0) mesh.meshName += " #" + std::to_string(chunkIndex + 1);
        for (int axis = 0; axis < 3; axis++) {
            mesh.bboxMin[axis] = bboxMin[axis];
            mesh.bboxMax[axis] = bboxMax[axis];
        }

        mesh.positions.reserve(vertices.size() * 3);
        mesh.normals.reserve(vertices.size() * 3);
        mesh.uvs.reserve(vertices.size() * 2);
        mesh.colors.reserve(vertices.size() * 4);
        for (const auto& v : vertices) {
            mesh.positions.push_back(v.x);
            mesh.positions.push_back(v.y);
            mesh.positions.push_back(v.z);
            mesh.normals.push_back(v.nx);
            mesh.normals.push_back(v.ny);
            mesh.normals.push_back(v.nz);
            mesh.uvs.push_back(v.u);
            mesh.uvs.push_back(v.v);
            mesh.colors.push_back(v.cr);
            mesh.colors.push_back(v.cg);
            mesh.colors.push_back(v.cb);
            mesh.colors.push_back(v.ca);
        }
        mesh.indices = indices;

        glGenVertexArrays(1, &mesh.vao);
        glGenBuffers(1, &mesh.vbo);
        glGenBuffers(1, &mesh.ebo);
        glBindVertexArray(mesh.vao);
        glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo);
        glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(BatchVertex), vertices.data(), GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(uint16_t), indices.data(), GL_STATIC_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(BatchVertex), (void*)0);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(BatchVertex), (void*)(3 * sizeof(float)));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(BatchVertex), (void*)(6 * sizeof(float)));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, sizeof(BatchVertex), (void*)(8 * sizeof(float)));
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, sizeof(BatchVertex), (void*)(12 * sizeof(float)));
        glEnableVertexAttribArray(4);
        glVertexAttribPointer(5, 4, GL_FLOAT, GL_FALSE, sizeof(BatchVertex), (void*)(16 * sizeof(float)));
        glEnableVertexAttribArray(5);
        glBindVertexArray(0);

        batchedMeshes.push_back(std::move(mesh));
        batchCount++;
    };

    for (const auto& [key, indicesInGroup] : groups) {
        if (indicesInGroup.size() < 2) continue;

        const RenderMesh& proto = previewMeshes[indicesInGroup[0]];
        std::vector<BatchVertex> vertices;
        std::vector<uint16_t> indices;
        int mergedInChunk = 0;
        int chunkIndex = 0;
        float bboxMin[3] = {1e30f, 1e30f, 1e30f};
        float bboxMax[3] = {-1e30f, -1e30f, -1e30f};

        auto flushChunk = [&]() {
            uploadBatch(proto, vertices, indices, bboxMin, bboxMax, mergedInChunk, chunkIndex);
            vertices.clear();
            indices.clear();
            mergedInChunk = 0;
            chunkIndex++;
            bboxMin[0] = bboxMin[1] = bboxMin[2] = 1e30f;
            bboxMax[0] = bboxMax[1] = bboxMax[2] = -1e30f;
        };

        for (int meshIdx : indicesInGroup) {
            const RenderMesh& m = previewMeshes[meshIdx];
            size_t vertexCount = m.positions.size() / 3;
            if (vertexCount == 0 || vertexCount > 65535) continue;

            std::vector<uint16_t> meshTriangles;
            appendTriangles(m, meshTriangles);
            if (meshTriangles.empty()) continue;

            if (!vertices.empty() && vertices.size() + vertexCount > 65535) {
                flushChunk();
            }

            uint16_t baseVertex = (uint16_t)vertices.size();
            for (size_t v = 0; v < vertexCount; v++) {
                BatchVertex bv{};
                bv.x = m.positions[v * 3 + 0];
                bv.y = m.positions[v * 3 + 1];
                bv.z = m.positions[v * 3 + 2];
                if (v * 3 + 2 < m.normals.size()) {
                    bv.nx = m.normals[v * 3 + 0];
                    bv.ny = m.normals[v * 3 + 1];
                    bv.nz = m.normals[v * 3 + 2];
                } else {
                    bv.ny = 1.0f;
                }
                if (v * 2 + 1 < m.uvs.size()) {
                    bv.u = m.uvs[v * 2 + 0];
                    bv.v = m.uvs[v * 2 + 1];
                }

                if (v * 4 + 3 < m.colors.size()) {
                    bv.cr = m.colors[v * 4 + 0];
                    bv.cg = m.colors[v * 4 + 1];
                    bv.cb = m.colors[v * 4 + 2];
                    bv.ca = m.colors[v * 4 + 3];
                } else {
                    bv.cr = bv.cg = bv.cb = bv.ca = 1.0f;
                }
                vertices.push_back(bv);
            }

            for (uint16_t idx : meshTriangles) {
                indices.push_back((uint16_t)(baseVertex + idx));
            }

            for (int axis = 0; axis < 3; axis++) {
                bboxMin[axis] = std::min(bboxMin[axis], m.bboxMin[axis]);
                bboxMax[axis] = std::max(bboxMax[axis], m.bboxMax[axis]);
            }

            consumed[meshIdx] = 1;
            mergedInChunk++;
            sourceMeshCount++;
        }

        if (mergedInChunk > 0) flushChunk();
    }

    if (sourceMeshCount == 0) return;

    std::vector<RenderMesh> rebuilt;
    rebuilt.reserve(previewMeshes.size() - sourceMeshCount + batchedMeshes.size());
    for (size_t i = 0; i < previewMeshes.size(); i++) {
        if (consumed[i]) {
            if (previewMeshes[i].vao) glDeleteVertexArrays(1, &previewMeshes[i].vao);
            if (previewMeshes[i].vbo) glDeleteBuffers(1, &previewMeshes[i].vbo);
            if (previewMeshes[i].ebo) glDeleteBuffers(1, &previewMeshes[i].ebo);
            if (previewMeshes[i].instanceVbo) glDeleteBuffers(1, &previewMeshes[i].instanceVbo);
            continue;
        }
        rebuilt.push_back(std::move(previewMeshes[i]));
    }
    for (auto& mesh : batchedMeshes) {
        rebuilt.push_back(std::move(mesh));
    }

    int oldCount = (int)previewMeshes.size();
    previewMeshes = std::move(rebuilt);
    selectedMeshIndex = -1;
    selectedMeshInstanceIndex = -1;
    (void)oldCount;
    (void)batchCount;
}

void SpiderManTool::AddMeshFromDataWithTransform(const std::vector<uint8_t>& pcmData, std::string modelName, std::function<unsigned int(uint32_t)> textureResolver, const std::string& sourcePack, uint32_t sourceOffset, const float* transform, uint32_t onlyMeshOffset) {
    if (collectWholeWorldInstances && transform && !sourcePack.empty()) {
        std::ostringstream key;
        key << sourcePack << '|' << sourceOffset << '|' << pcmData.size() << '|' << onlyMeshOffset;
        PendingWorldInstanceBatch& batch = pendingWholeWorldInstances[key.str()];
        if (batch.transforms.empty()) {
            batch.pcmData = pcmData;
            batch.modelName = modelName;
            batch.sourcePack = sourcePack;
            batch.sourceOffset = sourceOffset;
            batch.onlyMeshOffset = onlyMeshOffset;
        }
        std::array<float, 16> placement{};
        memcpy(placement.data(), transform, sizeof(float) * 16);
        batch.transforms.push_back(placement);
        batch.instanceNames.push_back(modelName);
        return;
    }

    ParseMaterialEntries(pcmData);

    BinaryReader br(pcmData);
    if (8 + 4 > pcmData.size()) return;
    br.Seek(8);
    uint32_t num = br.Read<uint32_t>();
    uint32_t ofs = br.Read<uint32_t>();

    if (num > 1000) return;
    if (ofs >= pcmData.size()) return;

    br.Seek(ofs);
    struct Info { uint16_t u1, type; uint32_t offset, u2; };
    std::vector<Info> infos;
    for(uint32_t i=0; i<num; i++) {
        Info inf; inf.u1 = br.Read<uint16_t>(); inf.type = br.Read<uint16_t>(); inf.offset = br.Read<uint32_t>(); inf.u2 = br.Read<uint32_t>(); infos.push_back(inf);
    }

    struct Vertex { float x,y,z; float nx,ny,nz; float u,v; float boneIdx[4]; float boneWgt[4]; float cr,cg,cb,ca; };

    bool loadedFirstLod = false;
    for(auto& inf : infos) {
        if (inf.type != 512) continue;
        if (onlyMeshOffset != 0xFFFFFFFFu && inf.offset != onlyMeshOffset) continue;
        if (!isWorldMode && loadedFirstLod) break;
        loadedFirstLod = true;

        if (inf.offset + 16 > pcmData.size()) continue;

        br.Seek(inf.offset); br.Skip(8);
        uint32_t numSm = br.Read<uint32_t>(); uint32_t infSmOfs = br.Read<uint32_t>();
        if (numSm > 256 || infSmOfs >= pcmData.size()) continue;

        br.Seek(infSmOfs);
        std::vector<std::pair<uint32_t, uint32_t>> smRefs;
        for(uint32_t s=0; s<numSm; s++) {
            uint32_t matRef = br.Read<uint32_t>();
            uint32_t smOfs = br.Read<uint32_t>();
            smRefs.push_back({matRef, smOfs});
        }

        for(auto& [matRef, smOfs] : smRefs) {
            if (smOfs + 96 > pcmData.size()) continue;

            br.Seek(smOfs);
            uint32_t meshNameRef = br.Read<uint32_t>();
            uint32_t ptrShaderRef = br.Read<uint32_t>();

            std::string meshName;
            if (meshNameRef != 0 && meshNameRef + 32 <= pcmData.size()) {
                size_t strStart = meshNameRef + 4;
                size_t end = strStart;
                while (end < strStart + 28 && end < pcmData.size() && pcmData[end] != 0) end++;
                meshName = std::string((char*)&pcmData[strStart], end - strStart);
            }

            MaterialDef mat = ResolveMaterialByMeshOffset(meshNameRef);
            unsigned int tex = 0;

            if (!mat.textureName.empty()) {
                if (textureResolver) {
                    std::string texNameLower = StrToLower(mat.textureName);
                    uint32_t hash1 = CalculateCRC32(texNameLower + ".dds");
                    tex = textureResolver(hash1);
                    if (tex == 0) {
                        uint32_t hash2 = CalculateCRC32(texNameLower);
                        tex = textureResolver(hash2);
                    }
                } else {
                    tex = LoadTextureByName(mat.textureName);
                }
            }

            if (tex == 0 && !modelName.empty()) {
                std::string cleanName = modelName;
                size_t lastDot = cleanName.find_last_of(".");
                if(lastDot != std::string::npos) cleanName = cleanName.substr(0, lastDot);
                cleanName = StrToLower(cleanName);

                if (textureResolver) {
                    uint32_t diffHash = CalculateCRC32(cleanName + "_d.dds");
                    tex = textureResolver(diffHash);
                } else {
                    tex = LoadTextureByName(cleanName + "_d");
                    if (tex == 0) tex = LoadTextureByName(cleanName);
                }
            }

            if (tex == 0 && !mat.meshName.empty()) {
                if (textureResolver) {
                    std::string meshNameLower = StrToLower(mat.meshName);
                    uint32_t hash4 = CalculateCRC32(meshNameLower + ".dds");
                    tex = textureResolver(hash4);
                } else {
                    tex = LoadTextureByName(mat.meshName);
                }
            }

            br.Seek(smOfs + 40);
            uint32_t itype = br.Read<uint32_t>(); uint32_t inum = br.Read<uint32_t>(); uint32_t iofs = br.Read<uint32_t>();
            br.Skip(4); uint32_t vnum = br.Read<uint32_t>(); uint32_t vofs = br.Read<uint32_t>();
            br.Skip(8); uint32_t stride = br.Read<uint32_t>();

            if (vnum > 100000 || inum > 300000 || vofs >= pcmData.size() || iofs >= pcmData.size() || stride == 0) continue;

            PCMSectionBonePalette bonePalette;
            bonePalette.load(pcmData, smOfs);

            br.Seek(vofs);
            std::vector<Vertex> vertices;
            bool valid = true;

            float bboxMin[3] = {1e30f, 1e30f, 1e30f};
            float bboxMax[3] = {-1e30f, -1e30f, -1e30f};

            for(uint32_t v=0; v<vnum; v++) {
                size_t startV = br.Tell();
                if (startV + stride > pcmData.size()) { valid = false; break; }
                Vertex vert;
                memset(&vert, 0, sizeof(vert));
                vert.x = br.Read<float>(); vert.y = br.Read<float>(); vert.z = br.Read<float>();

                vert.cr = vert.cg = vert.cb = vert.ca = 1.0f;

                if (stride == 64) {
                    if (startV + 24 <= pcmData.size()) {
                        br.Seek(startV + 12);
                        vert.nx = br.Read<float>(); vert.ny = br.Read<float>(); vert.nz = br.Read<float>();
                    }
                    if (startV + 32 <= pcmData.size()) {
                        br.Seek(startV + 24);
                        vert.u = br.Read<float>(); vert.v = br.Read<float>();
                    }

                    if (startV + 64 <= pcmData.size()) {
                        br.Seek(startV + 32);
                        for (int bi = 0; bi < 4; bi++) vert.boneIdx[bi] = br.Read<float>();
                        for (int bi = 0; bi < 4; bi++) vert.boneWgt[bi] = br.Read<float>();
                        float wTotal = vert.boneWgt[0] + vert.boneWgt[1] + vert.boneWgt[2] + vert.boneWgt[3];
                        if (wTotal > 1e-8f) {
                            float inv = 1.f / wTotal;
                            for (int bi = 0; bi < 4; bi++) vert.boneWgt[bi] *= inv;
                        } else {
                            for (int bi = 0; bi < 4; bi++) vert.boneWgt[bi] = 0.f;
                        }
                    }
                } else if (stride == 24) {
                    if (startV + 20 <= pcmData.size()) {
                        br.Seek(startV + 12);
                        vert.u = br.Read<float>(); vert.v = br.Read<float>();
                    }
                    if (startV + 24 <= pcmData.size()) {
                        br.Seek(startV + 20);
                        uint32_t packed = br.Read<uint32_t>();
                        DecodeD3DColor(packed, vert.cr, vert.cg, vert.cb, vert.ca);
                    }
                } else if (stride == 32 || stride == 60) {
                    if (startV + 20 <= pcmData.size()) {
                        br.Seek(startV + 12);
                        vert.u = br.Read<float>(); vert.v = br.Read<float>();
                    }
                    if (startV + 24 <= pcmData.size()) {
                        br.Seek(startV + 20);
                        uint32_t packed = br.Read<uint32_t>();
                        DecodeD3DColor(packed, vert.cr, vert.cg, vert.cb, vert.ca);
                    }
                }

                if (transform) {
                    TransformVertex(vert.x, vert.y, vert.z, transform);
                    if (stride == 64) {
                        TransformNormal(vert.nx, vert.ny, vert.nz, transform);
                    }
                }

                if (vert.x < bboxMin[0]) bboxMin[0] = vert.x;
                if (vert.y < bboxMin[1]) bboxMin[1] = vert.y;
                if (vert.z < bboxMin[2]) bboxMin[2] = vert.z;
                if (vert.x > bboxMax[0]) bboxMax[0] = vert.x;
                if (vert.y > bboxMax[1]) bboxMax[1] = vert.y;
                if (vert.z > bboxMax[2]) bboxMax[2] = vert.z;

                vertices.push_back(vert);
                br.Seek(startV + stride);
            }
            if (!valid) continue;

            br.Seek(iofs);
            std::vector<uint16_t> indices;
            if (iofs + inum * 2 > pcmData.size()) continue;
            for(uint32_t i=0; i<inum; i++) indices.push_back(br.Read<uint16_t>());
            if (vertices.empty() || indices.empty()) continue;

            if (stride != 64 && !vertices.empty() && !indices.empty()) {
                for (auto& v : vertices) { v.nx = 0; v.ny = 0; v.nz = 0; }

                if (itype == 4) {
                    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
                        uint16_t i0 = indices[i], i1 = indices[i+1], i2 = indices[i+2];
                        if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size()) continue;
                        float e1[3] = { vertices[i1].x - vertices[i0].x, vertices[i1].y - vertices[i0].y, vertices[i1].z - vertices[i0].z };
                        float e2[3] = { vertices[i2].x - vertices[i0].x, vertices[i2].y - vertices[i0].y, vertices[i2].z - vertices[i0].z };
                        float n[3]; Cross(e1, e2, n);
                        vertices[i0].nx += n[0]; vertices[i0].ny += n[1]; vertices[i0].nz += n[2];
                        vertices[i1].nx += n[0]; vertices[i1].ny += n[1]; vertices[i1].nz += n[2];
                        vertices[i2].nx += n[0]; vertices[i2].ny += n[1]; vertices[i2].nz += n[2];
                    }
                } else {
                    for (size_t i = 0; i + 2 < indices.size(); i++) {
                        uint16_t i0 = indices[i], i1 = indices[i+1], i2 = indices[i+2];
                        if (i0 == i1 || i1 == i2 || i0 == i2) continue;
                        if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size()) continue;
                        float e1[3] = { vertices[i1].x - vertices[i0].x, vertices[i1].y - vertices[i0].y, vertices[i1].z - vertices[i0].z };
                        float e2[3] = { vertices[i2].x - vertices[i0].x, vertices[i2].y - vertices[i0].y, vertices[i2].z - vertices[i0].z };
                        float n[3]; Cross(e1, e2, n);
                        if (i % 2 == 1) { n[0] = -n[0]; n[1] = -n[1]; n[2] = -n[2]; }
                        vertices[i0].nx += n[0]; vertices[i0].ny += n[1]; vertices[i0].nz += n[2];
                        vertices[i1].nx += n[0]; vertices[i1].ny += n[1]; vertices[i1].nz += n[2];
                        vertices[i2].nx += n[0]; vertices[i2].ny += n[1]; vertices[i2].nz += n[2];
                    }
                }

                for (auto& v : vertices) {
                    float len = sqrt(v.nx*v.nx + v.ny*v.ny + v.nz*v.nz);
                    if (len > 0.0001f) { v.nx /= len; v.ny /= len; v.nz /= len; }
                    else { v.ny = 1.0f; }
                }
            }

            RenderMesh mesh;
            mesh.indexCount = (int)indices.size();
            mesh.mode = (itype == 4) ? GL_TRIANGLES : GL_TRIANGLE_STRIP;
            mesh.textureId = tex;
            mesh.blendMode = mat.blendMode;
            mesh.isTranslucent = mat.isTranslucent;
            mesh.isAlphaTest = mat.isAlphaTest;
            mesh.shaderType = mat.shaderType;
            mesh.isPersonMaterial = mat.isPersonMaterial;
            mesh.personBaseColor = mat.personBaseColor;
            mesh.personLighting = mat.personLighting;
            mesh.personEnvA = mat.personEnvA;
            mesh.personEnvB = mat.personEnvB;
            mesh.personOutline = mat.personOutline;
            mesh.secondaryTextureName = mat.secondaryTextureName;
            mesh.materialShaderName = mat.alphaFlag;
            if (!mat.secondaryTextureName.empty()) {
                mesh.secondaryTextureId = LoadTextureByName(mat.secondaryTextureName);
            }
            if (stride == 64) {
                mesh.bonePalette = ResolveSectionBonePalette(
                    vertices,
                    bonePalette.palette,
                    (int)skeletonBones.size(),
                    PREVIEW_MAX_BONES);
            } else {
                mesh.bonePalette = bonePalette.palette;
            }

            mesh.isFakeShadow = false;

            std::string meshNameLower = StrToLower(meshName.empty() ? modelName : meshName);
            std::string modelNameLower = StrToLower(modelName);
            mesh.isColorVolume  = mat.isColorVolume  || IsColorVolumeMesh(meshNameLower);
            mesh.isShadowVolume = mat.isShadowVolume;

            mesh.isDebugTransparent = mesh.isColorVolume || mesh.isShadowVolume ||
                                      IsNonRenderableMeshName(modelNameLower) ||
                                      IsNonRenderableMeshName(meshNameLower);
            if (IsAdditiveGlowMeshName(modelNameLower) ||
                IsAdditiveGlowMeshName(meshNameLower)) {
                mesh.blendMode     = NGLBM_ADDITIVE;
                mesh.isTranslucent = true;
                mesh.isAlphaTest   = false;
            }
            if (IsWaterMeshName(modelNameLower) ||
                IsWaterMeshName(meshNameLower)) {
                mesh.isWater       = true;
                mesh.isTranslucent = true;
                mesh.isAlphaTest   = false;
                mesh.blendMode     = NGLBM_BLEND;
            }
            mesh.isHidden = ShouldHideMesh(meshNameLower);

            for (int i = 0; i < 3; i++) {
                mesh.bboxMin[i] = bboxMin[i];
                mesh.bboxMax[i] = bboxMax[i];
            }

            mesh.sourcePack = sourcePack;
            mesh.sourceOffset = sourceOffset;
            mesh.sourceSize = (uint32_t)pcmData.size();
            mesh.sourceMeshOffset = smOfs;
            mesh.meshName = meshName.empty() ? modelName : meshName;
            mesh.placementName = modelName;
            if (transform) {
                mesh.hasPlacementTransform = true;
                memcpy(mesh.placementTransform.data(), transform, sizeof(float) * 16);
            }

            mesh.skipPicking = false;

            mesh.positions.reserve(vertices.size() * 3);
            mesh.normals.reserve(vertices.size() * 3);
            mesh.uvs.reserve(vertices.size() * 2);
            mesh.colors.reserve(vertices.size() * 4);
            for (const auto& v : vertices) {
                mesh.positions.push_back(v.x);
                mesh.positions.push_back(v.y);
                mesh.positions.push_back(v.z);
                mesh.normals.push_back(v.nx);
                mesh.normals.push_back(v.ny);
                mesh.normals.push_back(v.nz);
                mesh.uvs.push_back(v.u);
                mesh.uvs.push_back(v.v);
                mesh.colors.push_back(v.cr);
                mesh.colors.push_back(v.cg);
                mesh.colors.push_back(v.cb);
                mesh.colors.push_back(v.ca);
            }
            mesh.indices = indices;

            mesh.textureName = mat.textureName;
            if (!mat.textureName.empty()) {
                auto animationIt = textureAnimationCache.find(StrToLower(mat.textureName));
                if (animationIt != textureAnimationCache.end()) {
                    mesh.textureFrames = animationIt->second;
                }
            }
            if (!mat.textureName.empty()) {
                mesh.textureHash = CalculateCRC32(StrToLower(mat.textureName) + ".dds");
            }

            glGenVertexArrays(1, &mesh.vao); glGenBuffers(1, &mesh.vbo); glGenBuffers(1, &mesh.ebo);
            glBindVertexArray(mesh.vao);
            glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo); glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(Vertex), vertices.data(), GL_STATIC_DRAW);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.ebo); glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(uint16_t), indices.data(), GL_STATIC_DRAW);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)0); glEnableVertexAttribArray(0);
            glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)(3*sizeof(float))); glEnableVertexAttribArray(1);
            glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)(6*sizeof(float))); glEnableVertexAttribArray(2);
            glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)(8*sizeof(float))); glEnableVertexAttribArray(3);
            glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)(12*sizeof(float))); glEnableVertexAttribArray(4);
            glVertexAttribPointer(5, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)(16*sizeof(float))); glEnableVertexAttribArray(5);
            glBindVertexArray(0);
            previewMeshes.push_back(mesh);
        }
    }
}

void SpiderManTool::AddMeshInstancesFromDataBatched(
    const std::vector<uint8_t>& pcmData,
    std::string modelName,
    std::function<unsigned int(uint32_t)> textureResolver,
    const std::string& sourcePack,
    uint32_t sourceOffset,
    const std::vector<std::array<float, 16>>& transforms,
    uint32_t onlyMeshOffset) {
    if (transforms.empty()) return;

    {
        const size_t firstMesh = previewMeshes.size();
        AddMeshFromDataWithTransform(pcmData, modelName, textureResolver,
                                     sourcePack, sourceOffset, nullptr,
                                     onlyMeshOffset);

        for (size_t meshIndex = firstMesh; meshIndex < previewMeshes.size(); ++meshIndex) {
            RenderMesh& mesh = previewMeshes[meshIndex];
            const float localMin[3] = {mesh.bboxMin[0], mesh.bboxMin[1], mesh.bboxMin[2]};
            const float localMax[3] = {mesh.bboxMax[0], mesh.bboxMax[1], mesh.bboxMax[2]};
            float unionMin[3] = {1e30f, 1e30f, 1e30f};
            float unionMax[3] = {-1e30f, -1e30f, -1e30f};

            mesh.instances.clear();
            mesh.instances.reserve(transforms.size());
            for (const auto& transform : transforms) {
                RenderMesh::Instance instance;
                instance.transform = transform;
                instance.name = modelName;
                TransformBounds(localMin, localMax, transform.data(),
                                instance.bboxMin, instance.bboxMax);
                for (int axis = 0; axis < 3; ++axis) {
                    unionMin[axis] = std::min(unionMin[axis], instance.bboxMin[axis]);
                    unionMax[axis] = std::max(unionMax[axis], instance.bboxMax[axis]);
                }
                mesh.instances.push_back(std::move(instance));
            }

            memcpy(mesh.bboxMin, unionMin, sizeof(unionMin));
            memcpy(mesh.bboxMax, unionMax, sizeof(unionMax));
            mesh.hasPlacementTransform = false;
            mesh.meshName += " x" + std::to_string(mesh.instances.size());
            RefreshInstanceBuffer(mesh);
        }
    }
    return;

    ParseMaterialEntries(pcmData);

    BinaryReader br(pcmData);
    if (8 + 4 > pcmData.size()) return;
    br.Seek(8);
    uint32_t num = br.Read<uint32_t>();
    uint32_t ofs = br.Read<uint32_t>();

    if (num > 1000) return;
    if (ofs >= pcmData.size()) return;

    br.Seek(ofs);
    struct Info { uint16_t u1, type; uint32_t offset, u2; };
    std::vector<Info> infos;
    for (uint32_t i = 0; i < num; i++) {
        Info inf;
        inf.u1 = br.Read<uint16_t>();
        inf.type = br.Read<uint16_t>();
        inf.offset = br.Read<uint32_t>();
        inf.u2 = br.Read<uint32_t>();
        infos.push_back(inf);
    }

    struct Vertex {
        float x, y, z;
        float nx, ny, nz;
        float u, v;
        float boneIdx[4];
        float boneWgt[4];
        float cr, cg, cb, ca;
    };

    auto appendTriangles = [](uint32_t primitiveType,
                              const std::vector<uint16_t>& srcIndices,
                              size_t vertexCount,
                              std::vector<uint16_t>& out) {
        if (primitiveType == 4) {
            for (size_t i = 0; i + 2 < srcIndices.size(); i += 3) {
                uint16_t i0 = srcIndices[i];
                uint16_t i1 = srcIndices[i + 1];
                uint16_t i2 = srcIndices[i + 2];
                if (i0 == i1 || i1 == i2 || i0 == i2) continue;
                if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount) continue;
                out.push_back(i0);
                out.push_back(i1);
                out.push_back(i2);
            }
        } else {
            for (size_t i = 0; i + 2 < srcIndices.size(); i++) {
                uint16_t i0 = srcIndices[i];
                uint16_t i1 = srcIndices[i + 1];
                uint16_t i2 = srcIndices[i + 2];
                if (i0 == i1 || i1 == i2 || i0 == i2) continue;
                if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount) continue;
                if ((i & 1) == 0) {
                    out.push_back(i0);
                    out.push_back(i1);
                    out.push_back(i2);
                } else {
                    out.push_back(i1);
                    out.push_back(i0);
                    out.push_back(i2);
                }
            }
        }
    };

    auto generateNormals = [](std::vector<Vertex>& vertices,
                              const std::vector<uint16_t>& triangleIndices) {
        for (auto& v : vertices) {
            v.nx = 0.0f;
            v.ny = 0.0f;
            v.nz = 0.0f;
        }

        for (size_t i = 0; i + 2 < triangleIndices.size(); i += 3) {
            uint16_t i0 = triangleIndices[i];
            uint16_t i1 = triangleIndices[i + 1];
            uint16_t i2 = triangleIndices[i + 2];
            if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size()) continue;

            float e1[3] = {
                vertices[i1].x - vertices[i0].x,
                vertices[i1].y - vertices[i0].y,
                vertices[i1].z - vertices[i0].z
            };
            float e2[3] = {
                vertices[i2].x - vertices[i0].x,
                vertices[i2].y - vertices[i0].y,
                vertices[i2].z - vertices[i0].z
            };
            float n[3];
            Cross(e1, e2, n);

            vertices[i0].nx += n[0]; vertices[i0].ny += n[1]; vertices[i0].nz += n[2];
            vertices[i1].nx += n[0]; vertices[i1].ny += n[1]; vertices[i1].nz += n[2];
            vertices[i2].nx += n[0]; vertices[i2].ny += n[1]; vertices[i2].nz += n[2];
        }

        for (auto& v : vertices) {
            float len = sqrt(v.nx * v.nx + v.ny * v.ny + v.nz * v.nz);
            if (len > 0.0001f) {
                v.nx /= len;
                v.ny /= len;
                v.nz /= len;
            } else {
                v.ny = 1.0f;
            }
        }
    };

    auto uploadBatch = [&](const std::vector<Vertex>& vertices,
                           const std::vector<uint16_t>& indices,
                           const float bboxMin[3],
                           const float bboxMax[3],
                           const MaterialDef& mat,
                           const std::string& meshName,
                           unsigned int tex,
                           const std::vector<uint16_t>& bonePalette,
                           int instanceCount,
                           int chunkIndex) {
        if (vertices.empty() || indices.empty()) return;

        RenderMesh mesh;
        mesh.indexCount = (int)indices.size();
        mesh.mode = GL_TRIANGLES;
        mesh.textureId = tex;
        mesh.blendMode = mat.blendMode;
        mesh.isTranslucent = mat.isTranslucent;
        mesh.isAlphaTest = mat.isAlphaTest;
        mesh.shaderType = mat.shaderType;
        mesh.isPersonMaterial = mat.isPersonMaterial;
        mesh.personBaseColor = mat.personBaseColor;
        mesh.personLighting = mat.personLighting;
        mesh.personEnvA = mat.personEnvA;
        mesh.personEnvB = mat.personEnvB;
        mesh.personOutline = mat.personOutline;
        mesh.secondaryTextureName = mat.secondaryTextureName;
        mesh.materialShaderName = mat.alphaFlag;
        if (!mat.secondaryTextureName.empty()) {
            mesh.secondaryTextureId = LoadTextureByName(mat.secondaryTextureName);
        }
        mesh.bonePalette = bonePalette;
        mesh.isFakeShadow = false;

        std::string displayName = meshName.empty() ? modelName : meshName;
        std::string meshNameLower = StrToLower(displayName);
        std::string modelNameLower = StrToLower(modelName);
        mesh.isColorVolume = mat.isColorVolume || IsColorVolumeMesh(meshNameLower);
        mesh.isShadowVolume = mat.isShadowVolume;
        mesh.isDebugTransparent = mesh.isColorVolume || mesh.isShadowVolume ||
                                  IsNonRenderableMeshName(modelNameLower) ||
                                  IsNonRenderableMeshName(meshNameLower);
        mesh.isHidden = ShouldHideMesh(meshNameLower);

        for (int i = 0; i < 3; i++) {
            mesh.bboxMin[i] = bboxMin[i];
            mesh.bboxMax[i] = bboxMax[i];
        }

        mesh.sourcePack = sourcePack;
        mesh.sourceOffset = sourceOffset;
        mesh.sourceSize = (uint32_t)pcmData.size();
        mesh.meshName = displayName + " x" + std::to_string(instanceCount);
        if (chunkIndex > 0) {
            mesh.meshName += " #" + std::to_string(chunkIndex + 1);
        }
        mesh.skipPicking = false;

        mesh.positions.reserve(vertices.size() * 3);
        mesh.normals.reserve(vertices.size() * 3);
        mesh.uvs.reserve(vertices.size() * 2);
        mesh.colors.reserve(vertices.size() * 4);
        for (const auto& v : vertices) {
            mesh.positions.push_back(v.x);
            mesh.positions.push_back(v.y);
            mesh.positions.push_back(v.z);
            mesh.normals.push_back(v.nx);
            mesh.normals.push_back(v.ny);
            mesh.normals.push_back(v.nz);
            mesh.uvs.push_back(v.u);
            mesh.uvs.push_back(v.v);
            mesh.colors.push_back(v.cr);
            mesh.colors.push_back(v.cg);
            mesh.colors.push_back(v.cb);
            mesh.colors.push_back(v.ca);
        }
        mesh.indices = indices;

        mesh.textureName = mat.textureName;
        if (!mat.textureName.empty()) {
            auto animationIt = textureAnimationCache.find(StrToLower(mat.textureName));
            if (animationIt != textureAnimationCache.end()) {
                mesh.textureFrames = animationIt->second;
            }
        }
        if (!mat.textureName.empty()) {
            mesh.textureHash = CalculateCRC32(StrToLower(mat.textureName) + ".dds");
        }

        glGenVertexArrays(1, &mesh.vao);
        glGenBuffers(1, &mesh.vbo);
        glGenBuffers(1, &mesh.ebo);
        glBindVertexArray(mesh.vao);
        glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo);
        glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(Vertex), vertices.data(), GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(uint16_t), indices.data(), GL_STATIC_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)0);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)(3 * sizeof(float)));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)(6 * sizeof(float)));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)(8 * sizeof(float)));
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)(12 * sizeof(float)));
        glEnableVertexAttribArray(4);
        glVertexAttribPointer(5, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)(16 * sizeof(float)));
        glEnableVertexAttribArray(5);
        glBindVertexArray(0);

        previewMeshes.push_back(std::move(mesh));
    };

    bool loadedFirstLod = false;
    constexpr size_t kMaxBatchVertices = 60000;

    for (auto& inf : infos) {
        if (inf.type != 512) continue;
        if (onlyMeshOffset != 0xFFFFFFFFu && inf.offset != onlyMeshOffset) continue;
        if (!isWorldMode && loadedFirstLod) break;
        loadedFirstLod = true;

        if (inf.offset + 16 > pcmData.size()) continue;

        br.Seek(inf.offset);
        br.Skip(8);
        uint32_t numSm = br.Read<uint32_t>();
        uint32_t infSmOfs = br.Read<uint32_t>();
        if (numSm > 256 || infSmOfs >= pcmData.size()) continue;

        br.Seek(infSmOfs);
        std::vector<std::pair<uint32_t, uint32_t>> smRefs;
        for (uint32_t s = 0; s < numSm; s++) {
            uint32_t matRef = br.Read<uint32_t>();
            uint32_t smOfs = br.Read<uint32_t>();
            smRefs.push_back({matRef, smOfs});
        }

        for (auto& [matRef, smOfs] : smRefs) {
            (void)matRef;
            if (smOfs + 96 > pcmData.size()) continue;

            br.Seek(smOfs);
            uint32_t meshNameRef = br.Read<uint32_t>();
            br.Read<uint32_t>();

            std::string meshName;
            if (meshNameRef != 0 && meshNameRef + 32 <= pcmData.size()) {
                size_t strStart = meshNameRef + 4;
                size_t end = strStart;
                while (end < strStart + 28 && end < pcmData.size() && pcmData[end] != 0) end++;
                meshName = std::string((char*)&pcmData[strStart], end - strStart);
            }

            MaterialDef mat = ResolveMaterialByMeshOffset(meshNameRef);
            unsigned int tex = 0;

            if (!mat.textureName.empty()) {
                if (textureResolver) {
                    std::string texNameLower = StrToLower(mat.textureName);
                    uint32_t hash1 = CalculateCRC32(texNameLower + ".dds");
                    tex = textureResolver(hash1);
                    if (tex == 0) {
                        uint32_t hash2 = CalculateCRC32(texNameLower);
                        tex = textureResolver(hash2);
                    }
                } else {
                    tex = LoadTextureByName(mat.textureName);
                }
            }

            if (tex == 0 && !modelName.empty()) {
                std::string cleanName = modelName;
                size_t lastDot = cleanName.find_last_of(".");
                if (lastDot != std::string::npos) cleanName = cleanName.substr(0, lastDot);
                cleanName = StrToLower(cleanName);

                if (textureResolver) {
                    tex = textureResolver(CalculateCRC32(cleanName + "_d.dds"));
                } else {
                    tex = LoadTextureByName(cleanName + "_d");
                    if (tex == 0) tex = LoadTextureByName(cleanName);
                }
            }

            if (tex == 0 && !mat.meshName.empty()) {
                if (textureResolver) {
                    std::string meshNameLower = StrToLower(mat.meshName);
                    tex = textureResolver(CalculateCRC32(meshNameLower + ".dds"));
                } else {
                    tex = LoadTextureByName(mat.meshName);
                }
            }

            br.Seek(smOfs + 40);
            uint32_t itype = br.Read<uint32_t>();
            uint32_t inum = br.Read<uint32_t>();
            uint32_t iofs = br.Read<uint32_t>();
            br.Skip(4);
            uint32_t vnum = br.Read<uint32_t>();
            uint32_t vofs = br.Read<uint32_t>();
            br.Skip(8);
            uint32_t stride = br.Read<uint32_t>();

            if (vnum > 100000 || inum > 300000 || vofs >= pcmData.size() ||
                iofs >= pcmData.size() || stride == 0) {
                continue;
            }

            PCMSectionBonePalette sectionBonePalette;
            sectionBonePalette.load(pcmData, smOfs);

            br.Seek(vofs);
            std::vector<Vertex> baseVertices;
            baseVertices.reserve(vnum);
            bool valid = true;

            for (uint32_t v = 0; v < vnum; v++) {
                size_t startV = br.Tell();
                if (startV + stride > pcmData.size()) {
                    valid = false;
                    break;
                }

                Vertex vert;
                memset(&vert, 0, sizeof(vert));
                vert.x = br.Read<float>();
                vert.y = br.Read<float>();
                vert.z = br.Read<float>();
                vert.cr = vert.cg = vert.cb = vert.ca = 1.0f;

                if (stride == 64) {
                    if (startV + 24 <= pcmData.size()) {
                        br.Seek(startV + 12);
                        vert.nx = br.Read<float>();
                        vert.ny = br.Read<float>();
                        vert.nz = br.Read<float>();
                    }
                    if (startV + 32 <= pcmData.size()) {
                        br.Seek(startV + 24);
                        vert.u = br.Read<float>();
                        vert.v = br.Read<float>();
                    }
                    if (startV + 64 <= pcmData.size()) {
                        br.Seek(startV + 32);
                        for (int bi = 0; bi < 4; bi++) vert.boneIdx[bi] = br.Read<float>();
                        for (int bi = 0; bi < 4; bi++) vert.boneWgt[bi] = br.Read<float>();
                        float wTotal = vert.boneWgt[0] + vert.boneWgt[1] + vert.boneWgt[2] + vert.boneWgt[3];
                        if (wTotal > 1e-8f) {
                            float inv = 1.0f / wTotal;
                            for (int bi = 0; bi < 4; bi++) vert.boneWgt[bi] *= inv;
                        } else {
                            for (int bi = 0; bi < 4; bi++) vert.boneWgt[bi] = 0.0f;
                        }
                    }
                } else if (stride == 24 || stride == 32 || stride == 60) {
                    if (startV + 20 <= pcmData.size()) {
                        br.Seek(startV + 12);
                        vert.u = br.Read<float>();
                        vert.v = br.Read<float>();
                    }
                    if (startV + 24 <= pcmData.size()) {
                        br.Seek(startV + 20);
                        uint32_t packed = br.Read<uint32_t>();
                        DecodeD3DColor(packed, vert.cr, vert.cg, vert.cb, vert.ca);
                    }
                }

                baseVertices.push_back(vert);
                br.Seek(startV + stride);
            }
            if (!valid || baseVertices.empty()) continue;

            if (iofs + (size_t)inum * 2 > pcmData.size()) continue;
            br.Seek(iofs);
            std::vector<uint16_t> sourceIndices;
            sourceIndices.reserve(inum);
            for (uint32_t i = 0; i < inum; i++) {
                sourceIndices.push_back(br.Read<uint16_t>());
            }
            if (sourceIndices.empty()) continue;

            std::vector<uint16_t> triangleIndices;
            appendTriangles(itype, sourceIndices, baseVertices.size(), triangleIndices);
            if (triangleIndices.empty()) continue;

            std::vector<uint16_t> resolvedBonePalette =
                (stride == 64)
                    ? ResolveSectionBonePalette(baseVertices, sectionBonePalette.palette,
                                                (int)skeletonBones.size(), PREVIEW_MAX_BONES)
                    : sectionBonePalette.palette;

            if (baseVertices.size() > kMaxBatchVertices) {
                for (const auto& transform : transforms) {
                    AddMeshFromDataWithTransform(pcmData, modelName, textureResolver,
                                                 sourcePack, sourceOffset,
                                                 transform.data(), onlyMeshOffset);
                }
                continue;
            }

            std::vector<Vertex> batchVertices;
            std::vector<uint16_t> batchIndices;
            int instancesInChunk = 0;
            int chunkIndex = 0;
            float bboxMin[3] = {1e30f, 1e30f, 1e30f};
            float bboxMax[3] = {-1e30f, -1e30f, -1e30f};

            auto resetBounds = [&]() {
                bboxMin[0] = bboxMin[1] = bboxMin[2] = 1e30f;
                bboxMax[0] = bboxMax[1] = bboxMax[2] = -1e30f;
            };

            auto flushChunk = [&]() {
                uploadBatch(batchVertices, batchIndices, bboxMin, bboxMax, mat, meshName,
                            tex, resolvedBonePalette, instancesInChunk, chunkIndex);
                batchVertices.clear();
                batchIndices.clear();
                instancesInChunk = 0;
                chunkIndex++;
                resetBounds();
            };

            batchVertices.reserve(std::min(kMaxBatchVertices, baseVertices.size() * transforms.size()));
            batchIndices.reserve(std::min(kMaxBatchVertices * 3, triangleIndices.size() * transforms.size()));

            for (const auto& transform : transforms) {
                if (!batchVertices.empty() &&
                    batchVertices.size() + baseVertices.size() > kMaxBatchVertices) {
                    flushChunk();
                }

                std::vector<Vertex> instanceVertices = baseVertices;
                for (auto& vert : instanceVertices) {
                    TransformVertex(vert.x, vert.y, vert.z, transform.data());
                    if (stride == 64) {
                        TransformNormal(vert.nx, vert.ny, vert.nz, transform.data());
                    }
                }
                if (stride != 64) {
                    generateNormals(instanceVertices, triangleIndices);
                }

                uint16_t baseVertex = (uint16_t)batchVertices.size();
                for (const auto& vert : instanceVertices) {
                    bboxMin[0] = std::min(bboxMin[0], vert.x);
                    bboxMin[1] = std::min(bboxMin[1], vert.y);
                    bboxMin[2] = std::min(bboxMin[2], vert.z);
                    bboxMax[0] = std::max(bboxMax[0], vert.x);
                    bboxMax[1] = std::max(bboxMax[1], vert.y);
                    bboxMax[2] = std::max(bboxMax[2], vert.z);
                    batchVertices.push_back(vert);
                }

                for (uint16_t idx : triangleIndices) {
                    batchIndices.push_back((uint16_t)(baseVertex + idx));
                }
                instancesInChunk++;
            }

            if (instancesInChunk > 0) {
                flushChunk();
            }
        }
    }
}

void SpiderManTool::LoadBackgroundMeshes() {

    std::vector<std::pair<std::string, std::string>> backgroundModels = {
        {"oceanmesh", "city_arena"},
        {"sky_day", "city_arena"}
    };

    for (const auto& [modelName, packName] : backgroundModels) {

        std::string packPath;
        for (const auto& path : foundPacks) {
            std::string stem = StrToLower(path.stem().string());
            if (stem == packName) {
                packPath = path.string();
                break;
            }
        }

        if (packPath.empty()) continue;

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
            if (r.size <= 4 || (size_t)r.offset + r.size > fileSize) continue;
            std::string entryName = "";
            if (dictionary.count(r.hash)) entryName = StrToLower(dictionary[r.hash]);
            if (entryName != modelName) continue;

            file.seekg(r.offset);
            uint32_t sig = 0;
            file.read((char*)&sig, 4);
            if (!file.good() || sig != 0x204D4350) { file.clear(); continue; }

            file.seekg(r.offset);
            std::vector<uint8_t> pcmData(r.size);
            file.read((char*)pcmData.data(), r.size);
            AddMeshFromData(pcmData, modelName, nullptr);
            break;
        }

        file.close();
    }
}

void SpiderManTool::LoadModelToGL(int index) {
    isWorldMode = false;
    if (index < 0 || index >= (int)entries.size()) return;
    const auto& e = entries[index];

    for (auto& m : previewMeshes) {
        if (m.vao) glDeleteVertexArrays(1, &m.vao);
        if (m.vbo) glDeleteBuffers(1, &m.vbo);
        if (m.ebo) glDeleteBuffers(1, &m.ebo);
        if (m.instanceVbo) glDeleteBuffers(1, &m.instanceVbo);
    }
    previewMeshes.clear();
    if (proceduralTentacleMesh.vao) glDeleteVertexArrays(1, &proceduralTentacleMesh.vao);
    if (proceduralTentacleMesh.vbo) glDeleteBuffers(1, &proceduralTentacleMesh.vbo);
    if (proceduralTentacleMesh.ebo) glDeleteBuffers(1, &proceduralTentacleMesh.ebo);
    if (proceduralTentacleMesh.instanceVbo)
        glDeleteBuffers(1, &proceduralTentacleMesh.instanceVbo);
    proceduralTentacleMesh = {};

    if (e.offset + e.size > pcPackData.size()) return;

    std::vector<uint8_t> pcmData(pcPackData.begin() + e.offset, pcPackData.begin() + e.offset + e.size);

    SelectSkeletonForMesh(e.name, e.hash);

    BuildSkeletonVisual(pcmData);

    LoadMorphForCurrentModel(e.hash);

    AddMeshFromData(pcmData, e.name, nullptr);

    modelCenter[0] = 0; modelCenter[1] = 0; modelCenter[2] = 0;
    modelRadius = 10.0f;
    modelHeadTarget[0] = 0; modelHeadTarget[1] = 0; modelHeadTarget[2] = 0;
    orbitDistance = 0.0f;

    if(!previewMeshes.empty()) {
        float minP[3] = {1e9, 1e9, 1e9};
        float maxP[3] = {-1e9, -1e9, -1e9};

        BinaryReader br(pcmData);
        br.Seek(8);
        uint32_t num = br.Read<uint32_t>();
        uint32_t ofs = br.Read<uint32_t>();

        if (num < 1000 && ofs < pcmData.size()) {
            br.Seek(ofs);
            struct Info { uint16_t u1, type; uint32_t offset, u2; };
            std::vector<Info> infos;
            for(uint32_t i=0; i<num; i++) {
                Info inf;
                inf.u1 = br.Read<uint16_t>();
                inf.type = br.Read<uint16_t>();
                inf.offset = br.Read<uint32_t>();
                inf.u2 = br.Read<uint32_t>();
                infos.push_back(inf);
            }

            for(auto& inf : infos) {
                if (inf.type != 512) continue;
                if (inf.offset + 16 > pcmData.size()) continue;
                br.Seek(inf.offset); br.Skip(8);
                uint32_t numSm = br.Read<uint32_t>(); uint32_t infSmOfs = br.Read<uint32_t>();
                if(infSmOfs >= pcmData.size()) continue;

                br.Seek(infSmOfs);
                std::vector<uint32_t> smOffsets;
                for(uint32_t s=0; s<numSm; s++) { br.Skip(4); smOffsets.push_back(br.Read<uint32_t>()); }

                for(uint32_t smOfs : smOffsets) {
                    if (smOfs + 64 > pcmData.size()) continue;
                    br.Seek(smOfs); br.Skip(40);
                    uint32_t itype = br.Read<uint32_t>();
                    uint32_t inum = br.Read<uint32_t>();
                    uint32_t iofs = br.Read<uint32_t>();
                    br.Skip(4);
                    uint32_t vnum = br.Read<uint32_t>();
                    uint32_t vofs = br.Read<uint32_t>();
                    br.Skip(8);
                    uint32_t stride = br.Read<uint32_t>();

                    if (vofs >= pcmData.size() || stride == 0) continue;
                    br.Seek(vofs);
                    for(uint32_t v=0; v<vnum; v++) {
                        size_t sv = br.Tell();
                        float x = br.Read<float>(); float y = br.Read<float>(); float z = br.Read<float>();
                        if(x < minP[0]) minP[0] = x; if(x > maxP[0]) maxP[0] = x;
                        if(y < minP[1]) minP[1] = y; if(y > maxP[1]) maxP[1] = y;
                        if(z < minP[2]) minP[2] = z; if(z > maxP[2]) maxP[2] = z;
                        br.Seek(sv + stride);
                    }
                }
            }
        }

        if (minP[0] != 1e9) {
            float cx = (minP[0] + maxP[0]) * 0.5f;
            float cy = (minP[1] + maxP[1]) * 0.5f;
            float cz = (minP[2] + maxP[2]) * 0.5f;

            const float diagonal = sqrtf(
                (maxP[0] - minP[0]) * (maxP[0] - minP[0]) +
                (maxP[1] - minP[1]) * (maxP[1] - minP[1]) +
                (maxP[2] - minP[2]) * (maxP[2] - minP[2]));
            modelCenter[0] = cx;
            modelCenter[1] = cy;
            modelCenter[2] = cz;
            modelRadius = std::max(0.5f, diagonal * 0.5f);

            // Focal point the orbit/zoom camera targets. A tall silhouette is a full-body
            // character whose head sits near the top; a compact mesh (bust/head/prop) is
            // already centred on its head. This is what "zoom towards the head" homes in on.
            const float width     = maxP[0] - minP[0];
            const float depth     = maxP[2] - minP[2];
            const float height    = maxP[1] - minP[1];
            // Use the THINNEST horizontal axis (front-to-back depth for a character), not the
            // widest: bind poses are usually T/A-posed, so arms make width ~= height and a
            // width-based test wrongly reads a standing figure as "compact". A standing body
            // is much taller than deep; a bust/head/prop is roughly as tall as it is thick.
            const float thickness = std::min(width, depth);
            const float headY     = (height > thickness * 1.8f) ? (minP[1] + 0.90f * height) : cy;
            modelHeadTarget[0] = cx;
            modelHeadTarget[1] = headY;
            modelHeadTarget[2] = cz;

            camYaw = 90.0f;
            camPitch = 18.0f;
            const float distance = modelRadius * 3.2f;
            orbitDistance = distance;   // start zoomed out, framed on the body centre
            const float yaw = camYaw * 3.14159265358979323846f / 180.0f;
            const float pitch = camPitch * 3.14159265358979323846f / 180.0f;
            const float horizontal = cosf(pitch) * distance;
            camPos[0] = cx + cosf(yaw) * horizontal;
            camPos[1] = cy + sinf(pitch) * distance;
            camPos[2] = cz + sinf(yaw) * horizontal;
            camFront[0] = cx - camPos[0];
            camFront[1] = cy - camPos[1];
            camFront[2] = cz - camPos[2];
            Normalize(camFront);
            camUp[0] = 0.0f;
            camUp[1] = 1.0f;
            camUp[2] = 0.0f;
        }
    }
}

void SpiderManTool::ConvertPCM(const std::vector<uint8_t>& pcmData, const std::string& outPath, uint32_t meshHash) {
    ParseMaterialEntries(pcmData);

    BinaryReader br(pcmData);
    if (8 + 4 > pcmData.size()) return;
    br.Seek(8);
    uint32_t num = br.Read<uint32_t>();
    uint32_t ofs = br.Read<uint32_t>();
    if (ofs >= pcmData.size()) return;
    br.Seek(ofs);
    struct Info { uint16_t u1, type; uint32_t offset, u2; };
    std::vector<Info> infos;
    for(uint32_t i=0; i<num; i++) {
        Info inf; inf.u1 = br.Read<uint16_t>(); inf.type = br.Read<uint16_t>(); inf.offset = br.Read<uint32_t>(); inf.u2 = br.Read<uint32_t>(); infos.push_back(inf);
    }

    struct SubmeshData {
        std::string name;
        std::string textureName;
        std::vector<std::string> textureCandidates;
        bool isTranslucent;
        bool isAlphaTest;
        std::vector<float> pos, norm, uvs, weights;
        std::vector<uint16_t> joints, indices;
        float minP[3], maxP[3];
        int sectionIndex = -1;   // PCM first-LOD section index, for aligning morph deltas
    };
    std::vector<SubmeshData> submeshes;
    std::vector<float> allIBMs;
    uint32_t totalBones = 0;
    std::string modelName = fs::path(outPath).stem().string();
    if (modelName.empty()) modelName = "Model";

    auto ReadName = [&](uint32_t offset) -> std::string {
        return ReadStringTableEntry(pcmData, offset);
    };

    bool loadedFirstLod = false;
    for(auto& inf : infos) {
        if (inf.type != 512) continue;
        if (loadedFirstLod) break;
        loadedFirstLod = true;

        if (inf.offset + 20 > pcmData.size()) continue;

        br.Seek(inf.offset);
        br.Skip(8);
        uint32_t numSm = br.Read<uint32_t>();
        uint32_t infoSmOfs = br.Read<uint32_t>();

        if (numSm > 256 || infoSmOfs >= pcmData.size()) continue;

        br.Seek(infoSmOfs);
        std::vector<std::pair<uint32_t, uint32_t>> smRefs;
        for(uint32_t s=0; s<numSm; s++) {
            uint32_t matRef = br.Read<uint32_t>();
            uint32_t smOfs = br.Read<uint32_t>();
            smRefs.push_back({matRef, smOfs});
        }

        for (size_t sectionIndex = 0; sectionIndex < smRefs.size(); ++sectionIndex) {
            const auto [matRef, smOfs] = smRefs[sectionIndex];
            (void)matRef;
            if (smOfs + 96 > pcmData.size()) continue;

            br.Seek(smOfs);
            uint32_t meshNameRef = br.Read<uint32_t>();

            MaterialDef mat = ResolveMaterialByMeshOffset(meshNameRef);

            br.Seek(smOfs + 0x28);
            uint32_t itype = br.Read<uint32_t>();
            uint32_t inum = br.Read<uint32_t>();
            uint32_t iofs = br.Read<uint32_t>();
            br.Skip(4);
            uint32_t vnum = br.Read<uint32_t>();
            uint32_t vofs = br.Read<uint32_t>();
            br.Skip(8);
            uint32_t stride = br.Read<uint32_t>();

            if (vnum > 100000 || inum > 300000 || vofs >= pcmData.size() || iofs >= pcmData.size() || stride == 0) continue;

            PCMSectionBonePalette bonePalette;
            bonePalette.load(pcmData, smOfs);

            SubmeshData sm;
            sm.sectionIndex = (int)sectionIndex;
            sm.name = ReadName(meshNameRef);
            sm.textureName = mat.textureName;
            sm.isTranslucent = mat.isTranslucent;
            sm.isAlphaTest = mat.isAlphaTest;
            auto pushTextureCandidate = [&](const std::string& candidate) {
                if (candidate.empty()) return;
                if (std::find(sm.textureCandidates.begin(), sm.textureCandidates.end(), candidate) == sm.textureCandidates.end()) {
                    sm.textureCandidates.push_back(candidate);
                }
            };
            auto pushSingularWingCandidate = [&](const std::string& candidate) {
                std::string key = GlbSanitizeTextureKey(candidate);
                if (key.size() > 6 && key.substr(key.size() - 6) == "_wings") {
                    pushTextureCandidate(key.substr(0, key.size() - 1));
                }
            };

            pushSingularWingCandidate(mat.meshName);
            pushSingularWingCandidate(sm.name);
            pushTextureCandidate(mat.textureName);
            if (!modelName.empty()) {
                std::string cleanName = modelName;
                size_t lastDot = cleanName.find_last_of(".");
                if (lastDot != std::string::npos) cleanName = cleanName.substr(0, lastDot);
                cleanName = StrToLower(cleanName);
                pushTextureCandidate(cleanName + "_d");
                pushTextureCandidate(cleanName);
            }
            pushTextureCandidate(mat.meshName);
            pushTextureCandidate(sm.name);

            for (int i = 0; i < 3; i++) {
                sm.minP[i] = 1e9f;
                sm.maxP[i] = -1e9f;
            }

            br.Seek(vofs);
            for(uint32_t v=0; v<vnum; v++) {
                size_t startV = br.Tell();
                if (startV + stride > pcmData.size()) break;

                float x = br.Read<float>(); float y = br.Read<float>(); float z = br.Read<float>();
                sm.pos.push_back(x); sm.pos.push_back(y); sm.pos.push_back(z);

                if (x < sm.minP[0]) sm.minP[0] = x;
                if (y < sm.minP[1]) sm.minP[1] = y;
                if (z < sm.minP[2]) sm.minP[2] = z;
                if (x > sm.maxP[0]) sm.maxP[0] = x;
                if (y > sm.maxP[1]) sm.maxP[1] = y;
                if (z > sm.maxP[2]) sm.maxP[2] = z;

                float nx = 0, ny = 1, nz = 0;
                float u = 0, v2 = 0;

                if (stride == 64) {
                    br.Seek(startV + 12);
                    nx = br.Read<float>(); ny = br.Read<float>(); nz = br.Read<float>();
                    u = br.Read<float>(); v2 = br.Read<float>();

                    if (startV + 64 <= pcmData.size()) {
                        br.Seek(startV + 32);
                        float bIdx[4], bWgt[4];
                        for (int bi = 0; bi < 4; bi++) bIdx[bi] = br.Read<float>();
                        for (int bi = 0; bi < 4; bi++) bWgt[bi] = br.Read<float>();

                        for (int bi = 0; bi < 4; bi++) {
                            int localIdx = (int)bIdx[bi];
                            if (localIdx >= 0 && bWgt[bi] > 0.f && !bonePalette.palette.empty())
                                bIdx[bi] = (float)bonePalette.mapIndex(localIdx);
                            else if (bWgt[bi] <= 0.f)
                                bIdx[bi] = 0.f;
                        }

                        float wTotal = bWgt[0] + bWgt[1] + bWgt[2] + bWgt[3];
                        if (wTotal > 1e-8f) {
                            float inv = 1.f / wTotal;
                            for (int bi = 0; bi < 4; bi++) bWgt[bi] *= inv;
                        } else {
                            bWgt[0] = 1.f;
                        }

                        for (int bi = 0; bi < 4; bi++) sm.joints.push_back((uint16_t)bIdx[bi]);
                        for (int bi = 0; bi < 4; bi++) sm.weights.push_back(bWgt[bi]);
                    }
                } else if (stride == 24) {
                    br.Seek(startV + 12);
                    u = br.Read<float>(); v2 = br.Read<float>();
                }

                sm.norm.push_back(nx); sm.norm.push_back(ny); sm.norm.push_back(nz);
                sm.uvs.push_back(u); sm.uvs.push_back(v2);

                br.Seek(startV + stride);
            }

            br.Seek(iofs);
            for(uint32_t i=0; i<inum; i++) {
                sm.indices.push_back(br.Read<uint16_t>());
            }

            if (itype != 4 && !sm.indices.empty()) {
                std::vector<uint16_t> newIndices;
                for(size_t i=0; i+2<sm.indices.size(); i++) {
                    uint16_t i0 = sm.indices[i], i1 = sm.indices[i+1], i2 = sm.indices[i+2];
                    if (i0 == i1 || i1 == i2 || i0 == i2) continue;
                    if (i % 2 == 0) {
                        newIndices.push_back(i0); newIndices.push_back(i1); newIndices.push_back(i2);
                    } else {
                        newIndices.push_back(i0); newIndices.push_back(i2); newIndices.push_back(i1);
                    }
                }
                sm.indices = newIndices;
            }

            if (!sm.pos.empty() && !sm.indices.empty()) {
                submeshes.push_back(sm);
            }
        }
    }

    if (submeshes.empty()) return;

    bool hasSkinning = false;
    for (auto& sm : submeshes) {
        if (!sm.joints.empty() && !sm.weights.empty()) {
            hasSkinning = true;
            break;
        }
    }

    PCMBoneMatrices boneMats;
    for (auto& inf : infos) {
        if (inf.type != 512) continue;
        boneMats.load(pcmData, inf.offset);
        break;
    }

    SkinningGLBWriter writer;
    // Resolve the mesh's morph (blend shapes) for export. Keyed by mesh hash; may live in
    // another pack (see FetchMorphBytesForHash). Side-effect-free — does not touch UI state.
    UsmMorph::File morphFile;
    if (meshHash != 0) {
        std::vector<uint8_t> morphBytes;
        if (FetchMorphBytesForHash(meshHash, morphBytes))
            morphFile = UsmMorph::Parse(morphBytes);
    }
    std::vector<int> boneNodeIndices;
    std::vector<std::array<float, 16>> restLocalMatrices;
    std::vector<GlbExportQuat> restLocalQuatNal;
    std::vector<std::array<float, 16>> logicalBindMatrices;
    std::vector<int> exportLogicalToMesh;
    std::vector<int> exportMeshToLogical;
    const NalSkeletonData* exportSkeleton = loadedSkeleton ? loadedSkeleton.get() : nullptr;

    auto readTextureLocation = [](const TextureLocation& loc, std::vector<uint8_t>& out) -> bool {
        if (loc.packPath.empty() || loc.size == 0) return false;
        std::ifstream texFile(loc.packPath, std::ios::binary);
        if (!texFile.is_open()) return false;
        texFile.seekg(loc.offset);
        out.resize(loc.size);
        texFile.read(reinterpret_cast<char*>(out.data()), loc.size);
        return texFile.good() || texFile.gcount() == (std::streamsize)loc.size;
    };

    auto resolveExactDDS = [&](const std::string& rawName, std::vector<uint8_t>& outDDS, std::string& resolvedName) -> bool {
        std::string nameLower = GlbSanitizeTextureKey(rawName);
        if (nameLower.empty()) return false;

        for (const auto& entry : entries) {
            if (!entry.isDds || entry.offset + entry.size > pcPackData.size()) continue;
            std::string entryName = StrToLower(entry.name);
            std::string entryBase = GlbSanitizeTextureKey(entryName);
            if (entryBase == nameLower || entryName == nameLower || entryName == nameLower + ".dds") {
                outDDS.assign(pcPackData.begin() + entry.offset, pcPackData.begin() + entry.offset + entry.size);
                resolvedName = entryBase.empty() ? nameLower : entryBase;
                return true;
            }
        }

        auto nameIt = globalTextureNameIndex.find(nameLower);
        if (nameIt != globalTextureNameIndex.end() && readTextureLocation(nameIt->second, outDDS)) {
            resolvedName = nameLower;
            return true;
        }
        nameIt = globalTextureNameIndex.find(nameLower + ".dds");
        if (nameIt != globalTextureNameIndex.end() && readTextureLocation(nameIt->second, outDDS)) {
            resolvedName = nameLower;
            return true;
        }

        uint32_t hashWithExt = CalculateCRC32(nameLower + ".dds");
        auto hashIt = globalTextureIndex.find(hashWithExt);
        if (hashIt != globalTextureIndex.end() && readTextureLocation(hashIt->second, outDDS)) {
            resolvedName = nameLower;
            return true;
        }

        uint32_t hashBare = CalculateCRC32(nameLower);
        hashIt = globalTextureIndex.find(hashBare);
        if (hashIt != globalTextureIndex.end() && readTextureLocation(hashIt->second, outDDS)) {
            resolvedName = nameLower;
            return true;
        }

        return false;
    };

    std::function<bool(const std::string&, std::vector<uint8_t>&, std::string&, int)> resolveDDSByName;
    resolveDDSByName = [&](const std::string& rawName, std::vector<uint8_t>& outDDS, std::string& resolvedName, int depth) -> bool {
        std::string nameLower = GlbSanitizeTextureKey(rawName);
        if (nameLower.empty()) return false;
        if (resolveExactDDS(nameLower, outDDS, resolvedName)) return true;

        if (depth <= 2) {
            for (const auto& entry : entries) {
                if (entry.isDds || entry.offset + entry.size > pcPackData.size()) continue;
                std::string entryName = StrToLower(entry.name);
                std::string entryBase = GlbSanitizeTextureKey(entryName);
                if (entryBase != nameLower && entryName != nameLower && entryName != nameLower + ".dat") continue;

                std::vector<uint8_t> listData(pcPackData.begin() + entry.offset, pcPackData.begin() + entry.offset + entry.size);
                std::vector<std::string> listTextures = GlbExtractTextureListNames(listData);
                std::vector<uint8_t> transparentFallbackDDS;
                std::string transparentFallbackName;
                for (const std::string& listTexture : listTextures) {
                    if (resolveDDSByName(listTexture, outDDS, resolvedName, depth + 1)) {
                        std::vector<uint8_t> rgba;
                        int width = 0, height = 0;
                        if (GlbDecodeDDSToRGBA(outDDS, rgba, width, height) && GlbRGBAFullyTransparent(rgba)) {
                            if (transparentFallbackDDS.empty()) {
                                transparentFallbackDDS = outDDS;
                                transparentFallbackName = resolvedName;
                            }
                            continue;
                        }
                        return true;
                    }
                }
                if (!transparentFallbackDDS.empty()) {
                    outDDS = std::move(transparentFallbackDDS);
                    resolvedName = transparentFallbackName;
                    return true;
                }
            }
        }

        if (depth > 2 || nameLower.compare(0, 4, "nat_") == 0) return false;

        size_t under = nameLower.find('_');
        if (under == 2 || under == 3) {
            std::string rest = nameLower.substr(under + 1);
            if (!rest.empty()) {
                if (resolveDDSByName("nat_" + rest, outDDS, resolvedName, depth + 1)) return true;
                if (resolveDDSByName(rest, outDDS, resolvedName, depth + 1)) return true;
            }
        }
        return false;
    };

    std::map<std::string, int> glbTextureByResolvedName;
    std::map<std::string, bool> glbTextureAlphaByResolvedName;
    auto textureIndexForCandidates = [&](const std::vector<std::string>& candidates, bool& hasTextureAlpha) -> int {
        hasTextureAlpha = false;
        for (const std::string& candidate : candidates) {
            std::vector<uint8_t> ddsData;
            std::string resolvedName;
            if (!resolveDDSByName(candidate, ddsData, resolvedName, 0)) continue;
            std::string textureKey = GlbSanitizeTextureKey(resolvedName);
            auto cached = glbTextureByResolvedName.find(textureKey);
            if (cached != glbTextureByResolvedName.end()) {
                auto alphaIt = glbTextureAlphaByResolvedName.find(textureKey);
                hasTextureAlpha = alphaIt != glbTextureAlphaByResolvedName.end() && alphaIt->second;
                return cached->second;
            }

            std::vector<uint8_t> rgba;
            std::vector<uint8_t> png;
            int width = 0, height = 0;
            if (!GlbDecodeDDSToRGBA(ddsData, rgba, width, height)) continue;
            if (GlbRGBAFullyTransparent(rgba)) continue;
            bool textureHasAlpha = GlbRGBAHasAlpha(rgba);
            if (!GlbEncodePNG(rgba, width, height, png)) continue;

            int texIndex = writer.AddPNGTexture(textureKey.empty() ? candidate : textureKey, png);
            if (texIndex >= 0) {
                glbTextureByResolvedName[textureKey] = texIndex;
                glbTextureAlphaByResolvedName[textureKey] = textureHasAlpha;
                hasTextureAlpha = textureHasAlpha;
                return texIndex;
            }
        }
        return -1;
    };

    int ibmAccessor = -1;
    if (hasSkinning && !boneMats.matrices.empty()) {
        totalBones = (uint32_t)boneMats.matrices.size();
        boneNodeIndices.assign(totalBones, -1);
        const bool entityMapMatches = activeBoneMapping.valid &&
            activeBoneMapping.meshPoseCount == totalBones &&
            activeBoneMapping.meshToLogical.size() == totalBones &&
            !activeBoneMapping.logicalToMesh.empty();
        const uint32_t logicalBoneCount = entityMapMatches
            ? (uint32_t)activeBoneMapping.logicalToMesh.size()
            : totalBones;
        exportLogicalToMesh.resize(logicalBoneCount);
        exportMeshToLogical.resize(totalBones);
        for (uint32_t i = 0; i < logicalBoneCount; ++i) {
            exportLogicalToMesh[i] = entityMapMatches ? activeBoneMapping.logicalToMesh[i] : (int)i;
        }
        for (uint32_t i = 0; i < totalBones; ++i) {
            exportMeshToLogical[i] = entityMapMatches ? activeBoneMapping.meshToLogical[i] : (int)i;
        }
        logicalBindMatrices.resize(logicalBoneCount);
        for (uint32_t logical = 0; logical < logicalBoneCount; ++logical) {
            const int mesh = exportLogicalToMesh[logical];
            if (mesh >= 0 && mesh < (int)totalBones)
                logicalBindMatrices[logical] = boneMats.matrices[mesh];
            else {
                logicalBindMatrices[logical].fill(0.0f);
                logicalBindMatrices[logical][0] = 1.0f;
                logicalBindMatrices[logical][5] = 1.0f;
                logicalBindMatrices[logical][10] = 1.0f;
                logicalBindMatrices[logical][15] = 1.0f;
            }
        }
        GlbBuildRestLocalMatrices(logicalBindMatrices, exportSkeleton,
                                  restLocalMatrices, restLocalQuatNal);

        std::vector<int> rootBoneNodes;
        for (uint32_t bi = 0; bi < totalBones; bi++) {
            int nodeIndex = 1 + (int)bi;
            rootBoneNodes.push_back(nodeIndex);
        }

        int skeletonRootNode = writer.AddNode("Skeleton", -1, -1, nullptr, rootBoneNodes);
        writer.SetSkinRoot(skeletonRootNode);
        writer.AddToScene(skeletonRootNode);

        for (uint32_t bi = 0; bi < totalBones; bi++) {
            std::string boneName = "bone_" + std::to_string(bi);
            if (exportSkeleton) {
                int logical = exportMeshToLogical.empty() ? (int)bi : exportMeshToLogical[bi];
                auto nameIt = exportSkeleton->bone_map.find(logical);
                if (nameIt != exportSkeleton->bone_map.end() && !nameIt->second.empty()) {
                    boneName = nameIt->second;
                }
            }

            float translation[3];
            float rotation[4];
            GlbMatrixToTRS(boneMats.matrices[bi].data(), translation, rotation);
            int boneNode = writer.AddNode(boneName, -1, -1, nullptr, {}, translation, rotation);
            boneNodeIndices[bi] = boneNode;
            writer.AddJoint(boneNode);
        }

        std::vector<float> ibmData;
        ibmData.reserve((size_t)totalBones * 16);
        for (auto& mat : boneMats.matrices) {
            float inv[16];
            if (!InvertMatrix(mat.data(), inv)) GlbMat4Identity(inv);
            for (int i = 0; i < 16; i++) ibmData.push_back(inv[i]);
        }
        int ibmView = writer.AddBufferView(ibmData.data(), ibmData.size() * sizeof(float), 0);
        ibmAccessor = writer.AddAccessor(ibmView, 5126, (int)totalBones, "MAT4");
    }

    // Merged model: every submesh becomes a primitive of ONE glTF mesh -> a single object in
    // Blender (the character's parts aren't modular). glTF requires all primitives in a mesh to
    // share the same morph-target count, so every primitive declares the full set: face sections
    // carry real POSITION deltas; all other primitives point at zero accessors (no bufferView =
    // all zeros, free), so the shape keys exist consistently but move only the face.
    auto sectionHasData = [&](int sectionIndex, int vertCount, size_t t) {
        return morphFile.valid && sectionIndex >= 0 && vertCount > 0 && t < morphFile.sets.size() &&
               sectionIndex < (int)morphFile.sets[t].sections.size() &&
               morphFile.sets[t].sections[sectionIndex].vertex_count == (uint32_t)vertCount &&
               (int)morphFile.sets[t].sections[sectionIndex].position.values.size() >= vertCount;
    };
    int morphTargetCount = (morphFile.valid && morphFile.sets.size() > 1)
        ? (int)(morphFile.sets.size() - 1) : 0;
    if (morphTargetCount > 0) {
        bool anyReal = false;
        for (auto& sm : submeshes) {
            const int vc = (int)sm.pos.size() / 3;
            for (size_t t = 1; t < morphFile.sets.size() && !anyReal; ++t)
                if (sectionHasData(sm.sectionIndex, vc, t)) anyReal = true;
            if (anyReal) break;
        }
        if (!anyReal) morphTargetCount = 0;   // morph didn't match this mesh; don't add dead keys
    }

    int meshIdx = writer.StartMesh(modelName);
    for (size_t si = 0; si < submeshes.size(); si++) {
        auto& sm = submeshes[si];

        bool hasTextureAlpha = false;
        int textureIndex = textureIndexForCandidates(sm.textureCandidates, hasTextureAlpha);
        int matIdx = writer.AddMaterial(sm.name.empty() ? "material_" + std::to_string(si) : sm.name, sm.isTranslucent, sm.isAlphaTest || hasTextureAlpha, textureIndex);

        int posView = writer.AddBufferView(sm.pos.data(), sm.pos.size() * sizeof(float), 34962);
        int posAcc = writer.AddAccessor(posView, 5126, (int)sm.pos.size() / 3, "VEC3", sm.minP, sm.maxP);

        int normView = writer.AddBufferView(sm.norm.data(), sm.norm.size() * sizeof(float), 34962);
        int normAcc = writer.AddAccessor(normView, 5126, (int)sm.norm.size() / 3, "VEC3");

        int uvView = writer.AddBufferView(sm.uvs.data(), sm.uvs.size() * sizeof(float), 34962);
        int uvAcc = writer.AddAccessor(uvView, 5126, (int)sm.uvs.size() / 2, "VEC2");

        int indView = writer.AddBufferView(sm.indices.data(), sm.indices.size() * sizeof(uint16_t), 34963);
        int indAcc = writer.AddAccessor(indView, 5123, (int)sm.indices.size(), "SCALAR");

        int jointAcc = -1, weightAcc = -1;
        if (!sm.joints.empty() && !sm.weights.empty()) {
            int jointView = writer.AddBufferView(sm.joints.data(), sm.joints.size() * sizeof(uint16_t), 34962);
            jointAcc = writer.AddAccessor(jointView, 5123, (int)sm.joints.size() / 4, "VEC4");

            int weightView = writer.AddBufferView(sm.weights.data(), sm.weights.size() * sizeof(float), 34962);
            weightAcc = writer.AddAccessor(weightView, 5126, (int)sm.weights.size() / 4, "VEC4");
        }

        std::vector<int> morphPosAccessors;
        if (morphTargetCount > 0) {
            const int vertCount = (int)sm.pos.size() / 3;
            for (size_t t = 1; t < morphFile.sets.size(); ++t) {
                if (sectionHasData(sm.sectionIndex, vertCount, t)) {
                    const auto& vals = morphFile.sets[t].sections[sm.sectionIndex].position.values;
                    std::vector<float> built((size_t)vertCount * 3, 0.0f);
                    float dmin[3] = {1e9f, 1e9f, 1e9f}, dmax[3] = {-1e9f, -1e9f, -1e9f};
                    for (int v = 0; v < vertCount; ++v)
                        for (int k = 0; k < 3; ++k) {
                            float f = vals[v][k];
                            built[(size_t)v * 3 + k] = f;
                            dmin[k] = std::min(dmin[k], f);
                            dmax[k] = std::max(dmax[k], f);
                        }
                    int dView = writer.AddBufferView(built.data(), built.size() * sizeof(float), 34962);
                    morphPosAccessors.push_back(writer.AddAccessor(dView, 5126, vertCount, "VEC3", dmin, dmax));
                } else {
                    // This shape key doesn't move this primitive: free all-zero accessor.
                    morphPosAccessors.push_back(writer.AddZeroAccessor(vertCount, "VEC3", 5126));
                }
            }
        }

        writer.AddPrimitive(posAcc, normAcc, uvAcc, indAcc, jointAcc, weightAcc, matIdx, morphPosAccessors);
    }

    std::vector<float> morphWeights;
    std::vector<std::string> morphNames;
    if (morphTargetCount > 0) {
        morphWeights.assign(morphTargetCount, 0.0f);
        for (int t = 1; t <= morphTargetCount; ++t) {
            std::string nm = (t <= (int)UsmViseme::RETAIL_CHANNEL_COUNT) ? "Viseme " : "Shape ";
            nm += (t < 10 ? "0" : "") + std::to_string(t);
            morphNames.push_back(nm);
        }
    }
    writer.EndMesh(morphWeights, morphNames);

    int nodeIdx = writer.AddNode(modelName, meshIdx, hasSkinning ? 0 : -1);
    writer.AddToScene(nodeIdx);

    int exportedAnimations = 0;
    if (hasSkinning && loadedAnimFile && loadedSkeleton && !restLocalMatrices.empty() && !boneNodeIndices.empty()) {
        for (int ai = 0; ai < (int)loadedAnimFile->animations.size(); ++ai) {
            const auto& anim = loadedAnimFile->animations[ai];

            if (anim.skeleton &&
                !nal_skeleton_pose_compatible(anim.skeleton.get(), loadedSkeleton.get()))
                continue;
            const NalSkeletonData& poseSkeleton = anim.skeleton
                ? *anim.skeleton
                : *loadedSkeleton;
            exportedAnimations += GlbAddAnimationToWriter(
                writer,
                anim,
                ai,
                poseSkeleton,
                restLocalMatrices,
                logicalBindMatrices,
                restLocalQuatNal,
                boneNodeIndices,
                exportLogicalToMesh);
        }
    }

    writer.WriteToFile(outPath, ibmAccessor);

    if (exportedAnimations > 0) {

    }
}

void SpiderManTool::AnalyzePCM(int index) {
    if (index < 0 || index >= (int)entries.size()) return;
    const auto& e = entries[index];
    if (e.offset + e.size > pcPackData.size()) return;

    currentPcmInfos.clear();
    currentPcmSkeleton = {0, 0};
    currentPcmIndex = index;

    std::vector<uint8_t> pcmData(pcPackData.begin() + e.offset, pcPackData.begin() + e.offset + e.size);

    AnalyzePCMDetailed(pcmData);

    BinaryReader br(pcmData);

    if (pcmData.size() < 16) return;

    br.Seek(8);
    uint32_t numEntries = br.Read<uint32_t>();
    uint32_t entryTableOfs = br.Read<uint32_t>();

    if (numEntries > 1000 || entryTableOfs >= pcmData.size()) return;

    br.Seek(entryTableOfs);
    struct EntryInfo { uint16_t type; uint16_t tag; uint32_t dataOffset; uint32_t nameOffset; };
    std::vector<EntryInfo> entryInfos;

    for (uint32_t i = 0; i < numEntries; i++) {
        EntryInfo ei;
        ei.type = br.Read<uint16_t>();
        ei.tag = br.Read<uint16_t>();
        ei.dataOffset = br.Read<uint32_t>();
        ei.nameOffset = br.Read<uint32_t>();
        entryInfos.push_back(ei);
    }

    for (auto& ei : entryInfos) {
        if (ei.tag == 512) {
            if (ei.dataOffset + 20 > pcmData.size()) continue;

            br.Seek(ei.dataOffset);
            br.Skip(8);
            uint32_t numSubMeshes = br.Read<uint32_t>();
            uint32_t subMeshInfoOfs = br.Read<uint32_t>();

            if (numSubMeshes > 256 || subMeshInfoOfs >= pcmData.size()) continue;

            br.Seek(subMeshInfoOfs);
            std::vector<uint32_t> smOffsets;
            for (uint32_t s = 0; s < numSubMeshes; s++) {
                br.Skip(4);
                smOffsets.push_back(br.Read<uint32_t>());
            }

            for (uint32_t smOfs : smOffsets) {
                if (smOfs + 80 > pcmData.size()) continue;

                br.Seek(smOfs);
                uint32_t meshNameRef = br.Read<uint32_t>();

                std::string meshName;
                if (meshNameRef != 0 && meshNameRef + 32 <= pcmData.size()) {
                    size_t strStart = meshNameRef + 4;
                    size_t end = strStart;
                    while (end < strStart + 28 && end < pcmData.size() && pcmData[end] != 0) end++;
                    meshName = std::string((char*)&pcmData[strStart], end - strStart);
                }

                br.Seek(smOfs + 40);
                uint32_t primType = br.Read<uint32_t>();
                uint32_t iCount = br.Read<uint32_t>();
                uint32_t iOffset = br.Read<uint32_t>();
                br.Skip(4);
                uint32_t vCount = br.Read<uint32_t>();
                uint32_t vOffset = br.Read<uint32_t>();
                br.Skip(8);
                uint32_t stride = br.Read<uint32_t>();

                PCMMeshInfo info;
                info.name = meshName;
                info.vCount = vCount;
                info.vOffset = vOffset;
                info.iCount = iCount;
                info.iOffset = iOffset;
                info.stride = stride;
                info.primitiveType = primType;
                info.hasUV = (stride >= 24);
                info.hasBones = (stride >= 44);

                currentPcmInfos.push_back(info);
            }
        }
        else if (ei.tag == 768) {
            br.Seek(ei.dataOffset);
            uint32_t boneCount = br.Read<uint32_t>();
            currentPcmSkeleton.count = boneCount;
            currentPcmSkeleton.offset = ei.dataOffset;
        }
    }
}

void SpiderManTool::AnalyzePCMDetailed(const std::vector<uint8_t>& pcmData) {
    currentPcmDetails = PCMFileInfo();

    if (pcmData.size() < 32) return;

    BinaryReader br(pcmData);

    br.Seek(8);
    currentPcmDetails.numEntries = br.Read<uint32_t>();
    currentPcmDetails.entryTableOffset = br.Read<uint32_t>();
    currentPcmDetails.fileSize = (uint32_t)pcmData.size();

    if (currentPcmDetails.numEntries > 1000 || currentPcmDetails.entryTableOffset >= pcmData.size()) return;

    struct AssetEntry {
        uint16_t size;
        uint16_t tag;
        uint32_t dataOffset;
        uint32_t nameOffset;
    };

    std::vector<AssetEntry> entries;
    br.Seek(currentPcmDetails.entryTableOffset);

    for (uint32_t i = 0; i < currentPcmDetails.numEntries; i++) {
        AssetEntry e;
        e.size = br.Read<uint16_t>();
        e.tag = br.Read<uint16_t>();
        e.dataOffset = br.Read<uint32_t>();
        e.nameOffset = br.Read<uint32_t>();
        entries.push_back(e);
    }

    auto readString = [&](uint32_t offset) -> std::string {
        if (offset == 0 || offset + 32 > pcmData.size()) return "";
        size_t strStart = offset + 4;
        size_t end = strStart;
        while (end < strStart + 60 && end < pcmData.size() && pcmData[end] != 0) end++;
        return std::string((char*)&pcmData[strStart], end - strStart);
    };

    for (const auto& e : entries) {
        if (e.tag != 256) continue;
        if (e.dataOffset + 0x64 > pcmData.size()) continue;

        PCMMaterialInfo mat;
        mat.nameOffset = e.nameOffset;
        mat.name = readString(e.nameOffset);
        mat.shaderSize = e.size;

        br.Seek(e.dataOffset);
        uint32_t meshNameOfs = br.Read<uint32_t>();
        uint32_t alphaFlagOfs = br.Read<uint32_t>();

        uint32_t textureNameOfs = 0;
        uint32_t texOffset = LocateTextureOffset(e.size);
        if (texOffset != 0 && e.dataOffset + texOffset + 4 <= pcmData.size()) {
            br.Seek(e.dataOffset + texOffset);
            textureNameOfs = br.Read<uint32_t>();
        }

        mat.meshName = readString(meshNameOfs);
        mat.alphaFlag = readString(alphaFlagOfs);
        mat.textureName = readString(textureNameOfs);

        uint32_t blendMode = ClassifyByShaderName(mat.alphaFlag);
        mat.isTranslucent  = (blendMode >= NGLBM_BLEND);

        currentPcmDetails.materials.push_back(mat);
    }
    currentPcmDetails.materialCount = (uint32_t)currentPcmDetails.materials.size();

    for (const auto& e : entries) {
        if (e.tag != 512) continue;
        if (e.dataOffset + 48 > pcmData.size()) continue;

        PCMLodInfo lod;
        lod.nameOffset = e.nameOffset;
        lod.name = readString(e.nameOffset);

        br.Seek(e.dataOffset);
        br.Skip(8);
        lod.submeshCount = br.Read<uint32_t>();
        uint32_t submeshRefsOfs = br.Read<uint32_t>();
        lod.boneCount = br.Read<uint32_t>();
        lod.bonesOffset = br.Read<uint32_t>();
        br.Skip(4);
        lod.nextLodOffset = br.Read<uint32_t>();

        br.Seek(e.dataOffset + 0x20);
        lod.boundsMin[0] = br.Read<float>();
        lod.boundsMin[1] = br.Read<float>();
        lod.boundsMin[2] = br.Read<float>();
        br.Skip(4);

        lod.lodDistance = 0.0f;
        if (lod.nextLodOffset > 0 && lod.nextLodOffset + 8 <= pcmData.size()) {
            br.Seek(lod.nextLodOffset + 4);
            lod.lodDistance = br.Read<float>();
        }

        currentPcmDetails.lods.push_back(lod);
        currentPcmDetails.boneCount = lod.boneCount;
        currentPcmDetails.bonesOffset = lod.bonesOffset;

        if (lod.submeshCount > 256 || submeshRefsOfs >= pcmData.size()) continue;

        br.Seek(submeshRefsOfs);
        std::vector<std::pair<uint32_t, uint32_t>> submeshRefs;
        for (uint32_t s = 0; s < lod.submeshCount; s++) {
            uint32_t matNameOfs = br.Read<uint32_t>();
            uint32_t smInfoOfs = br.Read<uint32_t>();
            submeshRefs.push_back({matNameOfs, smInfoOfs});
        }

        for (const auto& [matNameOfs, smInfoOfs] : submeshRefs) {
            if (smInfoOfs + 96 > pcmData.size()) continue;

            PCMSubmeshInfo sm;

            br.Seek(smInfoOfs);
            sm.nameOffset = br.Read<uint32_t>();
            sm.name = readString(sm.nameOffset);

            uint32_t ptrShaderRef = br.Read<uint32_t>();

            br.Seek(smInfoOfs + 0x0C);
            sm.boneMapOffset = br.Read<uint32_t>();
            sm.boneMapCount = br.Read<uint32_t>();

            br.Seek(smInfoOfs + 0x20);
            sm.boundingRadius = br.Read<float>();

            br.Seek(smInfoOfs + 0x28);
            sm.primitiveType = br.Read<uint32_t>();
            sm.indexCount = br.Read<uint32_t>();
            sm.indexOffset = br.Read<uint32_t>();
            br.Skip(4);
            sm.vertexCount = br.Read<uint32_t>();
            sm.vertexOffset = br.Read<uint32_t>();
            sm.vertexBufferSize = br.Read<uint32_t>();
            br.Skip(4);
            sm.stride = br.Read<uint32_t>();

            sm.hasNormals = (sm.stride >= 24);
            sm.hasUV = (sm.stride >= 32);
            sm.hasBones = (sm.stride >= 44);

            for (const auto& mat : currentPcmDetails.materials) {
                uint32_t matMeshOfs = 0;

                for (const auto& me : entries) {
                    if (me.tag == 256 && readString(me.nameOffset) == mat.name) {
                        br.Seek(me.dataOffset);
                        matMeshOfs = br.Read<uint32_t>();
                        break;
                    }
                }
                if (matMeshOfs == sm.nameOffset) {
                    sm.materialMeshName = mat.meshName;
                    sm.materialAlphaFlag = mat.alphaFlag;
                    sm.materialTexture = mat.textureName;
                    sm.isTranslucent = mat.isTranslucent;
                    sm.shaderSize = mat.shaderSize;

                    switch (mat.shaderSize) {
                        case 80: sm.shaderType = "CHARACTER"; break;
                        case 88: sm.shaderType = "CHARACTER_EXT"; break;
                        case 128: sm.shaderType = "STREET"; break;
                        case 132: sm.shaderType = "WORLD"; break;
                        case 136: sm.shaderType = "TRANSLUCENT"; break;
                        default: sm.shaderType = "UNKNOWN_" + std::to_string(mat.shaderSize); break;
                    }
                    break;
                }
            }

            currentPcmDetails.submeshes.push_back(sm);
            currentPcmDetails.totalVertices += sm.vertexCount;
            currentPcmDetails.totalIndices += sm.indexCount;
        }
    }

    currentPcmDetails.lodCount = (uint32_t)currentPcmDetails.lods.size();
    currentPcmDetails.totalSubmeshes = (uint32_t)currentPcmDetails.submeshes.size();

    if (currentPcmDetails.boneCount > 0 && currentPcmDetails.bonesOffset > 0 &&
        currentPcmDetails.bonesOffset + currentPcmDetails.boneCount * 64 <= pcmData.size()) {

        for (uint32_t i = 0; i < currentPcmDetails.boneCount; i++) {
            PCMBoneInfo bone;
            bone.index = (int)i;

            uint32_t boneOfs = currentPcmDetails.bonesOffset + i * 64;
            br.Seek(boneOfs + 48);
            bone.posX = br.Read<float>();
            bone.posY = br.Read<float>();
            bone.posZ = br.Read<float>();

            bone.parentIndex = -1;
            bone.inferredRole = "";

            currentPcmDetails.bones.push_back(bone);
        }
    }

    showPcmDetailsPanel = true;
}

void SpiderManTool::ExportSelectedWorldMesh(bool asGlb) {
    if (selectedMeshIndex < 0 || selectedMeshIndex >= (int)previewMeshes.size()) {

        return;
    }

    const auto& mesh = previewMeshes[selectedMeshIndex];

    fs::path outDir = fs::current_path() / "extracted" / "world_meshes";
    fs::create_directories(outDir);

    std::string baseName = mesh.meshName;
    if (baseName.empty()) {
        baseName = "mesh_" + std::to_string(selectedMeshIndex);
    }

    for (char& c : baseName) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
            c = '_';
        }
    }

    if (asGlb) {

        if (mesh.positions.empty() || mesh.indices.empty()) {

            return;
        }

        std::vector<uint16_t> triangleIndices;
        if (mesh.mode == GL_TRIANGLE_STRIP) {
            for (size_t i = 0; i + 2 < mesh.indices.size(); i++) {
                uint16_t i0 = mesh.indices[i];
                uint16_t i1 = mesh.indices[i + 1];
                uint16_t i2 = mesh.indices[i + 2];
                if (i0 == i1 || i1 == i2 || i0 == i2) continue;
                if (i % 2 == 0) {
                    triangleIndices.push_back(i0);
                    triangleIndices.push_back(i1);
                    triangleIndices.push_back(i2);
                } else {
                    triangleIndices.push_back(i0);
                    triangleIndices.push_back(i2);
                    triangleIndices.push_back(i1);
                }
            }
        } else {
            triangleIndices = mesh.indices;
        }

        if (triangleIndices.empty()) {

            return;
        }

        float minP[3] = {1e30f, 1e30f, 1e30f};
        float maxP[3] = {-1e30f, -1e30f, -1e30f};
        int vertexCount = (int)mesh.positions.size() / 3;
        for (int i = 0; i < vertexCount; i++) {
            float x = mesh.positions[i * 3];
            float y = mesh.positions[i * 3 + 1];
            float z = mesh.positions[i * 3 + 2];
            if (x < minP[0]) minP[0] = x;
            if (y < minP[1]) minP[1] = y;
            if (z < minP[2]) minP[2] = z;
            if (x > maxP[0]) maxP[0] = x;
            if (y > maxP[1]) maxP[1] = y;
            if (z > maxP[2]) maxP[2] = z;
        }

        std::vector<uint8_t> textureData;
        bool hasTexture = false;

        if (!mesh.textureName.empty()) {
            std::string texNameLower = StrToLower(mesh.textureName);

            if (globalTextureNameIndex.count(texNameLower)) {
                auto& loc = globalTextureNameIndex[texNameLower];
                std::ifstream texFile(loc.packPath, std::ios::binary);
                if (texFile.is_open()) {
                    texFile.seekg(loc.offset);
                    textureData.resize(loc.size);
                    texFile.read((char*)textureData.data(), loc.size);
                    texFile.close();
                    hasTexture = true;
                }
            }

            if (!hasTexture && mesh.textureHash != 0 && globalTextureIndex.count(mesh.textureHash)) {
                auto& loc = globalTextureIndex[mesh.textureHash];
                std::ifstream texFile(loc.packPath, std::ios::binary);
                if (texFile.is_open()) {
                    texFile.seekg(loc.offset);
                    textureData.resize(loc.size);
                    texFile.read((char*)textureData.data(), loc.size);
                    texFile.close();
                    hasTexture = true;
                }
            }
        }

        std::vector<uint8_t> binBuffer;
        auto alignBuffer = [&binBuffer]() {
            while (binBuffer.size() % 4 != 0) binBuffer.push_back(0);
        };
        auto addToBuffer = [&binBuffer](const void* data, size_t size) -> int {
            int offset = (int)binBuffer.size();
            const uint8_t* ptr = (const uint8_t*)data;
            binBuffer.insert(binBuffer.end(), ptr, ptr + size);
            return offset;
        };

        alignBuffer();
        int posOffset = addToBuffer(mesh.positions.data(), mesh.positions.size() * sizeof(float));
        int posLength = (int)(mesh.positions.size() * sizeof(float));

        alignBuffer();
        int normOffset = addToBuffer(mesh.normals.data(), mesh.normals.size() * sizeof(float));
        int normLength = (int)(mesh.normals.size() * sizeof(float));

        alignBuffer();
        int uvOffset = addToBuffer(mesh.uvs.data(), mesh.uvs.size() * sizeof(float));
        int uvLength = (int)(mesh.uvs.size() * sizeof(float));

        alignBuffer();
        int indOffset = addToBuffer(triangleIndices.data(), triangleIndices.size() * sizeof(uint16_t));
        int indLength = (int)(triangleIndices.size() * sizeof(uint16_t));

        int texOffset = 0, texLength = 0;
        if (hasTexture && !textureData.empty()) {
            alignBuffer();
            texOffset = addToBuffer(textureData.data(), textureData.size());
            texLength = (int)textureData.size();
        }

        alignBuffer();

        std::stringstream json;
        json << "{\"asset\":{\"version\":\"2.0\",\"generator\":\"USM\"},";

        json << "\"bufferViews\":[";
        json << "{\"buffer\":0,\"byteOffset\":" << posOffset << ",\"byteLength\":" << posLength << ",\"target\":34962},";
        json << "{\"buffer\":0,\"byteOffset\":" << normOffset << ",\"byteLength\":" << normLength << ",\"target\":34962},";
        json << "{\"buffer\":0,\"byteOffset\":" << uvOffset << ",\"byteLength\":" << uvLength << ",\"target\":34962},";
        json << "{\"buffer\":0,\"byteOffset\":" << indOffset << ",\"byteLength\":" << indLength << ",\"target\":34963}";
        if (hasTexture) {
            json << ",{\"buffer\":0,\"byteOffset\":" << texOffset << ",\"byteLength\":" << texLength << "}";
        }
        json << "],";

        json << "\"accessors\":[";
        json << "{\"bufferView\":0,\"componentType\":5126,\"count\":" << vertexCount << ",\"type\":\"VEC3\","
             << "\"min\":[" << minP[0] << "," << minP[1] << "," << minP[2] << "],"
             << "\"max\":[" << maxP[0] << "," << maxP[1] << "," << maxP[2] << "]},";
        json << "{\"bufferView\":1,\"componentType\":5126,\"count\":" << vertexCount << ",\"type\":\"VEC3\"},";
        json << "{\"bufferView\":2,\"componentType\":5126,\"count\":" << vertexCount << ",\"type\":\"VEC2\"},";
        json << "{\"bufferView\":3,\"componentType\":5123,\"count\":" << triangleIndices.size() << ",\"type\":\"SCALAR\"}";
        json << "],";

        if (hasTexture) {
            json << "\"images\":[{\"bufferView\":4,\"mimeType\":\"image/vnd-ms.dds\"}],";
            json << "\"samplers\":[{\"magFilter\":9729,\"minFilter\":9729,\"wrapS\":10497,\"wrapT\":10497}],";
            json << "\"textures\":[{\"sampler\":0,\"source\":0}],";
        }

        json << "\"materials\":[{\"name\":\"" << baseName << "\",\"doubleSided\":true,";
        if (hasTexture) {
            json << "\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0},\"metallicFactor\":0,\"roughnessFactor\":1},";
        } else {
            json << "\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.8,0.8,0.8,1],\"metallicFactor\":0,\"roughnessFactor\":1},";
        }
        const char* alphaMode = mesh.isAlphaTest ? "MASK" : (mesh.isTranslucent ? "BLEND" : "OPAQUE");
        json << "\"alphaMode\":\"" << alphaMode << "\"";
        if (mesh.isAlphaTest) json << ",\"alphaCutoff\":0.5";
        json << "}],";

        json << "\"meshes\":[{\"name\":\"" << baseName << "\",\"primitives\":[{"
             << "\"attributes\":{\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2},"
             << "\"indices\":3,\"material\":0}]}],";

        json << "\"nodes\":[{\"name\":\"" << baseName << "\",\"mesh\":0}],";

        json << "\"scenes\":[{\"nodes\":[0]}],\"scene\":0,";

        json << "\"buffers\":[{\"byteLength\":" << binBuffer.size() << "}]}";

        std::string jsonStr = json.str();
        while (jsonStr.size() % 4 != 0) jsonStr += " ";

        fs::path glbPath = outDir / (baseName + ".glb");
        std::ofstream out(glbPath, std::ios::binary);

        uint32_t magic = 0x46546C67;
        uint32_t version = 2;
        uint32_t totalLen = 12 + 8 + (uint32_t)jsonStr.size() + 8 + (uint32_t)binBuffer.size();

        out.write((char*)&magic, 4);
        out.write((char*)&version, 4);
        out.write((char*)&totalLen, 4);

        uint32_t jsonLen = (uint32_t)jsonStr.size();
        uint32_t jsonType = 0x4E4F534A;
        out.write((char*)&jsonLen, 4);
        out.write((char*)&jsonType, 4);
        out.write(jsonStr.c_str(), jsonLen);

        uint32_t binLen = (uint32_t)binBuffer.size();
        uint32_t binType = 0x004E4942;
        out.write((char*)&binLen, 4);
        out.write((char*)&binType, 4);
        out.write((char*)binBuffer.data(), binLen);

        out.close();

        if (hasTexture) {

            fs::path ddsPath = outDir / (baseName + ".dds");
            std::ofstream ddsOut(ddsPath, std::ios::binary);
            ddsOut.write((char*)textureData.data(), textureData.size());
            ddsOut.close();
            ShowNotification("Exported GLB + DDS to:\n" + outDir.string());
        } else {
            ShowNotification("Exported GLB to:\n" + glbPath.string() + "\n(no texture found)");
        }
    } else {

        if (mesh.sourcePack.empty() || mesh.sourceSize == 0) {

            return;
        }

        std::ifstream file(mesh.sourcePack, std::ios::binary);
        if (!file.is_open()) {

            return;
        }

        file.seekg(mesh.sourceOffset);
        std::vector<uint8_t> pcmData(mesh.sourceSize);
        file.read((char*)pcmData.data(), mesh.sourceSize);
        file.close();

        fs::path pcmPath = outDir / (baseName + ".pcm");
        std::ofstream out(pcmPath, std::ios::binary);
        if (out.is_open()) {
            out.write((char*)pcmData.data(), pcmData.size());
            out.close();
            ShowNotification("Exported PCM to:\n"     + pcmPath.string());
        }
    }
}
