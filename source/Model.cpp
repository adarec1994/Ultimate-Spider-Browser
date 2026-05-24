#include "SpiderManTool.h"
#include "NalIntegration.h"
#include <glad/glad.h>
#include <sstream>
#include <fstream>
#include <cmath>
#include <algorithm>
#include <map>

static constexpr int PREVIEW_MAX_BONES = 64;

static std::string ReadStringTableEntry(const std::vector<uint8_t>& data, uint32_t offset) {
    if (offset == 0 || offset + 32 > data.size()) return "";
    size_t strStart = offset + 4;
    size_t end = strStart;
    while (end < strStart + 28 && end < data.size() && data[end] != 0) end++;
    return std::string((char*)&data[strStart], end - strStart);
}

// Decode a packed D3DCOLOR uint32 (0xAARRGGBB) into floats. Used for the
// per-vertex baked color stored at +20 of stride-24/32/60 PCM vertices.
static inline void DecodeD3DColor(uint32_t v, float& r, float& g, float& b, float& a) {
    constexpr float k = 1.0f / 255.0f;
    b = (float)((v >>  0) & 0xFF) * k;
    g = (float)((v >>  8) & 0xFF) * k;
    r = (float)((v >> 16) & 0xFF) * k;
    a = (float)((v >> 24) & 0xFF) * k;
}

// Normalise the bone-palette + per-vertex bone indices for one mesh section.
//
// Why this exists: the GL shader uses a fixed `mat4 boneMatrices[64]` and the
// vertex stores section-LOCAL bone indices that look those slots up. PCM sections
// either ship an explicit palette (NBones + BonesIdx) -- in which case vertex
// indices are already section-local -- or ship no palette, in which case the
// vertex indices are GLOBAL skeleton bone IDs (per Archive/pcmesh-blender-master
// pcmesh.py:1353-1369). The previous code clamped any local idx >= 64 to zero,
// which silently destroyed skinning on (a) characters with >64 total bones, and
// (b) any palette-less section whose global IDs happen to exceed 64 -- both
// produce the "vertices warp all over" symptom because some weights snap to
// bone 0 while others move.
//
// The fix mirrors the Python reference: promote each raw index to a global ID
// first, then build a *synthetic* per-section palette from the unique globals
// actually referenced, and rewrite each vertex to point into that palette.
// Sections that fit cleanly into the existing palette keep it untouched.
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
            return -1;                              // out of palette range -> drop
        }
        return raw;                                 // no palette -> raw is global
    };

    // First check whether the file's palette already maps every vertex without
    // overflow. If so, leave it alone -- preserves the authored ordering.
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
        // File palette is fine. Just sanity-check vertex indices.
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

    // Build the global-ID set referenced by this section's vertices.
    std::map<int, int> globalToLocal; // global bone id -> new local slot
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
                    // Synthetic palette is full -- drop this weight rather than
                    // silently mapping it to slot 0 (which would warp the vert).
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
        // Re-normalise weights after any drops.
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

// Classify the shader-name string at material +0x04 (stored as a tlFixedString
// pointer to the shader's registered name — see OpenUSM ngl.cpp:2709). Empirically
// THE ONLY reliable signal: the in-memory m_blend_mode at +0x4C is always zero on
// disk (engine sets it after load via the shader's BindMaterial hook). Shader names
// observed in real world data: smtranslucent, ustranslucenttrilinear,
// ustranslucentinterior, smsimple, ussimpletrilinear, usstreet, ussimpleinterior,
// usshinyinterior, usbuildingsimple, usfloor, usmsimplemorphableinterior,
// uscolorvol, smshiny.
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

// Color volumes (uscolorvol) feed a separate post-process pass in the engine
// (USColorVolShaderSpace::gUSColorVolScene -> wds_render_manager::create_colorvol_scene)
// and are never drawn as visible geometry. They sit in level data as boxy
// invisible meshes that say "tint everything inside this volume". Without
// detection, they render as opaque white/black blocks all over the level.
static bool IsColorVolumeShader(const std::string& shaderName) {
    std::string n = shaderName;
    for (auto& c : n) c = (char)tolower((unsigned char)c);
    return n.find("colorvol") != std::string::npos ||
           n.find("color_vol") != std::string::npos ||
           n.find("colorvolume") != std::string::npos;
}

