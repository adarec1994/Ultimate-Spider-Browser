#pragma once
// NalAnimation.h - C++ port of pcanim.py
// Parses .pcanim container files and decodes animation track data
// LemonHaze / Claude - 2025

#include "NalAnimCodec.h"
#include "NalSkeleton.h"
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include <algorithm>

constexpr uint32_t NAL_ANIM_CONTAINER = 0x00010101;
constexpr uint32_t NAL_CHAR_ANIM      = 0x00010003;
constexpr uint32_t NAL_GEN_ANIM       = 0x00010200;
constexpr uint32_t NAL_FLAG_LOOPING    = 0x00000001;
constexpr uint32_t NAL_FLAG_SCENE_ANIM = 0x00020000;
constexpr float    NAL_PREVIEW_FPS     = 30.0f;

// ─── NAL component type → iComponentID mapping ───
static inline int nal_type_to_comp_id(uint32_t type_hash) {
    switch (type_hash) {
    case NalCompType::ArbitraryPO:               return NalComp::ARBITRARY_PO;
    case NalCompType::Generic:                    return NalComp::GENERIC;
    case NalCompType::FakerootEntropyCompressed:  return NalComp::FAKEROOT_STD;
    case NalCompType::TorsoHead_TwoNeck:
    case NalCompType::TorsoHead_OneNeck:           return NalComp::TORSO_HEAD;
    case NalCompType::LegsFeet_Compressed:        return NalComp::LEGS;
    case NalCompType::LegsFeet_IK:                return NalComp::LEGS_IK;
    case NalCompType::ArmsHands_Compressed:       return NalComp::ARMS;
    case NalCompType::ArmsHands_IK:               return NalComp::ARMS_IK;
    case NalCompType::Tentacles:                  return NalComp::TENTACLE;
    case NalCompType::FiveFinger_Top2KnuckleCurl: return NalComp::FING52;
    case NalCompType::FiveFinger_IndividualCurl:  return NalComp::FING5_CURL;
    case NalCompType::FiveFinger_ReducedAngular:  return NalComp::FING5_REDUCED;
    case NalCompType::FiveFinger_FullRotational:  return NalComp::FING5;
    default: return NalComp::GENERIC;
    }
}

// ─── Skeleton entry in animation file ───
struct NalAnimSkeletonEntry {
    int32_t p[2];
    int32_t hash;
    std::string name;
};

// ─── Decoded component data for one animation ───
struct NalAnimComponent {
    int comp_ix     = -1;
    int slot_ix     = -1;
    int flags       = 0;
    int name_id     = -1;
    uint32_t type_hash = 0;
    uint32_t mask   = 0;
    int ntracks     = 0;
    std::vector<uint8_t> codec_ixs;
    NalDecodedFrames decoded;
    std::string decode_error;
};

// ─── Single animation header + decoded data ───
struct NalAnimEntry {
    // Header fields
    int32_t  offset        = 0;
    int32_t  vtbl          = 0;
    int32_t  next_anim_rel = 0;
    uint32_t name_hash     = 0;
    std::string name;
    int32_t  skel_index    = -1;
    uint32_t version       = 0;
    float    duration      = 0.f;
    uint32_t flags         = 0;
    float    t_scale       = 0.f;

    // CharAnim data
    int32_t  instance_count        = 0;
    int32_t  comp_list_offs        = 0;
    int32_t  anim_user_data_offs   = 0;
    int32_t  track_data_offs       = 0;
    int32_t  internal_offs         = 0;
    int32_t  frame_count           = 0;
    float    current_time          = 0.f;
    int32_t  anim_track_count      = 0;

    bool is_looping()    const { return flags & NAL_FLAG_LOOPING; }
    bool is_scene_anim() const { return flags & NAL_FLAG_SCENE_ANIM; }
    bool is_char_anim()  const { return version == NAL_CHAR_ANIM; }
    bool is_gen_anim()   const { return version == NAL_GEN_ANIM; }

    int playback_frame_count() const {
        if (frame_count > 0) return frame_count;
        int decodedFrames = 0;
        for (const auto& comp : components) {
            decodedFrames = std::max(decodedFrames, (int)comp.decoded.frames.size());
        }
        return std::max(1, decodedFrames);
    }

