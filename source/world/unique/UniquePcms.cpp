static bool AdvanceMashVectorData(const std::vector<uint8_t>& data,
                                  size_t objectBase,
                                  size_t vectorOffset,
                                  size_t elementSize,
                                  size_t firstAlign,
                                  size_t& cursor,
                                  size_t* arrayOffset = nullptr,
                                  uint16_t* countOut = nullptr) {
    if (objectBase + vectorOffset + 8 > data.size()) return false;

    uint16_t count = ReadU16LE(data, objectBase + vectorOffset + 4);
    uint8_t isShared = data[objectBase + vectorOffset + 6];
    uint8_t fromMash = data[objectBase + vectorOffset + 7];
    if (isShared || !fromMash) return false;

    cursor = AlignUp(cursor, firstAlign);
    cursor = AlignUp(cursor, 4);
    if (cursor > data.size()) return false;

    size_t bytes = (size_t)count * elementSize;
    if (cursor + bytes > data.size()) return false;

    if (arrayOffset) *arrayOffset = cursor;
    if (countOut) *countOut = count;

    cursor += bytes;
    cursor = AlignUp(cursor, 4);
    return cursor <= data.size();
}

static std::vector<PackMeshFileResource> ReadPackMeshFileResources(const std::string& packPath) {
    std::vector<PackMeshFileResource> resources;

    std::ifstream file(packPath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) return resources;

    size_t fileSize = (size_t)file.tellg();
    if (fileSize < 0x30) return resources;

    std::vector<uint8_t> packHeader(0x30);
    file.seekg(0);
    file.read((char*)packHeader.data(), packHeader.size());
    if (!file.good()) return resources;

    uint32_t directoryOffset = ReadU32LE(packHeader, 0x18);
    uint32_t resourceBaseOffset = ReadU32LE(packHeader, 0x1C);
    if (directoryOffset == 0 || resourceBaseOffset == 0) return resources;
    if ((size_t)resourceBaseOffset > fileSize) return resources;

    std::vector<uint8_t> headerData(resourceBaseOffset);
    file.clear();
    file.seekg(0);
    file.read((char*)headerData.data(), headerData.size());
    if (!file.good()) return resources;

    constexpr size_t kGenericMashHeaderSize = 0x10;
    constexpr size_t kResourceDirectorySize = 0x2BC;
    size_t objectBase = (size_t)directoryOffset + kGenericMashHeaderSize;
    size_t cursor = objectBase + kResourceDirectorySize;
    if (cursor > headerData.size()) return resources;

    // resource_directory::un_mash_start walks these vectors in this order.
    cursor = AlignUp(cursor, 8);
    if (!AdvanceMashVectorData(headerData, objectBase, 0x00, 4, 4, cursor)) return resources;   // parents
    if (!AdvanceMashVectorData(headerData, objectBase, 0x08, 16, 8, cursor)) return resources;  // resource_locations
    if (!AdvanceMashVectorData(headerData, objectBase, 0x10, 12, 8, cursor)) return resources;  // texture_locations

    size_t meshFileArrayOffset = 0;
    uint16_t meshFileCount = 0;
    if (!AdvanceMashVectorData(headerData, objectBase, 0x18, 12, 8, cursor,
                               &meshFileArrayOffset, &meshFileCount)) {
        return resources;
    }

    for (uint16_t i = 0; i < meshFileCount; i++) {
        size_t entryOffset = meshFileArrayOffset + (size_t)i * 12;
        uint32_t hash = ReadU32LE(headerData, entryOffset);
        uint32_t typeAndSize = ReadU32LE(headerData, entryOffset + 4);
        uint32_t relativeOffset = ReadU32LE(headerData, entryOffset + 8);
        uint32_t type = typeAndSize & 0xFF;
        uint32_t size = typeAndSize >> 8;
        uint32_t absOffset = resourceBaseOffset + relativeOffset;

        if (type != 2 || size <= 4) continue;
        if ((size_t)absOffset + size > fileSize) continue;

        uint32_t sig = 0;
        size_t savePos = (size_t)file.tellg();
        file.clear();
        file.seekg(absOffset);
        file.read((char*)&sig, 4);
        file.clear();
        file.seekg(savePos);
        if (sig != 0x204D4350) continue;

        resources.push_back({hash, absOffset, size});
    }

    return resources;
}