// Stencil shadow volumes are rendered through the engine's stencil pass
// (wds_render_manager::render_stencil_shadows -> 0x0053D5E0) -- never as
// visible polygons. They're typically extruded silhouettes of dynamic objects.
// We detect them by shader name and the renderer skips them by default.
static bool IsShadowVolumeShader(const std::string& shaderName) {
    std::string n = shaderName;
    for (auto& c : n) c = (char)tolower((unsigned char)c);
    // Match "shadowvolume", "shadow_volume", "shadowvol", "smshadowvol".
    // We intentionally do NOT match plain "shadow" -- fake/decal shadows use
    // names like "us_decal3d" or share a "shadow" substring but ARE meant to
    // be visible (the blob under a character's feet).
    return n.find("shadowvolume") != std::string::npos ||
           n.find("shadow_volume") != std::string::npos ||
           n.find("shadowvol") != std::string::npos;
}

// Texture-name location depends on the derived material type, which we infer
// from the entry's size field. The PCM serialised material is one of several
// shader-specific structs in OpenUSM (Archive/OpenUSM):
//
//   28   uscolorvol         color volume, no texture
//   48   PCUV_ShaderMaterial (us_pcuv_shader.h): vtbl(4) + section*(4) + shader*(4)
//                            + empty[16] + field_1C(tlFixedString*) + texture*(4)
//                            + blend_mode(4) + 2 ints = 0x30 bytes
//                            → texture name at +0x1C. This is the shader used by
//                            many world props -- prior to this entry being added
//                            here, alleyway walls / doors / dumpsters all came
//                            back textureless.
//   60   us_floor-style small material, same head layout as PCUV
//                            → texture name at +0x1C (best-effort; some are
//                            genuinely textureless and the lookup will no-op)
//   64   USInteriorMaterial (us_interior.h): char[0x1C] + field_1C(tlFixedString*)
//                            + texture*(4) = 0x24 minimum, padded out by the
//                            wrapping shader to 0x40
//                            → texture name at +0x1C
//   68   ustranslucentinterior / usmsimplemorphableinterior: same as 64 + 4
//                            → texture name at +0x1C
//   80   nglMaterialBase (ngl.h:152, VALIDATE_SIZE 0x50): the base material
//                            class, m_blend_mode at +0x4C. Field at +0x18 is a
//                            tlFixedString* for the (first) texture name.
//                            → texture name at +0x18
//   88   nglMaterialBase + 8 bytes (some derived variant)
//                            → texture name at +0x18 (same field, struct grew)
//   116+ USExteriorMaterial (us_exterior.h): nglMaterialBase + 0x10 + field_60
//                            (tlFixedString*) + texture* + extras. Used by
//                            smshiny, usbuildingsimple, usstreet, smsimple,
//                            ustranslucenttrilinear, smtranslucent.
//                            → texture name at +0x60
//
// For interior shaders the +0x18 field of any wrapping nglMaterialBase is NOT
// dereferenced as a texture pointer; the shader extension at +0x1C is. Falling
// back to mesh-name lookup catches most interior textures because material
// name == texture name for those.
static uint32_t LocateTextureOffset(uint16_t size) {
    if (size == 48)               return 0x1C;   // PCUV_ShaderMaterial (the alley fix)
    if (size == 60)               return 0x1C;   // us_floor and similar small shaders
    if (size == 64 || size == 68) return 0x1C;   // interior material variants
    if (size == 80 || size == 88) return 0x18;   // nglMaterialBase / character / base
    if (size >= 116)              return 0x60;   // shiny / street / exterior derivatives
    return 0;                                    // unknown — caller should skip
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
        mat.shaderType = shaderType;

        // The on-disk m_blend_mode at +0x4C is always zero in practice — the engine
        // sets it at load time via the shader's BindMaterial hook. The shader-name
        // string is what's actually authored. (Confirmed empirically across an
        // entire world load: every byte at +0x4C reads as 0.)
        uint32_t blendMode = ClassifyByShaderName(mat.alphaFlag);
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

    // Vertex layout matches OpenUSM's PCUV vertex declaration once unpacked.
    // The trailing `cr/cg/cb/ca` quartet is per-vertex baked color (D3DCOLOR
    // uint32 at +20 of the stride-24/32/60 disk format, or white for stride 64).
    // It feeds the GL color attribute that the fragment shader multiplies the
    // texture by -- direct port of us_pcuv_PS.txt's `mul r0, t0, v0`.
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

            // Read per-section bone palette (maps local bone indices to global)
            // nglMeshSection: NBones at offset +8, BonesIdx at offset +12
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

                // Default to white -- correct for stride-64 (skinned) which has no
                // color slot, and a safe fallback if the color read fails.
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
                    // Read bone indices and weights (4 floats each, at offset 32 and 48).
                    // STORE THEM RAW for now -- they may be section-local (per
                    // palette) or already global (when NBones==0). The
                    // synthetic-palette pass below normalises both cases.
                    if (startV + 64 <= pcmData.size()) {
                        br.Seek(startV + 32);
                        for (int bi = 0; bi < 4; bi++) vert.boneIdx[bi] = br.Read<float>();
                        for (int bi = 0; bi < 4; bi++) vert.boneWgt[bi] = br.Read<float>();
                        // Normalise weights only -- defer index validation.
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
                    // D3DCOLOR baked vertex color at +20 (per pcmesh.py stride-24 layout).
                    if (startV + 24 <= pcmData.size()) {
                        br.Seek(startV + 20);
                        uint32_t packed = br.Read<uint32_t>();
                        DecodeD3DColor(packed, vert.cr, vert.cg, vert.cb, vert.ca);
                    }
                } else if (stride == 32 || stride == 60) {
                    // Same head layout as stride 24; trailing bytes are padding or
                    // shader-specific data we don't read.
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
            mesh.indexCount = (int)indices.size();
            mesh.mode = (itype == 4) ? GL_TRIANGLES : GL_TRIANGLE_STRIP;
            mesh.textureId = tex;
            mesh.blendMode = mat.blendMode;
            mesh.isTranslucent = mat.isTranslucent;
            mesh.isAlphaTest = mat.isAlphaTest;
            mesh.shaderType = mat.shaderType;
            // Resolve indices and palette together (handles the no-palette and
            // overflow cases that cause skinning to warp). Only meaningful for
            // stride-64 skinned sections.
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
            // Prefer shader-name detection (set by ParseMaterialEntries from
            // mat.alphaFlag); fall back to mesh-name heuristic.
            mesh.isColorVolume  = mat.isColorVolume  || IsColorVolumeMesh(meshNameLower);
            mesh.isShadowVolume = mat.isShadowVolume;
            // Mark non-renderable engine data (collision proxies, triggers,
            // placeholder meshes, color/shadow volumes) so the renderer draws
            // them as a translucent ghost overlay. Either the file name OR
            // the mesh section name can trigger this.
            mesh.isDebugTransparent = mesh.isColorVolume || mesh.isShadowVolume ||
                                      IsNonRenderableMeshName(modelNameLower) ||
                                      IsNonRenderableMeshName(meshNameLower);
            // Lens flares / light cones / glow sprites need additive blending.
            // The author shader for these (still in the binary) isn't covered
            // by our shader-name classifier; force the right blend mode here
            // so they read as glowing bloom instead of flat white quads.
            if (IsAdditiveGlowMeshName(modelNameLower) ||
                IsAdditiveGlowMeshName(meshNameLower)) {
                mesh.blendMode     = NGLBM_ADDITIVE;
                mesh.isTranslucent = true;
                mesh.isAlphaTest   = false;
            }
            // Water: route into the dedicated water shader path. Always
            // translucent so we see through to whatever's below the surface.
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


            mesh.textureName = mat.textureName;
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
            glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)(8*sizeof(float))); glEnableVertexAttribArray(3);  // boneIdx
            glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)(12*sizeof(float))); glEnableVertexAttribArray(4); // boneWgt
            glVertexAttribPointer(5, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)(16*sizeof(float))); glEnableVertexAttribArray(5); // baked vertex color (RGBA)
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

