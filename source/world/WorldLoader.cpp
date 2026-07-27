

void SpiderManTool::LoadAllWorldGeometries() {
    for (auto& mesh : previewMeshes) {
        if (mesh.vao) glDeleteVertexArrays(1, &mesh.vao);
        if (mesh.vbo) glDeleteBuffers(1, &mesh.vbo);
        if (mesh.ebo) glDeleteBuffers(1, &mesh.ebo);
        if (mesh.instanceVbo) glDeleteBuffers(1, &mesh.instanceVbo);
    }
    previewMeshes.clear();
    pendingWholeWorldInstances.clear();
    collectWholeWorldInstances = true;
    isWorldMode = true;
    isModelPreview = true;

    selectedMeshIndex = -1;
    selectedMeshInstanceIndex = -1;
    selectedMeshPcmData.clear();
    showWorldMeshDetails = false;

    camPos[0] = 0.0f; camPos[1] = 200.0f; camPos[2] = -600.0f;
    camFront[0] = 0.0f; camFront[1] = -0.3f; camFront[2] = -1.0f;
    camUp[0] = 0.0f; camUp[1] = 1.0f; camUp[2] = 0.0f;
    camYaw = -90.0f;
    camPitch = -15.0f;
    camSpeed = 500.0f;

    InitModelPreview();

    float baseTransform[16] = {0};
    baseTransform[0]  = -1.0f;
    baseTransform[5]  =  1.0f;
    baseTransform[10] =  1.0f;
    baseTransform[15] =  1.0f;

    std::map<uint32_t, PCMModelRef> pcmIndex;
    std::map<std::string, uint32_t> pcmNameToHash;
    BuildWorldPcmIndex(*this, pcmIndex, pcmNameToHash);

    currentWorldKind = WorldMeshKind::Zone;
    int zoneGeoCount = LoadWorldZoneChunks(*this, baseTransform);
    currentWorldKind = WorldMeshKind::Interior;
    int interiorMeshCount = LoadWorldInteriorMeshes(*this, baseTransform);

    std::map<uint32_t, std::vector<uint8_t>> pcmDataCache;

    std::set<uint32_t> zoneBaseHashes = BuildWorldZoneBaseHashes(foundPacks);

    std::map<uint32_t, MeshLocationBinding> globalMeshHashBindings;
    for (const auto& path : foundPacks) {
        AddMeshHashBindingsFromPack(path.string(), globalMeshHashBindings, false);
    }
    int totalInstances = 0;
    int loadedInstances = 0;
    int skippedNoModel = 0;
    int skippedNonRenderable = 0;
    int skippedNoTransform = 0;
    int totalLegoRecords = 0;
    int loadedLegoRecords = 0;
    int skippedLegoNoModel = 0;
    int skippedLegoFiltered = 0;

    std::map<LegoBatchKey, LegoBatch> legoBatches;

    struct InstanceBlockInfo {
        uint32_t offset;
        uint32_t size;
        uint32_t tocHash;
        uint8_t  tocType;
    };

    for (const auto& path : foundPacks) {
        std::string stem = StrToLower(path.stem().string());

        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) continue;

        size_t fileSize = file.tellg();
        if (fileSize < 32) { file.close(); continue; }

        file.seekg(24);
        uint32_t headerSize, packDataOffset;
        file.read((char*)&headerSize, 4);
        file.read((char*)&packDataOffset, 4);
        if (!file.good()) { file.close(); continue; }

        size_t headerReadSize = std::min((size_t)500000, fileSize);
        std::vector<uint8_t> tempHeader(headerReadSize);
        file.seekg(0);
        file.read((char*)tempHeader.data(), headerReadSize);

        size_t tocStart = FindTocStart(tempHeader, headerReadSize);
        if (tocStart == 0) { file.close(); continue; }

        file.clear();
        file.seekg(tocStart);

        std::vector<InstanceBlockInfo> instanceBlocks;

        while (file.good()) {
            uint32_t hash, type, offset, size;
            file.read((char*)&hash, 4);
            file.read((char*)&type, 4);
            file.read((char*)&offset, 4);
            file.read((char*)&size, 4);
            if (!file.good()) break;
            if (type >= 0x1000 || type == 0x0000) break;

            if (size > 64) {
                uint32_t absOfs = packDataOffset + offset;
                if (absOfs + size <= fileSize) {
                    if (type == 0x0A) {
                        instanceBlocks.push_back({absOfs, size, hash, 0x0A});
                    } else if (type == 0x04 && pcmIndex.count(hash)) {

                        instanceBlocks.push_back({absOfs, size, hash, 0x04});
                    }
                }
            }
        }

        std::vector<MeshLocationBinding> meshLocationBindings = BuildMeshLocationBindings(path.string());
        std::map<uint32_t, MeshLocationBinding> meshHashBindings = globalMeshHashBindings;
        for (const auto& binding : meshLocationBindings) {
            if (binding.meshHash != 0 && binding.pcmHash != 0) {
                meshHashBindings[binding.meshHash] = binding;
            }
        }
        for (const auto& blockInfo : instanceBlocks) {
            std::vector<uint8_t> blockData(blockInfo.size);
            file.clear();
            file.seekg(blockInfo.offset);
            file.read((char*)blockData.data(), blockInfo.size);
            if (!file.good()) continue;

            const uint32_t blockSize = blockInfo.size;

            bool usedSceneEntityRecords = false;
            if (blockInfo.tocType == 0x0A) {
                std::vector<SceneEntityMashRecord> sceneRecords = FindSceneEntityMashRecords(blockData);
                if (!sceneRecords.empty()) {
                    usedSceneEntityRecords = true;
                    int placedSceneEntities = 0;

                    for (const auto& rec : sceneRecords) {
                        totalInstances++;
                        if (rec.mashStart + 0x24 > blockData.size()) continue;

                        uint32_t instanceHash = ReadU32LE(blockData, rec.mashStart + 0x20);

                        std::string instanceName = dictionary.count(instanceHash)
                            ? StrToLower(dictionary[instanceHash])
                            : "";

                        std::vector<SceneMemberPlacement> memberPlacements;
                        const bool builtConglomerate = BuildConglomerateMemberPlacements(
                                blockData, rec, meshHashBindings, pcmIndex, memberPlacements);

                        if (builtConglomerate) {
                            currentWorldKind = WorldMeshKind::Conglomerate;
                            int placedMembers = 0;
                            for (const auto& member : memberPlacements) {
                                uint32_t memberPcmHash = member.meshRef.pcmHash;
                                if (memberPcmHash == 0 || !pcmIndex.count(memberPcmHash)) {
                                    skippedNoModel++;
                                    continue;
                                }

                                if (!pcmDataCache.count(memberPcmHash)) {
                                    const auto& memberRef = pcmIndex[memberPcmHash];
                                    std::ifstream pcmFile(memberRef.packPath, std::ios::binary);
                                    if (!pcmFile.is_open()) {
                                        skippedNoModel++;
                                        continue;
                                    }
                                    pcmDataCache[memberPcmHash].resize(memberRef.size);
                                    pcmFile.seekg(memberRef.absOffset);
                                    pcmFile.read((char*)pcmDataCache[memberPcmHash].data(), memberRef.size);
                                    pcmFile.close();

                                    if (pcmDataCache[memberPcmHash].size() < 16) {
                                        pcmDataCache.erase(memberPcmHash);
                                        skippedNoModel++;
                                        continue;
                                    }
                                }

                                float combinedTransform[16];
                                MultiplyMatrix4x4(member.matrix.data(), baseTransform, combinedTransform);

                                const auto& memberRef = pcmIndex[memberPcmHash];
                                std::string memberName = !member.meshRef.meshName.empty()
                                    ? member.meshRef.meshName
                                    : (dictionary.count(memberPcmHash)
                                        ? StrToLower(dictionary[memberPcmHash])
                                        : "scene_member");

                                AddMeshFromDataWithTransform(pcmDataCache[memberPcmHash], memberName, nullptr,
                                                             memberRef.packPath, memberRef.absOffset,
                                                             combinedTransform,
                                                             member.meshRef.meshOffsetInPcm);
                                loadedInstances++;
                                placedMembers++;
                            }

                            if (placedMembers > 0) {
                                placedSceneEntities += placedMembers;
                                continue;
                            }
                        }

                        EntityMeshRef meshRef = FindEntityMeshRef(
                            blockData, rec, meshHashBindings, pcmIndex, pcmNameToHash);
                        uint32_t pcmHash = meshRef.pcmHash;
                        if (pcmHash == 0 || !pcmIndex.count(pcmHash)) {
                            skippedNoModel++;
                            continue;
                        }

                        std::string pcmName = dictionary.count(pcmHash) ? StrToLower(dictionary[pcmHash]) : "";

                        float instanceMatrix[16];
                        size_t transformStart = rec.mashStart + 0x44;
                        size_t transformEnd = rec.mashStart + rec.mashSize;
                        if (!FindPlacementMatrixInRange(blockData, transformStart, transformEnd, instanceMatrix)) {
                            skippedNoTransform++;
                            continue;
                        }

                        float combinedTransform[16];
                        MultiplyMatrix4x4(instanceMatrix, baseTransform, combinedTransform);

                        if (!pcmDataCache.count(pcmHash)) {
                            const auto& ref = pcmIndex[pcmHash];
                            std::ifstream pcmFile(ref.packPath, std::ios::binary);
                            if (!pcmFile.is_open()) {
                                skippedNoModel++;
                                continue;
                            }
                            pcmDataCache[pcmHash].resize(ref.size);
                            pcmFile.seekg(ref.absOffset);
                            pcmFile.read((char*)pcmDataCache[pcmHash].data(), ref.size);
                            pcmFile.close();

                            if (pcmDataCache[pcmHash].size() < 16) {
                                pcmDataCache.erase(pcmHash);
                                skippedNoModel++;
                                continue;
                            }
                        }

                        const auto& ref = pcmIndex[pcmHash];
                        std::string modelName = !meshRef.meshName.empty()
                            ? meshRef.meshName
                            : (!instanceName.empty()
                                ? instanceName
                                : (pcmName.empty() ? "scene_entity" : pcmName));

                        currentWorldKind = WorldMeshKind::SceneEntity;
                        AddMeshFromDataWithTransform(pcmDataCache[pcmHash], modelName, nullptr,
                                                     ref.packPath, ref.absOffset, combinedTransform,
                                                     meshRef.meshOffsetInPcm);
                        loadedInstances++;
                        placedSceneEntities++;
                    }

                    (void)placedSceneEntities;
                }
            }

            if (kEnableSceneEntityNameFallback && !usedSceneEntityRecords) {

            const uint32_t MARKER_ACE  = 0x7ACE5BAD;

            std::vector<size_t> acePositions;

            for (size_t i = 0; i + 4 <= blockSize; i += 4) {
                uint32_t val;
                memcpy(&val, &blockData[i], 4);
                if (val == MARKER_ACE)      acePositions.push_back(i);
            }

            for (size_t aceIdx = 0; aceIdx < acePositions.size(); aceIdx++) {
                size_t acePos = acePositions[aceIdx];
                totalInstances++;

                if (acePos + 20 > blockSize) continue;
                uint32_t instanceHash;
                memcpy(&instanceHash, &blockData[acePos + 16], 4);

                if (instanceHash == 0) continue;

                std::string instanceName;
                if (dictionary.count(instanceHash)) {
                    instanceName = StrToLower(dictionary[instanceHash]);
                }

                if (false) {
                    skippedNonRenderable++;
                    continue;
                }

                uint32_t pcmHash = 0;

                if (blockInfo.tocType == 0x04) {
                    pcmHash = blockInfo.tocHash;
                }

                if (pcmIndex.count(instanceHash)) {
                    pcmHash = instanceHash;
                }

                if (pcmHash == 0 && !instanceName.empty()) {
                    std::string stripped = StripNumericSuffix(instanceName);
                    if (stripped != instanceName)
                        pcmHash = TryResolveName(stripped, pcmIndex, pcmNameToHash);
                }

                if (pcmHash == 0 && !instanceName.empty()) {
                    std::string noPrefix = StripZonePrefix(instanceName);
                    std::string base = StripNumericSuffix(noPrefix);
                    if (base != instanceName)
                        pcmHash = TryResolveName(base, pcmIndex, pcmNameToHash);
                }

                if (pcmHash == 0 && !instanceName.empty()) {
                    std::string noPrefix = StripZonePrefix(instanceName);
                    if (noPrefix != instanceName)
                        pcmHash = TryResolveName(noPrefix, pcmIndex, pcmNameToHash);
                }

                if (pcmHash == 0 && !instanceName.empty()) {
                    std::string noPrefix = StripZonePrefix(instanceName);
                    std::string base = StripNumericSuffix(noPrefix);
                    std::string refName = "ref_" + base;
                    uint32_t h = HashString33(refName);
                    if (pcmIndex.count(h)) pcmHash = h;
                    if (pcmHash == 0 && pcmNameToHash.count(refName))
                        pcmHash = pcmNameToHash[refName];
                }

                if (pcmHash == 0 && !instanceName.empty()) {
                    std::string noPrefix = StripZonePrefix(instanceName);
                    const char* midPrefixes[] = {"ent_", "col_", "rf_", nullptr};
                    for (int mp = 0; midPrefixes[mp] && pcmHash == 0; mp++) {
                        size_t mpLen = strlen(midPrefixes[mp]);
                        if (noPrefix.size() > mpLen && noPrefix.substr(0, mpLen) == midPrefixes[mp]) {
                            std::string inner = noPrefix.substr(mpLen);

                            std::string innerNum = StripNumericSuffix(inner);
                            if (!innerNum.empty())
                                pcmHash = TryResolveName(innerNum, pcmIndex, pcmNameToHash);
                            if (pcmHash == 0 && !innerNum.empty())
                                pcmHash = TryResolveName(ExpandAbbreviations(innerNum), pcmIndex, pcmNameToHash);

                            if (pcmHash == 0) {
                                inner = StripZonePrefix(inner);
                                inner = StripNumericSuffix(inner);
                                inner = ExpandAbbreviations(inner);
                                if (!inner.empty())
                                    pcmHash = TryResolveName(inner, pcmIndex, pcmNameToHash);
                            }
                        }
                    }
                }

                if (pcmHash == 0 && !instanceName.empty()) {
                    std::string noPrefix = StripZonePrefix(instanceName);
                    std::string base = StripNumericSuffix(noPrefix);

                    for (auto* midP : {"ent_", "col_", "rf_"}) {
                        size_t mpLen = strlen(midP);
                        if (base.size() > mpLen && base.substr(0, mpLen) == midP) {
                            base = StripNumericSuffix(base.substr(mpLen));

                            base = StripZonePrefix(base);
                            base = StripNumericSuffix(base);
                            break;
                        }
                    }
                    if (!base.empty() && base != instanceName) {

                        std::string zoneModel = stem + "_" + base;
                        pcmHash = TryResolveName(zoneModel, pcmIndex, pcmNameToHash);

                        if (pcmHash == 0) {
                            std::string zoneModelEx = stem + "_" + ExpandAbbreviations(base);
                            pcmHash = TryResolveName(zoneModelEx, pcmIndex, pcmNameToHash);
                        }
                    }
                }

                if (pcmHash == 0 && !instanceName.empty()) {
                    if (pcmNameToHash.count(instanceName))
                        pcmHash = pcmNameToHash[instanceName];

                    if (pcmHash == 0) {
                        std::string expanded = ExpandAbbreviations(instanceName);
                        if (expanded != instanceName && pcmNameToHash.count(expanded))
                            pcmHash = pcmNameToHash[expanded];
                    }
                }

                if (pcmHash == 0 && instanceName.size() > 5 && instanceName.substr(0, 5) == "city_") {
                    std::string noCity = instanceName.substr(5);
                    std::string base = StripNumericSuffix(noCity);
                    if (base.size() > 4 && base.substr(base.size() - 4) == "_new") {
                        base = base.substr(0, base.size() - 4);
                    }
                    pcmHash = TryResolveName(base, pcmIndex, pcmNameToHash);
                    if (pcmHash == 0) {
                        pcmHash = TryResolveName("city_" + base, pcmIndex, pcmNameToHash);
                    }
                    if (pcmHash == 0) {
                        pcmHash = TryResolveName("jh_" + base, pcmIndex, pcmNameToHash);
                    }
                }

                if (pcmHash == 0 && !instanceName.empty()) {
                    std::string noPrefix = StripZonePrefix(instanceName);
                    if (noPrefix.size() > 7 && noPrefix.substr(0, 4) == "ent_") {
                        std::string inner = noPrefix.substr(4);
                        inner = StripZonePrefix(inner);
                        inner = StripNumericSuffix(inner);
                        inner = ExpandAbbreviations(inner);
                        if (!inner.empty())
                            pcmHash = TryResolveName(inner, pcmIndex, pcmNameToHash);
                    }
                }

                if (pcmHash == 0 && !instanceName.empty()) {
                    std::string noPrefix = StripZonePrefix(instanceName);
                    std::string base = StripNumericSuffix(noPrefix);
                    base = ExpandAbbreviations(base);
                    if (base == "sidewalk_corner" || base.find("sidewalk_corner") != std::string::npos) {

                        std::string core = base;
                        if (core.size() > 3 && core.substr(core.size() - 3) == "_4m")
                            core = core.substr(0, core.size() - 3);
                        else if (core.size() > 3 && core.substr(core.size() - 3) == "_5m")
                            core = core.substr(0, core.size() - 3);

                        pcmHash = TryResolveName(core + "_5m", pcmIndex, pcmNameToHash);
                        if (pcmHash == 0)
                            pcmHash = TryResolveName(core + "_4m", pcmIndex, pcmNameToHash);
                    }
                }

                if (pcmHash == 0 && !instanceName.empty()) {
                    std::string noPrefix = StripZonePrefix(instanceName);
                    std::string base = StripNumericSuffix(noPrefix);
                    base = ExpandAbbreviations(base);
                    if (!base.empty()) {
                        pcmHash = TryResolveName(base + "a", pcmIndex, pcmNameToHash);
                    }
                }

                if (pcmHash == 0 && !instanceName.empty()) {
                    std::string noPrefix = StripZonePrefix(instanceName);
                    std::string base = StripNumericSuffix(noPrefix);
                    if (base.size() > 2 && std::isalpha((unsigned char)base[0]) &&
                        std::isalpha((unsigned char)base[1]) && std::islower((unsigned char)base[2])) {
                        std::string inner = base.substr(2);
                        pcmHash = TryResolveName(inner, pcmIndex, pcmNameToHash);
                    }
                }

                if (pcmHash == 0 && !instanceName.empty()) {
                    std::string noPrefix = StripZonePrefix(instanceName);
                    std::string base = StripNumericSuffix(noPrefix);
                    if (base.size() > 3 && base.substr(0, 3) == "rf_") {
                        std::string inner = base.substr(3);
                        pcmHash = TryResolveName(inner, pcmIndex, pcmNameToHash);
                    }
                }

                if (pcmHash == 0 && !instanceName.empty()) {
                    std::string noPrefix = StripZonePrefix(instanceName);
                    std::string base = StripNumericSuffix(noPrefix);
                    base = ExpandAbbreviations(base);
                    for (char d = '1'; d <= '9' && pcmHash == 0; d++) {
                        std::string numPrefixed = std::string(1, d) + base;
                        uint32_t h = HashString33(numPrefixed);
                        if (pcmIndex.count(h)) { pcmHash = h; break; }
                        if (pcmNameToHash.count(numPrefixed)) { pcmHash = pcmNameToHash[numPrefixed]; break; }
                    }
                }

                if (pcmHash == 0 && !instanceName.empty()) {
                    std::string noPrefix = StripZonePrefix(instanceName);
                    std::string base = StripNumericSuffix(noPrefix);
                    base = ExpandAbbreviations(base);

                    if (base.size() > 4 && base.substr(0, 4) == "ent_")
                        base = base.substr(4);
                    base = StripZonePrefix(base);
                    base = StripNumericSuffix(base);
                    if (!base.empty()) {
                        std::string objName = "obj_" + base;
                        pcmHash = TryResolveName(objName, pcmIndex, pcmNameToHash);
                    }
                }

                if (pcmHash == 0 && !instanceName.empty()) {
                    std::string noPrefix = StripZonePrefix(instanceName);
                    std::string base = StripNumericSuffix(noPrefix);

                    for (auto* p : {"ent_", "col_", "rf_"}) {
                        if (base.size() > strlen(p) && base.substr(0, strlen(p)) == p)
                            base = base.substr(strlen(p));
                    }
                    base = StripZonePrefix(base);
                    base = StripNumericSuffix(base);
                    base = ExpandAbbreviations(base);

                    if (base.size() >= 5) {
                        for (const auto& [name, hash] : pcmNameToHash) {
                            if (name.size() >= base.size() &&
                                name.substr(name.size() - base.size()) == base) {
                                pcmHash = hash;
                                break;
                            }
                        }
                    }
                }

                if (pcmHash == 0 && !instanceName.empty()) {
                    std::string noPrefix = StripZonePrefix(instanceName);
                    std::string base = StripNumericSuffix(noPrefix);
                    base = ExpandAbbreviations(base);
                    if (!base.empty() && base.size() >= 4) {
                        for (const auto& [name, hash] : pcmNameToHash) {
                            if (name.size() > base.size() && name.substr(0, base.size()) == base &&
                                (name[base.size()] == '_' || std::isdigit((unsigned char)name[base.size()]))) {
                                pcmHash = hash;
                                break;
                            }
                        }
                    }
                }

                if (pcmHash == 0) {
                    skippedNoModel++;
                    continue;
                }

                float instanceMatrix[16];
                bool hasTransform = false;

                for (size_t scanOfs = acePos + 0x44;
                     scanOfs + 64 <= blockSize && scanOfs < acePos + 0x2000;
                     scanOfs += 4)
                {
                    float candidate[16];
                    memcpy(candidate, &blockData[scanOfs], 64);

                    if (std::fabs(candidate[3]) > 0.01f ||
                        std::fabs(candidate[7]) > 0.01f ||
                        std::fabs(candidate[11]) > 0.01f) continue;
                    if (std::fabs(candidate[15] - 1.0f) > 0.1f) continue;

                    bool rowsOk = true;
                    for (int r = 0; r < 3 && rowsOk; r++) {
                        float len2 = 0;
                        for (int c = 0; c < 3; c++)
                            len2 += candidate[r*4+c] * candidate[r*4+c];
                        float len = std::sqrt(len2);
                        if (len < 0.9f || len > 1.1f) rowsOk = false;
                    }
                    if (!rowsOk) continue;

                    memcpy(instanceMatrix, candidate, 64);
                    hasTransform = true;
                    break;
                }

                if (!hasTransform) {
                    skippedNoTransform++;
                    continue;
                }

                float combinedTransform[16];
                MultiplyMatrix4x4(instanceMatrix, baseTransform, combinedTransform);

                if (!pcmDataCache.count(pcmHash)) {
                    const auto& ref = pcmIndex[pcmHash];
                    std::ifstream pcmFile(ref.packPath, std::ios::binary);
                    if (!pcmFile.is_open()) continue;
                    pcmDataCache[pcmHash].resize(ref.size);
                    pcmFile.seekg(ref.absOffset);
                    pcmFile.read((char*)pcmDataCache[pcmHash].data(), ref.size);
                    pcmFile.close();

                    if (pcmDataCache[pcmHash].size() < 16) {
                        pcmDataCache.erase(pcmHash);
                        continue;
                    }
                }

                const auto& pcmData = pcmDataCache[pcmHash];
                const auto& ref = pcmIndex[pcmHash];

                std::string modelName = instanceName.empty()
                    ? (dictionary.count(pcmHash) ? StrToLower(dictionary[pcmHash]) : "unknown")
                    : instanceName;

                currentWorldKind = WorldMeshKind::SceneEntity;
                AddMeshFromDataWithTransform(pcmData, modelName, nullptr,
                                             ref.packPath, ref.absOffset,
                                             combinedTransform);
                loadedInstances++;
            }
            }

            if (blockInfo.tocType == 0x0A) {
                currentWorldKind = WorldMeshKind::Lego;
                QueueLegoPlacementsFromBlock(*this, blockData, meshHashBindings, pcmIndex,
                                             baseTransform, legoBatches,
                                             totalLegoRecords, loadedLegoRecords,
                                             skippedLegoNoModel);
            }

            if (kEnableGuessedOrphanPlacementRecords && blockInfo.tocType == 0x0A) {

                size_t recStart = 0;
                int recCount = 0;
                for (size_t probe = 0; probe + 0x20 <= blockSize; probe += 4) {
                    uint16_t rt;
                    memcpy(&rt, &blockData[probe + 4], 2);
                    if (rt != 9) continue;
                    float px;
                    memcpy(&px, &blockData[probe + 8], 4);
                    if (std::fabs(px) > 10000.0f) continue;

                    if (probe + 0x40 <= blockSize) {
                        uint16_t rt2;
                        memcpy(&rt2, &blockData[probe + 0x24], 2);
                        if (rt2 != 9) continue;
                    }

                    recStart = probe;
                    size_t r = probe;
                    while (r + 0x20 <= blockSize) {
                        uint16_t rtt;
                        memcpy(&rtt, &blockData[r + 4], 2);
                        if (rtt != 9) break;
                        float rx;
                        memcpy(&rx, &blockData[r + 8], 4);
                        if (std::fabs(rx) > 10000.0f) break;
                        recCount++;
                        r += 0x20;
                    }
                    break;
                }

                if (recCount > 0) {

                    std::vector<uint32_t> pcm15Hashes;
                    for (const auto& meshRes : ReadPackMeshFileResources(path.string())) {
                        pcm15Hashes.push_back(meshRes.hash);
                    }
                    if (pcm15Hashes.empty())
                    {
                        file.clear();
                        file.seekg(tocStart);
                        while (file.good()) {
                            uint32_t th, tt, to2, ts;
                            file.read((char*)&th, 4); file.read((char*)&tt, 4);
                            file.read((char*)&to2, 4); file.read((char*)&ts, 4);
                            if (!file.good() || tt >= 0x1000 || tt == 0) break;
                            if (tt == 0x15) pcm15Hashes.push_back(th);
                        }
                    }
                    int numPcm15 = (int)pcm15Hashes.size();

                    int placedFromRecords = 0;
                    for (int ri = 0; ri < recCount; ri++) {
                        size_t ro = recStart + ri * 0x20;
                        uint16_t f14;
                        memcpy(&f14, &blockData[ro + 20], 2);

                        int pcmIdx = DecodePlacementPcmIndex(f14, numPcm15);
                        if (pcmIdx < 0 || pcmIdx >= numPcm15) continue;

                        uint32_t pcmH = pcm15Hashes[pcmIdx];
                        if (zoneBaseHashes.count(pcmH)) continue;
                        if (!pcmIndex.count(pcmH)) continue;

                        std::string pcmName = dictionary.count(pcmH) ? StrToLower(dictionary[pcmH]) : "";

                        if (!pcmDataCache.count(pcmH)) {
                            const auto& ref = pcmIndex[pcmH];
                            std::ifstream pcmFile(ref.packPath, std::ios::binary);
                            if (!pcmFile.is_open()) continue;
                            pcmDataCache[pcmH].resize(ref.size);
                            pcmFile.seekg(ref.absOffset);
                            pcmFile.read((char*)pcmDataCache[pcmH].data(), ref.size);
                            pcmFile.close();
                        }

                        uint16_t angle;
                        memcpy(&angle, &blockData[ro + 6], 2);
                        float x, y, z;
                        memcpy(&x, &blockData[ro + 8], 4);
                        memcpy(&y, &blockData[ro + 12], 4);
                        memcpy(&z, &blockData[ro + 16], 4);

                        float yawRad = angle * 3.14159265f / 180.0f;
                        float cy = std::cos(yawRad), sy = std::sin(yawRad);
                        float mat[16] = {
                            cy,  0, sy, 0,
                            0,   1,  0, 0,
                            -sy, 0, cy, 0,
                            x,   y,  z, 1
                        };
                        float combined[16];
                        MultiplyMatrix4x4(mat, baseTransform, combined);

                        const auto& pRef = pcmIndex[pcmH];

                        AddMeshFromDataWithTransform(pcmDataCache[pcmH], pcmName, nullptr,
                                                     pRef.packPath, pRef.absOffset, combined);
                        placedFromRecords++;
                    }

                    (void)placedFromRecords;

                }
            }
        }
        file.close();
    }

    currentWorldKind = WorldMeshKind::Lego;
    FlushLegoBatches(*this, pcmIndex, pcmDataCache, legoBatches,
                     loadedLegoRecords, skippedLegoNoModel);

    (void)zoneGeoCount;
    (void)interiorMeshCount;
    (void)totalInstances;
    (void)loadedInstances;
    (void)skippedNoModel;
    (void)skippedNonRenderable;
    (void)skippedNoTransform;
    (void)totalLegoRecords;
    (void)skippedLegoNoModel;
    (void)skippedLegoFiltered;

    pcmDataCache.clear();
    FlushPendingWholeWorldInstances();

    currentWorldKind = WorldMeshKind::Skybox;
    LoadSkybox();
    currentWorldKind = WorldMeshKind::SceneEntity;
    InstanceWholeWorldMeshes();

    // World collision lives across every pack, not in currentDir, so gather it here.
    LoadCollisionMeshesForWorld();

    isModelLoaded = true;
}
