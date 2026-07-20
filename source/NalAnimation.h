#pragma once
// NalAnimation.h - C++ port of pcanim.py
// Parses .pcanim container files and decodes animation track data
// LemonHaze / Claude - 2025

#include "NalAnimCodec.h"
#include "NalSkeleton.h"
#include "NalGenericCodec.h"
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include <algorithm>
#include <map>
#include <memory>

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

// ─── A loaded skeleton the anim decoder can bind to ───
// The engine resolves each anim's skeleton per-anim: nalLoadAnimFileInternal
// looks up skeletons[anim.skel_index] (a tlFixedString) in the skeleton
// directory and stores it on the anim (OpenUSM nal_system.cpp:282). The hash is
// the tlresource string_hash, which matches PackDirectory skeleton_locations.
struct NalSkeletonRef {
    uint32_t hash = 0;
    std::string name;
    std::shared_ptr<NalSkeletonData> data;
};

// ─── Decoded component data for one animation ───
struct NalAnimComponent {
    int comp_ix     = -1;
    int slot_ix     = -1;
    int flags       = 0;
    int name_id     = -1;
    uint32_t type_hash = 0;
    uint32_t mask   = 0;
    // ArbitraryPO uses a variable-length bitset.  `mask` remains the low word
    // for fixed-mask components and diagnostics; never use it alone to address
    // ArbitraryPO channels beyond 31.
    std::vector<uint32_t> mask_words;
    int arbitrary_active_quat_count = 0;
    int ntracks     = 0;
    std::vector<uint8_t> codec_ixs;
    std::vector<uint8_t> encoded_data; // exact component entropy stream
    int per_anim_data_offset = -1;
    int track_data_offset = -1;
    NalDecodedFrames decoded;
    std::string decode_error;

    bool channel_enabled(int channel) const {
        if (channel < 0) return false;
        const size_t word = (size_t)channel >> 5;
        const uint32_t bit = uint32_t{1} << (channel & 31);
        if (!mask_words.empty())
            return word < mask_words.size() && (mask_words[word] & bit) != 0;
        return word == 0 && (mask & bit) != 0;
    }
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
    // Preserved retail header float at +0x30.  It is not playback duration;
    // sub_5F06B0 reads +0x38 (t_scale) for the animation time span.
    float    header_float_30 = 0.f;
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
        if (t_scale > 0.0f) return t_scale;
        return (float)playback_frame_count() / NAL_PREVIEW_FPS;
    }

    // Decoded components
    std::vector<NalAnimComponent> components;
    NalGenericDecodedAnimation generic_decoded;
    std::vector<std::string> warnings;

    // Skeleton this anim was decoded against (resolved via skel_index).
    // Null when no matching skeleton was found and a fallback was used.
    std::shared_ptr<NalSkeletonData> skeleton;
    std::string skeleton_name;   // name from the anim file's skeleton table
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
    std::vector<uint8_t> source_bytes;
    std::vector<std::string> warnings;
};

// ─── Build component slots from skeleton data (matches _build_component_slots) ───
struct NalAnimComponentSlot {
    int slot_ix = 0;
    int name_id = -1;
    uint32_t type_hash = 0;
    int flags = 0;
    int comp_ix = -1;
    int arbitrary_quat_channel_count = 12;
    int arbitrary_position_channel_count = 4;
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
            if (c.type_id == NalCompType::ArbitraryPO && c.default_pose.valid) {
                slot.arbitrary_quat_channel_count = c.default_pose.quat_count;
                slot.arbitrary_position_channel_count = c.default_pose.position_count;
            }
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

        // Read the component mask.  sub_5F2270 addresses this as a packed
        // uint32 bitset and rounds (quatCount + positionCount) up to words.
        // Venom has 41 ArbitraryPO channels, hence two serialized mask words.
        int mask_word_count = 1;
        int arbitrary_channel_count = 0;
        if (comp_ix == NalComp::ARBITRARY_PO) {
            arbitrary_channel_count = std::max(0, slot.arbitrary_quat_channel_count) +
                                      std::max(0, slot.arbitrary_position_channel_count);
            mask_word_count = std::max(1, (arbitrary_channel_count + 31) / 32);
        }
        const int mask_byte_count = mask_word_count * 4;
        if (per_anim_data_offs < 0 ||
            per_anim_data_offs + mask_byte_count > (int)blob.size()) {
            anim.warnings.push_back("Animation component mask out of range");
            track_ix++;
            continue;
        }
        std::vector<uint32_t> mask_words(mask_word_count, 0);
        memcpy(mask_words.data(), blob.data() + per_anim_data_offs, mask_byte_count);
        uint32_t mask = mask_words[0];

