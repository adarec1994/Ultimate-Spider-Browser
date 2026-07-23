#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

enum ResourceKeyType : uint32_t {
    RES_KEY_NONE = 0, RES_KEY_ANIMATION = 1, RES_KEY_NAL_SKL = 2, RES_KEY_ALS_FILE = 3,
    RES_KEY_ENTITY = 4, RES_KEY_EXTERNAL_ENT = 5, RES_KEY_TEXTURE = 6, RES_KEY_MPAL_TEXTURE = 7,
    RES_KEY_IFL = 8, RES_KEY_DESCRIPTOR = 9, RES_KEY_SCN_ENTITY = 10, RES_KEY_SCN_AI_SPLINE_PATH = 11,
    RES_KEY_SCN_AUDIO_BOX = 12, RES_KEY_SCN_QUAD_PATH = 13, RES_KEY_SCN_BOX_TRIGGER = 14,
    RES_KEY_SCRIPT = 15, RES_KEY_SCRIPT_INST = 16, RES_KEY_NGL_FONT = 17, RES_KEY_PANEL = 18,
    RES_KEY_TEXTFILE = 19, RES_KEY_ICON = 20, RES_KEY_MESH = 21, RES_KEY_MORPH = 22,
    RES_KEY_MATERIAL = 23, RES_KEY_COLLISION_MESH = 24, RES_KEY_PACK = 25, RES_KEY_SCENE_ANIM = 26,
    RES_KEY_MISSION_TABLE = 27, RES_KEY_SCRIPT_HEADER_FILE = 29, RES_KEY_LOD = 40, RES_KEY_SIN = 41,
    RES_KEY_SCRIPT_GV = 42, RES_KEY_SCRIPT_SV = 43, RES_KEY_TOKEN_LIST = 44,
    RES_KEY_DISTRICT_GRAPH = 45, RES_KEY_PATH = 46, RES_KEY_PATROL_DEF = 47, RES_KEY_LANGUAGE = 48,
    RES_KEY_SLF_LIST = 49, RES_KEY_VISEME_STREAM = 50, RES_KEY_MESH_FILE_STRUCT = 51,
    RES_KEY_MORPH_FILE_STRUCT = 52,
    RES_KEY_MATERIAL_FILE_STRUCT = 53, RES_KEY_FX_CACHE = 54, RES_KEY_AI_STATE_GRAPH = 55,
    RES_KEY_BASE_AI = 56, RES_KEY_CUT_SCENE = 57, RES_KEY_AI_INTERACTION = 58,
    RES_KEY_GAB_DATABASE = 59, RES_KEY_SOUND_ALIAS_DATABASE = 60, RES_KEY_IFC_ATTRIBUTE = 67,
    RES_KEY_Z = 70,
};

