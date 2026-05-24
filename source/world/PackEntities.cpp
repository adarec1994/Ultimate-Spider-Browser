void SpiderManTool::LoadPackEntities(const std::string& packFilePath, const float* baseTransform) {
    ResetWorldMeshDebugCategories();

    std::ifstream file(packFilePath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) return;

    size_t fileSize = file.tellg();
    if (fileSize < 64) { file.close(); return; }

    file.seekg(24);
    uint32_t headerSize, packDataOffset;
    file.read((char*)&headerSize, 4);
    file.read((char*)&packDataOffset, 4);
    if (!file.good()) { file.close(); return; }

    size_t hdrReadSize = std::min((size_t)500000, fileSize);
    std::vector<uint8_t> tempHeader(hdrReadSize);
    file.seekg(0);
    file.read((char*)tempHeader.data(), hdrReadSize);

    size_t tocStart = FindTocStart(tempHeader, hdrReadSize);
    if (tocStart == 0) { file.close(); return; }

    // â”€â”€ Read TOC â”€â”€
    struct TocEntry { uint32_t hash, type, absOffset, size; };
    std::vector<TocEntry> toc;
    file.clear();
    file.seekg(tocStart);
    while (file.good()) {
        uint32_t h, t, o, s;
        file.read((char*)&h, 4); file.read((char*)&t, 4);
        file.read((char*)&o, 4); file.read((char*)&s, 4);
        if (!file.good() || t >= 0x1000 || t == 0) break;
        toc.push_back({h, t, packDataOffset + o, s});
    }

    // â”€â”€ Identify zone base hash â”€â”€
    // The zone base mesh (e.g., IGC for IG.PCPACK) is the largest type 0x15 entry.
    // Also match by name: stem + "c" (e.g., "igc" for "ig").
    std::vector<PackMeshFileResource> packMeshResources = ReadPackMeshFileResources(packFilePath);

    fs::path pp(packFilePath);
    std::string stem = StrToLower(pp.stem().string());
    std::string zoneBaseName = stem + "c";
    uint32_t zoneBaseHash = HashString33(zoneBaseName);
    // Also find by dictionary name match and by largest size
    uint32_t largestPcmHash = 0;
    uint32_t largestPcmSize = 0;
    for (auto& te : toc) {
        if (te.type == 0x15 && te.size > largestPcmSize) {
            largestPcmSize = te.size;
            largestPcmHash = te.hash;
        }
        if (te.type == 0x15 && dictionary.count(te.hash)) {
            std::string n = StrToLower(dictionary[te.hash]);
            if (n == zoneBaseName) zoneBaseHash = te.hash;
        }
    }

    // â”€â”€ Build local PCM index â”€â”€
    for (const auto& meshRes : packMeshResources) {
        if (meshRes.size > largestPcmSize) {
            largestPcmSize = meshRes.size;
            largestPcmHash = meshRes.hash;
        }
        if (dictionary.count(meshRes.hash)) {
            std::string n = StrToLower(dictionary[meshRes.hash]);
            if (n == zoneBaseName) zoneBaseHash = meshRes.hash;
        }
    }

    struct LocalPcm { uint32_t hash; uint32_t absOffset; uint32_t size; std::string name; };
    std::vector<LocalPcm> localPcms;
    if (!packMeshResources.empty()) {
        for (const auto& meshRes : packMeshResources) {
            if (meshRes.size > 64) {
                std::string n = dictionary.count(meshRes.hash) ? StrToLower(dictionary[meshRes.hash]) : "";
                localPcms.push_back({meshRes.hash, meshRes.absOffset, meshRes.size, n});
            }
        }
    } else {
        for (auto& te : toc) {
            if (te.type == 0x15 && te.size > 64) {
                std::string n = dictionary.count(te.hash) ? StrToLower(dictionary[te.hash]) : "";
                localPcms.push_back({te.hash, te.absOffset, te.size, n});
            }
        }
    }

    if (localPcms.empty()) { file.close(); return; }

    // Textures are resolved automatically by AddMeshFromDataWithTransform
    // via LoadTextureByName when textureResolver is nullptr

    // â”€â”€ Cache PCM data â”€â”€
    std::map<uint32_t, std::vector<uint8_t>> pcmCache;
    for (auto& lp : localPcms) {
        if (lp.absOffset + lp.size > fileSize) continue;
        file.clear(); file.seekg(lp.absOffset);
        pcmCache[lp.hash].resize(lp.size);
        file.read((char*)pcmCache[lp.hash].data(), lp.size);
    }

    // â”€â”€ Build nameâ†’hash lookup â”€â”€
    std::map<std::string, uint32_t> nameToHash;
    std::map<uint32_t, PCMModelRef> localPcmIndex;
    for (auto& lp : localPcms) {
        if (!lp.name.empty()) nameToHash[lp.name] = lp.hash;
        PCMModelRef ref;
        ref.packPath = packFilePath;
        ref.absOffset = lp.absOffset;
        ref.size = lp.size;
        localPcmIndex[lp.hash] = ref;
    }

    // â”€â”€ Load CITY_ARENA PCMs for cross-pack references â”€â”€
    {
        fs::path packDir = fs::path(packFilePath).parent_path();
        fs::path caPath;
        for (auto& entry : fs::directory_iterator(packDir)) {
            if (StrToLower(entry.path().filename().string()) == "city_arena.pcpack") {
                caPath = entry.path(); break;
            }
        }
        if (!caPath.empty() && fs::exists(caPath) && caPath != fs::path(packFilePath)) {
            std::ifstream caFile(caPath, std::ios::binary);
            if (caFile.is_open()) {
                caFile.seekg(0, std::ios::end);
                size_t caFileSize = caFile.tellg();
                caFile.seekg(24);
                uint32_t caHS, caDataOff;
                caFile.read((char*)&caHS, 4);
                caFile.read((char*)&caDataOff, 4);

                // Read header to find TOC
                size_t hdrRead = std::min((size_t)caDataOff, std::min(caFileSize, (size_t)0x100000));
                std::vector<uint8_t> caHdr(hdrRead);
                caFile.seekg(0);
                caFile.read((char*)caHdr.data(), hdrRead);

                size_t caTocStart = 0;
                for (size_t ci = 0; ci + 8 < hdrRead; ci += 4) {
                    uint32_t v; memcpy(&v, &caHdr[ci], 4);
                    if (v != 0xE3E3E3E3) continue;
                    for (size_t cj = ci+4; cj + 4 < hdrRead; cj++) {
                        uint32_t v2; memcpy(&v2, &caHdr[cj], 4);
                        if (v2 == 0xE3E3E3E3) { caTocStart = cj + 4; break; }
                    }
                    break;
                }

                // Pass 1: collect all type 0x15 TOC entries
                struct CaEntry { uint32_t hash, absOffset, size; };
                std::vector<CaEntry> caEntries;
                if (caTocStart > 0 && caTocStart + 16 <= hdrRead) {
                    size_t cp = caTocStart;
                    while (cp + 16 <= hdrRead) {
                        uint32_t ch, ct, co, cs;
                        memcpy(&ch, &caHdr[cp], 4); memcpy(&ct, &caHdr[cp+4], 4);
                        memcpy(&co, &caHdr[cp+8], 4); memcpy(&cs, &caHdr[cp+12], 4);
                        if (ct >= 0x1000 || ct == 0) break;
                        if (ct == 0x15 && !pcmCache.count(ch)) {
                            caEntries.push_back({ch, caDataOff + co, cs});
                        }
                        cp += 16;
                    }
                }

                // Pass 2: load PCM data
                int caLoaded = 0;
                for (auto& ce : caEntries) {
                    if (ce.absOffset + ce.size > caFileSize) continue;
                    pcmCache[ce.hash].resize(ce.size);
                    caFile.seekg(ce.absOffset);
                    caFile.read((char*)pcmCache[ce.hash].data(), ce.size);
                    std::string cn = dictionary.count(ce.hash) ? StrToLower(dictionary[ce.hash]) : "";
                    if (!cn.empty()) nameToHash[cn] = ce.hash;
                    PCMModelRef ref;
                    ref.packPath = caPath.string();
                    ref.absOffset = ce.absOffset;
                    ref.size = ce.size;
                    localPcmIndex[ce.hash] = ref;
                    caLoaded++;
                }
                caFile.close();
                (void)caLoaded;
            }
        }
    }

    // â”€â”€ Process 0x0A block: entity instances + orphan placement records â”€â”€
    int placedEntities = 0, placedOrphans = 0, placedLegos = 0;
    int skippedLegoNoModel = 0, skippedLegoFiltered = 0;
    std::vector<MeshLocationBinding> meshLocationBindings = BuildMeshLocationBindings(packFilePath);
    std::map<uint32_t, MeshLocationBinding> meshHashBindings;
    fs::path packDirForBindings = fs::path(packFilePath).parent_path();
    if (!packDirForBindings.empty() && fs::exists(packDirForBindings)) {
        for (const auto& entry : fs::directory_iterator(packDirForBindings)) {
            if (!entry.is_regular_file()) continue;
            std::string ext = StrToLower(entry.path().extension().string());
            if (ext == ".pcpack") {
                AddMeshHashBindingsFromPack(entry.path().string(), meshHashBindings, false);
            }
        }
    }
    for (const auto& binding : meshLocationBindings) {
        if (binding.meshHash != 0 && binding.pcmHash != 0) {
            meshHashBindings[binding.meshHash] = binding;
        }
    }

    for (auto& te : toc) {
        if (te.type != 0x0A) continue;
        if (te.absOffset + te.size > fileSize) continue;

        std::vector<uint8_t> blockData(te.size);
        file.clear(); file.seekg(te.absOffset);
        file.read((char*)blockData.data(), te.size);
        if (!file.good()) continue;
        uint32_t blockSize = te.size;

        bool usedSceneEntityRecords = false;
        std::vector<SceneEntityMashRecord> sceneRecords = FindSceneEntityMashRecords(blockData);
        if (!sceneRecords.empty()) {
            usedSceneEntityRecords = true;
            int placedFromScene = 0;

            for (const auto& rec : sceneRecords) {
                if (rec.mashStart + 0x24 > blockData.size()) continue;
                uint32_t instanceHash = ReadU32LE(blockData, rec.mashStart + 0x20);

                std::string instanceName = dictionary.count(instanceHash)
                    ? StrToLower(dictionary[instanceHash])
                    : "";

                std::vector<SceneMemberPlacement> memberPlacements;
                if (BuildConglomerateMemberPlacements(
                        blockData, rec, meshHashBindings, localPcmIndex, memberPlacements)) {
                    int placedMembers = 0;
                    for (const auto& member : memberPlacements) {
                        uint32_t memberPcmHash = member.meshRef.pcmHash;
                        if (memberPcmHash == 0) continue;

                        if (!pcmCache.count(memberPcmHash)) {
                            auto refIt = localPcmIndex.find(memberPcmHash);
                            if (refIt == localPcmIndex.end()) continue;
                            const auto& ref = refIt->second;
                            std::ifstream pcmFile(ref.packPath, std::ios::binary);
                            if (!pcmFile.is_open()) continue;
                            pcmCache[memberPcmHash].resize(ref.size);
                            pcmFile.seekg(ref.absOffset);
                            pcmFile.read((char*)pcmCache[memberPcmHash].data(), ref.size);
                            pcmFile.close();
                            if (pcmCache[memberPcmHash].size() < 16) {
                                pcmCache.erase(memberPcmHash);
                                continue;
                            }
                        }

                        float combined[16];
                        MultiplyMatrix4x4(member.matrix.data(), baseTransform, combined);

                        auto refIt = localPcmIndex.find(memberPcmHash);
                        const std::string& srcPack = (refIt != localPcmIndex.end()) ? refIt->second.packPath : packFilePath;
                        uint32_t srcOffset = (refIt != localPcmIndex.end()) ? refIt->second.absOffset : 0;
                        std::string modelName = !member.meshRef.meshName.empty()
                            ? member.meshRef.meshName
                            : (dictionary.count(memberPcmHash)
                                ? StrToLower(dictionary[memberPcmHash])
                                : "scene_member");

                        RecordWorldMeshPlacementDebug("conglomerate", modelName,
                                                      srcPack, srcOffset,
                                                      combined);
                        AddMeshFromDataWithTransform(pcmCache[memberPcmHash], modelName, nullptr,
                                                     srcPack, srcOffset, combined,
                                                     member.meshRef.meshOffsetInPcm);
                        placedEntities++;
                        placedMembers++;
                    }

                    if (placedMembers > 0) {
                        placedFromScene += placedMembers;
                        continue;
                    }
                }

                EntityMeshRef meshRef = FindEntityMeshRef(
                    blockData, rec, meshHashBindings, localPcmIndex, nameToHash);
                uint32_t pcmHash = meshRef.pcmHash;
                if (pcmHash == 0) continue;

                std::string pcmName = dictionary.count(pcmHash) ? StrToLower(dictionary[pcmHash]) : "";

                if (!pcmCache.count(pcmHash)) {
                    auto refIt = localPcmIndex.find(pcmHash);
                    if (refIt == localPcmIndex.end()) continue;
                    const auto& ref = refIt->second;
                    std::ifstream pcmFile(ref.packPath, std::ios::binary);
                    if (!pcmFile.is_open()) continue;
                    pcmCache[pcmHash].resize(ref.size);
                    pcmFile.seekg(ref.absOffset);
                    pcmFile.read((char*)pcmCache[pcmHash].data(), ref.size);
                    pcmFile.close();
                    if (pcmCache[pcmHash].size() < 16) {
                        pcmCache.erase(pcmHash);
                        continue;
                    }
                }

                float mat[16];
                if (!FindPlacementMatrixInRange(blockData,
                                                rec.mashStart + 0x44,
                                                rec.mashStart + rec.mashSize,
                                                mat)) {
                    continue;
                }

                float combined[16];
                MultiplyMatrix4x4(mat, baseTransform, combined);

                auto refIt = localPcmIndex.find(pcmHash);
                const std::string& srcPack = (refIt != localPcmIndex.end()) ? refIt->second.packPath : packFilePath;
                uint32_t srcOffset = (refIt != localPcmIndex.end()) ? refIt->second.absOffset : 0;
                std::string modelName = !meshRef.meshName.empty()
                    ? meshRef.meshName
                    : (!instanceName.empty() ? instanceName : (pcmName.empty() ? "scene_entity" : pcmName));
                RecordWorldMeshPlacementDebug("streetlights/entities", modelName,
                                              srcPack, srcOffset,
                                              combined);
                AddMeshFromDataWithTransform(pcmCache[pcmHash], modelName, nullptr,
                                             srcPack, srcOffset, combined,
                                             meshRef.meshOffsetInPcm);
                placedEntities++;
                placedFromScene++;
            }

            (void)placedFromScene;
        }

        if (kEnableSceneEntityNameFallback && !usedSceneEntityRecords) {
        // Find all 7ACE5BAD markers
        const uint32_t MARKER = 0x7ACE5BAD;
        std::vector<size_t> acePositions;
        for (size_t i = 0; i + 4 <= blockSize; i += 4) {
            uint32_t val; memcpy(&val, &blockData[i], 4);
            if (val == MARKER) acePositions.push_back(i);
        }

        // â”€â”€ Pass 1: Named entity instances â”€â”€
        std::set<uint32_t> matchedPcmHashes;
        int hash0Count = 0, filteredCount = 0, noNameCount = 0;
        for (size_t ai = 0; ai < acePositions.size(); ai++) {
            size_t acePos = acePositions[ai];
            if (acePos + 20 > blockSize) continue;
            uint32_t instanceHash; memcpy(&instanceHash, &blockData[acePos + 16], 4);
            if (instanceHash == 0) { hash0Count++; continue; }

            std::string instanceName;
            if (dictionary.count(instanceHash))
                instanceName = StrToLower(dictionary[instanceHash]);

            if (instanceName.empty()) {
                noNameCount++;
                continue;
            }
            // Non-renderable instance types are no longer filtered here; they
            // load and render as a translucent ghost via isDebugTransparent.
            if (false) {
                filteredCount++;
                continue;
            }

            // Resolve to PCM hash using same pipeline as world loader
            uint32_t pcmHash = 0;
            if (pcmCache.count(instanceHash)) pcmHash = instanceHash;

            if (pcmHash == 0 && !instanceName.empty()) {
                std::string s = StripNumericSuffix(instanceName);
                if (s != instanceName) pcmHash = TryResolveName(s, localPcmIndex, nameToHash);
                if (pcmHash == 0) pcmHash = TryResolveName(s, localPcmIndex, nameToHash);
            }
            if (pcmHash == 0 && !instanceName.empty()) {
                std::string np = StripZonePrefix(instanceName);
                std::string b = StripNumericSuffix(np);
                if (b != instanceName) {
                    pcmHash = TryResolveName(b, localPcmIndex, nameToHash);
                    if (pcmHash == 0) pcmHash = TryResolveName(b, localPcmIndex, nameToHash);
                }
            }
            if (pcmHash == 0 && !instanceName.empty()) {
                std::string np = StripZonePrefix(instanceName);
                if (np != instanceName) {
                    pcmHash = TryResolveName(np, localPcmIndex, nameToHash);
                    if (pcmHash == 0) pcmHash = TryResolveName(np, localPcmIndex, nameToHash);
                }
            }
            if (pcmHash == 0 && !instanceName.empty()) {
                std::string np = StripZonePrefix(instanceName);
                const char* mids[] = {"ent_", "col_", "rf_", nullptr};
                for (int mp = 0; mids[mp] && pcmHash == 0; mp++) {
                    size_t ml = strlen(mids[mp]);
                    if (np.size() > ml && np.substr(0, ml) == mids[mp]) {
                        std::string inner = StripNumericSuffix(np.substr(ml));
                        pcmHash = TryResolveName(inner, localPcmIndex, nameToHash);
                        if (pcmHash == 0) pcmHash = TryResolveName(inner, localPcmIndex, nameToHash);
                        if (pcmHash == 0) {
                            inner = StripZonePrefix(inner);
                            inner = StripNumericSuffix(inner);
                            pcmHash = TryResolveName(inner, localPcmIndex, nameToHash);
                            if (pcmHash == 0) pcmHash = TryResolveName(inner, localPcmIndex, nameToHash);
                        }
                    }
                }
            }
            // Zone-prefixed model lookup
            if (pcmHash == 0 && !instanceName.empty()) {
                std::string np = StripZonePrefix(instanceName);
                std::string b = StripNumericSuffix(np);
                for (auto* midP : {"ent_", "col_", "rf_"}) {
                    size_t ml = strlen(midP);
                    if (b.size() > ml && b.substr(0, ml) == midP) {
                        b = StripNumericSuffix(b.substr(ml));
                        b = StripZonePrefix(b);
                        b = StripNumericSuffix(b);
                        break;
                    }
                }
                if (!b.empty() && b != instanceName) {
                    std::string zm = stem + "_" + b;
                    pcmHash = TryResolveName(zm, localPcmIndex, nameToHash);
                    if (pcmHash == 0) pcmHash = TryResolveName(zm, localPcmIndex, nameToHash);
                }
            }

            // Must resolve AND have cached PCM data
            if (pcmHash == 0) continue;
            if (!pcmCache.count(pcmHash)) {
                continue;
            }
            matchedPcmHashes.insert(pcmHash);

            // Find transform
            float mat[16];
            bool hasTransform = false;
            for (size_t scanOfs = acePos + 0x44;
                 scanOfs + 64 <= blockSize && scanOfs < acePos + 0x2000;
                 scanOfs += 4)
            {
                float c[16]; memcpy(c, &blockData[scanOfs], 64);
                if (std::fabs(c[3]) > 0.01f || std::fabs(c[7]) > 0.01f ||
                    std::fabs(c[11]) > 0.01f) continue;
                if (std::fabs(c[15] - 1.0f) > 0.1f) continue;
                bool ok = true;
                for (int r = 0; r < 3 && ok; r++) {
                    float l2 = c[r*4]*c[r*4]+c[r*4+1]*c[r*4+1]+c[r*4+2]*c[r*4+2];
                    if (std::sqrt(l2) < 0.9f || std::sqrt(l2) > 1.1f) ok = false;
                }
                if (!ok) continue;
                memcpy(mat, c, 64);
                hasTransform = true;
                break;
            }
            if (!hasTransform) continue;

            float combined[16];
            MultiplyMatrix4x4(mat, baseTransform, combined);
            auto refIt = localPcmIndex.find(pcmHash);
            const std::string& srcPack = (refIt != localPcmIndex.end()) ? refIt->second.packPath : packFilePath;
            uint32_t srcOffset = (refIt != localPcmIndex.end()) ? refIt->second.absOffset : 0;
            RecordWorldMeshPlacementDebug("streetlights/entities", instanceName,
                                          srcPack, srcOffset,
                                          combined);
            AddMeshFromDataWithTransform(pcmCache[pcmHash], instanceName, nullptr,
                                         srcPack, srcOffset, combined);
            placedEntities++;
        }
        (void)hash0Count;
        (void)noNameCount;
        (void)filteredCount;
        }

        if (!meshHashBindings.empty()) {
            size_t legoStart = 0;
            if (FindLegoMapDataStart(blockData, legoStart)) {
                std::vector<LegoPlacementRecord> legoRecords =
                    FindLegoPlacementRecords(blockData, legoStart);

                int placedFromThisBlock = 0;
                for (const auto& rec : legoRecords) {
                    auto bindingIt = meshHashBindings.find(rec.meshHash);
                    if (bindingIt == meshHashBindings.end()) {
                        skippedLegoNoModel++;
                        continue;
                    }
                    const MeshLocationBinding& binding = bindingIt->second;
                    uint32_t pcmHash = binding.pcmHash;
                    if (pcmHash == 0) {
                        skippedLegoNoModel++;
                        continue;
                    }

                    std::string pcmName = dictionary.count(pcmHash) ? StrToLower(dictionary[pcmHash]) : "";

                    if (!pcmCache.count(pcmHash)) {
                        auto refIt = localPcmIndex.find(pcmHash);
                        if (refIt == localPcmIndex.end()) {
                            skippedLegoNoModel++;
                            continue;
                        }

                        const auto& ref = refIt->second;
                        std::ifstream pcmFile(ref.packPath, std::ios::binary);
                        if (!pcmFile.is_open()) {
                            skippedLegoNoModel++;
                            continue;
                        }
                        pcmCache[pcmHash].resize(ref.size);
                        pcmFile.seekg(ref.absOffset);
                        pcmFile.read((char*)pcmCache[pcmHash].data(), ref.size);
                        pcmFile.close();
                        if (pcmCache[pcmHash].size() < 16) {
                            pcmCache.erase(pcmHash);
                            skippedLegoNoModel++;
                            continue;
                        }
                    }

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

                    auto refIt = localPcmIndex.find(pcmHash);
                    const std::string& srcPack = (refIt != localPcmIndex.end()) ? refIt->second.packPath : packFilePath;
                    uint32_t srcOffset = (refIt != localPcmIndex.end()) ? refIt->second.absOffset : 0;
                    std::string modelName = pcmName.empty() ? "lego_prop" : pcmName;
                    RecordWorldMeshPlacementDebug("lego", modelName,
                                                  srcPack, srcOffset,
                                                  combined);
                    AddMeshFromDataWithTransform(pcmCache[pcmHash], modelName, nullptr,
                                                 srcPack, srcOffset, combined,
                                                 binding.meshOffsetInPcm);
                    placedLegos++;
                    placedFromThisBlock++;
                }

                (void)placedFromThisBlock;
            }
        }

        // â”€â”€ Pass 2: Placement records (stride-0x20, type=9) with f14 = PCM index â”€â”€
        if (kEnableGuessedOrphanPlacementRecords) {
            // Find contiguous type=9 records
            size_t recStart = 0;
            int recCount = 0;
            for (size_t probe = 0; probe + 0x20 <= blockSize; probe += 4) {
                uint16_t rt; memcpy(&rt, &blockData[probe + 4], 2);
                if (rt != 9) continue;
                float px; memcpy(&px, &blockData[probe + 8], 4);
                if (std::fabs(px) > 10000.0f) continue;
                if (probe + 0x40 <= blockSize) {
                    uint16_t rt2; memcpy(&rt2, &blockData[probe + 0x24], 2);
                    if (rt2 != 9) continue;
                }
                recStart = probe;
                size_t r = probe;
                while (r + 0x20 <= blockSize) {
                    uint16_t rtt; memcpy(&rtt, &blockData[r + 4], 2);
                    if (rtt != 9) break;
                    float rx; memcpy(&rx, &blockData[r + 8], 4);
                    if (std::fabs(rx) > 10000.0f) break;
                    recCount++;
                    r += 0x20;
                }
                break;
            }

            if (recCount > 0) {
                // OpenUSM uses resource_directory.mesh_file_locations order here.
                std::vector<uint32_t> pcm15Hashes;
                for (const auto& meshRes : packMeshResources) {
                    pcm15Hashes.push_back(meshRes.hash);
                }
                if (pcm15Hashes.empty()) {
                    for (auto& t2 : toc) {
                        if (t2.type == 0x15) pcm15Hashes.push_back(t2.hash);
                    }
                }
                int numPcm15 = (int)pcm15Hashes.size();

                // Runtime mesh ordering differs from TOC hash order

                // Count per-mesh placements
                std::map<int, int> meshPlaceCounts;
                std::map<int, std::string> meshSkipReason;

                // Pre-scan: detect world-space PCMs (vertex bounds > 50 units)
                // These are baked geometry that should load once at identity, not per-record
                std::set<uint32_t> worldSpacePcms;
                for (int pi = 0; pi < numPcm15; pi++) {
                    uint32_t ph = pcm15Hashes[pi];
                    if (!pcmCache.count(ph)) continue;
                    const auto& pcd = pcmCache[ph];
                    if (pcd.size() < 80 || *(uint32_t*)pcd.data() != 0x204D4350) continue;
                    uint32_t num = *(uint32_t*)&pcd[8];
                    uint32_t ofs = *(uint32_t*)&pcd[12];
                    if (num > 500 || ofs >= pcd.size()) continue;
                    float minX=1e30f, maxX=-1e30f, minZ=1e30f, maxZ=-1e30f;
                    bool checked = false;
                    for (uint32_t ei = 0; ei < num && ei < 50; ei++) {
                        uint32_t eoff = ofs + ei * 12;
                        if (eoff + 12 > pcd.size()) break;
                        uint16_t etyp = *(uint16_t*)&pcd[eoff + 2];
                        uint32_t eofs = *(uint32_t*)&pcd[eoff + 4];
                        if (etyp != 512 || eofs + 16 > pcd.size()) continue;
                        uint32_t nSm = *(uint32_t*)&pcd[eofs + 8];
                        uint32_t smO = *(uint32_t*)&pcd[eofs + 12];
                        if (nSm > 256 || smO >= pcd.size()) continue;
                        for (uint32_t si = 0; si < nSm && si < 4; si++) {
                            uint32_t roff = smO + si * 8;
                            if (roff + 8 > pcd.size()) break;
                            uint32_t smOff = *(uint32_t*)&pcd[roff + 4];
                            if (smOff + 80 > pcd.size()) continue;
                            uint32_t vn = *(uint32_t*)&pcd[smOff + 56];
                            uint32_t vo = *(uint32_t*)&pcd[smOff + 60];
                            uint32_t st = *(uint32_t*)&pcd[smOff + 72];
                            if (vn > 100000 || vo >= pcd.size() || st == 0) continue;
                            for (uint32_t vi = 0; vi < std::min(vn, 100u); vi++) {
                                uint32_t voff = vo + vi * st;
                                if (voff + 12 > pcd.size()) break;
                                float vx = *(float*)&pcd[voff], vz = *(float*)&pcd[voff+8];
                                minX = std::min(minX, vx); maxX = std::max(maxX, vx);
                                minZ = std::min(minZ, vz); maxZ = std::max(maxZ, vz);
                                checked = true;
                            }
                            break;
                        }
                        break;
                    }
                    if (checked && ((maxX - minX) > 50.0f || (maxZ - minZ) > 50.0f)) {
                        worldSpacePcms.insert(ph);
                    }
                }

                for (int ri = 0; ri < recCount; ri++) {
                    size_t ro = recStart + ri * 0x20;
                    uint16_t f14; memcpy(&f14, &blockData[ro + 20], 2);

                    int pcmIdx = DecodePlacementPcmIndex(f14, numPcm15);
                    if (pcmIdx < 0 || pcmIdx >= numPcm15) { meshSkipReason[f14] = "out of range"; continue; }

                    uint32_t pcmH = pcm15Hashes[pcmIdx];
                    std::string pcmName = dictionary.count(pcmH) ? StrToLower(dictionary[pcmH]) : "";

                    if (pcmH == zoneBaseHash || pcmH == largestPcmHash) { meshSkipReason[pcmIdx] = "zone base"; continue; }
                    if (!pcmCache.count(pcmH)) { meshSkipReason[pcmIdx] = "no data"; continue; }
                    // Non-renderable and "col_" prefix meshes used to skip
                    // here; they now load as translucent debug overlays via
                    // RenderMesh::isDebugTransparent. Reason logging kept off
                    // for those rows since they'd otherwise flood the log.
                    if (worldSpacePcms.count(pcmH)) { meshSkipReason[pcmIdx] = "world-space (load once)"; continue; }

                    meshPlaceCounts[pcmIdx]++;

                    uint16_t angle; memcpy(&angle, &blockData[ro + 6], 2);
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
                    RecordWorldMeshPlacementDebug("unique pcms", pcmName,
                                                  packFilePath, 0,
                                                  combined);
                    AddMeshFromDataWithTransform(pcmCache[pcmH], pcmName, nullptr,
                                                 packFilePath, 0, combined);
                    placedOrphans++;
                }

                // Load world-space PCMs once at identity + any unplaced ones
                std::set<uint32_t> placedHashes;
                for (auto& [idx, cnt] : meshPlaceCounts) placedHashes.insert(pcm15Hashes[idx]);
                for (auto& [idx, reason] : meshSkipReason) {
                    if (idx < numPcm15) placedHashes.insert(pcm15Hashes[idx]);
                }
                for (int pi = 0; pi < numPcm15; pi++) {
                    uint32_t ph = pcm15Hashes[pi];
                    if (ph == zoneBaseHash || ph == largestPcmHash) continue;
                    if (!pcmCache.count(ph)) continue;
                    std::string pn = dictionary.count(ph) ? StrToLower(dictionary[ph]) : "";
                    // Non-renderable / col_-prefixed meshes load and render
                    // as translucent debug overlays now.

                    if (worldSpacePcms.count(ph)) {
                        // Compute centroid of placement records that reference this mesh
                        float cx = 0, cy = 0, cz = 0;
                        int ccount = 0;
                        for (int ri2 = 0; ri2 < recCount; ri2++) {
                            size_t ro2 = recStart + ri2 * 0x20;
                            uint16_t rf14; memcpy(&rf14, &blockData[ro2 + 20], 2);
                            int ridx = DecodePlacementPcmIndex(rf14, numPcm15);
                            if (ridx < 0 || ridx >= numPcm15) continue;
                            { // use ALL records for zone center
                                float rx, ry, rz;
                                memcpy(&rx, &blockData[ro2 + 8], 4);
                                memcpy(&ry, &blockData[ro2 + 12], 4);
                                memcpy(&rz, &blockData[ro2 + 16], 4);
                                cx += rx; cy += ry; cz += rz;
                                ccount++;
                            }
                        }
                        if (ccount > 0) { cx /= ccount; cz /= ccount; } cy = 0.0f;
                        float mat[16] = {
                            1, 0, 0, 0,
                            0, 1, 0, 0,
                            0, 0, 1, 0,
                            cx, cy, cz, 1
                        };
                        float combined[16];
                        MultiplyMatrix4x4(mat, baseTransform, combined);
                        RecordWorldMeshPlacementDebug("unique pcms", pn,
                                                      packFilePath, 0,
                                                      combined);
                        AddMeshFromDataWithTransform(pcmCache[ph], pn, nullptr,
                                                     packFilePath, 0, combined);
                        placedOrphans++;
                    } else if (!placedHashes.count(ph)) {
                        (void)pn;
                    }
                }
            }
        }
    }

    file.close();
    BatchWorldMeshesByType();
    DumpWorldMeshDebugCategories(*this);
    DumpWorldOriginPlacementDebug(*this);

    (void)placedEntities;
    (void)placedLegos;
    (void)placedOrphans;
    (void)skippedLegoFiltered;
    (void)skippedLegoNoModel;
    (void)stem;
}