        int codec_ixs_abs = per_anim_data_offs + mask_byte_count;
        int arbitrary_active_quat_count = 0;
        int ntracks = 0;
        if (comp_ix == NalComp::ARBITRARY_PO) {
            int active_channel_count = 0;
            for (int channel = 0; channel < arbitrary_channel_count; ++channel) {
                const bool enabled = (mask_words[(size_t)channel >> 5] &
                                      (uint32_t{1} << (channel & 31))) != 0;
                if (!enabled) continue;
                ++active_channel_count;
                if (channel < slot.arbitrary_quat_channel_count)
                    ++arbitrary_active_quat_count;
            }
            ntracks = 3 * active_channel_count;
        } else {
            ntracks = nal_get_num_tracks_for_comp(comp_ix, mask);
        }
        if (ntracks < 0) { track_ix++; continue; }

        // Read codec indices
        if (codec_ixs_abs < 0 || codec_ixs_abs + ntracks > (int)blob.size()) {
            anim.warnings.push_back("Animation codec table out of range");
            track_ix++;
            continue;
        }
        std::vector<uint8_t> codec_ixs(blob.data() + codec_ixs_abs, blob.data() + codec_ixs_abs + ntracks);

        // Read the exact entropy-stream extent from the track-offset table.
        // nal_get_num_bytes_for_comp describes the engine's runtime state
        // allocation; it is NOT a serialized stream size.  Using it as one
        // truncated long clips (then NalBitStream returned zeroes), leaving a
        // non-zero velocity to accumulate until skin bones stretched away.
        std::vector<uint8_t> encoded_data;
        int track_data_abs = -1;
        int track_data_end = -1;
        int32_t serialized_track_count = 0;
        if (track_list_abs + 4 <= (int)blob.size())
            memcpy(&serialized_track_count, blob.data() + track_list_abs, 4);
        if (serialized_track_count <= 0 || serialized_track_count > 256 ||
            track_ix >= serialized_track_count) {
            anim.warnings.push_back("Animation track-offset table is invalid");
            track_ix++;
            continue;
        }

        int track_entry = track_list_abs + (track_ix + 1) * 4;
        if (track_entry + 4 <= (int)blob.size()) {
            int32_t track_offset = 0;
            memcpy(&track_offset, blob.data() + track_entry, 4);
            track_data_abs = track_list_abs + track_offset;
        }
        if (track_ix + 1 < serialized_track_count) {
            int next_entry = track_list_abs + (track_ix + 2) * 4;
            if (next_entry + 4 <= (int)blob.size()) {
                int32_t next_offset = 0;
                memcpy(&next_offset, blob.data() + next_entry, 4);
                track_data_end = track_list_abs + next_offset;
            }
        } else {
            // The final stream runs to the next linked animation record.  The
            // last animation owns the remaining container bytes.
            track_data_end = anim.next_anim_rel > 0
                ? anim.offset + anim.next_anim_rel
                : (int)blob.size();
        }

        if (track_data_abs < 0 || track_data_end <= track_data_abs ||
            track_data_end > (int)blob.size()) {
            anim.warnings.push_back("Animation encoded track data out of range");
            track_ix++;
            continue;
        }
        encoded_data.assign(blob.data() + track_data_abs, blob.data() + track_data_end);

        // Decode frames
        NalAnimComponent comp;
        comp.comp_ix = comp_ix;
        comp.slot_ix = slot.slot_ix;
        comp.flags = flags;
        comp.name_id = slot.name_id;
        comp.type_hash = slot.type_hash;
        comp.mask = mask;
        comp.mask_words = std::move(mask_words);
        comp.arbitrary_active_quat_count = arbitrary_active_quat_count;
        comp.ntracks = ntracks;
        comp.codec_ixs = codec_ixs;
        comp.encoded_data = encoded_data;
        comp.per_anim_data_offset = per_anim_data_offs;
        comp.track_data_offset = track_data_abs;

        try {
            // Source of truth: USM.exe sub_5FF1A0 @ 0x5FF1CD multiplies the
            // global frame step by CharAnim+84 (current_time). Header+56 is
            // t_scale, but it is not the entropy-integrator scale argument.
            comp.decoded = nal_decode_component_frames(
                comp_ix, codec_ixs, encoded_data, mask,
                std::max(1, anim.frame_count), anim.current_time, anim.is_scene_anim(),
                arbitrary_active_quat_count);
        } catch (...) {
            comp.decode_error = "Decode failed for comp " + std::to_string(comp_ix);
            anim.warnings.push_back(comp.decode_error);
        }

