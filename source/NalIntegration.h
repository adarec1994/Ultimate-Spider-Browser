#pragma once
// NalIntegration.h - Integration layer connecting NalSkeleton/NalAnimation into SpiderManTool
// Add this #include to SpiderManTool.h and add the members/methods below to the class
// LemonHaze / Claude - 2025

#include "NalSkeleton.h"
#include "NalAnimation.h"
#include <optional>

// ─── Add these members to SpiderManTool class ───
// (paste into SpiderManTool.h inside the class body)
/*
    // === NAL Skeleton / Animation support ===
    std::optional<NalSkeletonData> loadedSkeleton;
    std::optional<NalAnimFile>     loadedAnimFile;
    int selectedAnimIndex = -1;
    int currentAnimFrame  = 0;
    bool isAnimPlaying    = false;
    float animPlaybackTime = 0.f;

    void LoadSkeletonForModel(int fileIndex);
    void LoadAnimationFile(const std::string& path);
    void UpdateAnimation(float deltaTime);
    std::string FindSkeletonPath(const std::string& modelName);
    std::string FindAnimationPath(const std::string& modelName);
*/

// ─── Skinned vertex struct for stride-64 mesh sections ───
// Replaces the simple Vertex struct in Model.cpp
struct SkinnedVertex {
    float x, y, z;          // position
    float nx, ny, nz;       // normal
    float u, v;              // texcoord
    float boneIdx[4];       // bone indices (as floats, per nglMeshSection)
    float boneWgt[4];       // bone weights
};

// ─── Per-section bone palette reader ───
// Reads NBones and BonesIdx from nglMeshSection to build local→global bone index map
// In pcmesh.py: meshSection.NBones, meshSection.BonesIdx
// nglMeshSection layout (from pcmesh.py):
//   +0x00: Name(4), Material(4), NBones(4), BonesIdx(4)
//   +0x10: SphereCenter(16), SphereRadius(4), Flags(4)
//   +0x20: m_primitiveType(4), NIndices(4), m_indices(4), m_indexBuffer(4)
//   +0x30: NVertices(4), VertexBuffer{m_vertexData(4), Size(4), m_vertexBuffer(4)}
//   +0x40: m_stride(4), ...
struct PCMSectionBonePalette {
    uint32_t nBones = 0;
    uint32_t bonesIdxOffset = 0;
    std::vector<uint16_t> palette;

    bool load(const std::vector<uint8_t>& pcmData, uint32_t sectionOffset) {
        if (sectionOffset + 16 > pcmData.size()) return false;
        memcpy(&nBones, &pcmData[sectionOffset + 8], 4);
        memcpy(&bonesIdxOffset, &pcmData[sectionOffset + 12], 4);
        if (nBones == 0 || bonesIdxOffset == 0) return false;
        if (bonesIdxOffset + nBones * 2 > pcmData.size()) return false;
        palette.resize(nBones);
        memcpy(palette.data(), &pcmData[bonesIdxOffset], nBones * 2);
        return true;
    }

    // Map local bone index → global
    int mapIndex(int localIdx) const {
        if (localIdx < 0 || palette.empty()) return localIdx;
        if (localIdx < (int)palette.size()) return (int)palette[localIdx];
        return localIdx;
    }
};

// ─── Read bone matrices from PCM mesh ───
// nglMesh layout: Name(4), Flags(4), NSections(4), Sections(4), NBones(4), Bones(4), ...
// Each bone matrix is 4x4 floats (64 bytes)
struct PCMBoneMatrices {
    std::vector<std::array<float, 16>> matrices;

    bool load(const std::vector<uint8_t>& pcmData, uint32_t meshOffset) {
        if (meshOffset + 24 > pcmData.size()) return false;
        uint32_t nBones, bonesOffset;
        memcpy(&nBones, &pcmData[meshOffset + 16], 4);
        memcpy(&bonesOffset, &pcmData[meshOffset + 20], 4);
        if (nBones == 0 || bonesOffset == 0) return false;
        if (bonesOffset + nBones * 64 > pcmData.size()) return false;
        matrices.resize(nBones);
        for (uint32_t i = 0; i < nBones; ++i) {
            memcpy(matrices[i].data(), &pcmData[bonesOffset + i * 64], 64);
        }
        return true;
    }
};

