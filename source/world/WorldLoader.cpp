// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  LoadAllWorldGeometries  â€“  zone meshes + instanced props
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
void SpiderManTool::LoadAllWorldGeometries() {
    previewMeshes.clear();
    isWorldMode = true;
    isModelPreview = true;

    selectedMeshIndex = -1;
    selectedMeshPcmData.clear();
    showWorldMeshHexEditor = false;

    camPos[0] = 0.0f; camPos[1] = 200.0f; camPos[2] = -600.0f;
    camFront[0] = 0.0f; camFront[1] = -0.3f; camFront[2] = -1.0f;
    camUp[0] = 0.0f; camUp[1] = 1.0f; camUp[2] = 0.0f;
    camYaw = -90.0f;
    camPitch = -15.0f;
    camSpeed = 500.0f;

    InitModelPreview();
    ResetWorldMeshDebugCategories();

    // Base transform: flip X axis to match rendering convention
    float baseTransform[16] = {0};
    baseTransform[0]  = -1.0f;
    baseTransform[5]  =  1.0f;
    baseTransform[10] =  1.0f;
    baseTransform[15] =  1.0f;

    // â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
    //  PASS 1 â€“ Build global PCM model index from ALL packs
    // â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
    std::map<uint32_t, PCMModelRef> pcmIndex;       // hash â†’ model location
    std::map<std::string, uint32_t> pcmNameToHash;   // lowercase name â†’ hash
    BuildWorldPcmIndex(*this, pcmIndex, pcmNameToHash);

    // â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
    //  PASS 2 â€“ Load base zone geometry (terrain/buildings)
    // â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
    int zoneGeoCount = LoadWorldZoneChunks(*this, baseTransform);
    int interiorMeshCount = LoadWorldInteriorMeshes(*this, baseTransform);

    // â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
    //  PASS 3 â€“ Load instanced props from type 0x0A and type 0x04 data
    // â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
    // Cache PCM data to avoid re-reading the same model from disk
    std::map<uint32_t, std::vector<uint8_t>> pcmDataCache;

    // Track which zone hashes we already loaded as base geometry
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

    int conglomDebugRecordsSeen = 0;
    int conglomDebugBuildableRecords = 0;
    int conglomDebugRenderedMembers = 0;
    int conglomDebugNoMeshRefs = 0;
    int conglomDebugNoMemberPo = 0;
    int conglomDebugNoRootPo = 0;
    int conglomDebugZeroLocalPo = 0;
    std::vector<std::string> conglomDebugSamples;

    auto formatConglomPos = [](const std::array<float, 3>& pos) {
        std::ostringstream ss;
        ss << "(" << pos[0] << ", " << pos[1] << ", " << pos[2] << ")";
        return ss.str();
    };

    auto addConglomDebugSample = [&](const std::string& packStem,
                                     const std::string& instanceName,
                                     const ConglomeratePlacementDebug& debug) {
        if (conglomDebugSamples.size() >= 24) return;

        std::string probeName = StrToLower(instanceName + " " + debug.firstMeshName + " " + debug.lastMeshName);
        const bool interestingName =
            probeName.find("streetlamp") != std::string::npos ||
            probeName.find("trafficlight") != std::string::npos ||
            probeName.find("lamp") != std::string::npos;

        if (!interestingName &&
            !debug.allMatrixTranslationsNearZero &&
            debug.meshRefCount == debug.matrixCount &&
            debug.matrixReadOk) {
            return;
        }

        std::ostringstream ss;
        ss << "  " << packStem;
        if (!instanceName.empty()) ss << " entity=" << instanceName;
        ss << " class=" << debug.classId
           << " flags=0x" << std::hex << debug.headerFlags << std::dec
           << " refs=" << debug.meshRefCount
           << " member_abs_po=" << debug.matrixCount
           << " placements=" << debug.placementCount
           << " readPo=" << (debug.matrixReadOk ? "yes" : "no")
           << " zeroLocalPo=" << (debug.allMatrixTranslationsNearZero ? "yes" : "no")
           << " rootPo=" << (debug.rootMatrixReadOk ? "yes" : "no");
        if (debug.rootMatrixReadOk) {
            ss << " rootPos=" << formatConglomPos(debug.rootMatrixPos);
        }
        if (!debug.firstMeshName.empty()) {
            ss << " first=" << debug.firstMeshName
               << " firstPos=" << formatConglomPos(debug.firstMatrixPos);
        }
        if (!debug.lastMeshName.empty() && debug.lastMeshName != debug.firstMeshName) {
            ss << " last=" << debug.lastMeshName
               << " lastPos=" << formatConglomPos(debug.lastMatrixPos);
        }
        conglomDebugSamples.push_back(ss.str());
    };

    std::map<LegoBatchKey, LegoBatch> legoBatches;

    // Block info: offset, size, tocHash, tocType
    struct InstanceBlockInfo {
        uint32_t offset;
        uint32_t size;
        uint32_t tocHash;   // For type 0x04, this IS the PCM model hash
        uint8_t  tocType;   // 0x0A or 0x04
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

        // Collect type 0x0A (instance placement) and type 0x04 (entity defs)
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
                        // Type 0x04: entity definition block â€“ the TOC hash IS the PCM
                        // model hash. Only collect if we have a matching PCM model.
                        instanceBlocks.push_back({absOfs, size, hash, 0x04});
                    }
                }
            }
        }

        // Process each instance block
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
                        ConglomeratePlacementDebug conglomDebug;
                        const bool builtConglomerate = BuildConglomerateMemberPlacements(
                                blockData, rec, meshHashBindings, pcmIndex, memberPlacements, &conglomDebug);
                        if (conglomDebug.isConglomerate) {
                            conglomDebugRecordsSeen++;
                            if (builtConglomerate) conglomDebugBuildableRecords++;
                            if (conglomDebug.meshRefCount == 0) conglomDebugNoMeshRefs++;
                            if (!conglomDebug.matrixReadOk || conglomDebug.matrixCount == 0) conglomDebugNoMemberPo++;
                            if (!conglomDebug.rootMatrixReadOk) conglomDebugNoRootPo++;
                            if (conglomDebug.allMatrixTranslationsNearZero) conglomDebugZeroLocalPo++;
                            addConglomDebugSample(stem, instanceName, conglomDebug);
                        }

                        if (builtConglomerate) {
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

                                RecordWorldMeshPlacementDebug("conglomerate", memberName,
                                                              memberRef.packPath, memberRef.absOffset,
                                                              combinedTransform);
                                AddMeshFromDataWithTransform(pcmDataCache[memberPcmHash], memberName, nullptr,
                                                             memberRef.packPath, memberRef.absOffset,
                                                             combinedTransform,
                                                             member.meshRef.meshOffsetInPcm);
                                loadedInstances++;
                                placedMembers++;
                            }

                            if (placedMembers > 0) {
                                placedSceneEntities += placedMembers;
                                conglomDebugRenderedMembers += placedMembers;
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

                        RecordWorldMeshPlacementDebug("streetlights/entities", modelName,
                                                      ref.packPath, ref.absOffset,
                                                      combinedTransform);
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
            // Scan for all marker positions (aligned to 4 bytes)
            const uint32_t MARKER_ACE  = 0x7ACE5BAD;

            std::vector<size_t> acePositions;

            for (size_t i = 0; i + 4 <= blockSize; i += 4) {
                uint32_t val;
                memcpy(&val, &blockData[i], 4);
                if (val == MARKER_ACE)      acePositions.push_back(i);
            }

            // Process each instance record
            for (size_t aceIdx = 0; aceIdx < acePositions.size(); aceIdx++) {
                size_t acePos = acePositions[aceIdx];
                totalInstances++;


                // Instance name hash is at 7ACE5BAD + 16
                if (acePos + 20 > blockSize) continue;
                uint32_t instanceHash;
                memcpy(&instanceHash, &blockData[acePos + 16], 4);

                // Skip null hashes (padding/invalid entries)
                if (instanceHash == 0) continue;

                // Skip if this is a zone base hash (already loaded as terrain)

                // Get the instance name for filtering / name resolution
                std::string instanceName;
                if (dictionary.count(instanceHash)) {
                    instanceName = StrToLower(dictionary[instanceHash]);
                }

                // Non-renderable instances (collision, triggers, placeholders) used
                // to be skipped here. We now let them load so the user can see
                // them as a translucent ghost overlay -- AddMeshFromData marks
                // matching mesh names via IsNonRenderableMeshName.
                if (false) {
                    skippedNonRenderable++;
                    continue;
                }

                // â”€â”€ Resolve instance hash â†’ PCM model hash â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
                uint32_t pcmHash = 0;

                // For type 0x04 blocks, the TOC hash IS the PCM model hash
                if (blockInfo.tocType == 0x04) {
                    pcmHash = blockInfo.tocHash;
                }

                // 1) Direct match
                if (pcmIndex.count(instanceHash)) {
                    pcmHash = instanceHash;
                }

                // 2) Strip numeric suffix: "ee_storesign03" â†’ "ee_storesign"
                if (pcmHash == 0 && !instanceName.empty()) {
                    std::string stripped = StripNumericSuffix(instanceName);
                    if (stripped != instanceName)
                        pcmHash = TryResolveName(stripped, pcmIndex, pcmNameToHash);
                }

                // 3) Strip zone prefix + numeric suffix: "ee_lightsa05" â†’ "lightsa"
                if (pcmHash == 0 && !instanceName.empty()) {
                    std::string noPrefix = StripZonePrefix(instanceName);
                    std::string base = StripNumericSuffix(noPrefix);
                    if (base != instanceName)
                        pcmHash = TryResolveName(base, pcmIndex, pcmNameToHash);
                }

                // 4) Strip only zone prefix: "gf_ironwerk" â†’ "ironwerk"
                if (pcmHash == 0 && !instanceName.empty()) {
                    std::string noPrefix = StripZonePrefix(instanceName);
                    if (noPrefix != instanceName)
                        pcmHash = TryResolveName(noPrefix, pcmIndex, pcmNameToHash);
                }

                // 5) Try "ref_" prefix: "jh_penthoused" â†’ "ref_penthoused"
                if (pcmHash == 0 && !instanceName.empty()) {
                    std::string noPrefix = StripZonePrefix(instanceName);
                    std::string base = StripNumericSuffix(noPrefix);
                    std::string refName = "ref_" + base;
                    uint32_t h = HashString33(refName);
                    if (pcmIndex.count(h)) pcmHash = h;
                    if (pcmHash == 0 && pcmNameToHash.count(refName))
                        pcmHash = pcmNameToHash[refName];
                }

                // 6) Strip intermediate prefixes (ent_, col_, rf_):
                //    "fg_ent_fg_penthouseb" â†’ strip zone â†’ "ent_fg_penthouseb"
                //    â†’ strip ent_ â†’ "fg_penthouseb" â†’ TryResolveName âœ“
                //    Also try after stripping embedded zone: "penthouseb"
                if (pcmHash == 0 && !instanceName.empty()) {
                    std::string noPrefix = StripZonePrefix(instanceName);
                    const char* midPrefixes[] = {"ent_", "col_", "rf_", nullptr};
                    for (int mp = 0; midPrefixes[mp] && pcmHash == 0; mp++) {
                        size_t mpLen = strlen(midPrefixes[mp]);
                        if (noPrefix.size() > mpLen && noPrefix.substr(0, mpLen) == midPrefixes[mp]) {
                            std::string inner = noPrefix.substr(mpLen);
                            // Try BEFORE stripping embedded zone (e.g., "fg_penthouseb")
                            std::string innerNum = StripNumericSuffix(inner);
                            if (!innerNum.empty())
                                pcmHash = TryResolveName(innerNum, pcmIndex, pcmNameToHash);
                            if (pcmHash == 0 && !innerNum.empty())
                                pcmHash = TryResolveName(ExpandAbbreviations(innerNum), pcmIndex, pcmNameToHash);
                            // Try AFTER stripping embedded zone (e.g., "penthouseb")
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

                // 7) Zone-prefixed model lookup: for instances in zone ZZ,
                //    try "zz_" + base in case the PCM model is zone-specific
                //    (e.g., fg_penthouseb is a PCM in FG.PCPACK)
                if (pcmHash == 0 && !instanceName.empty()) {
                    std::string noPrefix = StripZonePrefix(instanceName);
                    std::string base = StripNumericSuffix(noPrefix);
                    // Also try after stripping double prefixes
                    for (auto* midP : {"ent_", "col_", "rf_"}) {
                        size_t mpLen = strlen(midP);
                        if (base.size() > mpLen && base.substr(0, mpLen) == midP) {
                            base = StripNumericSuffix(base.substr(mpLen));
                            // Strip embedded zone prefix too
                            base = StripZonePrefix(base);
                            base = StripNumericSuffix(base);
                            break;
                        }
                    }
                    if (!base.empty() && base != instanceName) {
                        // Try with the pack's zone prefix
                        std::string zoneModel = stem + "_" + base;
                        pcmHash = TryResolveName(zoneModel, pcmIndex, pcmNameToHash);
                        // Also try expanded abbreviation with zone prefix
                        if (pcmHash == 0) {
                            std::string zoneModelEx = stem + "_" + ExpandAbbreviations(base);
                            pcmHash = TryResolveName(zoneModelEx, pcmIndex, pcmNameToHash);
                        }
                    }
                }

                // 8) Exact name match in pcmNameToHash (last resort)
                if (pcmHash == 0 && !instanceName.empty()) {
                    if (pcmNameToHash.count(instanceName))
                        pcmHash = pcmNameToHash[instanceName];
                    // Also try with abbreviation expansion on the full name
                    if (pcmHash == 0) {
                        std::string expanded = ExpandAbbreviations(instanceName);
                        if (expanded != instanceName && pcmNameToHash.count(expanded))
                            pcmHash = pcmNameToHash[expanded];
                    }
                }

                // 9) Handle city_* prefix (e.g., city_baxter_spire01 â†’ jh_baxter_spire)
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

                // 10) Double zone prefix: "jj_ent_jj_streetlampa05" â†’ strip both
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

                // 11) sidewalk_corner without size â†’ try both _4m and _5m variants
                if (pcmHash == 0 && !instanceName.empty()) {
                    std::string noPrefix = StripZonePrefix(instanceName);
                    std::string base = StripNumericSuffix(noPrefix);
                    base = ExpandAbbreviations(base);
                    if (base == "sidewalk_corner" || base.find("sidewalk_corner") != std::string::npos) {
                        // Strip any existing size suffix and try both variants
                        std::string core = base;
                        if (core.size() > 3 && core.substr(core.size() - 3) == "_4m")
                            core = core.substr(0, core.size() - 3);
                        else if (core.size() > 3 && core.substr(core.size() - 3) == "_5m")
                            core = core.substr(0, core.size() - 3);
                        // Try _5m first (more common), then _4m
                        pcmHash = TryResolveName(core + "_5m", pcmIndex, pcmNameToHash);
                        if (pcmHash == 0)
                            pcmHash = TryResolveName(core + "_4m", pcmIndex, pcmNameToHash);
                    }
                }

                // 12) Append variant suffix 'a': "trafficlight" â†’ "trafficlighta"
                if (pcmHash == 0 && !instanceName.empty()) {
                    std::string noPrefix = StripZonePrefix(instanceName);
                    std::string base = StripNumericSuffix(noPrefix);
                    base = ExpandAbbreviations(base);
                    if (!base.empty()) {
                        pcmHash = TryResolveName(base + "a", pcmIndex, pcmNameToHash);
                    }
                }

                // 13) Embedded zone prefix without underscore: "iitablea" â†’ "tablea"
                if (pcmHash == 0 && !instanceName.empty()) {
                    std::string noPrefix = StripZonePrefix(instanceName);
                    std::string base = StripNumericSuffix(noPrefix);
                    if (base.size() > 2 && std::isalpha((unsigned char)base[0]) &&
                        std::isalpha((unsigned char)base[1]) && std::islower((unsigned char)base[2])) {
                        std::string inner = base.substr(2);
                        pcmHash = TryResolveName(inner, pcmIndex, pcmNameToHash);
                    }
                }

                // 14) "rf_" prefix models: "rf_skylightb" â†’ "skylightb"
                if (pcmHash == 0 && !instanceName.empty()) {
                    std::string noPrefix = StripZonePrefix(instanceName);
                    std::string base = StripNumericSuffix(noPrefix);
                    if (base.size() > 3 && base.substr(0, 3) == "rf_") {
                        std::string inner = base.substr(3);
                        pcmHash = TryResolveName(inner, pcmIndex, pcmNameToHash);
                    }
                }

                // 15) Numeric-prefixed PCM models: many PCMs have a digit prefix
                //     like "1stor_comhe01_trm02", "2obj_edge_01", "6rf_watertow_woodb".
                //     Instances reference them without the prefix.
                //     Try prepending "1".."9" to the resolved base name.
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

                // 16) "obj_" prefix: instances like "dumpster01" â†’ "obj_dumpster"
                if (pcmHash == 0 && !instanceName.empty()) {
                    std::string noPrefix = StripZonePrefix(instanceName);
                    std::string base = StripNumericSuffix(noPrefix);
                    base = ExpandAbbreviations(base);
                    // Strip ent_ if present
                    if (base.size() > 4 && base.substr(0, 4) == "ent_")
                        base = base.substr(4);
                    base = StripZonePrefix(base);
                    base = StripNumericSuffix(base);
                    if (!base.empty()) {
                        std::string objName = "obj_" + base;
                        pcmHash = TryResolveName(objName, pcmIndex, pcmNameToHash);
                    }
                }

                // 17) Try the instance name as a substring of PCM names
                //     e.g. "drum_cone" matches "rf_cone" won't work, but
                //     "parked_car" might match a longer PCM name
                if (pcmHash == 0 && !instanceName.empty()) {
                    std::string noPrefix = StripZonePrefix(instanceName);
                    std::string base = StripNumericSuffix(noPrefix);
                    // Strip ent_, col_ prefixes
                    for (auto* p : {"ent_", "col_", "rf_"}) {
                        if (base.size() > strlen(p) && base.substr(0, strlen(p)) == p)
                            base = base.substr(strlen(p));
                    }
                    base = StripZonePrefix(base);
                    base = StripNumericSuffix(base);
                    base = ExpandAbbreviations(base);
                    // Search for base as a suffix in PCM names
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

                // 18) Prefix matching for parameterized names (e.g., stor_come â†’ stor_come_07_10)
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

                // â”€â”€ Find transform â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
                // Find transform: scan forward for valid po matrix.
                // Each entity (including conglom members) has a world-space po
                // somewhere after its 7ACE5BAD marker. Scan forward until found.
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
// â”€â”€ Compose: instance transform Ã— base X-flip â”€â”€â”€â”€â”€â”€â”€
                float combinedTransform[16];
                MultiplyMatrix4x4(instanceMatrix, baseTransform, combinedTransform);

                // â”€â”€ Load PCM model data (cached) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
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

                RecordWorldMeshPlacementDebug("streetlights/entities", modelName,
                                              ref.packPath, ref.absOffset,
                                              combinedTransform);
                AddMeshFromDataWithTransform(pcmData, modelName, nullptr,
                                             ref.packPath, ref.absOffset,
                                             combinedTransform);
                loadedInstances++;
            } // end instance record loop
            }

            // â”€â”€ PASS 2: Placement records for orphan PCMs â”€â”€â”€â”€â”€â”€â”€â”€
            // Some PCMs (bushes, trees, lamps, walls, gates) are placed via
            // stride-0x20 records embedded after the last entity in the 0x0A block.
            // Format per record: pad(4) type(2) angle(2) X(4) Y(4) Z(4) f14(2) f16(2) f18(4) group(4)
            // OpenUSM's parse code 11 is the lego/static-prop map. Its
            // placement records reference resource_directory.mesh_locations,
            // not PCM order, so resolve mesh -> owning PCM before loading.
            if (blockInfo.tocType == 0x0A) {
                QueueLegoPlacementsFromBlock(*this, blockData, meshHashBindings, pcmIndex,
                                             baseTransform, legoBatches,
                                             totalLegoRecords, loadedLegoRecords,
                                             skippedLegoNoModel);
            }

            if (kEnableGuessedOrphanPlacementRecords && blockInfo.tocType == 0x0A) {
                // Find contiguous type=9 records by scanning
                size_t recStart = 0;
                int recCount = 0;
                for (size_t probe = 0; probe + 0x20 <= blockSize; probe += 4) {
                    uint16_t rt;
                    memcpy(&rt, &blockData[probe + 4], 2);
                    if (rt != 9) continue;
                    float px;
                    memcpy(&px, &blockData[probe + 8], 4);
                    if (std::fabs(px) > 10000.0f) continue;

                    // Check next record also type=9 (contiguous block)
                    if (probe + 0x40 <= blockSize) {
                        uint16_t rt2;
                        memcpy(&rt2, &blockData[probe + 0x24], 2);
                        if (rt2 != 9) continue;
                    }

                    // Found start of records - count them
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
                    // OpenUSM loads mesh files from resource_directory.mesh_file_locations.
                    // The placement index follows that order; TOC order is only a fallback.
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

                    // Runtime mesh ordering differs from TOC hash order

                    // Mesh width does not imply world-space data. IG park pieces such
                    // as ig_col_park/ig_gate are wide local meshes with real records.

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
                        // Non-renderable meshes (collision, triggers, placeholders)
                        // are now loaded and marked debug-transparent in
                        // AddMeshFromData; no longer skipped here.
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
                        RecordWorldMeshPlacementDebug("unique pcms", pcmName,
                                                      pRef.packPath, pRef.absOffset,
                                                      combined);
                        AddMeshFromDataWithTransform(pcmDataCache[pcmH], pcmName, nullptr,
                                                     pRef.packPath, pRef.absOffset, combined);
                        placedFromRecords++;
                    }

                    (void)placedFromRecords;

                }
            } // end placement records pass
        } // end instanceBlocks loop
        file.close();
    } // end pack loop

    FlushLegoBatches(*this, pcmIndex, pcmDataCache, legoBatches,
                     loadedLegoRecords, skippedLegoNoModel);

    Log("Conglomerate placement debug:");
    Log("  records seen: " + std::to_string(conglomDebugRecordsSeen));
    Log("  records with mesh refs + member_abs_po: " + std::to_string(conglomDebugBuildableRecords));
    Log("  rendered member meshes: " + std::to_string(conglomDebugRenderedMembers));
    Log("  records with no mesh refs: " + std::to_string(conglomDebugNoMeshRefs));
    Log("  records with missing/unreadable member_abs_po: " + std::to_string(conglomDebugNoMemberPo));
    Log("  records with missing/unreadable root PO: " + std::to_string(conglomDebugNoRootPo));
    Log("  records where all member_abs_po translations are near 0,0,0: " + std::to_string(conglomDebugZeroLocalPo));
    Log("  samples:");
    if (conglomDebugSamples.empty()) {
        Log("    (none)");
    } else {
        for (const auto& sample : conglomDebugSamples) {
            Log(sample);
        }
    }

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

    LoadSkybox();
    BatchWorldMeshesByType();
    DumpWorldMeshDebugCategories(*this);
    DumpWorldOriginPlacementDebug(*this);

    isModelLoaded = true;
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  LoadSkybox