inline const char* ResourceKeyTypeName(uint32_t t) {
    switch (t) {
    case RES_KEY_NONE: return "NONE";
    case RES_KEY_ANIMATION: return "ANIMATION";
    case RES_KEY_NAL_SKL: return "NAL_SKL";
    case RES_KEY_ALS_FILE: return "ALS_FILE";
    case RES_KEY_ENTITY: return "ENTITY";
    case RES_KEY_EXTERNAL_ENT: return "EXTERNAL_ENT";
    case RES_KEY_TEXTURE: return "TEXTURE";
    case RES_KEY_MPAL_TEXTURE: return "MPAL_TEXTURE";
    case RES_KEY_IFL: return "IFL";
    case RES_KEY_DESCRIPTOR: return "DESCRIPTOR";
    case RES_KEY_SCN_ENTITY: return "SCN_ENTITY";
    case RES_KEY_SCN_AI_SPLINE_PATH: return "SCN_AI_SPLINE_PATH";
    case RES_KEY_SCN_AUDIO_BOX: return "SCN_AUDIO_BOX";
    case RES_KEY_SCN_QUAD_PATH: return "SCN_QUAD_PATH";
    case RES_KEY_SCN_BOX_TRIGGER: return "SCN_BOX_TRIGGER";
    case RES_KEY_SCRIPT: return "SCRIPT";
    case RES_KEY_SCRIPT_INST: return "SCRIPT_INST";
    case RES_KEY_NGL_FONT: return "NGL_FONT";
    case RES_KEY_PANEL: return "PANEL";
    case RES_KEY_TEXTFILE: return "TEXTFILE";
    case RES_KEY_ICON: return "ICON";
    case RES_KEY_MESH: return "MESH";
    case RES_KEY_MORPH: return "MORPH";
    case RES_KEY_MATERIAL: return "MATERIAL";
    case RES_KEY_COLLISION_MESH: return "COLLISION_MESH";
    case RES_KEY_PACK: return "PACK";
    case RES_KEY_SCENE_ANIM: return "SCENE_ANIM";
    case RES_KEY_MISSION_TABLE: return "MISSION_TABLE";
    case RES_KEY_SCRIPT_HEADER_FILE: return "SCRIPT_HEADER_FILE";
    case RES_KEY_LOD: return "LOD";
    case RES_KEY_SIN: return "SIN";
    case RES_KEY_SCRIPT_GV: return "SCRIPT_GV";
    case RES_KEY_SCRIPT_SV: return "SCRIPT_SV";
    case RES_KEY_TOKEN_LIST: return "TOKEN_LIST";
    case RES_KEY_DISTRICT_GRAPH: return "DISTRICT_GRAPH";
    case RES_KEY_PATH: return "PATH";
    case RES_KEY_PATROL_DEF: return "PATROL_DEF";
    case RES_KEY_LANGUAGE: return "LANGUAGE";
    case RES_KEY_SLF_LIST: return "SLF_LIST";
    case RES_KEY_VISEME_STREAM: return "VISEME_STREAM";
    case RES_KEY_MESH_FILE_STRUCT: return "MESH_FILE_STRUCT";
    case RES_KEY_MORPH_FILE_STRUCT: return "MORPH_FILE_STRUCT";
    case RES_KEY_MATERIAL_FILE_STRUCT: return "MATERIAL_FILE_STRUCT";
    case RES_KEY_FX_CACHE: return "FX_CACHE";
    case RES_KEY_AI_STATE_GRAPH: return "AI_STATE_GRAPH";
    case RES_KEY_BASE_AI: return "BASE_AI";
    case RES_KEY_CUT_SCENE: return "CUT_SCENE";
    case RES_KEY_AI_INTERACTION: return "AI_INTERACTION";
    case RES_KEY_GAB_DATABASE: return "GAB_DATABASE";
    case RES_KEY_SOUND_ALIAS_DATABASE: return "SOUND_ALIAS_DATABASE";
    case RES_KEY_IFC_ATTRIBUTE: return "IFC_ATTRIBUTE";
    default: return "UNKNOWN";
    }
}

enum TlResourceType : uint8_t {
    TLRES_NONE = 0, TLRES_TEXTURE = 1, TLRES_MESH_FILE = 2, TLRES_MESH = 3,
    TLRES_MORPH_FILE = 4, TLRES_MORPH = 5, TLRES_MATERIAL_FILE = 6, TLRES_MATERIAL = 7,
    TLRES_ANIM_FILE = 8, TLRES_ANIM = 9, TLRES_SCENE_ANIM = 10, TLRES_SKELETON = 11,
};

struct PackResourceEntry {
    uint32_t hash = 0;
    uint32_t type = 0;
    uint32_t offset = 0;
    uint32_t size = 0;
};

struct PackTlResource {
    uint32_t nameHash = 0;
    uint8_t  type = 0;
    uint32_t size = 0;
    uint32_t offset = 0;
};

struct PackDirectory {
    bool valid = false;
    std::string error;

    uint32_t versions[5] = {};
    uint32_t flags = 0;
    uint32_t dirOffset = 0;
    uint32_t mashSize = 0;
    uint32_t dataBase = 0;

    std::vector<PackResourceEntry> resources;

    std::vector<PackTlResource> textures, meshFiles, meshes, morphFiles, morphs,
                                materialFiles, materials, animFiles, anims,
                                sceneAnims, skeletons;

    static PackDirectory Parse(const std::vector<uint8_t>& blob);
};

