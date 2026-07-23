

static bool IsInteriorScenePackName(const std::string& stem) {
    std::string lower = StrToLower(stem);
    return lower.size() >= 6 && lower.substr(2, 4) == "_int";
}

static bool IsInteriorSceneMeshName(const std::string& name) {
    std::string lower = StrToLower(name);
    size_t intPos = lower.find("_int_");
    if (intPos == std::string::npos) return false;

    std::string tail = lower.substr(intPos + 5);
    size_t lastSep = tail.find_last_of('_');
    std::string last = (lastSep == std::string::npos) ? tail : tail.substr(lastSep + 1);

    if (last.size() == 2 &&
        std::isalpha((unsigned char)last[0]) &&
        (last[1] == 'c' || last[1] == 'r')) {
        return true;
    }

    if (last.size() == 3 &&
        last[0] == 'v' &&
        std::isdigit((unsigned char)last[1]) &&
        (last[2] == 'c' || last[2] == 'r')) {
        return true;
    }

    return false;
}

static int LoadWorldInteriorMeshes(SpiderManTool& tool, const float* baseTransform) {
    int loaded = 0;

    for (const auto& path : tool.foundPacks) {
        std::string stem = StrToLower(path.stem().string());
        if (!IsInteriorScenePackName(stem)) continue;

        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) continue;

        size_t fileSize = (size_t)file.tellg();
        for (const auto& meshRes : ReadPackMeshFileResources(path.string())) {
            if (meshRes.size <= 4 || (size_t)meshRes.absOffset + meshRes.size > fileSize) continue;

            std::string entryName = tool.dictionary.count(meshRes.hash)
                ? StrToLower(tool.dictionary[meshRes.hash])
                : "";
            if (entryName.empty()) {
                std::ostringstream ss;
                ss << "0x" << std::hex << meshRes.hash;
                entryName = ss.str();
            }
            if (!IsInteriorSceneMeshName(entryName)) continue;

            std::vector<uint8_t> pcmData(meshRes.size);
            file.clear();
            file.seekg(meshRes.absOffset);
            file.read((char*)pcmData.data(), meshRes.size);
            if (!file.good()) continue;
            if (pcmData.size() < 4 || ReadU32LE(pcmData, 0) != 0x204D4350) continue;

            tool.AddMeshFromDataWithTransform(pcmData, entryName, nullptr,
                                              path.string(), meshRes.absOffset,
                                              baseTransform);
            loaded++;
        }

        file.close();
    }

    return loaded;
}