static std::vector<PackTlResourceLocation> ReadPackMeshLocations(const std::string& packPath) {
    std::vector<PackTlResourceLocation> meshLocations;

    std::ifstream file(packPath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) return meshLocations;

    size_t fileSize = (size_t)file.tellg();
    if (fileSize < 0x30) return meshLocations;

    std::vector<uint8_t> packHeader(0x30);
    file.seekg(0);
    file.read((char*)packHeader.data(), packHeader.size());
    if (!file.good()) return meshLocations;

    uint32_t directoryOffset = ReadU32LE(packHeader, 0x18);
    uint32_t resourceBaseOffset = ReadU32LE(packHeader, 0x1C);
    if (directoryOffset == 0 || resourceBaseOffset == 0) return meshLocations;
    if ((size_t)resourceBaseOffset > fileSize) return meshLocations;

    std::vector<uint8_t> headerData(resourceBaseOffset);
    file.clear();
    file.seekg(0);
    file.read((char*)headerData.data(), headerData.size());
    if (!file.good()) return meshLocations;

    constexpr size_t kGenericMashHeaderSize = 0x10;
    constexpr size_t kResourceDirectorySize = 0x2BC;
    size_t objectBase = (size_t)directoryOffset + kGenericMashHeaderSize;
    size_t cursor = objectBase + kResourceDirectorySize;
    if (cursor > headerData.size()) return meshLocations;

    // resource_directory::un_mash_start walks these vectors in this order.
    cursor = AlignUp(cursor, 8);
    if (!AdvanceMashVectorData(headerData, objectBase, 0x00, 4, 4, cursor)) return meshLocations;   // parents
    if (!AdvanceMashVectorData(headerData, objectBase, 0x08, 16, 8, cursor)) return meshLocations;  // resource_locations
    if (!AdvanceMashVectorData(headerData, objectBase, 0x10, 12, 8, cursor)) return meshLocations;  // texture_locations
    if (!AdvanceMashVectorData(headerData, objectBase, 0x18, 12, 8, cursor)) return meshLocations;  // mesh_file_locations

    size_t meshArrayOffset = 0;
    uint16_t meshCount = 0;
    if (!AdvanceMashVectorData(headerData, objectBase, 0x20, 12, 8, cursor,
                               &meshArrayOffset, &meshCount)) {
        return meshLocations;
    }

    meshLocations.reserve(meshCount);
    for (uint16_t i = 0; i < meshCount; i++) {
        size_t entryOffset = meshArrayOffset + (size_t)i * 12;
        uint32_t hash = ReadU32LE(headerData, entryOffset);
        uint32_t typeAndSize = ReadU32LE(headerData, entryOffset + 4);
        uint32_t relativeOffset = ReadU32LE(headerData, entryOffset + 8);
        uint32_t type = typeAndSize & 0xFF;
        uint32_t size = typeAndSize >> 8;
        uint32_t absOffset = resourceBaseOffset + relativeOffset;
        if (type == 3) meshLocations.push_back({hash, type, absOffset, size});
    }

    return meshLocations;
}

static std::vector<uint32_t> ExtractPcmMeshHashes(const std::vector<uint8_t>& pcmData) {
    std::vector<uint32_t> meshHashes;
    if (pcmData.size() < 16 || ReadU32LE(pcmData, 0) != 0x204D4350) return meshHashes;

    uint32_t numEntries = ReadU32LE(pcmData, 8);
    uint32_t entriesOffset = ReadU32LE(pcmData, 12);
    if (numEntries > 1000 || entriesOffset >= pcmData.size()) return meshHashes;

    for (uint32_t i = 0; i < numEntries; i++) {
        size_t entryOffset = (size_t)entriesOffset + (size_t)i * 12;
        if (entryOffset + 12 > pcmData.size()) break;
        uint16_t entryType = ReadU16LE(pcmData, entryOffset + 2);
        if (entryType != 512) continue;

        uint32_t meshOffset = ReadU32LE(pcmData, entryOffset + 4);
        if ((size_t)meshOffset + 4 > pcmData.size()) continue;

        uint32_t meshNameOffset = ReadU32LE(pcmData, meshOffset);
        if (meshNameOffset != 0 && (size_t)meshNameOffset + 4 <= pcmData.size()) {
            meshHashes.push_back(ReadU32LE(pcmData, meshNameOffset));
        }
    }

    return meshHashes;
}