void SpiderManTool::BatchWorldMeshesByType() {
    if (!isWorldMode || previewMeshes.empty()) return;

    struct BatchVertex {
        float x, y, z;
        float nx, ny, nz;
        float u, v;
        float boneIdx[4];
        float boneWgt[4];
        // Per-vertex baked color (RGBA in [0,1]); see comment on `struct Vertex`.
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
                // Carry per-vertex baked color through the batch; default to white
                // if the source mesh predates colors.
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
    (void)oldCount;
    (void)batchCount;
}

void SpiderManTool::AddMeshFromDataWithTransform(const std::vector<uint8_t>& pcmData, std::string modelName, std::function<unsigned int(uint32_t)> textureResolver, const std::string& sourcePack, uint32_t sourceOffset, const float* transform, uint32_t onlyMeshOffset) {
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

    // Vertex layout matches OpenUSM's PCUV vertex declaration once unpacked.
    // The trailing `cr/cg/cb/ca` quartet is per-vertex baked color (D3DCOLOR
    // uint32 at +20 of the stride-24/32/60 disk format, or white for stride 64).
    // It feeds the GL color attribute that the fragment shader multiplies the
    // texture by -- direct port of us_pcuv_PS.txt's `mul r0, t0, v0`.
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

            // Read per-section bone palette
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

                // Default to white -- see comment in the matching block of AddMeshFromData.
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
                    // Read raw bone indices/weights; ResolveSectionBonePalette
                    // below normalises them against the section/global palette
                    // (see comment on the helper in this file).
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
            // See matching block in AddMeshFromData for the rationale.
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

            mesh.textureName = mat.textureName;
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
            glVertexAttribPointer(5, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)(16*sizeof(float))); glEnableVertexAttribArray(5); // baked vertex color (RGBA)
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
            br.Read<uint32_t>(); // ptrShaderRef

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

        file.seekg(24);
        uint32_t headerSize, packDataOffset;
        file.read((char*)&headerSize, 4);
        file.read((char*)&packDataOffset, 4);

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

            if (size > 4) {
                size_t filePos = file.tellg();
                uint32_t absOffset = (uint32_t)(packDataOffset + offset);

                if (absOffset + 4 > fileSize) {
                    file.seekg(filePos);
                    continue;
                }

                file.seekg(absOffset);
                uint32_t sig = 0;
                file.read((char*)&sig, 4);

                if (file.good() && sig == 0x204D4350) {
                    std::string entryName = "";
                    if (dictionary.count(hash)) entryName = StrToLower(dictionary[hash]);
                    if (entryName == modelName) {
                        file.seekg(absOffset);
                        std::vector<uint8_t> pcmData(size);
                        file.read((char*)pcmData.data(), size);
                        AddMeshFromData(pcmData, modelName, nullptr);
                        file.close();
                        break;
                    }
                }

                file.clear();
                file.seekg(filePos);
            }
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
    }
    previewMeshes.clear();

    if (e.offset + e.size > pcPackData.size()) return;

    std::vector<uint8_t> pcmData(pcPackData.begin() + e.offset, pcPackData.begin() + e.offset + e.size);

    // Pick the best-matching skeleton for this mesh BEFORE parsing vertices --
    // the bone-palette path below needs `loadedSkeleton` to be the right one.
    // LoadSkeletonForCurrentPack populates skeletonCandidates with every
    // .pcskel-shaped entry; we just promote the one whose name matches.
    SelectSkeletonForMesh(e.name);

    AddMeshFromData(pcmData, e.name, nullptr);

    // Build skeleton visualization from bone data
    BuildSkeletonVisual(pcmData);

    modelCenter[0] = 0; modelCenter[1] = 0; modelCenter[2] = 0;
    modelRadius = 10.0f;

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

            float radius = sqrt(pow(maxP[0]-minP[0],2) + pow(maxP[1]-minP[1],2) + pow(maxP[2]-minP[2],2));
            if (radius < 1.0f) radius = 5.0f;

            camPos[0] = cx;
            camPos[1] = cy + radius * 0.5f;
            camPos[2] = cz + radius * 1.5f;

            float target[3] = {cx, cy, cz};
            float dir[3] = {target[0]-camPos[0], target[1]-camPos[1], target[2]-camPos[2]};
            Normalize(dir);
            camFront[0] = dir[0]; camFront[1] = dir[1]; camFront[2] = dir[2];

            camYaw = atan2(camFront[2], camFront[0]) * 180.0f / 3.14159f;
            camPitch = asin(camFront[1]) * 180.0f / 3.14159f;
        }
    }
}

