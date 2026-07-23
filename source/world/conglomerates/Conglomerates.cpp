static uint16_t GetSceneEntityClassId(const std::vector<uint8_t>& blockData,
                                      const SceneEntityMashRecord& rec) {
    if (rec.sharedMashStart + 14 > blockData.size()) return 0xFFFFu;
    return ReadU16LE(blockData, rec.sharedMashStart + 12);
}

static std::vector<EntityMeshRef> FindEntityMeshRefs(
    const std::vector<uint8_t>& blockData,
    const SceneEntityMashRecord& rec,
    const std::map<uint32_t, MeshLocationBinding>& meshHashBindings,
    const std::map<uint32_t, PCMModelRef>& pcmIndex) {
    std::vector<EntityMeshRef> refs;
    std::set<size_t> seenOffsets;

    auto scanRange = [&](size_t scanStart, size_t scanEnd) {
        scanEnd = std::min(scanEnd, blockData.size());
        if (scanStart >= scanEnd) return;

        for (size_t off = AlignUp(scanStart, 4); off + 32 <= scanEnd; off += 4) {
            if (!seenOffsets.insert(off).second) continue;

            uint32_t hash = 0;
            std::string name;
            if (!TryReadTlFixedString(blockData, off, hash, name)) continue;

            EntityMeshRef ref;
            auto bindingIt = meshHashBindings.find(hash);
            if (bindingIt != meshHashBindings.end() && bindingIt->second.pcmHash != 0) {
                ref.pcmHash = bindingIt->second.pcmHash;
                ref.meshOffsetInPcm = bindingIt->second.meshOffsetInPcm;
                ref.meshName = name;
            } else if (pcmIndex.count(hash)) {
                ref.pcmHash = hash;
                ref.meshName = name;
            }

            if (ref.pcmHash != 0) refs.push_back(ref);
        }
    };

    scanRange(rec.mashStart, rec.mashStart + rec.mashSize);
    if (rec.sharedMashStart != rec.mashStart) {
        scanRange(rec.sharedMashStart, rec.sharedMashStart + rec.sharedMashSize);
    }

    return refs;
}

static bool ReadMashVectorDescriptor(const std::vector<uint8_t>& blockData,
                                     size_t vectorOffset,
                                     uint16_t& count,
                                     bool& shared) {
    if (vectorOffset + 8 > blockData.size()) return false;
    count = ReadU16LE(blockData, vectorOffset + 4);
    shared = blockData[vectorOffset + 6] != 0;
    return count < 1024;
}

static bool IsValidPoMatrix(const float* candidate) {
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

static bool ReadMashPoVector(const std::vector<uint8_t>& blockData,
                             size_t vectorOffset,
                             size_t& normalCursor,
                             size_t& sharedCursor,
                             std::vector<std::array<float, 16>>& matrices) {
    matrices.clear();

    uint16_t count = 0;
    bool shared = false;
    if (!ReadMashVectorDescriptor(blockData, vectorOffset, count, shared)) return false;
    if (count == 0) return true;

    size_t& cursor = shared ? sharedCursor : normalCursor;
    cursor = AlignUp(cursor, 16);
    cursor = AlignUp(cursor, 4);

    const size_t dataStart = cursor;
    const size_t byteCount = (size_t)count * 64;
    if (dataStart + byteCount > blockData.size()) return false;

    matrices.reserve(count);
    for (uint16_t i = 0; i < count; i++) {
        std::array<float, 16> matrix{};
        memcpy(matrix.data(), blockData.data() + dataStart + (size_t)i * 64, 64);
        if (!IsValidPoMatrix(matrix.data())) return false;
        matrices.push_back(matrix);
    }

    cursor = AlignUp(dataStart + byteCount, 4);
    return true;
}

static bool ReadConglomerateMemberAbsPo(const std::vector<uint8_t>& blockData,
                                        const SceneEntityMashRecord& rec,
                                        std::vector<std::array<float, 16>>& matrices) {
    matrices.clear();

    const uint32_t normalObjectSize = 0x130;
    const size_t headerStart = rec.sharedMashStart;
    if (headerStart + 16 > blockData.size()) return false;

    const uint32_t sharedOffset = ReadU32LE(blockData, headerStart + 8);
    const uint16_t headerFlags = ReadU16LE(blockData, headerStart + 14);
    const size_t objectStart = (rec.mashStart == rec.sharedMashStart)
        ? rec.mashStart + 16
        : rec.mashStart;
    if (objectStart + normalObjectSize > blockData.size()) return false;
    if (sharedOffset < 16 || headerStart + sharedOffset > blockData.size()) return false;

    size_t normalCursor = objectStart + normalObjectSize;
    size_t sharedCursor = headerStart + sharedOffset;

    if (sharedCursor + 4 > blockData.size()) return false;
    sharedCursor += 4;

    if ((headerFlags & 0x40) != 0) return false;

    return ReadMashPoVector(blockData, objectStart + 0xD0,
                            normalCursor, sharedCursor, matrices);
}

static bool FindConglomerateRootMatrix(const std::vector<uint8_t>& blockData,
                                       const SceneEntityMashRecord& rec,
                                       std::array<float, 16>& matrixOut) {
    bool found = false;
    const size_t objectStart = (rec.mashStart == rec.sharedMashStart)
        ? rec.mashStart + 16
        : rec.mashStart;
    const size_t scanStart = objectStart + 0x130;
    const size_t scanEnd = rec.mashStart + rec.mashSize;

    for (size_t scanOfs = AlignUp(scanStart, 4); scanOfs + 64 <= scanEnd; scanOfs += 4) {
        std::array<float, 16> candidate{};
        memcpy(candidate.data(), blockData.data() + scanOfs, 64);
        if (!IsValidPlacementMatrix(candidate.data())) continue;
        matrixOut = candidate;
        found = true;
    }

    return found;
}

static bool BuildConglomerateMemberPlacements(
    const std::vector<uint8_t>& blockData,
    const SceneEntityMashRecord& rec,
    const std::map<uint32_t, MeshLocationBinding>& meshHashBindings,
    const std::map<uint32_t, PCMModelRef>& pcmIndex,
    std::vector<SceneMemberPlacement>& placements) {
    placements.clear();

    const uint16_t classId = GetSceneEntityClassId(blockData, rec);
    if (classId != 5) return false;

    std::vector<EntityMeshRef> meshRefs =
        FindEntityMeshRefs(blockData, rec, meshHashBindings, pcmIndex);
    if (meshRefs.empty()) return false;

    std::vector<std::array<float, 16>> matrices;
    const bool matrixReadOk = ReadConglomerateMemberAbsPo(blockData, rec, matrices);
    if (!matrixReadOk) return false;
    if (matrices.empty()) return false;

    std::array<float, 16> rootMatrix{};
    const bool rootMatrixReadOk = FindConglomerateRootMatrix(blockData, rec, rootMatrix);
    if (!rootMatrixReadOk) return false;

    const size_t count = meshRefs.size();
    if (count == 0) return false;

    for (size_t i = 0; i < count; i++) {
        SceneMemberPlacement placement;
        placement.meshRef = meshRefs[i];
        placement.matrix = rootMatrix;
        placements.push_back(placement);
    }

    return !placements.empty();
}
