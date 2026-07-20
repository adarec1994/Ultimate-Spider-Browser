#pragma once

// The NAL component skeleton writes matrices in its logical all_model_po order.
// A skinned conglomerate does not pass that array directly to ngl.  Each skin
// bone entity carries two byte indices in the cooked ENTITY resource:
//
//   entity_base::field_40  -> skeleton_interface::abs_po / PCM mesh-bone index
//   entity_base::rel_po_idx - 1 -> conglomerate::all_model_po / NAL index
//
// USM.exe sub_4BE0D0 loads skeleton_interface::abs_po, sub_4DB590 selects
// all_model_po[rel_po_idx - 1], and skeleton_interface::connect_bone_abs_po
// (called from conglomerate::_un_mash) points that entity at abs_po[field_40].
// This parser recovers the serialized permutation so preview and export use
// the same two index spaces as the game.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

struct EntityBoneMapping {
    bool valid = false;
    uint32_t meshPoseCount = 0;
    std::vector<int> logicalToMesh;
    std::vector<int> meshToLogical;
    std::string error;
};

namespace entity_bone_mapping_detail {

inline bool ReadU16(const std::vector<uint8_t>& data, size_t off, uint16_t& value) {
    if (off + 2 > data.size()) return false;
    std::memcpy(&value, data.data() + off, 2);
    return true;
}

inline bool ReadU32(const std::vector<uint8_t>& data, size_t off, uint32_t& value) {
    if (off + 4 > data.size()) return false;
    std::memcpy(&value, data.data() + off, 4);
    return true;
}

} // namespace entity_bone_mapping_detail

inline EntityBoneMapping ParseEntityBoneMapping(const std::vector<uint8_t>& data) {
    using namespace entity_bone_mapping_detail;
    EntityBoneMapping out;

    // generic_mash_header + complete PC conglomerate image.
    constexpr uint32_t kConglomerateImageSize = 0x130;
    constexpr uint32_t kMashVtableSentinel = 0x7ACE5BADu;
    if (data.size() < 0x10 + kConglomerateImageSize + 0x14) {
        out.error = "ENTITY resource is too small";
        return out;
    }

    uint32_t safetyKey = 0, flags = 0, sharedOffset = 0;
    uint16_t classId = 0, interfaceFlags = 0;
    ReadU32(data, 0x00, safetyKey);
    ReadU32(data, 0x04, flags);
    ReadU32(data, 0x08, sharedOffset);
    ReadU16(data, 0x0C, classId);
    ReadU16(data, 0x0E, interfaceFlags);

    const uint32_t expectedSafety =
        ((sharedOffset + 0x7BADBA5Du - (flags & 0x0FFFFFFFu) + classId + interfaceFlags) &
         0x0FFFFFFFu) | 0x70000000u;
    if (safetyKey != expectedSafety) {
        out.error = "ENTITY generic-mash safety key mismatch";
        return out;
    }
    if (classId != 5) {
        out.error = "ENTITY root is not a conglomerate";
        return out;
    }
    if ((interfaceFlags & 0x40u) == 0) {
        out.error = "conglomerate has no skeleton_interface";
        return out;
    }
    if (sharedOffset <= 0x10 + kConglomerateImageSize || sharedOffset > data.size()) {
        out.error = "ENTITY shared-stream offset is outside the resource";
        return out;
    }

    // conglomerate::_un_mash first aligns the main cursor to four and reads the
    // 0x14-byte skeleton_interface image.  po_count is the final uint32.
    const size_t skeletonInterface = 0x10 + kConglomerateImageSize;
    uint32_t poseCount = 0;
    if (!ReadU32(data, skeletonInterface + 0x10, poseCount) || poseCount == 0 || poseCount > 255) {
        out.error = "invalid skeleton_interface pose count";
        return out;
    }
    out.meshPoseCount = poseCount;

    struct Candidate {
        size_t offset = 0;
        uint8_t mesh = 0;
        uint8_t logical = 0;
    };
    std::vector<Candidate> candidates;

    // Child entity images are 16-byte aligned in the main mash stream.  All
    // derived entity types preserve entity_base's first 0x44 bytes.  The
    // serialized vtable sentinel plus the exact common-field constraints keep
    // this independent of the child's derived class size while avoiding data
    // payloads.  A result is accepted only if it assigns every mesh pose once
    // and assigns distinct logical poses.  The logical index space is allowed
    // to be sparse and larger than po_count: NICK_FURY has 59 mesh poses but
    // references logical indices 0..59 with logical slot 57 absent.
    for (size_t image = 0x10; image + 0x44 <= sharedOffset; image += 0x10) {
        uint32_t sentinel = 0;
        ReadU32(data, image, sentinel);
        if (sentinel != kMashVtableSentinel) continue;

        uint16_t regionIndex = 0;
        ReadU16(data, image + 0x3C, regionIndex);
        const uint8_t mesh = data[image + 0x40];
        const uint8_t logicalPlusOne = data[image + 0x42];
        if (regionIndex != 0xFFFFu || mesh >= poseCount || logicalPlusOne == 0)
            continue;
        candidates.push_back({image, mesh, uint8_t(logicalPlusOne - 1)});
    }

    // There may be non-skin conglomerate members before the skin-bone block.
    // Find complete windows in stream order; require exactly one valid window
    // so malformed/ambiguous resources never silently receive a guessed map.
    std::vector<std::vector<Candidate>> validWindows;
    if (candidates.size() >= poseCount) {
        for (size_t first = 0; first + poseCount <= candidates.size(); ++first) {
            std::vector<uint8_t> seenMesh(poseCount, 0);
            std::vector<uint8_t> seenLogical(256, 0);
            bool valid = true;
            for (size_t i = 0; i < poseCount; ++i) {
                const auto& c = candidates[first + i];
                if (seenMesh[c.mesh] || seenLogical[c.logical]) {
                    valid = false;
                    break;
                }
                seenMesh[c.mesh] = 1;
                seenLogical[c.logical] = 1;
            }
            if (!valid || std::find(seenMesh.begin(), seenMesh.end(), uint8_t(0)) != seenMesh.end())
                continue;
            validWindows.emplace_back(candidates.begin() + first,
                                      candidates.begin() + first + poseCount);
        }
    }

    if (validWindows.empty()) {
        out.error = "no complete skin-bone entity permutation found";
        return out;
    }

    // Overlapping start positions can describe the same exact set only when
    // there are extraneous candidates.  Collapse byte-identical mappings, but
    // reject genuinely different complete mesh-to-logical assignments.
    std::vector<int> chosenMeshToLogical(poseCount, -1);
    auto materialize = [&](const std::vector<Candidate>& window) {
        std::vector<int> map(poseCount, -1);
        for (const auto& c : window) map[c.mesh] = c.logical;
        return map;
    };
    chosenMeshToLogical = materialize(validWindows.front());
    for (size_t i = 1; i < validWindows.size(); ++i) {
        if (materialize(validWindows[i]) != chosenMeshToLogical) {
            out.error = "ambiguous skin-bone entity permutations";
            return out;
        }
    }

    int maxLogical = *std::max_element(chosenMeshToLogical.begin(), chosenMeshToLogical.end());
    out.meshToLogical = std::move(chosenMeshToLogical);
    out.logicalToMesh.assign((size_t)maxLogical + 1, -1);
    for (uint32_t mesh = 0; mesh < poseCount; ++mesh) {
        const int logical = out.meshToLogical[mesh];
        out.logicalToMesh[logical] = (int)mesh;
    }
    out.valid = true;
    return out;
}
