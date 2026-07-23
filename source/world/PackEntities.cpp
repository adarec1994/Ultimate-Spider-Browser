void SpiderManTool::LoadPackEntities(const std::string& packFilePath, const float* baseTransform) {

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

    std::vector<PackMeshFileResource> packMeshResources = ReadPackMeshFileResources(packFilePath);

    fs::path pp(packFilePath);
    std::string stem = StrToLower(pp.stem().string());
    std::string zoneBaseName = stem + "c";
    uint32_t zoneBaseHash = HashString33(zoneBaseName);

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

    std::map<uint32_t, std::vector<uint8_t>> pcmCache;
    for (auto& lp : localPcms) {
        if (lp.absOffset + lp.size > fileSize) continue;
        file.clear(); file.seekg(lp.absOffset);
        pcmCache[lp.hash].resize(lp.size);
        file.read((char*)pcmCache[lp.hash].data(), lp.size);
    }

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

                AddMeshFromDataWithTransform(pcmCache[pcmHash], modelName, nullptr,
                                             srcPack, srcOffset, combined,
                                             meshRef.meshOffsetInPcm);
                placedEntities++;
                placedFromScene++;
            }

            (void)placedFromScene;
        }

        if (kEnableSceneEntityNameFallback && !usedSceneEntityRecords) {

        const uint32_t MARKER = 0x7ACE5BAD;
        std::vector<size_t> acePositions;
        for (size_t i = 0; i + 4 <= blockSize; i += 4) {
            uint32_t val; memcpy(&val, &blockData[i], 4);
            if (val == MARKER) acePositions.push_back(i);
        }

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

            if (false) {
                filteredCount++;
                continue;
            }

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

            if (pcmHash == 0) continue;
            if (!pcmCache.count(pcmHash)) {
                continue;
            }
            matchedPcmHashes.insert(pcmHash);

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

                    AddMeshFromDataWithTransform(pcmCache[pcmHash], modelName, nullptr,
                                                 srcPack, srcOffset, combined,
                                                 binding.meshOffsetInPcm);
                    placedLegos++;
                    placedFromThisBlock++;
                }

                (void)placedFromThisBlock;
            }
        }

        if (kEnableGuessedOrphanPlacementRecords) {

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

                for (int ri = 0; ri < recCount; ri++) {
                    size_t ro = recStart + ri * 0x20;
                    uint16_t f14; memcpy(&f14, &blockData[ro + 20], 2);

                    int pcmIdx = DecodePlacementPcmIndex(f14, numPcm15);
                    if (pcmIdx < 0 || pcmIdx >= numPcm15) continue;

                    uint32_t pcmH = pcm15Hashes[pcmIdx];
                    std::string pcmName = dictionary.count(pcmH) ? StrToLower(dictionary[pcmH]) : "";

                    if (pcmH == zoneBaseHash || pcmH == largestPcmHash) continue;
                    if (!pcmCache.count(pcmH)) continue;

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

                    AddMeshFromDataWithTransform(pcmCache[pcmH], pcmName, nullptr,
                                                 packFilePath, 0, combined);
                    placedOrphans++;
                }
            }
        }
    }

    file.close();
    BatchWorldMeshesByType();

    (void)placedEntities;
    (void)placedLegos;
    (void)placedOrphans;
    (void)skippedLegoFiltered;
    (void)skippedLegoNoModel;
    (void)stem;
}
