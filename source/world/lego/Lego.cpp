static bool ShouldSkipLegoPcmName(const std::string& nameLower, const std::string& packStem) {
    (void)nameLower;
    (void)packStem;
    return false;
}

static bool FindLegoMapDataStart(const std::vector<uint8_t>& data, size_t& legoStart) {
    size_t cursor = 0;
    if (cursor + 4 > data.size()) return false;

    uint32_t parseCode = ReadU32LE(data, cursor);
    cursor += 4;
    if (parseCode == 18) return false;

    if (parseCode == 13) {
        if (cursor + 4 > data.size()) return false;
        uint32_t vobbCount = ReadU32LE(data, cursor);
        cursor += 4;
        if (vobbCount > 100000) return false;
        cursor = AlignUp(cursor, 16);
        if (cursor + (size_t)vobbCount * 0x30 > data.size()) return false;
        cursor += (size_t)vobbCount * 0x30;
        if (cursor + 4 > data.size()) return false;
        parseCode = ReadU32LE(data, cursor);
        cursor += 4;
    }

    while (cursor <= data.size()) {
        if (parseCode == 11) {
            legoStart = AlignUp(cursor, 8);
            return legoStart < data.size();
        }
        if (!SkipScenePayload(data, parseCode, cursor)) return false;
        if (cursor + 4 > data.size()) return false;
        parseCode = ReadU32LE(data, cursor);
        cursor += 4;
        if (parseCode == 18) return false;
    }

    return false;
}

struct LegoPlacementRecord {
    uint32_t meshHash = 0;
    uint16_t headingIndex = 0;
    uint16_t meshArrayIndex = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct LegoBatchKey {
    uint32_t pcmHash = 0;
    uint32_t meshOffsetInPcm = 0xFFFFFFFFu;

    bool operator<(const LegoBatchKey& other) const {
        if (pcmHash != other.pcmHash) return pcmHash < other.pcmHash;
        return meshOffsetInPcm < other.meshOffsetInPcm;
    }
};

struct LegoBatch {
    std::string modelName;
    std::string packPath;
    uint32_t sourceOffset = 0;
    std::vector<std::array<float, 16>> transforms;
};

static std::vector<LegoPlacementRecord> FindLegoPlacementRecords(
    const std::vector<uint8_t>& blockData,
    size_t legoStart) {
    std::vector<LegoPlacementRecord> records;
    if (legoStart + 24 > blockData.size()) return records;

    const uint16_t meshCount = ReadU16LE(blockData, legoStart + 16);
    const uint16_t materialCount = ReadU16LE(blockData, legoStart + 18);
    const uint16_t nodeCount = ReadU16LE(blockData, legoStart + 20);
    const uint16_t consumedSize = ReadU16LE(blockData, legoStart + 22);
    if (meshCount == 0 || nodeCount == 0) return records;
    if (meshCount > 4096 || materialCount > 4096 || nodeCount > 20000) return records;

    const size_t meshArrayOffset = (legoStart + 31) & ~(size_t)7;
    const size_t materialArrayOffset = meshArrayOffset + (size_t)meshCount * 4;
    const size_t nodeArrayOffset = AlignUp(materialArrayOffset + (size_t)materialCount * 4, 4);
    const size_t nodeArrayEnd = nodeArrayOffset + (size_t)nodeCount * 0x20;
    if (meshArrayOffset + (size_t)meshCount * 4 > blockData.size()) return {};
    if (nodeArrayEnd > blockData.size()) return {};
    if (consumedSize != 0 && legoStart + consumedSize <= nodeArrayOffset) return {};

    records.reserve(nodeCount);
    for (uint16_t i = 0; i < nodeCount; i++) {
        const size_t nodeOffset = nodeArrayOffset + (size_t)i * 0x20;
        const uint16_t meshArrayIndex = ReadU16LE(blockData, nodeOffset + 24);
        if (meshArrayIndex >= meshCount) continue;

        const uint32_t meshHash = ReadU32LE(blockData, meshArrayOffset + (size_t)meshArrayIndex * 4);
        if (meshHash == 0) continue;

        const float x = ReadF32LE(blockData, nodeOffset + 4);
        const float y = ReadF32LE(blockData, nodeOffset + 8);
        const float z = ReadF32LE(blockData, nodeOffset + 12);
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) continue;
        if (std::fabs(x) > 100000.0f || std::fabs(y) > 100000.0f ||
            std::fabs(z) > 100000.0f) {
            continue;
        }

        LegoPlacementRecord record;
        record.meshHash = meshHash;
        record.headingIndex = ReadU16LE(blockData, nodeOffset + 2);
        record.meshArrayIndex = meshArrayIndex;
        record.x = x;
        record.y = y;
        record.z = z;
        records.push_back(record);
    }