// ─── Utility: read skinned vertex from stride-64 buffer ───
static inline SkinnedVertex ReadSkinnedVertex(const uint8_t* ptr, uint32_t stride) {
    SkinnedVertex v = {};
    memcpy(&v.x, ptr, 12); // pos
    if (stride >= 64) {
        memcpy(&v.nx, ptr + 12, 12); // normal
        memcpy(&v.u, ptr + 24, 8);   // uv
        memcpy(&v.boneIdx, ptr + 32, 16); // bone indices (4 floats)
        memcpy(&v.boneWgt, ptr + 48, 16); // bone weights (4 floats)
    } else if (stride >= 24) {
        memcpy(&v.u, ptr + 12, 8);  // uv only
    }
    return v;
}

// ─── Map bone indices through section palette ───
// Matches pcmesh.py lines 1354-1369
static inline void MapBoneIndicesToGlobal(SkinnedVertex& v, const PCMSectionBonePalette& palette) {
    for (int i = 0; i < 4; ++i) {
        int localIdx = (int)v.boneIdx[i];
        if (localIdx < 0 || v.boneWgt[i] <= 0.f) {
            v.boneIdx[i] = 0.f;
            v.boneWgt[i] = 0.f;
        } else {
            v.boneIdx[i] = (float)palette.mapIndex(localIdx);
        }
    }
    // Normalize weights
    float total = v.boneWgt[0] + v.boneWgt[1] + v.boneWgt[2] + v.boneWgt[3];
    if (total > 1e-8f) {
        float inv = 1.f / total;
        for (int i = 0; i < 4; ++i) v.boneWgt[i] *= inv;
    } else {
        v.boneWgt[0] = 1.f;
    }
}

// ─── Search for skeleton/animation files near a pack ───
// The PCPACK resource directory can contain pcskel/pcanim resources.
// This helper searches the pack's entries for skeleton files.
static inline int FindPCSkelEntry(const std::vector<FileEntry>& entries) {
    for (int i = 0; i < (int)entries.size(); ++i) {
        // Skeleton files have a specific signature we can check
        std::string lower = StrToLower(entries[i].name);
        if (lower.find(".pcskel") != std::string::npos ||
            lower.find("skeleton") != std::string::npos ||
            lower.find("_skel") != std::string::npos) {
            return i;
        }
    }
    return -1;
}

static inline int FindPCAnimEntry(const std::vector<FileEntry>& entries) {
    for (int i = 0; i < (int)entries.size(); ++i) {
        std::string lower = StrToLower(entries[i].name);
        if (lower.find(".pcanim") != std::string::npos ||
            lower.find("anim") != std::string::npos) {
            // Check it's not just a name containing "anim" - verify the data signature
            return i;
        }
    }
    return -1;
}

// ─── Extract skeleton data from PCPACK inline data ───
static inline std::optional<NalSkeletonData> ParseSkeletonFromPCPACK(
    const std::vector<uint8_t>& pcPackData,
    const std::vector<FileEntry>& entries,
    const std::string& tempDir = ".")
{
    int idx = FindPCSkelEntry(entries);
    if (idx < 0) return std::nullopt;

    const auto& e = entries[idx];
    if (e.offset + e.size > pcPackData.size()) return std::nullopt;

    // Write temp file and parse it
    std::string tempPath = tempDir + "/temp_skel.pcskel";
    {
        std::ofstream tmp(tempPath, std::ios::binary);
        if (!tmp.is_open()) return std::nullopt;
        tmp.write((const char*)&pcPackData[e.offset], e.size);
    }

    auto result = ParseNalSkeleton(tempPath);
    std::remove(tempPath.c_str());
    return result;
}
