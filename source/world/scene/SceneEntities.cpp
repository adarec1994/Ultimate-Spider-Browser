static bool SkipSceneEntitySection(const std::vector<uint8_t>& data, size_t& cursor) {
    if (cursor + 8 > data.size()) return false;
    cursor += 4; // field_3C
    uint32_t groupCount = ReadU32LE(data, cursor);
    cursor += 4;
    if (groupCount > 10000) return false;

    for (uint32_t groupIdx = 0; groupIdx < groupCount; groupIdx++) {
        if (cursor + 4 > data.size()) return false;
        uint32_t entityCount = ReadU32LE(data, cursor);
        cursor += 4;
        if (entityCount > 100000) return false;

        for (uint32_t entityIdx = 0; entityIdx < entityCount; entityIdx++) {
            if (cursor + 4 > data.size()) return false;
            uint32_t entityMashSize = ReadU32LE(data, cursor);
            cursor += 4;
            cursor = AlignUp(cursor, 16);
            if (entityMashSize > data.size() || cursor + entityMashSize > data.size()) return false;
            cursor += entityMashSize;
            cursor = AlignUp(cursor, 4);

            if (cursor + 4 > data.size()) return false;
            uint32_t regionStringCount = ReadU32LE(data, cursor);
            cursor += 4;
            if (regionStringCount > 100000) return false;
            cursor = AlignUp(cursor, 8);
            // OpenUSM uses fixedstring<8> here, which is 8 dwords (32 bytes),
            // not an 8-byte string. Using 8 desyncs the later parse codes and
            // makes lego maps disappear or get found at the wrong offset.
            if (cursor + (size_t)regionStringCount * 32 > data.size()) return false;
            cursor += (size_t)regionStringCount * 32;
        }
    }

    return cursor <= data.size();
}

static bool SkipScenePayload(const std::vector<uint8_t>& data, uint32_t parseCode, size_t& cursor) {
    switch (parseCode) {
    case 0:
        return SkipSceneEntitySection(data, cursor);
    case 3: {
        size_t rootOffset = AlignUp(cursor, 0x40);
        if (rootOffset + 0x64 > data.size()) return false;
        uint32_t usedSize = ReadU32LE(data, rootOffset + 0x60);
        if (usedSize == 0 || usedSize > data.size() - cursor) return false;
        cursor += usedSize;
        return cursor <= data.size();
    }
    case 6: {
        if (cursor + 4 > data.size()) return false;
        uint32_t triggerCount = ReadU32LE(data, cursor);
        if (triggerCount > 100000) return false;
        // world_dynamics_system::un_mash_box_triggers uses fixedstring<8>
        // (32 bytes), flags (4), convex_box (0x78), and vector3d (12).
        size_t bytes = 4 + (size_t)triggerCount * (32 + 4 + 0x78 + 12);
        if (cursor + bytes > data.size()) return false;
        cursor += bytes;
        return true;
    }
    case 9: {
        if (cursor + 8 > data.size()) return false;
        uint32_t frameMapCount = ReadU32LE(data, cursor);
        if (frameMapCount == 0 || frameMapCount > 10000) return false;
        size_t pos = cursor + 8 + (size_t)frameMapCount * 4;
        if (pos > data.size()) return false;
        for (uint32_t i = 0; i < frameMapCount; i++) {
            if (pos + 0x2C > data.size()) return false;
            uint32_t textureCount = ReadU32LE(data, pos + 0x28);
            if (textureCount > 10000) return false;
            pos += 0x30 + (size_t)textureCount * 0x24;
            if (pos > data.size()) return false;
        }
        cursor = pos;
        return true;
    }
    case 14:
    case 17: {
        if (cursor + 4 > data.size()) return false;
        uint32_t blobSize = ReadU32LE(data, cursor);
        cursor += 4;
        if (blobSize > data.size() || cursor + blobSize > data.size()) return false;
        cursor += blobSize;
        cursor = AlignUp(cursor, 4);
        return cursor <= data.size();
    }
    case 15: {
        if (cursor + 4 > data.size()) return false;
        uint32_t fadeGroupCount = ReadU32LE(data, cursor);
        if (fadeGroupCount >= 255) return false;
        cursor += 4;
        if (fadeGroupCount > 0) {
            if (cursor + (size_t)fadeGroupCount * 4 > data.size()) return false;
            cursor += (size_t)fadeGroupCount * 4;
            cursor = AlignUp(cursor, 16);
            if (cursor + (size_t)fadeGroupCount * 16 > data.size()) return false;
            cursor += (size_t)fadeGroupCount * 16;
        }
        return cursor <= data.size();
    }
    default:
        return false;
    }
}

