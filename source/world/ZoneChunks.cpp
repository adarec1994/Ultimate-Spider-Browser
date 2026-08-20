static bool IsShortWorldBaseName(const std::string& nameLower) {
    if (nameLower.empty() || nameLower.size() > 3) return false;
    for (char c : nameLower) {
        if (!std::isalpha((unsigned char)c)) return false;
    }
    return true;
}

static bool IsWorldScenePackName(const std::string& stem) {
    if (stem.size() == 2 &&
        std::isalpha((unsigned char)stem[0]) &&
        std::isalpha((unsigned char)stem[1])) {
        return true;
    }

    if (stem.size() > 4 &&
        std::isalpha((unsigned char)stem[0]) &&
        std::isalpha((unsigned char)stem[1]) &&
        stem[2] == '_' &&
        stem[3] == 'v') {
        for (size_t i = 4; i < stem.size(); i++) {
            if (!std::isdigit((unsigned char)stem[i])) return false;
        }
        return true;
    }

    return false;
}

static int LoadWorldZoneChunks(SpiderManTool& tool, const float* baseTransform) {
    for (const auto& path : tool.foundPacks) {
        std::string stem = StrToLower(path.stem().string());
        if (!IsWorldScenePackName(stem)) continue;

        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) continue;

        size_t fileSize = file.tellg();
        if (fileSize < 32) { file.close(); continue; }

        const std::string regionMeshName = stem + "c";
        const std::string regionMeshAltName = stem + "r";
        for (const auto& meshRes : ReadPackMeshFileResources(path.string())) {
            if (meshRes.size <= 4 || (size_t)meshRes.absOffset + meshRes.size > fileSize) continue;

            std::string entryName = tool.dictionary.count(meshRes.hash)
                ? StrToLower(tool.dictionary[meshRes.hash])
                : "";
            if (entryName != regionMeshName && entryName != regionMeshAltName) continue;

            std::vector<uint8_t> pcmData(meshRes.size);
            file.clear();
            file.seekg(meshRes.absOffset);
            file.read((char*)pcmData.data(), meshRes.size);
            if (!file.good()) continue;
            if (pcmData.size() < 4 || ReadU32LE(pcmData, 0) != 0x204D4350) continue;

            tool.AddMeshFromDataWithTransform(pcmData, entryName, nullptr,
                                              path.string(), meshRes.absOffset, baseTransform);
        }

        file.close();
    }

    int zoneGeoCount = (int)tool.previewMeshes.size();
    return zoneGeoCount;
}

static std::set<uint32_t> BuildWorldZoneBaseHashes(const std::vector<fs::path>& foundPacks) {
    std::set<uint32_t> zoneBaseHashes;
    for (const auto& path : foundPacks) {
        std::string stem = StrToLower(path.stem().string());
        if (!IsWorldScenePackName(stem)) continue;
        zoneBaseHashes.insert(HashString33(stem));
    }
    return zoneBaseHashes;
}