        anim.components.push_back(comp);
        track_ix++;
    }
}

// ─── Main parser: open and decode a .pcanim file ───
// `skeletons` lists every skeleton available in the pack; each anim is decoded
// against the skeleton selected by its own skel_index (matched by hash, then by
// case-insensitive name), matching the engine's nalLoadAnimFileInternal. `skel`
// is the fallback when no ref matches (or when `skeletons` is empty).
inline NalAnimFile ParseNalAnimation(const std::string& filepath,
                                     const std::vector<NalSkeletonRef>& skeletons,
                                     const NalSkeletonData* skel = nullptr,
                                     bool decode = true) {
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
    result.source_bytes = blob;

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

        // Read the 60-byte header exactly. +0x30 is an unknown/preserved float;
        // +0x38 is the time span consumed by sub_5F06B0.
        memcpy(&anim.vtbl, blob.data() + cur, 4);
        memcpy(&anim.next_anim_rel, blob.data() + cur + 4, 4);
        memcpy(&anim.name_hash, blob.data() + cur + 8, 4);
        char aname[28] = {};
        memcpy(aname, blob.data() + cur + 12, 28);
        anim.name = std::string(aname, strnlen(aname, 28));
        memcpy(&anim.skel_index, blob.data() + cur + 40, 4);
        memcpy(&anim.version, blob.data() + cur + 44, 4);
        memcpy(&anim.header_float_30, blob.data() + cur + 48, 4);
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

    // Decode track data. Comp-slot tables are built per skeleton and cached;
    // each anim decodes against its own skeleton (skel_index → skeleton table).
    if (decode) {
        std::map<const NalSkeletonData*, std::vector<NalAnimComponentSlot>> slot_cache;
        auto slots_for = [&](const NalSkeletonData* s) -> const std::vector<NalAnimComponentSlot>& {
            auto it = slot_cache.find(s);
            if (it == slot_cache.end())
                it = slot_cache.emplace(s, nal_build_component_slots(s)).first;
            return it->second;
        };
        auto iequals = [](const std::string& a, const std::string& b) {
            if (a.size() != b.size()) return false;
            for (size_t i = 0; i < a.size(); ++i)
                if (::tolower((unsigned char)a[i]) != ::tolower((unsigned char)b[i])) return false;
            return true;
        };

        for (auto& anim : result.animations) {
            const NalSkeletonData* skelData = skel;
            if (anim.skel_index >= 0 && anim.skel_index < (int)result.skeletons.size()) {
                const auto& se = result.skeletons[anim.skel_index];
                anim.skeleton_name = se.name;
                for (const auto& ref : skeletons) {
                    bool hashMatch = se.hash != 0 && ref.hash == (uint32_t)se.hash;
                    bool nameMatch = !se.name.empty() && iequals(ref.name, se.name);
                    if ((hashMatch || nameMatch) && ref.data) {
                        anim.skeleton = ref.data;
                        skelData = ref.data.get();
                        break;
                    }
                }
                if (!anim.skeleton && !se.name.empty()) {
                    anim.warnings.push_back("Skeleton '" + se.name + "' not found in pack; decoded with fallback");
                }
            }

            // Generic animations use an independent active-slot bitset,
            // component header, and chunked entropy stream.  Decode it only
            // after resolving the exact skeleton named by this animation.
            if (anim.is_gen_anim()) {
                if (skelData && skelData->skeleton_kind == "generic") {
                    anim.generic_decoded = nal_decode_generic_animation(
                        blob, static_cast<size_t>(anim.offset),
                        std::max(1, anim.frame_count), *skelData);
                    anim.warnings.insert(anim.warnings.end(),
                        anim.generic_decoded.warnings.begin(),
                        anim.generic_decoded.warnings.end());
                } else {
                    anim.warnings.push_back("Generic animation has no matching generic skeleton");
                }
                continue;
            }
            if (!anim.is_char_anim()) continue;

            const auto& comp_slots = slots_for(skelData);
            nal_decode_anim_components(blob, anim, comp_slots);
            if (anim.components.empty() && anim.comp_list_offs >= 8) {
                anim.warnings.push_back("No components decoded with slot table; trying legacy component-id table");
                nal_decode_anim_components(blob, anim, comp_slots, true);
            }
        }
    }

    return result;
}

// Back-compat wrapper: decode everything against a single skeleton.
inline NalAnimFile ParseNalAnimation(const std::string& filepath, const NalSkeletonData* skel = nullptr, bool decode = true) {
    return ParseNalAnimation(filepath, {}, skel, decode);
}