    float playback_duration() const {
        return (float)playback_frame_count() / NAL_PREVIEW_FPS;
    }

    // Decoded components
    std::vector<NalAnimComponent> components;
    std::vector<std::string> warnings;
};

// ─── Full animation file ───
struct NalAnimFile {
    uint32_t version          = 0;
    uint32_t flags            = 0;
    int32_t  size_string_table = 0;
    int32_t  num_skeletons    = 0;
    uint32_t name_hash        = 0;
    std::string name;
    int32_t  num_anims        = 0;
    uint32_t first_anim       = 0;

    std::vector<NalAnimSkeletonEntry> skeletons;
    std::vector<NalAnimEntry> animations;
    std::vector<std::string> warnings;
};

// ─── Build component slots from skeleton data (matches _build_component_slots) ───
struct NalAnimComponentSlot {
    int slot_ix = 0;
    int name_id = -1;
    uint32_t type_hash = 0;
    int flags = 0;
    int comp_ix = -1;
};

static inline std::vector<NalAnimComponentSlot> nal_build_component_slots(const NalSkeletonData* skel) {
    if (skel && !skel->components.empty()) {
        std::vector<NalAnimComponentSlot> slots;
        for (auto& c : skel->components) {
            NalAnimComponentSlot slot;
            slot.slot_ix = c.component_index;
            slot.name_id = c.component_index;
            slot.type_hash = c.type_id;
            slot.flags = c.component_flags;
            slot.comp_ix = nal_type_to_comp_id(c.type_id);
            slots.push_back(slot);
        }
        return slots;
    }

    const int default_order[] = {
        NalComp::ARBITRARY_PO, NalComp::GENERIC, NalComp::FAKEROOT_STD,
        NalComp::TORSO_HEAD, NalComp::TORSO_HEAD_STD, NalComp::LEGS,
        NalComp::LEGS_IK, NalComp::ARMS, NalComp::ARMS_IK, NalComp::TENTACLE,
        NalComp::FING52, NalComp::FING5_CURL, NalComp::FING5_REDUCED,
        NalComp::FING5
    };
    std::vector<NalAnimComponentSlot> slots;
    for (int comp_ix : default_order) {
        NalAnimComponentSlot slot;
        slot.slot_ix = comp_ix;
        slot.comp_ix = comp_ix;
        slots.push_back(slot);
    }
    return slots;
}

