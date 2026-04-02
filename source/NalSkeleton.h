#pragma once
// NalSkeleton.h - C++ port of pcskel.py
// Parses .pcskel files and builds bone hierarchy for Ultimate Spider-Browser
// LemonHaze / Claude - 2025

#include <cstdint>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <map>
#include <array>
#include <fstream>
#include <algorithm>
#include <functional>

// ─── Component type hashes (match pcskel.py NalComponentType) ───
namespace NalCompType {
    constexpr uint32_t ArbitraryPO               = 0xC5E45DCF;
    constexpr uint32_t Generic                    = 0xEC4755BD;
    constexpr uint32_t FakerootEntropyCompressed  = 0xB916E121;
    constexpr uint32_t TorsoHead_TwoNeck          = 0x7E916D6A;
    constexpr uint32_t TorsoHead_OneNeck           = 0x70EA5DF2;
    constexpr uint32_t LegsFeet_Compressed        = 0x47CBEBDB;
    constexpr uint32_t LegsFeet_IK                = 0xA556994F;
    constexpr uint32_t ArmsHands_Compressed       = 0xE01F4F4D;
    constexpr uint32_t ArmsHands_IK               = 0xF0AD5C8E;
    constexpr uint32_t Tentacles                  = 0x464A04D8;
    constexpr uint32_t FiveFinger_Top2KnuckleCurl = 0xE7D9A8D3;
    constexpr uint32_t FiveFinger_IndividualCurl  = 0xE7A8F925;
    constexpr uint32_t FiveFinger_ReducedAngular  = 0xE78254B9;
    constexpr uint32_t FiveFinger_FullRotational  = 0xAFEB6A28;
}

namespace NalCompFlags {
    constexpr int HAS_TRACK_DATA    = 0x1;
    constexpr int HAS_PER_ANIM_DATA = 0x2;
    constexpr int HAS_SKEL_DATA     = 0x40;
}

// ─── Bone role enums (match pcskel.py) ───
namespace TorsoBone   { enum E { PELVIS=0, SPINE, SPINE1, SPINE2, NECK, HEAD, NECK_AUX, COUNT=7 }; }
namespace LegBone     { enum E { L_TOE=0, R_TOE, L_FOOT, R_FOOT, L_THIGH, L_CALF, R_THIGH, R_CALF, PELVIS, COUNT=9 }; }
namespace ArmBone     { enum E { L_CLAV=0, L_UPPER, L_FORE, L_HAND, R_CLAV, R_UPPER, R_FORE, R_HAND,
                                 L_TWIST0, L_TWIST1, R_TWIST0, R_TWIST1, NECK_PARENT, COUNT=13 }; }
namespace ArmIKBone   { enum E { L_CLAV=0, R_CLAV, L_HAND, R_HAND, L_UPPER, R_UPPER, L_FORE, R_FORE,
                                 L_TWIST0, L_TWIST1, R_TWIST0, R_TWIST1, NECK_PARENT, PELVIS, COUNT=14 }; }
namespace FingerBone  { enum E { L0=0,R0,L1,L2,L3,L4,R1,R2,R3,R4, L01,R01,L11,L21,L31,L41,R11,R21,R31,R41,
                                 L02,R02,L12,L22,L32,L42,R12,R22,R32,R42, L_HAND_PARENT,R_HAND_PARENT, COUNT=32 }; }

// ─── IK data (matches pcskel.py read_ik_skel_data) ───
struct NalIKData {
    float fUpperIKc       = 0.f;
    float fUpperIKInvc    = 0.f;
    float fLowerIKc       = 0.f;
    float fLowerIKInvc    = 0.f;
    float fUpperArmLength = 0.f;
    float fLowerArmLength = 0.f;
};

// ─── Quaternion as (w,x,y,z) ───
struct NalQuat {
    float w = 1.f, x = 0.f, y = 0.f, z = 0.f;
};

// ─── Per-component parsed data ───
struct NalComponentData {
    int    component_index = -1;
    int    component_flags = 0;
    uint32_t type_id       = 0;
    std::string type_name;

    // Bone indices for this component (variable size depending on type)
    std::vector<int> bone_indices;

    // Offset locations (vec3 arrays)
    std::vector<std::array<float,3>> offset_locs;

    // IK data (for legs_ik and arms_ik)
    NalIKData ik_data[2];
    bool has_ik = false;

    // Fore twist locs (arms)
    std::vector<std::array<float,3>> fore_twist_locs;

    // For ArbitraryPO: generic nodes
    struct GenericNode {
        std::string name;
        uint16_t quat_ix = 0, pos_ix = 0;
        int16_t  my_matrix_ix = -1, parent_matrix_ix = -1;
        uint16_t is_quat_anim = 0, is_pos_anim = 0;
    };
    std::vector<GenericNode> arb_nodes;

    // Default pose data
    struct DefaultPose {
        std::string kind;
        std::vector<NalQuat> quats;
        std::array<float,3> pelvis_pos = {0,0,0};
        // IK pose extras
        std::array<float,3> foot_pos[2] = {};
        float knee_spin[2] = {0,0};
        // Arms IK
        std::array<float,3> hand_pos[2] = {};
        float elbow_spin[2] = {0,0};
        std::vector<NalQuat> hand_quats;
        // Fakeroot
        float floor_offset = 0;
        uint32_t signal_start = 0, num_signals = 0;
        // Fing52
        std::vector<float> base_y_tracks, base_z_tracks, hinge_tracks, other_tracks;
        uint32_t available_tracks = 0;
        // Finger curl
        std::vector<float> finger_curl;
        // Tentacles
        std::vector<float> tentacle_values;
        // ArbitraryPO pose
        int quat_count = 0, position_count = 0;
        std::vector<std::array<float,3>> positions;
        bool valid = false;
    };
    DefaultPose default_pose;
};

