struct LandmarkEntry {
    const char* displayName;
    const char* textureSuffix;
    const char* meshKeyword;
};

static const LandmarkEntry kLandmarks[] = {
    { "The Baxter Building",                "BAXTERBLDG", "baxter"       },
    { "Daily Bugle",                        "DAILYBUGLE", "bugle"        },
    { "Doctor Strange's Sanctum Sanctorum", "DOCSTRANGE", "strange"      },
    { "Eastside Tunnel",                    "EASTTUNELL", "tunnelentrance" },
    { "Empire State Art Museum",            "ESAM",       "esam"         },
    { "Empire State University",            "ESU",        "esu_"         },
    { "Latverian Embassy",                  "LATVERIAN",  "latver"       },
    { "Midtown High School",                "MIDTWNHIGH", "midtown"      },
    { "Park Castle",                        "PARKCASTLE", "castle"       },
    { "Parker's House",                     "PARKRHOUSE", "parkerhouse"  },
    { "Queensborough Bridge",               "QUENBRIDGE", "bridgesupport"},
    { "Reed Richards Science Center",       "REEDRICH",   "reed"         },
    { "Statue of Liberty",                  "STULIBERTY", "liberty"      },
    { "The Place",                          "THESPOT",    "thespot"      },
    { "Trask Tower",                        "TRASKTOWER", "trask"        },
};
static const int kLandmarkCount = (int)(sizeof(kLandmarks) / sizeof(kLandmarks[0]));

int SpiderManTool::LandmarkCount() { return kLandmarkCount; }

const char* SpiderManTool::LandmarkDisplayName(int i) {
    return (i >= 0 && i < kLandmarkCount) ? kLandmarks[i].displayName : "";
}

const char* SpiderManTool::LandmarkTextureSuffix(int i) {
    return (i >= 0 && i < kLandmarkCount) ? kLandmarks[i].textureSuffix : "";
}

const char* GameTokenTypeName(int type) {
    switch (type) {
        case GTOKEN_LANDMARK: return "Landmark";
        case GTOKEN_COMIC:    return "Comic";
        case GTOKEN_SECRET:   return "Secret";
        case GTOKEN_MISSION:  return "Mission";
        case GTOKEN_RACE:     return "Race";
        case GTOKEN_VENOM:    return "Venom Race";
        case GTOKEN_COMBAT:   return "Combat";
        case GTOKEN_HEALTH:   return "Health";
        case GTOKEN_JS_RACE:  return "Storm Race";
        default:              return "?";
    }
}

const char* GameTokenTextureName(int type) {
    switch (type) {
        case GTOKEN_LANDMARK: return "token_landmark";
        case GTOKEN_COMIC:    return "token_comic";
        case GTOKEN_SECRET:   return "token_secret";
        case GTOKEN_MISSION:  return "token_mission";
        case GTOKEN_RACE:     return "token_race";
        case GTOKEN_VENOM:    return "token_venom";
        case GTOKEN_COMBAT:   return "token_combat";
        case GTOKEN_HEALTH:   return "token_health";
        case GTOKEN_JS_RACE:  return "token_js_race";
        default:              return "";
    }
}

static bool PlausibleRacePoint(float x, float y, float z) {
    if (!(x == x) || !(y == y) || !(z == z)) return false;
    if (std::fabs(x) >= 4000.0f || std::fabs(y) >= 800.0f || std::fabs(z) >= 4000.0f) return false;
    return (std::fabs(x) + std::fabs(y) + std::fabs(z)) > 1.0f;
}

static size_t CollectRaceWaypoints(const std::vector<uint8_t>& buf, size_t first, size_t stride,
                                   std::vector<std::array<float, 3>>* out) {
    size_t count = 0;
    for (int k = 0; k < 40; k++) {
        size_t off = first + (size_t)k * stride;
        if (off + 12 > buf.size()) break;
        float p[3];
        memcpy(p, buf.data() + off, sizeof(p));
        if (!PlausibleRacePoint(p[0], p[1], p[2])) break;
        if (out) {
            out->push_back({ -p[0], p[1], p[2] });
        }
        count++;
    }
    return count;
}

static bool ParseRaceBlob(const std::vector<uint8_t>& buf, RaceDef& race) {
    if (buf.size() < 0x140) return false;

    for (size_t i = 0x100; i + 0x30 < buf.size(); i += 4) {
        float t[4];
        memcpy(t, buf.data() + i, sizeof(t));
        bool ascending = t[0] < t[1] && t[1] < t[2] && t[2] < t[3];
        bool inRange = true;
        for (int k = 0; k < 4; k++) {
            if (!(t[k] > 1.0f && t[k] < 1200.0f)) { inRange = false; break; }
        }
        if (!ascending || !inRange) continue;

        const size_t first = i + 0x18;
        size_t bestStride = 0, bestCount = 0;
        for (size_t stride : { (size_t)0x24, (size_t)0x40 }) {
            size_t n = CollectRaceWaypoints(buf, first, stride, nullptr);
            if (n > bestCount) { bestCount = n; bestStride = stride; }
        }
        if (bestCount < 3) continue;

        memcpy(race.medalTimes, t, sizeof(t));
        race.waypoints.clear();
        CollectRaceWaypoints(buf, first, bestStride, &race.waypoints);
        return true;
    }
    return false;
}