static std::vector<MeshLocationBinding> BuildMeshLocationBindings(const std::string& packPath) {
    std::vector<PackTlResourceLocation> meshLocations = ReadPackMeshLocations(packPath);
    std::vector<MeshLocationBinding> bindings(meshLocations.size());
    for (size_t i = 0; i < meshLocations.size(); i++) {
        bindings[i].meshHash = meshLocations[i].hash;
    }
    if (meshLocations.empty()) return bindings;

    std::vector<PackMeshFileResource> meshFiles = ReadPackMeshFileResources(packPath);
    if (!meshFiles.empty()) {
        std::sort(meshFiles.begin(), meshFiles.end(),
                  [](const PackMeshFileResource& a, const PackMeshFileResource& b) {
                      return a.absOffset < b.absOffset;
                  });

        // OpenUSM's tlresource_location for TLRESOURCE_TYPE_MESH points at the
        // mesh struct inside the PCM. Resolve only by the containing PCM range.
        for (size_t i = 0; i < meshLocations.size(); i++) {
            uint32_t meshOffset = meshLocations[i].absOffset;
            for (const auto& meshFile : meshFiles) {
                uint64_t start = meshFile.absOffset;
                uint64_t end = start + meshFile.size;
                if ((uint64_t)meshOffset >= start && (uint64_t)meshOffset < end) {
                    bindings[i].pcmHash = meshFile.hash;
                    bindings[i].meshOffsetInPcm = meshOffset - meshFile.absOffset;
                    break;
                }
                if ((uint64_t)meshOffset < start) break;
            }
        }
    }

    return bindings;
}

static void AddMeshHashBindingsFromPack(const std::string& packPath,
                                        std::map<uint32_t, MeshLocationBinding>& out,
                                        bool overwriteExisting) {
    for (const auto& binding : BuildMeshLocationBindings(packPath)) {
        if (binding.meshHash == 0 || binding.pcmHash == 0) continue;
        if (overwriteExisting || !out.count(binding.meshHash)) {
            out[binding.meshHash] = binding;
        }
    }
}

static int DecodePlacementPcmIndex(uint16_t rawIndex, int meshCount) {
    int idx = (int)rawIndex;
    if (idx >= 0 && idx < meshCount) return idx;

    if (idx >= meshCount && idx < 2 * meshCount) {
        int mirrored = idx - meshCount + 1;
        if (mirrored >= 0 && mirrored < meshCount) return mirrored;
    }

    int lowByte = (int)(rawIndex & 0xFF);
    if ((rawIndex >> 8) != 0 && lowByte >= 0 && lowByte < meshCount) return lowByte;

    return -1;
}