// ─── Bone node for hierarchy ───
struct NalBoneNode {
    int         index       = -1;
    std::string name;
    int         parent_index = -1;
};

// ─── Parsed skeleton result ───
struct NalSkeletonData {
    // Header
    uint32_t    cls = 0, version = 0;
    std::string name, category;
    std::string skeleton_kind = "character"; // "character", "generic", "panel"
    int         num_components = 0;

    // Components with per-skel data
    std::vector<NalComponentData> components;

    // Bone hierarchy (index → name, index → parent_index)
    std::map<int, std::string> bone_map;
    std::map<int, int>         parent_map;

    // Generic skeleton nodes
    struct GenericNode {
        int node_index = -1;
        uint32_t name_hash = 0;
        std::string name;
        int kind = 0, pose_offset = 0, data_offset = 0, parent_index = -1;
    };
    std::vector<GenericNode> generic_nodes;

    std::vector<std::string> warnings;
};

// ─── Helper: read a tlFixedString (4-byte hash + 28-byte name) ───
static inline std::pair<uint32_t, std::string> nal_read_fixed_string(std::ifstream& f) {
    uint32_t hash = 0;
    char buf[28] = {};
    f.read(reinterpret_cast<char*>(&hash), 4);
    f.read(buf, 28);
    std::string name(buf, strnlen(buf, 28));
    return {hash, name};
}

static inline std::array<float,3> nal_read_vec3(std::ifstream& f) {
    std::array<float,3> v;
    f.read(reinterpret_cast<char*>(v.data()), 12);
    return v;
}

static inline NalQuat nal_read_quat_xyzw(const uint8_t* p) {
    float xyzw[4];
    memcpy(xyzw, p, 16);
    return {xyzw[3], xyzw[0], xyzw[1], xyzw[2]}; // store as (w,x,y,z)
}

static inline std::vector<NalQuat> nal_unpack_quat_list(const uint8_t* blob, int count) {
    std::vector<NalQuat> out;
    out.reserve(count);
    for (int i = 0; i < count; ++i) {
        out.push_back(nal_read_quat_xyzw(blob + i * 16));
    }
    return out;
}

// ─── Component name lookup ───
static inline std::string nal_comp_type_name(uint32_t t) {
    switch (t) {
    case NalCompType::ArbitraryPO:               return "ArbitraryPO";
    case NalCompType::Generic:                    return "Generic";
    case NalCompType::FakerootEntropyCompressed:  return "FakerootEntropyCompressed";
    case NalCompType::TorsoHead_TwoNeck:          return "TorsoHead_TwoNeck";
    case NalCompType::TorsoHead_OneNeck:           return "TorsoHead_OneNeck";
    case NalCompType::LegsFeet_Compressed:        return "LegsFeet_Compressed";
    case NalCompType::LegsFeet_IK:                return "LegsFeet_IK";
    case NalCompType::ArmsHands_Compressed:       return "ArmsHands_Compressed";
    case NalCompType::ArmsHands_IK:               return "ArmsHands_IK";
    case NalCompType::Tentacles:                  return "Tentacles";
    case NalCompType::FiveFinger_Top2KnuckleCurl: return "FiveFinger_Top2KnuckleCurl";
    case NalCompType::FiveFinger_IndividualCurl:  return "FiveFinger_IndividualCurl";
    case NalCompType::FiveFinger_ReducedAngular:  return "FiveFinger_ReducedAngular";
    case NalCompType::FiveFinger_FullRotational:  return "FiveFinger_FullRotational";
    default: return "Unknown";
    }
}

// ─── Pose index calculator (matches pcskel.py _get_pose_index_for_comp_index) ───
static inline int nal_get_pose_index(const std::vector<std::pair<int,int>>& meta, int comp_index) {
    if (comp_index < 0 || comp_index >= (int)meta.size()) return -1;
    if (meta[comp_index].second & NalCompFlags::HAS_TRACK_DATA) return -1;
    int pose_ix = -1;
    for (int i = 0; i <= comp_index; ++i) {
        if ((meta[i].second & NalCompFlags::HAS_TRACK_DATA) == 0)
            ++pose_ix;
    }
    return pose_ix;
}