void SpiderManTool::ParseRaceScriptInstances(const std::string& packFilePath) {
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

    struct Candidate { uint32_t hash, absOffset, size; };
    std::vector<Candidate> candidates;
    file.clear();
    file.seekg(tocStart);
    while (file.good()) {
        uint32_t h, t, o, s;
        file.read((char*)&h, 4); file.read((char*)&t, 4);
        file.read((char*)&o, 4); file.read((char*)&s, 4);
        if (!file.good() || t >= 0x1000 || t == 0) break;
        if (t != RES_KEY_SCRIPT_INST || s < 800 || s > 8192) continue;
        if ((size_t)packDataOffset + o + s > fileSize) continue;
        candidates.push_back({ h, packDataOffset + o, s });
    }

    fs::path pp(packFilePath);
    std::string stem = StrToLower(pp.stem().string());

    for (const auto& c : candidates) {
        std::vector<uint8_t> buf(c.size);
        file.clear();
        file.seekg(c.absOffset);
        file.read((char*)buf.data(), c.size);
        if (!file.good()) { file.clear(); continue; }

        std::string tail((const char*)buf.data(), buf.size());
        const bool isTrick = tail.find("trick_race_data") != std::string::npos;
        if (!isTrick) continue;

        RaceDef race;
        race.zone = stem;
        race.isVenom = tail.find("venom_trick_race_data") != std::string::npos;
        race.name = dictionary.count(c.hash) ? dictionary[c.hash] : (stem + "_trick_race");
        if (!ParseRaceBlob(buf, race)) {
            race.waypoints.clear();
        }
        raceDefs.push_back(std::move(race));
    }

    file.close();
}

static int TrailingNumber(const std::string& name) {
    size_t end = name.size();
    while (end > 0 && !isdigit((unsigned char)name[end - 1])) end--;
    if (end == 0) return -1;
    size_t start = end;
    while (start > 0 && isdigit((unsigned char)name[start - 1])) start--;
    if (start == end) return -1;
    try { return std::stoi(name.substr(start, end - start)); } catch (...) { return -1; }
}

void SpiderManTool::ResolveGameTokenLinks() {
    struct NamedPoint { float p[3]; int landmark; };
    std::vector<NamedPoint> landmarkAnchors;

    auto considerMesh = [&](const std::string& rawName, const float* bboxMin, const float* bboxMax) {
        if (rawName.empty()) return;
        std::string n = StrToLower(rawName);
        for (int li = 0; li < kLandmarkCount; li++) {
            if (n.find(kLandmarks[li].meshKeyword) == std::string::npos) continue;
            NamedPoint np{};
            for (int a = 0; a < 3; a++) np.p[a] = (bboxMin[a] + bboxMax[a]) * 0.5f;
            np.landmark = li;
            landmarkAnchors.push_back(np);
            break;
        }
    };

    for (const auto& m : previewMeshes) {
        if (m.instances.empty()) {
            considerMesh(m.placementName.empty() ? m.meshName : m.placementName, m.bboxMin, m.bboxMax);
        } else {
            for (const auto& inst : m.instances) {
                considerMesh(inst.name.empty() ? m.meshName : inst.name, inst.bboxMin, inst.bboxMax);
            }
        }
    }

    for (auto& gd : gameTokenDefs) {
        gd.categoryIndex = TrailingNumber(gd.name);
        gd.unlockIndex = -1;
        gd.raceIndex = -1;

        if (gd.type == GTOKEN_COMIC) {
            gd.unlockIndex = gd.categoryIndex;
        } else if (gd.type == GTOKEN_LANDMARK) {
            int best = -1;
            float bestDist = 250.0f * 250.0f;
            for (const auto& a : landmarkAnchors) {
                float dx = a.p[0] - gd.position[0];
                float dy = a.p[1] - gd.position[1];
                float dz = a.p[2] - gd.position[2];
                float d = dx * dx + dy * dy + dz * dz;
                if (d < bestDist) { bestDist = d; best = a.landmark; }
            }
            if (best >= 0) gd.unlockIndex = best;
            else if (gd.categoryIndex >= 1 && gd.categoryIndex <= kLandmarkCount)
                gd.unlockIndex = gd.categoryIndex - 1;
        } else if (gd.type == GTOKEN_RACE || gd.type == GTOKEN_VENOM || gd.type == GTOKEN_JS_RACE) {
            const bool wantVenom = (gd.type == GTOKEN_VENOM);
            int best = -1;
            float bestDist = 400.0f * 400.0f;
            for (int ri = 0; ri < (int)raceDefs.size(); ri++) {
                const auto& r = raceDefs[ri];
                if (r.isVenom != wantVenom) continue;
                if (r.waypoints.empty()) continue;
                const auto& w = r.waypoints.front();
                float dx = w[0] - gd.position[0];
                float dy = w[1] - gd.position[1];
                float dz = w[2] - gd.position[2];
                float d = dx * dx + dy * dy + dz * dz;
                if (d < bestDist) { bestDist = d; best = ri; }
            }
            gd.raceIndex = best;
        }
    }
}