static void BuildWorldPcmIndex(SpiderManTool& tool,
                               std::map<uint32_t, PCMModelRef>& pcmIndex,
                               std::map<std::string, uint32_t>& pcmNameToHash) {
    for (const auto& path : tool.foundPacks) {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) continue;

        size_t fileSize = file.tellg();
        if (fileSize < 32) { file.close(); continue; }

        file.seekg(24);
        uint32_t headerSize, packDataOffset;
        file.read((char*)&headerSize, 4);
        file.read((char*)&packDataOffset, 4);
        if (!file.good()) { file.close(); continue; }

        for (const auto& meshRes : ReadPackMeshFileResources(path.string())) {
            PCMModelRef ref;
            ref.packPath = path.string();
            ref.absOffset = meshRes.absOffset;
            ref.size = meshRes.size;
            pcmIndex[meshRes.hash] = ref;

            if (tool.dictionary.count(meshRes.hash)) {
                std::string nameLower = StrToLower(tool.dictionary[meshRes.hash]);
                pcmNameToHash[nameLower] = meshRes.hash;
            }
        }

        size_t headerReadSize = std::min((size_t)500000, fileSize);
        std::vector<uint8_t> tempHeader(headerReadSize);
        file.seekg(0);
        file.read((char*)tempHeader.data(), headerReadSize);

        size_t tocStart = FindTocStart(tempHeader, headerReadSize);
        if (tocStart == 0) { file.close(); continue; }

        file.clear();
        file.seekg(tocStart);

        while (file.good()) {
            uint32_t hash, type, offset, size;
            file.read((char*)&hash, 4);
            file.read((char*)&type, 4);
            file.read((char*)&offset, 4);
            file.read((char*)&size, 4);
            if (!file.good()) break;
            if (type >= 0x1000 || type == 0x0000) break;

            if ((type == 0x15 || type == 0x17) && size > 4) {
                uint32_t absOffset = packDataOffset + offset;
                if (absOffset + 4 <= fileSize) {
                    size_t savePos = file.tellg();
                    file.seekg(absOffset);
                    uint32_t sig = 0;
                    file.read((char*)&sig, 4);
                    file.clear();
                    file.seekg(savePos);

                    if (sig == 0x204D4350) {
                        if (type == 0x15 || !pcmIndex.count(hash)) {
                            PCMModelRef ref;
                            ref.packPath = path.string();
                            ref.absOffset = absOffset;
                            ref.size = size;
                            pcmIndex[hash] = ref;
                        }

                        if (tool.dictionary.count(hash)) {
                            std::string nameLower = StrToLower(tool.dictionary[hash]);
                            if (type == 0x15 || !pcmNameToHash.count(nameLower)) {
                                pcmNameToHash[nameLower] = hash;
                            }
                        }
                    }
                }
            }
        }
        file.close();
    }

    (void)tool;
}

static int LoadWorldOceanMesh(SpiderManTool& tool, const float* baseTransform) {
    int loaded = 0;
    const uint32_t oceanMeshHash = HashString33("oceanmesh");

    for (const auto& path : tool.foundPacks) {
        std::string stem = StrToLower(path.stem().string());
        if (stem != "city_arena") continue;

        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) continue;

        size_t fileSize = (size_t)file.tellg();
        if (fileSize < 32) { file.close(); break; }

        file.seekg(24);
        uint32_t headerSize = 0;
        uint32_t packDataOffset = 0;
        file.read((char*)&headerSize, 4);
        file.read((char*)&packDataOffset, 4);
        if (!file.good()) { file.close(); break; }

        size_t headerReadSize = std::min((size_t)500000, fileSize);
        std::vector<uint8_t> tempHeader(headerReadSize);
        file.seekg(0);
        file.read((char*)tempHeader.data(), headerReadSize);
        size_t tocStart = FindTocStart(tempHeader, headerReadSize);
        if (tocStart == 0) { file.close(); break; }

        file.clear();
        file.seekg(tocStart);
        while (file.good()) {
            uint32_t hash = 0;
            uint32_t type = 0;
            uint32_t offset = 0;
            uint32_t size = 0;
            file.read((char*)&hash, 4);
            file.read((char*)&type, 4);
            file.read((char*)&offset, 4);
            file.read((char*)&size, 4);
            if (!file.good()) break;
            if (type >= 0x1000 || type == 0) break;
            if (size <= 4) continue;

            if (hash != oceanMeshHash || type != 0x15) continue;

            uint32_t absOffset = packDataOffset + offset;
            if ((size_t)absOffset + size > fileSize) continue;

            std::vector<uint8_t> pcmData(size);
            file.clear();
            file.seekg(absOffset);
            file.read((char*)pcmData.data(), size);
            if (!file.good()) continue;
            if (pcmData.size() < 4 || ReadU32LE(pcmData, 0) != 0x204D4350) continue;

            RecordWorldMeshPlacementDebug("unique pcms", "oceanmesh",
                                          path.string(), absOffset,
                                          baseTransform);
            tool.AddMeshFromDataWithTransform(pcmData, "oceanmesh", nullptr,
                                              path.string(), absOffset,
                                              baseTransform);
            loaded++;
            break;
        }

        file.close();
        break;
    }

    return loaded;
}