// ─── Decode one animation's components (matches _decode_anim_components) ───
static inline void nal_decode_anim_components(
    const std::vector<uint8_t>& blob,
    NalAnimEntry& anim,
    const std::vector<NalAnimComponentSlot>& comp_slots,
    bool legacy_layout = false)
{
    int comp_list_abs = anim.offset + anim.comp_list_offs - (legacy_layout ? 8 : 0);
    int anim_list_abs = anim.offset + anim.anim_user_data_offs;
    int track_list_abs = anim.offset + anim.track_data_offs;

    if (comp_list_abs < 0 || anim_list_abs < 0 || track_list_abs < 0) {
        anim.warnings.push_back("Invalid component table offsets");
        return;
    }

    int anim_user_data_ix = 0;
    int track_ix = 0;

    for (const auto& slot : comp_slots) {
        int comp_ix = slot.comp_ix;
        int table_ix = legacy_layout ? comp_ix : slot.slot_ix;
        if (table_ix < 0) continue;

        // Read flags from comp list
        if (comp_list_abs + table_ix * 4 < 0 ||
            comp_list_abs + table_ix * 4 + 4 > (int)blob.size()) continue;
        int32_t flags;
        memcpy(&flags, blob.data() + comp_list_abs + table_ix * 4, 4);

        if ((flags & 0x1) == 0) continue; // no track data

        // Read per-anim data
        int per_anim_data_offs = anim_list_abs;
        if (flags & 0x2) { // HAS_PER_ANIM_DATA
            int table_entry = anim_list_abs + (anim_user_data_ix + 1) * 4;
            if (table_entry + 4 <= (int)blob.size()) {
                int32_t elem_offset;
                memcpy(&elem_offset, blob.data() + table_entry, 4);
                per_anim_data_offs += elem_offset;
            }
            ++anim_user_data_ix;
        }

        if (!nal_has_track(flags)) continue;
        if (comp_ix < 0) {
            anim.warnings.push_back("Unknown animation component slot " + std::to_string(slot.slot_ix));
            ++track_ix;
            continue;
        }

        // Read mask
        if (per_anim_data_offs < 0 || per_anim_data_offs + 4 > (int)blob.size()) {
            anim.warnings.push_back("Animation component mask out of range");
            track_ix++;
            continue;
        }
        int32_t mask;
        memcpy(&mask, blob.data() + per_anim_data_offs, 4);

        int codec_ixs_abs = per_anim_data_offs + 4;
        int ntracks = nal_get_num_tracks_for_comp(comp_ix, (uint32_t)mask);
        if (ntracks < 0) { track_ix++; continue; }

        // Read codec indices
        if (codec_ixs_abs < 0 || codec_ixs_abs + ntracks > (int)blob.size()) {
            anim.warnings.push_back("Animation codec table out of range");
            track_ix++;
            continue;
        }
        std::vector<uint8_t> codec_ixs(blob.data() + codec_ixs_abs, blob.data() + codec_ixs_abs + ntracks);

        // Read encoded data
        int encoded_size = nal_get_num_bytes_for_comp(comp_ix, (uint32_t)mask);
        if (encoded_size < 0) { track_ix++; continue; }

        std::vector<uint8_t> encoded_data;
        if (encoded_size > 0) {
            int track_entry = track_list_abs + (track_ix + 1) * 4;
            if (track_entry + 4 <= (int)blob.size()) {
                int32_t track_offset;
                memcpy(&track_offset, blob.data() + track_entry, 4);
                int track_data_abs = track_list_abs + track_offset;
                if (track_data_abs >= 0 && track_data_abs + encoded_size <= (int)blob.size()) {
                    encoded_data.assign(blob.data() + track_data_abs, blob.data() + track_data_abs + encoded_size);
                }
            }
            if (encoded_data.empty()) {
                anim.warnings.push_back("Animation encoded track data out of range");
                track_ix++;
                continue;
            }
        }

        // Decode frames
        NalAnimComponent comp;
        comp.comp_ix = comp_ix;
        comp.slot_ix = slot.slot_ix;
        comp.flags = flags;
        comp.name_id = slot.name_id;
        comp.type_hash = slot.type_hash;
        comp.mask = (uint32_t)mask;
        comp.ntracks = ntracks;
        comp.codec_ixs = codec_ixs;

        try {
            comp.decoded = nal_decode_component_frames(
                comp_ix, codec_ixs, encoded_data, (uint32_t)mask,
                anim.frame_count, anim.current_time, anim.is_scene_anim());
        } catch (...) {
            comp.decode_error = "Decode failed for comp " + std::to_string(comp_ix);
            anim.warnings.push_back(comp.decode_error);
        }

        anim.components.push_back(comp);
        track_ix++;
    }
}