unsigned int SpiderManTool::GetLandmarkImage(int landmarkIndex, bool big) {
    if (landmarkIndex < 0 || landmarkIndex >= kLandmarkCount) return 0;
    std::string name = std::string(big ? "U_I_BIG_" : "U_I_FULL_") +
                       kLandmarks[landmarkIndex].textureSuffix;
    return LoadTextureByName(name);
}

unsigned int SpiderManTool::GetComicCoverImage(int coverIndex) {
    if (coverIndex < 1 || coverIndex > 75) return 0;
    char buf[32];
    snprintf(buf, sizeof(buf), "U_COV_%02d_COV", coverIndex);
    unsigned int tex = LoadTextureByName(buf);
    if (tex) return tex;
    for (const char* suffix : { "A", "B", "C" }) {
        snprintf(buf, sizeof(buf), "U_COV_%02d%s_COV", coverIndex, suffix);
        tex = LoadTextureByName(buf);
        if (tex) return tex;
    }
    return 0;
}

void SpiderManTool::LoadGameTokenMeshes() {
    if (gameTokenDefs.empty()) return;

    const uint32_t tokensHash = HashString33("tokens");

    std::vector<uint8_t> pcmData;
    std::string sourcePack;
    uint32_t sourceOffset = 0;

    for (const auto& packEntry : foundPacks) {
        const std::string packPath = packEntry.string();
        std::ifstream file(packPath, std::ios::binary | std::ios::ate);
        if (!file.is_open()) continue;
        size_t fileSize = file.tellg();
        if (fileSize < 64) { file.close(); continue; }

        file.seekg(24);
        uint32_t headerSize, packDataOffset;
        file.read((char*)&headerSize, 4);
        file.read((char*)&packDataOffset, 4);
        if (!file.good()) { file.close(); continue; }

        size_t hdrReadSize = std::min((size_t)500000, fileSize);
        std::vector<uint8_t> tempHeader(hdrReadSize);
        file.seekg(0);
        file.read((char*)tempHeader.data(), hdrReadSize);
        size_t tocStart = FindTocStart(tempHeader, hdrReadSize);
        if (tocStart == 0) { file.close(); continue; }

        uint32_t absOffset = 0, size = 0;
        file.clear();
        file.seekg(tocStart);
        while (file.good()) {
            uint32_t h, t, o, s;
            file.read((char*)&h, 4); file.read((char*)&t, 4);
            file.read((char*)&o, 4); file.read((char*)&s, 4);
            if (!file.good() || t >= 0x1000 || t == 0) break;
            if (h == tokensHash && t == RES_KEY_MESH && s > 64) {
                absOffset = packDataOffset + o;
                size = s;
                break;
            }
        }

        if (absOffset && (size_t)absOffset + size <= fileSize) {
            pcmData.resize(size);
            file.clear();
            file.seekg(absOffset);
            file.read((char*)pcmData.data(), size);
            if (file.good()) {
                sourcePack = packPath;
                sourceOffset = absOffset;
                file.close();
                break;
            }
            pcmData.clear();
        }
        file.close();
    }

    if (pcmData.empty()) return;

    std::vector<std::array<float, 16>> transforms[GTOKEN_TYPE_COUNT];
    for (const auto& gd : gameTokenDefs) {
        if (gd.type < 0 || gd.type >= GTOKEN_TYPE_COUNT) continue;
        std::array<float, 16> m{};
        m[0] = m[5] = m[10] = m[15] = 1.0f;
        m[12] = gd.position[0];
        m[13] = gd.position[1];
        m[14] = gd.position[2];
        transforms[gd.type].push_back(m);
    }

    const WorldMeshKind savedKind = currentWorldKind;
    currentWorldKind = WorldMeshKind::SceneEntity;
    const bool savedCollect = collectWholeWorldInstances;
    collectWholeWorldInstances = false;

    for (int type = 0; type < GTOKEN_TYPE_COUNT; type++) {
        if (transforms[type].empty()) continue;

        const size_t firstMesh = previewMeshes.size();
        std::string modelName = std::string("token_") + StrToLower(GameTokenTypeName(type));
        AddMeshInstancesFromDataBatched(pcmData, modelName, nullptr,
                                        sourcePack, sourceOffset, transforms[type]);

        const unsigned int tex = LoadTextureByName(GameTokenTextureName(type));
        for (size_t i = firstMesh; i < previewMeshes.size(); i++) {
            auto& mesh = previewMeshes[i];
            if (tex) {
                mesh.textureId = tex;
                mesh.textureName = GameTokenTextureName(type);
                mesh.textureFrames.clear();
            }
            mesh.isGameToken = true;
            mesh.gameTokenType = type;
            mesh.isAlphaTest = true;
            for (size_t k = 0; k < mesh.instances.size() && k < transforms[type].size(); k++) {
                mesh.instances[k].name = modelName;
            }
        }
    }

    currentWorldKind = savedKind;
    collectWholeWorldInstances = savedCollect;
}
