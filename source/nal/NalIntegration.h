#pragma once

#include "NalSkeleton.h"
#include "NalAnimation.h"
#include <optional>

struct SkinnedVertex {
    float x, y, z;
    float nx, ny, nz;
    float u, v;
    float boneIdx[4];
    float boneWgt[4];
};

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

    int mapIndex(int localIdx) const {
        if (localIdx < 0 || palette.empty()) return localIdx;
        if (localIdx < (int)palette.size()) return (int)palette[localIdx];
        return localIdx;
    }
};

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

static inline SkinnedVertex ReadSkinnedVertex(const uint8_t* ptr, uint32_t stride) {
    SkinnedVertex v = {};
    memcpy(&v.x, ptr, 12);
    if (stride >= 64) {
        memcpy(&v.nx, ptr + 12, 12);
        memcpy(&v.u, ptr + 24, 8);
        memcpy(&v.boneIdx, ptr + 32, 16);
        memcpy(&v.boneWgt, ptr + 48, 16);
    } else if (stride >= 24) {
        memcpy(&v.u, ptr + 12, 8);
    }
    return v;
}

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

    float total = v.boneWgt[0] + v.boneWgt[1] + v.boneWgt[2] + v.boneWgt[3];
    if (total > 1e-8f) {
        float inv = 1.f / total;
        for (int i = 0; i < 4; ++i) v.boneWgt[i] *= inv;
    } else {
        v.boneWgt[0] = 1.f;
    }
}

static inline int FindPCSkelEntry(const std::vector<FileEntry>& entries) {
    for (int i = 0; i < (int)entries.size(); ++i) {

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

            return i;
        }
    }
    return -1;
}

static inline std::optional<NalSkeletonData> ParseSkeletonFromPCPACK(
    const std::vector<uint8_t>& pcPackData,
    const std::vector<FileEntry>& entries,
    const std::string& tempDir = ".")
{
    int idx = FindPCSkelEntry(entries);
    if (idx < 0) return std::nullopt;

    const auto& e = entries[idx];
    if (e.offset + e.size > pcPackData.size()) return std::nullopt;

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