struct SceneEntityMashRecord {
    size_t mashStart = 0;
    size_t mashSize = 0;
    size_t sharedMashStart = 0;
    size_t sharedMashSize = 0;
};

struct EntityMeshRef {
    uint32_t pcmHash = 0;
    uint32_t meshOffsetInPcm = 0xFFFFFFFFu;
    std::string meshName;
};

struct SceneMemberPlacement {
    EntityMeshRef meshRef;
    std::array<float, 16> matrix{};
};

static bool IsValidPlacementMatrix(const float* candidate) {
    if (std::fabs(candidate[3]) > 0.01f ||
        std::fabs(candidate[7]) > 0.01f ||
        std::fabs(candidate[11]) > 0.01f) return false;
    if (std::fabs(candidate[15] - 1.0f) > 0.1f) return false;

    for (int r = 0; r < 3; r++) {
        float len2 = 0.0f;
        for (int c = 0; c < 3; c++) {
            len2 += candidate[r * 4 + c] * candidate[r * 4 + c];
        }
        float len = std::sqrt(len2);
        if (len < 0.9f || len > 1.1f) return false;
    }

    if (std::fabs(candidate[12]) > 100000.0f ||
        std::fabs(candidate[13]) > 100000.0f ||
        std::fabs(candidate[14]) > 100000.0f) {
        return false;
    }

    return true;
}

static bool FindPlacementMatrixInRange(const std::vector<uint8_t>& data,
                                       size_t start,
                                       size_t end,
                                       float* matrixOut) {
    end = std::min(end, data.size());
    for (size_t scanOfs = start; scanOfs + 64 <= end; scanOfs += 4) {
        float candidate[16];
        memcpy(candidate, data.data() + scanOfs, 64);
        if (!IsValidPlacementMatrix(candidate)) continue;
        memcpy(matrixOut, candidate, 64);
        return true;
    }
    return false;
}

static std::vector<std::array<float, 16>> FindPlacementMatricesInRange(
    const std::vector<uint8_t>& data,
    size_t start,
    size_t end) {
    std::vector<std::array<float, 16>> matrices;
    end = std::min(end, data.size());

    for (size_t scanOfs = start; scanOfs + 64 <= end; scanOfs += 4) {
        std::array<float, 16> candidate{};
        memcpy(candidate.data(), data.data() + scanOfs, 64);
        if (!IsValidPlacementMatrix(candidate.data())) continue;
        matrices.push_back(candidate);
    }

    return matrices;
}

static bool TryReadTlFixedString(const std::vector<uint8_t>& data,
                                 size_t offset,
                                 uint32_t& hashOut,
                                 std::string& nameOut) {
    if (offset + 32 > data.size()) return false;

    uint32_t hash = ReadU32LE(data, offset);
    const char* chars = (const char*)data.data() + offset + 4;
    size_t len = 0;
    while (len < 28 && chars[len] != '\0') {
        unsigned char c = (unsigned char)chars[len];
        if (c < 0x20 || c > 0x7E) return false;
        len++;
    }
    if (len == 0 || len >= 28) return false;

    std::string name(chars, len);
    if (HashString33(name) != hash) return false;

    hashOut = hash;
    nameOut = StrToLower(name);
    return true;
}