namespace packdir_detail {

struct MashStream {
    const uint8_t* data;
    size_t size;
    size_t pos;
    bool ok = true;

    void rebase(size_t n) {
        size_t pad = (n - (pos % n)) % n;
        pos += pad;
        if (pos > size) ok = false;
    }
    const uint8_t* get(size_t n) {
        if (pos + n > size) { ok = false; return nullptr; }
        const uint8_t* p = data + pos;
        pos += n;
        return p;
    }
    uint32_t getU32() {
        const uint8_t* p = get(4);
        if (!p) return 0;
        uint32_t v; memcpy(&v, p, 4);
        return v;
    }
};

struct VecHeader {
    uint16_t size = 0;
    bool shared = false;
};

inline VecHeader readVecHeader(const uint8_t* img, size_t imgSize, size_t off) {
    VecHeader h;
    if (off + 8 > imgSize) return h;
    memcpy(&h.size, img + off + 4, 2);
    h.shared = img[off + 6] != 0;
    return h;
}

inline const uint8_t* unmashSimpleVector(MashStream& ms, MashStream& ss, const VecHeader& h,
                                         size_t elemSize, bool align8) {
    MashStream& st = h.shared ? ss : ms;
    st.rebase(align8 ? 8 : 4);
    st.rebase(4);
    const uint8_t* data = h.size ? st.get(elemSize * h.size) : nullptr;
    st.rebase(4);
    return data;
}

inline const uint8_t* unmashCountedVector(MashStream& ms, MashStream& ss, const VecHeader& h,
                                          size_t elemSize, bool firstAlign8) {
    if (h.shared) {
        ss.rebase(firstAlign8 ? 8 : 4);
        uint32_t dupMain = ss.getU32();
        uint32_t dupShared = ss.getU32();
        if (firstAlign8) ss.getU32();
        uint32_t v9 = ss.getU32();
        ss.rebase(firstAlign8 ? 8 : 4);
        ss.rebase(4);
        const uint8_t* data = h.size ? ss.get(elemSize * h.size) : nullptr;
        if (v9 != 0) {
            ms.get(dupMain);
            ss.get(dupShared - elemSize * h.size);
        }
        ss.rebase(4);
        return data;
    }
    ms.rebase(firstAlign8 ? 8 : 4);
    ms.rebase(4);
    const uint8_t* data = h.size ? ms.get(elemSize * h.size) : nullptr;
    ms.rebase(4);
    return data;
}

}