    return records;
}

static void QueueLegoPlacementsFromBlock(
    SpiderManTool& tool,
    const std::vector<uint8_t>& blockData,
    const std::map<uint32_t, MeshLocationBinding>& meshHashBindings,
    const std::map<uint32_t, PCMModelRef>& pcmIndex,
    const float* baseTransform,
    std::map<LegoBatchKey, LegoBatch>& legoBatches,
    int& totalLegoRecords,
    int& loadedLegoRecords,
    int& skippedLegoNoModel) {
    if (meshHashBindings.empty()) return;

    size_t legoStart = 0;
    if (!FindLegoMapDataStart(blockData, legoStart)) return;

    std::vector<LegoPlacementRecord> legoRecords =
        FindLegoPlacementRecords(blockData, legoStart);
    totalLegoRecords += (int)legoRecords.size();

    int loadedFromThisBlock = 0;
    for (const auto& rec : legoRecords) {
        auto bindingIt = meshHashBindings.find(rec.meshHash);
        if (bindingIt == meshHashBindings.end()) {
            skippedLegoNoModel++;
            continue;
        }

        const MeshLocationBinding& binding = bindingIt->second;
        uint32_t pcmHash = binding.pcmHash;
        if (pcmHash == 0 || !pcmIndex.count(pcmHash)) {
            skippedLegoNoModel++;
            continue;
        }

        std::string pcmName = tool.dictionary.count(pcmHash) ? StrToLower(tool.dictionary[pcmHash]) : "";

        float yawRad = (float)rec.headingIndex * (3.14159265f / 180.0f);
        float cy = std::cos(yawRad), sy = std::sin(yawRad);
        float mat[16] = {
            cy,  0, sy, 0,
            0,   1,  0, 0,
            -sy, 0, cy, 0,
            rec.x, rec.y, rec.z, 1
        };
        float combined[16];
        MultiplyMatrix4x4(mat, baseTransform, combined);

        const auto& legoRef = pcmIndex.at(pcmHash);
        std::string legoName = pcmName.empty() ? "lego_prop" : pcmName;
        LegoBatchKey key{pcmHash, binding.meshOffsetInPcm};
        LegoBatch& batch = legoBatches[key];
        if (batch.transforms.empty()) {
            batch.modelName = legoName;
            batch.packPath = legoRef.packPath;
            batch.sourceOffset = legoRef.absOffset;
        }

        std::array<float, 16> transformArray{};
        memcpy(transformArray.data(), combined, sizeof(combined));
        batch.transforms.push_back(transformArray);
        loadedLegoRecords++;
        loadedFromThisBlock++;
    }

    (void)loadedFromThisBlock;
}

static void FlushLegoBatches(
    SpiderManTool& tool,
    const std::map<uint32_t, PCMModelRef>& pcmIndex,
    std::map<uint32_t, std::vector<uint8_t>>& pcmDataCache,
    std::map<LegoBatchKey, LegoBatch>& legoBatches,
    int loadedLegoRecords,
    int& skippedLegoNoModel) {
    int legoBatchMeshCountBefore = (int)tool.previewMeshes.size();
    int legoBatchSourcesLoaded = 0;

    for (auto& [key, batch] : legoBatches) {
        if (batch.transforms.empty()) continue;
        if (!pcmDataCache.count(key.pcmHash)) {
            const auto& legoRef = pcmIndex.at(key.pcmHash);
            std::ifstream pcmFile(legoRef.packPath, std::ios::binary);
            if (!pcmFile.is_open()) {
                skippedLegoNoModel += (int)batch.transforms.size();
                continue;
            }
            pcmDataCache[key.pcmHash].resize(legoRef.size);
            pcmFile.seekg(legoRef.absOffset);
            pcmFile.read((char*)pcmDataCache[key.pcmHash].data(), legoRef.size);
            pcmFile.close();

            if (pcmDataCache[key.pcmHash].size() < 16) {
                pcmDataCache.erase(key.pcmHash);
                skippedLegoNoModel += (int)batch.transforms.size();
                continue;
            }
        }

        tool.AddMeshInstancesFromDataBatched(pcmDataCache[key.pcmHash], batch.modelName, nullptr,
                                             batch.packPath, batch.sourceOffset,
                                             batch.transforms, key.meshOffsetInPcm);
        legoBatchSourcesLoaded++;
    }

    (void)legoBatchMeshCountBefore;
    (void)legoBatchSourcesLoaded;
    (void)loadedLegoRecords;
}