void SpiderManTool::ConvertPCM(const std::vector<uint8_t>& pcmData, const std::string& outPath) {
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
        bool isTranslucent;
        bool isAlphaTest;
        std::vector<float> pos, norm, uvs, weights;
        std::vector<uint16_t> joints, indices;
        float minP[3], maxP[3];
    };
    std::vector<SubmeshData> submeshes;
    std::vector<float> allIBMs;
    uint32_t totalBones = 0;
    std::string modelName = "Model";

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

        for(auto& [matRef, smOfs] : smRefs) {
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

            // Read bone palette for this section
            PCMSectionBonePalette bonePalette;
            bonePalette.load(pcmData, smOfs);

            SubmeshData sm;
            sm.name = ReadName(meshNameRef);
            sm.textureName = mat.textureName;
            sm.isTranslucent = mat.isTranslucent;
            sm.isAlphaTest = mat.isAlphaTest;

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

                    // Read bone indices + weights (4 floats each at offsets 32, 48)
                    if (startV + 64 <= pcmData.size()) {
                        br.Seek(startV + 32);
                        float bIdx[4], bWgt[4];
                        for (int bi = 0; bi < 4; bi++) bIdx[bi] = br.Read<float>();
                        for (int bi = 0; bi < 4; bi++) bWgt[bi] = br.Read<float>();

                        // Map through palette
                        for (int bi = 0; bi < 4; bi++) {
                            int localIdx = (int)bIdx[bi];
                            if (localIdx >= 0 && bWgt[bi] > 0.f && !bonePalette.palette.empty())
                                bIdx[bi] = (float)bonePalette.mapIndex(localIdx);
                            else if (bWgt[bi] <= 0.f)
                                bIdx[bi] = 0.f;
                        }
                        // Normalize weights
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

    // Check if any submesh has bone data
    bool hasSkinning = false;
    for (auto& sm : submeshes) {
        if (!sm.joints.empty() && !sm.weights.empty()) {
            hasSkinning = true;
            break;
        }
    }

    // Read bone matrices from PCM for inverse bind matrices
    // Look for mesh entry to get bone matrices offset
    PCMBoneMatrices boneMats;
    for (auto& inf : infos) {
        if (inf.type != 512) continue;
        boneMats.load(pcmData, inf.offset);
        break;
    }

    SkinningGLBWriter writer;

    // If we have skinning data and bone matrices, set up skin nodes
    int ibmAccessor = -1;
    if (hasSkinning && !boneMats.matrices.empty()) {
        totalBones = (uint32_t)boneMats.matrices.size();

        // Add bone nodes
        for (uint32_t bi = 0; bi < totalBones; bi++) {
            std::string boneName = "bone_" + std::to_string(bi);
            int boneNode = writer.AddNode(boneName);
            writer.AddJoint(boneNode);
        }

        // Write inverse bind matrices
        // The PCM stores bind matrices; we use them as-is for the IBM
        // (Blender/viewers will compute the actual inverse)
        std::vector<float> ibmData;
        for (auto& mat : boneMats.matrices) {
            // Convert row-major 4x4 to column-major for glTF
            for (int c = 0; c < 4; c++)
                for (int r = 0; r < 4; r++)
                    ibmData.push_back(mat[r * 4 + c]);
        }
        int ibmView = writer.AddBufferView(ibmData.data(), ibmData.size() * sizeof(float), 0);
        ibmAccessor = writer.AddAccessor(ibmView, 5126, (int)totalBones, "MAT4");
    }

    for (size_t si = 0; si < submeshes.size(); si++) {
        auto& sm = submeshes[si];

        int matIdx = writer.AddMaterial(sm.name.empty() ? "material_" + std::to_string(si) : sm.name, sm.isTranslucent, sm.isAlphaTest);

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

        std::string meshName = sm.name.empty() ? "submesh_" + std::to_string(si) : sm.name;
        int meshIdx = writer.StartMesh(meshName);
        writer.AddPrimitive(posAcc, normAcc, uvAcc, indAcc, jointAcc, weightAcc, matIdx);
        writer.EndMesh();

        int skinIdx = (hasSkinning && jointAcc >= 0) ? 0 : -1;
        int nodeIdx = writer.AddNode(meshName, meshIdx, skinIdx);
        writer.AddToScene(nodeIdx);
    }

    writer.WriteToFile(outPath, ibmAccessor);
    Log("Converted to: " + outPath);
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

        // Shader-name is the only reliable blend-mode source; see ParseMaterialEntries.
        // PCMMaterialInfo (analyze panel) doesn't carry color/shadow-volume flags --
        // the renderer reads those from MaterialDef. Keep this branch limited to the
        // info this struct exposes.
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


            // Parent hierarchy is in nalCompSkeleton (not in PCM bone data)
            bone.parentIndex = -1;
            bone.inferredRole = "";

            currentPcmDetails.bones.push_back(bone);
        }
    }


    showPcmDetailsPanel = true;
}

void SpiderManTool::ExportSelectedWorldMesh(bool asGlb) {
    if (selectedMeshIndex < 0 || selectedMeshIndex >= (int)previewMeshes.size()) {
        Log("No mesh selected");
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
            Log("Selected mesh has no geometry data for export");
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
            Log("No valid triangles in mesh");
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
            Log("No source PCM data for selected mesh");
            return;
        }

        std::ifstream file(mesh.sourcePack, std::ios::binary);
        if (!file.is_open()) {
            Log("Failed to open source pack");
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