// ─── Main parser: open and decode a .pcanim file ───
inline NalAnimFile ParseNalAnimation(const std::string& filepath, const NalSkeletonData* skel = nullptr, bool decode = true) {
    NalAnimFile result;

    std::ifstream f(filepath, std::ios::binary | std::ios::ate);
    if (!f.is_open()) {
        result.warnings.push_back("Failed to open: " + filepath);
        return result;
    }

    size_t file_size = f.tellg();
    f.seekg(0);
    std::vector<uint8_t> blob(file_size);
    f.read(reinterpret_cast<char*>(blob.data()), file_size);
    f.close();

    if (blob.size() < 64) {
        result.warnings.push_back("File too small");
        return result;
    }

    // Check version
    uint32_t ver_test;
    memcpy(&ver_test, blob.data(), 4);
    if (ver_test != NAL_ANIM_CONTAINER) {
        result.warnings.push_back("Not a PCANIM container (version=0x" + std::to_string(ver_test) + ")");
        return result;
    }

    // Read file header (matches pcanim.py)
    memcpy(&result.version, blob.data(), 4);
    memcpy(&result.flags, blob.data() + 4, 4);
    memcpy(&result.size_string_table, blob.data() + 8, 4);
    memcpy(&result.num_skeletons, blob.data() + 12, 4);

    // Name at offset 16
    if (blob.size() >= 48) {
        memcpy(&result.name_hash, blob.data() + 16, 4);
        char name_buf[28] = {};
        memcpy(name_buf, blob.data() + 20, 28);
        result.name = std::string(name_buf, strnlen(name_buf, 28));
    }

    memcpy(&result.num_anims, blob.data() + 48, 4);
    memcpy(&result.first_anim, blob.data() + 52, 4);

    // Read skeleton entries
    int skel_base = 64;
    for (int i = 0; i < std::max(0, result.num_skeletons); ++i) {
        int off = skel_base + i * 32;
        if (off + 32 > (int)blob.size()) break;

        NalAnimSkeletonEntry se;
        memcpy(&se.p[0], blob.data() + off, 4);
        memcpy(&se.p[1], blob.data() + off + 4, 4);
        memcpy(&se.hash, blob.data() + off + 8, 4);
        char name_buf[20] = {};
        memcpy(name_buf, blob.data() + off + 12, 20);
        se.name = std::string(name_buf, strnlen(name_buf, 20));
        result.skeletons.push_back(se);
    }

    // Read animations
    int cur = (int)result.first_anim;
    int limit = std::max(4096, result.num_anims);
    int count = 0;

    while (cur > 0 && cur < (int)blob.size() - 60 && count < limit) {
        NalAnimEntry anim;
        anim.offset = cur;

        // Read anim header (60 bytes: vtbl(4) + nextAnim(4) + name(32) + skelIx(4) + version(4) + duration(4) + flags(4) + t_scale(4))
        memcpy(&anim.vtbl, blob.data() + cur, 4);
        memcpy(&anim.next_anim_rel, blob.data() + cur + 4, 4);
        memcpy(&anim.name_hash, blob.data() + cur + 8, 4);
        char aname[28] = {};
        memcpy(aname, blob.data() + cur + 12, 28);
        anim.name = std::string(aname, strnlen(aname, 28));
        memcpy(&anim.skel_index, blob.data() + cur + 40, 4);
        memcpy(&anim.version, blob.data() + cur + 44, 4);
        memcpy(&anim.duration, blob.data() + cur + 48, 4);
        memcpy(&anim.flags, blob.data() + cur + 52, 4);
        memcpy(&anim.t_scale, blob.data() + cur + 56, 4);

        // CharAnim extended data
        if (anim.is_char_anim() && cur + 164 <= (int)blob.size()) {
            memcpy(&anim.instance_count, blob.data() + cur + 60, 4);
            memcpy(&anim.comp_list_offs, blob.data() + cur + 64, 4);
            memcpy(&anim.anim_user_data_offs, blob.data() + cur + 68, 4);
            memcpy(&anim.track_data_offs, blob.data() + cur + 72, 4);
            memcpy(&anim.internal_offs, blob.data() + cur + 76, 4);
            memcpy(&anim.frame_count, blob.data() + cur + 80, 4);
            memcpy(&anim.current_time, blob.data() + cur + 84, 4);
            if (cur + 132 <= (int)blob.size()) {
                memcpy(&anim.anim_track_count, blob.data() + cur + 128, 4);
            }
        }
        else if (anim.is_gen_anim() && cur + 72 <= (int)blob.size()) {
            memcpy(&anim.frame_count, blob.data() + cur + 68, 4);
        }

        result.animations.push_back(anim);
        count++;

        if (anim.next_anim_rel == 0) break;
        // next_anim_rel is relative to end of header read position
        // In the C++ standalone: ifs.seekg(((int)ifs.tellg()) + anim.data.header.NextAnim - sizeof nalCharAnimData)
        // Here: the header struct is at cur, and nalCharAnimData is the full header (164 bytes for char anim)
        cur = cur + anim.next_anim_rel;
    }

    // Decode track data
    if (decode) {
        auto comp_slots = nal_build_component_slots(skel);
        for (auto& anim : result.animations) {
            if (!anim.is_char_anim()) continue;
            nal_decode_anim_components(blob, anim, comp_slots);
            if (anim.components.empty() && anim.comp_list_offs >= 8) {
                anim.warnings.push_back("No components decoded with slot table; trying legacy component-id table");
                nal_decode_anim_components(blob, anim, comp_slots, true);
            }
        }
    }

    return result;
}