// ─── Main parser ───
inline NalSkeletonData ParseNalSkeleton(const std::string& filepath) {
    NalSkeletonData result;
    std::ifstream f(filepath, std::ios::binary);
    if (!f.is_open()) {
        result.warnings.push_back("Failed to open: " + filepath);
        return result;
    }

    // Header
    f.read(reinterpret_cast<char*>(&result.cls), 4);
    f.read(reinterpret_cast<char*>(&result.version), 4);
    auto [name_hash, name_str] = nal_read_fixed_string(f);
    auto [cat_hash, cat_str]   = nal_read_fixed_string(f);
    result.name     = name_str;
    result.category = cat_str;

    std::string cat_lower = cat_str;
    std::transform(cat_lower.begin(), cat_lower.end(), cat_lower.begin(), ::tolower);

    // Generic skeleton
    if (cat_lower.find("generic") != std::string::npos || result.version == 0x10200) {
        result.skeleton_kind = "generic";
        // Parse generic skeleton (matches pcskel.py _parse_generic_skeleton)
        uint32_t generic_header[14];
        f.read(reinterpret_cast<char*>(generic_header), 56);

        uint32_t field_64 = generic_header[7];
        uint32_t field_6c = generic_header[9];
        uint32_t node_count = generic_header[11];
        uint32_t comp_record_count = generic_header[13];

        const uint32_t data_region_off = 0xE0;
        const uint32_t node_record_size = 48;
        uint32_t table_a_off = data_region_off + field_64;
        uint32_t node_records_off = (table_a_off + field_6c + 3) & ~3u;

        f.seekg(0, std::ios::end);
        size_t file_size = f.tellg();

        result.generic_nodes.reserve(node_count);
        for (uint32_t ni = 0; ni < node_count; ++ni) {
            uint32_t rec_off = node_records_off + ni * node_record_size;
            if (rec_off + node_record_size > file_size) break;
            f.seekg(rec_off);
            auto [nh, nn] = nal_read_fixed_string(f);
            int32_t kind, pose_off, data_off, parent_ix;
            f.read(reinterpret_cast<char*>(&kind), 4);
            f.read(reinterpret_cast<char*>(&pose_off), 4);
            f.read(reinterpret_cast<char*>(&data_off), 4);
            f.read(reinterpret_cast<char*>(&parent_ix), 4);

            NalSkeletonData::GenericNode gn;
            gn.node_index = ni;
            gn.name_hash  = nh;
            gn.name       = nn;
            gn.kind       = kind;
            gn.pose_offset = pose_off;
            gn.data_offset = data_off;
            gn.parent_index = parent_ix;
            result.generic_nodes.push_back(gn);

            // Build bone_map / parent_map
            if (!nn.empty()) {
                result.bone_map[ni] = nn;
                result.parent_map[ni] = (parent_ix >= 0 && parent_ix < (int)node_count) ? parent_ix : -1;
            }
        }
        return result;
    }

    if (cat_lower.find("panel") != std::string::npos) {
        result.skeleton_kind = "panel";
        result.warnings.push_back("Panel skeleton not implemented");
        return result;
    }

    // ─── Character skeleton ───
    result.skeleton_kind = "character";

    int32_t num_skel_data;
    f.read(reinterpret_cast<char*>(&num_skel_data), 4);
    f.seekg(24, std::ios::cur); // gap[24]

    uint32_t num_components;
    int32_t pose_data_align, pose_data_size;
    int32_t components_offs, per_skel_data_int_offs, default_pose_offsets_offs;
    f.read(reinterpret_cast<char*>(&num_components), 4);
    f.read(reinterpret_cast<char*>(&pose_data_align), 4);
    f.read(reinterpret_cast<char*>(&pose_data_size), 4);
    f.read(reinterpret_cast<char*>(&components_offs), 4);
    f.read(reinterpret_cast<char*>(&per_skel_data_int_offs), 4);
    f.read(reinterpret_cast<char*>(&default_pose_offsets_offs), 4);
    result.num_components = num_components;

    // Read component meta: (type, flags) pairs
    struct CompMeta { int32_t index; uint32_t type; int32_t flags; };
    std::vector<CompMeta> comp_meta;
    // Also store as (type, flags) for pose index lookup
    std::vector<std::pair<int,int>> meta_for_pose;

    if (num_components > 0 && components_offs > 0) {
        f.seekg(components_offs);
        for (uint32_t i = 0; i < num_components; ++i) {
            CompMeta cm;
            f.read(reinterpret_cast<char*>(&cm.index), 4);
            f.read(reinterpret_cast<char*>(&cm.type), 4);
            f.read(reinterpret_cast<char*>(&cm.flags), 4);
            comp_meta.push_back(cm);
            meta_for_pose.push_back({(int)cm.type, cm.flags});
        }
    }

    // Initialize component data vector
    result.components.resize(num_components);
    for (uint32_t i = 0; i < num_components; ++i) {
        result.components[i].component_index = i;
        result.components[i].type_id = comp_meta[i].type;
        result.components[i].component_flags = comp_meta[i].flags;
        result.components[i].type_name = nal_comp_type_name(comp_meta[i].type);
    }

    // ─── Per-skel data blocks ───
    if (per_skel_data_int_offs > 0 && num_components > 0) {
        f.seekg(per_skel_data_int_offs);
        int32_t num_blocks;
        f.read(reinterpret_cast<char*>(&num_blocks), 4);

        if (num_blocks > 0 && num_blocks < 100 && f.good()) {
        std::vector<int32_t> block_offsets(num_blocks);
        f.read(reinterpret_cast<char*>(block_offsets.data()), num_blocks * 4);

        // Find which component indices have HAS_SKEL_DATA
        std::vector<int> skel_data_indices;
        for (int i = 0; i < (int)comp_meta.size(); ++i) {
            if (comp_meta[i].flags & NalCompFlags::HAS_SKEL_DATA)
                skel_data_indices.push_back(i);
        }

        int block_base = per_skel_data_int_offs;
        for (int bi = 0; bi < std::min(num_blocks, (int32_t)skel_data_indices.size()); ++bi) {
            int comp_idx = skel_data_indices[bi];
            if (comp_idx < 0 || comp_idx >= (int)result.components.size()) continue;
            auto& cd = result.components[comp_idx];
            uint32_t comp_type = comp_meta[comp_idx].type;
            int comp_block_start = block_base + block_offsets[bi];
            f.seekg(comp_block_start);
            if (!f.good()) break;

            try {
            switch (comp_type) {
            case NalCompType::TorsoHead_TwoNeck:
            case NalCompType::TorsoHead_OneNeck: {
                // Read: emptyNeckOrient(16) + emptyNeckPos(12) + offsetLocs[5](60) + boneIxs[6](24) + otherMatrixIxs[1](4)
                f.seekg(16 + 12, std::ios::cur); // skip orient + pos
                cd.offset_locs.resize(5);
                for (int i = 0; i < 5; ++i) cd.offset_locs[i] = nal_read_vec3(f);
                cd.bone_indices.resize(TorsoBone::COUNT);
                for (int i = 0; i < 6; ++i) { uint32_t v; f.read(reinterpret_cast<char*>(&v), 4); cd.bone_indices[i] = v; }
                { uint32_t v; f.read(reinterpret_cast<char*>(&v), 4); cd.bone_indices.push_back(v); } // NECK_AUX
                break;
            }
            case NalCompType::LegsFeet_IK: {
                // offsetLocs[8](96) + theIKData[2](48) + boneIxs[8](32) + otherMatrixIxs[1](4)
                cd.offset_locs.resize(8);
                for (int i = 0; i < 8; ++i) cd.offset_locs[i] = nal_read_vec3(f);
                for (int i = 0; i < 2; ++i) f.read(reinterpret_cast<char*>(&cd.ik_data[i]), sizeof(NalIKData));
                cd.has_ik = true;
                cd.bone_indices.resize(LegBone::COUNT);
                for (int i = 0; i < 8; ++i) { uint32_t v; f.read(reinterpret_cast<char*>(&v), 4); cd.bone_indices[i] = v; }
                { uint32_t v; f.read(reinterpret_cast<char*>(&v), 4); cd.bone_indices.push_back(v); } // PELVIS
                break;
            }
            case NalCompType::LegsFeet_Compressed: {
                // offsetLocs[8](96) + boneIxs[8](32) + otherMatrixIxs[1](4)
                cd.offset_locs.resize(8);
                for (int i = 0; i < 8; ++i) cd.offset_locs[i] = nal_read_vec3(f);
                cd.bone_indices.resize(LegBone::COUNT);
                for (int i = 0; i < 8; ++i) { uint32_t v; f.read(reinterpret_cast<char*>(&v), 4); cd.bone_indices[i] = v; }
                { uint32_t v; f.read(reinterpret_cast<char*>(&v), 4); cd.bone_indices.push_back(v); }
                break;
            }
            case NalCompType::ArmsHands_Compressed: {
                // offsetLocs[8](96) + foreTwistLocs[4](48) + boneIxs[8](32) + otherMatrixIxs[5](20)
                cd.offset_locs.resize(8);
                for (int i = 0; i < 8; ++i) cd.offset_locs[i] = nal_read_vec3(f);
                cd.fore_twist_locs.resize(4);
                for (int i = 0; i < 4; ++i) cd.fore_twist_locs[i] = nal_read_vec3(f);
                cd.bone_indices.resize(ArmBone::COUNT);
                for (int i = 0; i < 8; ++i) { uint32_t v; f.read(reinterpret_cast<char*>(&v), 4); cd.bone_indices[i] = v; }
                for (int i = 8; i < ArmBone::COUNT; ++i) { uint32_t v; f.read(reinterpret_cast<char*>(&v), 4); cd.bone_indices[i] = v; }
                break;
            }
            case NalCompType::ArmsHands_IK: {
                // offsetLocs[8](96) + foreTwistLocs[4](48) + theIKData[2](48) + boneIxs[8](32) + otherMatrixIxs[6](24)
                cd.offset_locs.resize(8);
                for (int i = 0; i < 8; ++i) cd.offset_locs[i] = nal_read_vec3(f);
                cd.fore_twist_locs.resize(4);
                for (int i = 0; i < 4; ++i) cd.fore_twist_locs[i] = nal_read_vec3(f);
                for (int i = 0; i < 2; ++i) f.read(reinterpret_cast<char*>(&cd.ik_data[i]), sizeof(NalIKData));
                cd.has_ik = true;
                cd.bone_indices.resize(ArmIKBone::COUNT);
                for (int i = 0; i < 8; ++i) { uint32_t v; f.read(reinterpret_cast<char*>(&v), 4); cd.bone_indices[i] = v; }
                for (int i = 8; i < ArmIKBone::COUNT; ++i) { uint32_t v; f.read(reinterpret_cast<char*>(&v), 4); cd.bone_indices[i] = v; }
                break;
            }
            case NalCompType::FiveFinger_Top2KnuckleCurl:
            case NalCompType::FiveFinger_IndividualCurl:
            case NalCompType::FiveFinger_ReducedAngular:
            case NalCompType::FiveFinger_FullRotational: {
                // offsetLocs[30](360) + boneIxs[30](120) + otherMatrixIxs[2](8)
                cd.offset_locs.resize(30);
                for (int i = 0; i < 30; ++i) cd.offset_locs[i] = nal_read_vec3(f);
                cd.bone_indices.resize(FingerBone::COUNT);
                for (int i = 0; i < 30; ++i) { int32_t v; f.read(reinterpret_cast<char*>(&v), 4); cd.bone_indices[i] = v; }
                for (int i = 30; i < FingerBone::COUNT; ++i) { uint32_t v; f.read(reinterpret_cast<char*>(&v), 4); cd.bone_indices[i] = v; }
                break;
            }
            case NalCompType::FakerootEntropyCompressed: {
                // No per-skel data block typically; if present, skip
                break;
            }
            case NalCompType::Tentacles: {
                // Variable; skip for now
                result.warnings.push_back("Tentacles per-skel data not fully parsed");
                break;
            }
            case NalCompType::ArbitraryPO: {
                // ArbitraryPO header: boneCount(4) + unkCount(4) + unkCount2(4) + unkOffset(4)
                //                    + startOffset(4) + startOffset2(4) + blockStart(4) + startOffset4(4)
                uint32_t arb_header[8];
                f.read(reinterpret_cast<char*>(arb_header), 32);
                uint32_t bone_count = arb_header[0];
                uint32_t block_start_off = arb_header[6];
                if (bone_count > 200) break; // sanity check

                f.seekg(comp_block_start + block_start_off);
                cd.arb_nodes.resize(bone_count);
                for (uint32_t ni = 0; ni < bone_count; ++ni) {
                    auto [nh, nn] = nal_read_fixed_string(f);
                    uint16_t qix, pix;
                    int16_t my_ix, par_ix;
                    uint16_t bqa, bpa;
                    int32_t unk2;
                    f.read(reinterpret_cast<char*>(&qix), 2);
                    f.read(reinterpret_cast<char*>(&pix), 2);
                    f.read(reinterpret_cast<char*>(&my_ix), 2);
                    f.read(reinterpret_cast<char*>(&par_ix), 2);
                    f.read(reinterpret_cast<char*>(&bqa), 2);
                    f.read(reinterpret_cast<char*>(&bpa), 2);
                    f.read(reinterpret_cast<char*>(&unk2), 4);

                    cd.arb_nodes[ni].name = nn;
                    cd.arb_nodes[ni].quat_ix = qix;
                    cd.arb_nodes[ni].pos_ix = pix;
                    cd.arb_nodes[ni].my_matrix_ix = my_ix;
                    cd.arb_nodes[ni].parent_matrix_ix = par_ix;
                    cd.arb_nodes[ni].is_quat_anim = bqa;
                    cd.arb_nodes[ni].is_pos_anim = bpa;
                }
                break;
            }
            default:
                result.warnings.push_back("Unhandled per-skel type: " + nal_comp_type_name(comp_type));
                break;
            }
            } catch (...) {
                result.warnings.push_back("Exception parsing per-skel block " + std::to_string(bi));
            }
        }
        } // end num_blocks bounds check
    }

    // ─── Default poses ───
    if (default_pose_offsets_offs > 0 && num_components > 0) {
        f.seekg(default_pose_offsets_offs);
        int32_t num_pose_blocks;
        f.read(reinterpret_cast<char*>(&num_pose_blocks), 4);

        if (num_pose_blocks > 0 && num_pose_blocks < 4096) {
            std::vector<uint32_t> pose_offsets(num_pose_blocks);
            f.read(reinterpret_cast<char*>(pose_offsets.data()), num_pose_blocks * 4);

            int total_pose_bytes = pose_data_size;
            if (total_pose_bytes <= 0) total_pose_bytes = (int)pose_offsets.back();
            if (total_pose_bytes > 0 && total_pose_bytes < 1024 * 1024) {

            f.seekg(default_pose_offsets_offs);
            std::vector<uint8_t> pose_blob(total_pose_bytes);
            f.read(reinterpret_cast<char*>(pose_blob.data()), total_pose_bytes);

            for (uint32_t ci = 0; ci < num_components; ++ci) {
                int pi = nal_get_pose_index(meta_for_pose, ci);
                if (pi < 0 || pi >= num_pose_blocks) continue;

                uint32_t start = pose_offsets[pi];
                uint32_t end = (pi + 1 < num_pose_blocks) ? pose_offsets[pi + 1] : total_pose_bytes;
                if (end <= start || end > (uint32_t)total_pose_bytes) continue;

                const uint8_t* pdata = pose_blob.data() + start;
                size_t psize = end - start;
                uint32_t ct = comp_meta[ci].type;
                auto& dp = result.components[ci].default_pose;

                if ((ct == NalCompType::TorsoHead_TwoNeck || ct == NalCompType::TorsoHead_OneNeck) && psize >= 112) {
                    dp.kind = "torso_pose";
                    dp.quats = nal_unpack_quat_list(pdata, 6);
                    memcpy(dp.pelvis_pos.data(), pdata + 96, 12);
                    dp.valid = true;
                }
                else if (ct == NalCompType::LegsFeet_IK && psize >= 96) {
                    dp.kind = "legs_ik_pose";
                    dp.quats = nal_unpack_quat_list(pdata, 4);
                    memcpy(dp.foot_pos[0].data(), pdata + 64, 12);
                    memcpy(dp.foot_pos[1].data(), pdata + 76, 12);
                    memcpy(dp.knee_spin, pdata + 88, 8);
                    dp.valid = true;
                }
                else if (ct == NalCompType::LegsFeet_Compressed && psize >= 64) {
                    dp.kind = "legs_pose";
                    int nq = (psize >= 128) ? 8 : 4;
                    dp.quats = nal_unpack_quat_list(pdata, nq);
                    dp.valid = true;
                }
                else if (ct == NalCompType::ArmsHands_Compressed && psize >= 128) {
                    dp.kind = "arms_pose";
                    dp.quats = nal_unpack_quat_list(pdata, 8);
                    dp.valid = true;
                }
                else if (ct == NalCompType::ArmsHands_IK && psize >= 96) {
                    dp.kind = "arms_ik_pose";
                    dp.quats = nal_unpack_quat_list(pdata, 2);
                    dp.hand_quats = nal_unpack_quat_list(pdata + 32, 2);
                    memcpy(dp.hand_pos[0].data(), pdata + 64, 12);
                    memcpy(dp.hand_pos[1].data(), pdata + 76, 12);
                    memcpy(dp.elbow_spin, pdata + 88, 8);
                    dp.valid = true;
                }
                else if (ct == NalCompType::FakerootEntropyCompressed && psize >= 48) {
                    dp.kind = "fakeroot_pose";
                    dp.quats = nal_unpack_quat_list(pdata, 1);
                    float pos_floor[4];
                    memcpy(pos_floor, pdata + 16, 16);
                    dp.pelvis_pos = {pos_floor[0], pos_floor[1], pos_floor[2]};
                    dp.floor_offset = pos_floor[3];
                    memcpy(&dp.signal_start, pdata + 32, 4);
                    memcpy(&dp.num_signals, pdata + 36, 4);
                    dp.valid = true;
                }
                else if (ct == NalCompType::FiveFinger_Top2KnuckleCurl && psize >= 192) {
                    dp.kind = "fing52_pose";
                    dp.quats = nal_unpack_quat_list(pdata, 2);
                    float packed[34];
                    memcpy(packed, pdata, 136);
                    dp.base_y_tracks.assign(packed + 8, packed + 16);
                    dp.base_z_tracks.assign(packed + 16, packed + 24);
                    dp.hinge_tracks.assign(packed + 24, packed + 34);
                    float other[10];
                    memcpy(other, pdata + 144, 40);
                    dp.other_tracks.assign(other, other + 10);
                    memcpy(&dp.available_tracks, pdata + 184, 4);
                    dp.valid = true;
                }
                else if (ct == NalCompType::FiveFinger_ReducedAngular && psize >= 176) {
                    dp.kind = "fing5_reduced_pose";
                    dp.quats = nal_unpack_quat_list(pdata, 11);
                    dp.valid = true;
                }
                else if (ct == NalCompType::FiveFinger_IndividualCurl && psize >= 112) {
                    dp.kind = "fing5_curl_pose";
                    dp.quats = nal_unpack_quat_list(pdata, 4);
                    float curls[10];
                    memcpy(curls, pdata + 64, 40);
                    dp.finger_curl.assign(curls, curls + 10);
                    dp.valid = true;
                }
                else if (ct == NalCompType::FiveFinger_FullRotational && psize >= 480) {
                    dp.kind = "fing5_full_pose";
                    dp.quats = nal_unpack_quat_list(pdata, 30);
                    dp.valid = true;
                }
                else if (ct == NalCompType::ArbitraryPO && psize >= 16) {
                    dp.kind = "arbitrary_po_pose";
                    uint32_t qc, tc;
                    memcpy(&qc, pdata, 4);
                    memcpy(&tc, pdata + 4, 4);
                    dp.quat_count = qc;
                    int pc = tc - qc;
                    dp.position_count = pc;
                    size_t expected = 16 + qc * 16 + pc * 12;
                    if (expected <= psize) {
                        dp.quats = (qc > 0) ? nal_unpack_quat_list(pdata + 16, qc) : std::vector<NalQuat>{};
                        size_t pos_off = 16 + qc * 16;
                        dp.positions.resize(pc);
                        for (int i = 0; i < pc; ++i)
                            memcpy(dp.positions[i].data(), pdata + pos_off + i * 12, 12);
                        dp.valid = true;
                    }
                }
                else if (ct == NalCompType::Tentacles && psize >= 60) {
                    dp.kind = "tentacles_pose";
                    dp.tentacle_values.resize(15);
                    memcpy(dp.tentacle_values.data(), pdata, 60);
                    dp.valid = true;
                }
            }
        }
        } // end total_pose_bytes check
    }

    // ─── Build bone hierarchy from component data ───
    // Matches pcskel.py apply_torso_chain, apply_leg_chains, apply_arm_chains, apply_finger_chains

    // Collect bone names from components
    auto add_bone = [&](int idx, const std::string& role_name) {
        if (idx < 0) return;
        if (result.bone_map.find(idx) == result.bone_map.end()) {
            result.bone_map[idx] = role_name;
            result.parent_map[idx] = -1;
        }
    };
    auto set_parent = [&](int child_idx, int parent_idx) {
        if (child_idx < 0 || parent_idx < 0) return;
        result.parent_map[child_idx] = parent_idx;
    };

    // --- Torso chain ---
    int spine2_id = -1;
    for (auto& cd : result.components) {
        if (cd.type_id != NalCompType::TorsoHead_TwoNeck && cd.type_id != NalCompType::TorsoHead_OneNeck) continue;
        if ((int)cd.bone_indices.size() < 6) continue;

        int pelvis = cd.bone_indices[TorsoBone::PELVIS];
        int spine  = cd.bone_indices[TorsoBone::SPINE];
        int sp1    = cd.bone_indices[TorsoBone::SPINE1];
        int sp2    = cd.bone_indices[TorsoBone::SPINE2];
        int neck   = cd.bone_indices[TorsoBone::NECK];
        int head   = cd.bone_indices[TorsoBone::HEAD];
        spine2_id  = sp2;

        add_bone(pelvis, "pelvis"); add_bone(spine, "spine"); add_bone(sp1, "spine1");
        add_bone(sp2, "spine2"); add_bone(neck, "neck"); add_bone(head, "head");
        if ((int)cd.bone_indices.size() > 6) {
            int neck_aux = cd.bone_indices[TorsoBone::NECK_AUX];
            add_bone(neck_aux, "neck_aux");
        }

        set_parent(spine, pelvis); set_parent(sp1, spine); set_parent(sp2, sp1);
        set_parent(neck, sp2); set_parent(head, neck);
    }

    // --- Leg chains ---
    for (auto& cd : result.components) {
        bool is_legs_ik = (cd.type_id == NalCompType::LegsFeet_IK);
        bool is_legs    = (cd.type_id == NalCompType::LegsFeet_Compressed);
        if (!is_legs_ik && !is_legs) continue;
        if ((int)cd.bone_indices.size() < 8) continue;

        int l_toe = cd.bone_indices[LegBone::L_TOE], r_toe = cd.bone_indices[LegBone::R_TOE];
        int l_foot = cd.bone_indices[LegBone::L_FOOT], r_foot = cd.bone_indices[LegBone::R_FOOT];
        int l_thigh = cd.bone_indices[LegBone::L_THIGH], l_calf = cd.bone_indices[LegBone::L_CALF];
        int r_thigh = cd.bone_indices[LegBone::R_THIGH], r_calf = cd.bone_indices[LegBone::R_CALF];
        int pelvis = ((int)cd.bone_indices.size() > 8) ? cd.bone_indices[LegBone::PELVIS] : -1;

        add_bone(l_toe, "l_toe"); add_bone(r_toe, "r_toe");
        add_bone(l_foot, "l_foot"); add_bone(r_foot, "r_foot");
        add_bone(l_thigh, "l_thigh"); add_bone(l_calf, "l_calf");
        add_bone(r_thigh, "r_thigh"); add_bone(r_calf, "r_calf");

        // Chains: pelvis→thigh→calf→foot→toe
        if (pelvis >= 0) { set_parent(l_thigh, pelvis); set_parent(r_thigh, pelvis); }
        set_parent(l_calf, l_thigh); set_parent(l_foot, l_calf); set_parent(l_toe, l_foot);
        set_parent(r_calf, r_thigh); set_parent(r_foot, r_calf); set_parent(r_toe, r_foot);
    }

    // --- Arm chains ---
    bool has_arms_ik = false;
    for (auto& cd : result.components) {
        if (cd.type_id == NalCompType::ArmsHands_IK) { has_arms_ik = true; break; }
    }

    for (auto& cd : result.components) {
        bool is_arms_ik = (cd.type_id == NalCompType::ArmsHands_IK);
        bool is_arms    = (cd.type_id == NalCompType::ArmsHands_Compressed);
        if (!is_arms_ik && !is_arms) continue;
        if (is_arms && has_arms_ik) continue; // IK takes priority

        if (is_arms_ik && (int)cd.bone_indices.size() >= ArmIKBone::COUNT) {
            int l_clav = cd.bone_indices[ArmIKBone::L_CLAV], r_clav = cd.bone_indices[ArmIKBone::R_CLAV];
            int l_hand = cd.bone_indices[ArmIKBone::L_HAND], r_hand = cd.bone_indices[ArmIKBone::R_HAND];
            int l_upper = cd.bone_indices[ArmIKBone::L_UPPER], r_upper = cd.bone_indices[ArmIKBone::R_UPPER];
            int l_fore = cd.bone_indices[ArmIKBone::L_FORE], r_fore = cd.bone_indices[ArmIKBone::R_FORE];
            int neck_par = cd.bone_indices[ArmIKBone::NECK_PARENT];

            add_bone(l_clav, "l_clavicle"); add_bone(r_clav, "r_clavicle");
            add_bone(l_upper, "l_upperarm"); add_bone(r_upper, "r_upperarm");
            add_bone(l_fore, "l_forearm"); add_bone(r_fore, "r_forearm");
            add_bone(l_hand, "l_hand"); add_bone(r_hand, "r_hand");

            // Parent clavicles to neck_parent or spine2
            int clav_par = (neck_par >= 0 && result.bone_map.count(neck_par)) ? neck_par : spine2_id;
            if (clav_par >= 0) { set_parent(l_clav, clav_par); set_parent(r_clav, clav_par); }

            set_parent(l_upper, l_clav); set_parent(l_fore, l_upper); set_parent(l_hand, l_fore);
            set_parent(r_upper, r_clav); set_parent(r_fore, r_upper); set_parent(r_hand, r_fore);

            // Twist bones
            if ((int)cd.bone_indices.size() > ArmIKBone::R_TWIST1) {
                int lt0 = cd.bone_indices[ArmIKBone::L_TWIST0], lt1 = cd.bone_indices[ArmIKBone::L_TWIST1];
                int rt0 = cd.bone_indices[ArmIKBone::R_TWIST0], rt1 = cd.bone_indices[ArmIKBone::R_TWIST1];
                add_bone(lt0, "l_fore_twist0"); add_bone(lt1, "l_fore_twist1");
                add_bone(rt0, "r_fore_twist0"); add_bone(rt1, "r_fore_twist1");
                set_parent(lt0, l_fore); set_parent(lt1, lt0);
                set_parent(rt0, r_fore); set_parent(rt1, rt0);
            }
        }
        else if (is_arms && (int)cd.bone_indices.size() >= ArmBone::COUNT) {
            int l_clav = cd.bone_indices[ArmBone::L_CLAV], r_clav = cd.bone_indices[ArmBone::R_CLAV];
            int l_upper = cd.bone_indices[ArmBone::L_UPPER], r_upper = cd.bone_indices[ArmBone::R_UPPER];
            int l_fore = cd.bone_indices[ArmBone::L_FORE], r_fore = cd.bone_indices[ArmBone::R_FORE];
            int l_hand = cd.bone_indices[ArmBone::L_HAND], r_hand = cd.bone_indices[ArmBone::R_HAND];
            int neck_par = cd.bone_indices[ArmBone::NECK_PARENT];

            add_bone(l_clav, "l_clavicle"); add_bone(r_clav, "r_clavicle");
            add_bone(l_upper, "l_upperarm"); add_bone(r_upper, "r_upperarm");
            add_bone(l_fore, "l_forearm"); add_bone(r_fore, "r_forearm");
            add_bone(l_hand, "l_hand"); add_bone(r_hand, "r_hand");

            int clav_par = (neck_par >= 0 && result.bone_map.count(neck_par)) ? neck_par : spine2_id;
            if (clav_par >= 0) { set_parent(l_clav, clav_par); set_parent(r_clav, clav_par); }

            set_parent(l_upper, l_clav); set_parent(l_fore, l_upper); set_parent(l_hand, l_fore);
            set_parent(r_upper, r_clav); set_parent(r_fore, r_upper); set_parent(r_hand, r_fore);

            int lt0 = cd.bone_indices[ArmBone::L_TWIST0], lt1 = cd.bone_indices[ArmBone::L_TWIST1];
            int rt0 = cd.bone_indices[ArmBone::R_TWIST0], rt1 = cd.bone_indices[ArmBone::R_TWIST1];
            add_bone(lt0, "l_fore_twist0"); add_bone(lt1, "l_fore_twist1");
            add_bone(rt0, "r_fore_twist0"); add_bone(rt1, "r_fore_twist1");
            set_parent(lt0, l_fore); set_parent(lt1, lt0);
            set_parent(rt0, r_fore); set_parent(rt1, rt0);
        }
    }

    // --- Finger chains ---
    for (auto& cd : result.components) {
        bool is_finger = (cd.type_id == NalCompType::FiveFinger_Top2KnuckleCurl ||
                          cd.type_id == NalCompType::FiveFinger_IndividualCurl ||
                          cd.type_id == NalCompType::FiveFinger_ReducedAngular ||
                          cd.type_id == NalCompType::FiveFinger_FullRotational);
        if (!is_finger || (int)cd.bone_indices.size() < FingerBone::COUNT) continue;

        auto bi = [&](int role) -> int { return cd.bone_indices[role]; };

        // Left thumb: 0→01→02, Left fingers: N→N1→N2
        std::vector<std::pair<int,int>> chains = {
            {bi(FingerBone::L0), bi(FingerBone::L01)}, {bi(FingerBone::L01), bi(FingerBone::L02)},
            {bi(FingerBone::L1), bi(FingerBone::L11)}, {bi(FingerBone::L11), bi(FingerBone::L12)},
            {bi(FingerBone::L2), bi(FingerBone::L21)}, {bi(FingerBone::L21), bi(FingerBone::L22)},
            {bi(FingerBone::L3), bi(FingerBone::L31)}, {bi(FingerBone::L31), bi(FingerBone::L32)},
            {bi(FingerBone::L4), bi(FingerBone::L41)}, {bi(FingerBone::L41), bi(FingerBone::L42)},
            // Right
            {bi(FingerBone::R0), bi(FingerBone::R01)}, {bi(FingerBone::R01), bi(FingerBone::R02)},
            {bi(FingerBone::R1), bi(FingerBone::R11)}, {bi(FingerBone::R11), bi(FingerBone::R12)},
            {bi(FingerBone::R2), bi(FingerBone::R21)}, {bi(FingerBone::R21), bi(FingerBone::R22)},
            {bi(FingerBone::R3), bi(FingerBone::R31)}, {bi(FingerBone::R31), bi(FingerBone::R32)},
            {bi(FingerBone::R4), bi(FingerBone::R41)}, {bi(FingerBone::R41), bi(FingerBone::R42)},
        };
        for (auto& [p, c] : chains) {
            if (p >= 0 && c >= 0 && result.bone_map.count(p) && result.bone_map.count(c))
                set_parent(c, p);
        }

        // Parent finger bases to hand parents
        int l_hp = bi(FingerBone::L_HAND_PARENT), r_hp = bi(FingerBone::R_HAND_PARENT);
        for (int b : {bi(FingerBone::L0), bi(FingerBone::L1), bi(FingerBone::L2), bi(FingerBone::L3), bi(FingerBone::L4)})
            if (l_hp >= 0 && b >= 0 && result.bone_map.count(l_hp)) set_parent(b, l_hp);
        for (int b : {bi(FingerBone::R0), bi(FingerBone::R1), bi(FingerBone::R2), bi(FingerBone::R3), bi(FingerBone::R4)})
            if (r_hp >= 0 && b >= 0 && result.bone_map.count(r_hp)) set_parent(b, r_hp);
    }

    return result;
}

// ─── Helper: find skeleton file in PCPACK for an entity ───
inline std::string FindSkeletonInPacks(const std::vector<std::string>& pack_paths,
                                        const std::string& entity_name) {
    // Skeleton files typically match the entity name
    // This is a placeholder - in practice you'd search resource_directory entries
    // For now, search for .pcskel files in the same directory
    for (auto& pp : pack_paths) {
        std::string dir = pp.substr(0, pp.find_last_of("/\\"));
        std::string skel_path = dir + "/" + entity_name + ".pcskel";
        std::ifstream test(skel_path);
        if (test.good()) return skel_path;
    }
    return "";
}