static std::vector<SceneEntityMashRecord> FindSceneEntityMashRecords(const std::vector<uint8_t>& blockData) {
    std::vector<SceneEntityMashRecord> records;
    size_t cursor = 0;
    if (cursor + 4 > blockData.size()) return records;

    uint32_t parseCode = ReadU32LE(blockData, cursor);
    cursor += 4;
    if (parseCode == 13) {
        if (cursor + 4 > blockData.size()) return records;
        uint32_t vobbCount = ReadU32LE(blockData, cursor);
        cursor += 4;
        if (vobbCount > 100000) return records;
        cursor = AlignUp(cursor, 16);
        if (cursor + (size_t)vobbCount * 0x30 > blockData.size()) return records;
        cursor += (size_t)vobbCount * 0x30;
        if (cursor + 4 > blockData.size()) return records;
        parseCode = ReadU32LE(blockData, cursor);
        cursor += 4;
    }

    if (parseCode != 0 || cursor + 8 > blockData.size()) return records;

    cursor += 4; // field_3C
    uint32_t groupCount = ReadU32LE(blockData, cursor);
    cursor += 4;
    if (groupCount > 10000) return {};

    for (uint32_t groupIdx = 0; groupIdx < groupCount; groupIdx++) {
        if (cursor + 4 > blockData.size()) return {};
        uint32_t entityCount = ReadU32LE(blockData, cursor);
        cursor += 4;
        if (entityCount > 100000) return {};

        size_t groupSharedMashStart = 0;
        size_t groupSharedMashSize = 0;
        bool hasGroupSharedMash = false;

        for (uint32_t entityIdx = 0; entityIdx < entityCount; entityIdx++) {
            if (cursor + 4 > blockData.size()) return {};
            uint32_t entityMashSize = ReadU32LE(blockData, cursor);
            cursor += 4;
            cursor = AlignUp(cursor, 16);
            if (entityMashSize == 0 || entityMashSize > blockData.size() ||
                cursor + entityMashSize > blockData.size()) {
                return {};
            }

            if (!hasGroupSharedMash) {
                groupSharedMashStart = cursor;
                groupSharedMashSize = entityMashSize;
                hasGroupSharedMash = true;
            }

            records.push_back({cursor, entityMashSize, groupSharedMashStart, groupSharedMashSize});
            cursor += entityMashSize;
            cursor = AlignUp(cursor, 4);

            if (cursor + 4 > blockData.size()) return {};
            uint32_t regionStringCount = ReadU32LE(blockData, cursor);
            cursor += 4;
            if (regionStringCount > 100000) return {};
            cursor = AlignUp(cursor, 8);
            if (cursor + (size_t)regionStringCount * 32 > blockData.size()) return {};
            cursor += (size_t)regionStringCount * 32;
        }
    }

    return records;
}

static EntityMeshRef FindEntityMeshRef(
    const std::vector<uint8_t>& blockData,
    const SceneEntityMashRecord& rec,
    const std::map<uint32_t, MeshLocationBinding>& meshHashBindings,
    const std::map<uint32_t, PCMModelRef>& pcmIndex,
    const std::map<std::string, uint32_t>& pcmNameToHash) {
    (void)pcmNameToHash;
    EntityMeshRef result;
    auto scanRange = [&](size_t scanStart, size_t scanEnd, EntityMeshRef& out) -> bool {
        scanEnd = std::min(scanEnd, blockData.size());
        if (scanStart >= scanEnd) return false;

        for (size_t off = AlignUp(scanStart, 4); off + 32 <= scanEnd; off += 4) {
            uint32_t hash = 0;
            std::string name;
            if (!TryReadTlFixedString(blockData, off, hash, name)) continue;

            auto bindingIt = meshHashBindings.find(hash);
            if (bindingIt != meshHashBindings.end() && bindingIt->second.pcmHash != 0) {
                out.pcmHash = bindingIt->second.pcmHash;
                out.meshOffsetInPcm = bindingIt->second.meshOffsetInPcm;
                out.meshName = name;
                return true;
            }

            if (pcmIndex.count(hash)) {
                out.pcmHash = hash;
                out.meshName = name;
                return true;
            }
        }
        return false;
    };

    size_t scanEnd = std::min(rec.mashStart + rec.mashSize, blockData.size());
    size_t sharedStart = rec.mashStart;
    bool hasSharedStart = false;
    if (rec.mashSize >= 16) {
        uint32_t sharedOffset = ReadU32LE(blockData, rec.mashStart + 8);
        if (sharedOffset >= 16 && (size_t)sharedOffset < rec.mashSize) {
            sharedStart = rec.mashStart + sharedOffset;
            hasSharedStart = true;
        }
    }

    if (hasSharedStart && scanRange(sharedStart, scanEnd, result)) return result;
    if (scanRange(rec.mashStart, scanEnd, result)) return result;

    // OpenUSM passes the first mash in a scene entity group back into
    // parse_entity_mash as the shared data pointer for the following entities.
    if (rec.sharedMashStart != rec.mashStart && rec.sharedMashSize >= 16) {
        size_t sharedMashEnd = std::min(rec.sharedMashStart + rec.sharedMashSize, blockData.size());
        uint32_t sharedOffset = ReadU32LE(blockData, rec.sharedMashStart + 8);
        if (sharedOffset >= 16 && (size_t)sharedOffset < rec.sharedMashSize) {
            if (scanRange(rec.sharedMashStart + sharedOffset, sharedMashEnd, result)) return result;
        }

        if (scanRange(rec.sharedMashStart, sharedMashEnd, result)) return result;
    }

    return result;
}