inline PackDirectory PackDirectory::Parse(const std::vector<uint8_t>& blob) {
    using namespace packdir_detail;
    constexpr size_t DIR_IMAGE_SIZE = 0x2BC;

    PackDirectory dir;
    if (blob.size() < 0x40) { dir.error = "file too small"; return dir; }

    memcpy(dir.versions, blob.data(), 20);
    memcpy(&dir.flags, blob.data() + 0x14, 4);
    memcpy(&dir.dirOffset, blob.data() + 0x18, 4);
    memcpy(&dir.mashSize, blob.data() + 0x1C, 4);

    if (dir.dirOffset + 0x10 + DIR_IMAGE_SIZE > blob.size() || dir.mashSize > blob.size() ||
        dir.mashSize <= dir.dirOffset) {
        dir.error = "bad header offsets";
        return dir;
    }

    uint32_t safetyKey, hFlags; int32_t sharedOffs; uint16_t classId, fieldE;
    memcpy(&safetyKey, blob.data() + dir.dirOffset, 4);
    memcpy(&hFlags, blob.data() + dir.dirOffset + 4, 4);
    memcpy(&sharedOffs, blob.data() + dir.dirOffset + 8, 4);
    memcpy(&classId, blob.data() + dir.dirOffset + 12, 2);
    memcpy(&fieldE, blob.data() + dir.dirOffset + 14, 2);

    uint32_t expectKey = (((uint32_t)sharedOffs + 0x7BADBA5Du - (hFlags & 0xFFFFFFFu) +
                           classId + fieldE) & 0xFFFFFFFu) | 0x70000000u;
    if (safetyKey != expectKey) { dir.error = "mash safety key mismatch"; return dir; }

    const uint8_t* img = blob.data() + dir.dirOffset + 0x10;
    size_t imgSize = DIR_IMAGE_SIZE;

    MashStream ms{blob.data(), blob.size(), dir.dirOffset + 0x10 + DIR_IMAGE_SIZE};
    MashStream ss{blob.data(), blob.size(), dir.dirOffset + (size_t)sharedOffs};
    ms.rebase(8);

    VecHeader vParents = readVecHeader(img, imgSize, 0x00);
    unmashSimpleVector(ms, ss, vParents, 4,  false);

    VecHeader vRes = readVecHeader(img, imgSize, 0x08);
    const uint8_t* resData = unmashCountedVector(ms, ss, vRes, 16, true);

    struct TlVecDef { size_t imgOff; std::vector<PackTlResource>* out; };
    const TlVecDef tlDefs[] = {
        {0x10, &dir.textures}, {0x18, &dir.meshFiles}, {0x20, &dir.meshes},
        {0x28, &dir.morphFiles}, {0x30, &dir.morphs}, {0x38, &dir.materialFiles},
        {0x40, &dir.materials}, {0x48, &dir.animFiles}, {0x50, &dir.anims},
        {0x58, &dir.sceneAnims}, {0x60, &dir.skeletons},
    };
    const uint8_t* tlData[11] = {};
    VecHeader tlHdr[11];
    for (int i = 0; i < 11; ++i) {
        tlHdr[i] = readVecHeader(img, imgSize, tlDefs[i].imgOff);
        tlData[i] = unmashCountedVector(ms, ss, tlHdr[i], 12, true);
    }

    VecHeader vGroups = readVecHeader(img, imgSize, 0x68);
    if (vGroups.shared) { dir.error = "shared pack_groups not supported"; return dir; }
    ms.rebase(8);
    ms.rebase(4);
    const uint8_t* groupData = vGroups.size ? ms.get(32u * vGroups.size) : nullptr;
    for (uint16_t gi = 0; gi < vGroups.size && ms.ok; ++gi) {
        const uint8_t* g = groupData + gi * 32;
        ms.get(8);
        VecHeader fsVec = readVecHeader(g, 32, 8);
        unmashSimpleVector(ms, ss, fsVec, 16,  true);
        ms.rebase(4);
        int32_t slotCount; memcpy(&slotCount, g + 0x1C, 4);
        if (slotCount > 0) ms.get(4u * (uint32_t)slotCount);
    }
    ms.rebase(4);

    VecHeader vPools = readVecHeader(img, imgSize, 0x70);
    unmashCountedVector(ms, ss, vPools, 12,  false);

    if (!ms.ok || !ss.ok) { dir.error = "mash stream overrun"; return dir; }

    dir.dataBase = dir.mashSize;

    if (resData) {
        dir.resources.reserve(vRes.size);
        for (uint16_t i = 0; i < vRes.size; ++i) {
            PackResourceEntry e;
            memcpy(&e.hash, resData + i * 16 + 0, 4);
            memcpy(&e.type, resData + i * 16 + 4, 4);
            uint32_t relOff, sz;
            memcpy(&relOff, resData + i * 16 + 8, 4);
            memcpy(&sz, resData + i * 16 + 12, 4);
            e.offset = dir.dataBase + relOff;
            e.size = sz;
            dir.resources.push_back(e);
        }
    }

    for (int i = 0; i < 11; ++i) {
        if (!tlData[i]) continue;
        auto& out = *tlDefs[i].out;
        out.reserve(tlHdr[i].size);
        for (uint16_t k = 0; k < tlHdr[i].size; ++k) {
            PackTlResource t;
            uint32_t mtype, dptr;
            memcpy(&t.nameHash, tlData[i] + k * 12 + 0, 4);
            memcpy(&mtype, tlData[i] + k * 12 + 4, 4);
            memcpy(&dptr, tlData[i] + k * 12 + 8, 4);
            t.type = (uint8_t)(mtype & 0xFF);
            t.size = mtype >> 8;
            t.offset = dir.dataBase + dptr;
            out.push_back(t);
        }
    }

    dir.valid = true;
    return dir;
}
