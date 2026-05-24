// Interior world-loading support lives here.
// Full-world loading currently does not place interior packs; keep this file
// as the category boundary so the interior path can be added without touching
// lego/static prop or zone chunk code again.
static bool IsInteriorScenePackName(const std::string& stem) {
    std::string lower = StrToLower(stem);
    return lower.size() >= 6 && lower.substr(2, 4) == "_int";
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

            std::vector<uint8_t> pcmData(meshRes.size);
            file.clear();
            file.seekg(meshRes.absOffset);
            file.read((char*)pcmData.data(), meshRes.size);
            if (!file.good()) continue;
            if (pcmData.size() < 4 || ReadU32LE(pcmData, 0) != 0x204D4350) continue;

            RecordWorldMeshPlacementDebug("interiors", entryName,
                                          path.string(), meshRes.absOffset,
                                          baseTransform);
            tool.AddMeshFromDataWithTransform(pcmData, entryName, nullptr,
                                              path.string(), meshRes.absOffset,
                                              baseTransform);
            loaded++;
        }

        file.close();
    }

    return loaded;
}
