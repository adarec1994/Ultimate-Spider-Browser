void SpiderManTool::LoadSkybox() {
    for (const auto& path : foundPacks) {
        std::string stem = StrToLower(path.stem().string());
        if (stem != "city_arena") continue;

        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) continue;

        size_t fileSize = file.tellg();
        if (fileSize < 32) { file.close(); continue; }

        file.seekg(24);
        uint32_t headerSize, dataOffset;
        file.read((char*)&headerSize, 4);
        file.read((char*)&dataOffset, 4);
        if (!file.good()) { file.close(); continue; }

        size_t start = 0;
        const uint32_t magic = 0xE3E3E3E3;
        size_t headerReadSize = std::min((size_t)500000, fileSize);
        std::vector<uint8_t> tempHeader(headerReadSize);
        file.seekg(0);
        file.read((char*)tempHeader.data(), headerReadSize);

        for (size_t i = 0; i + 4 <= tempHeader.size(); i++) {
            if (*(uint32_t*)&tempHeader[i] == magic) {
                for (size_t j = i + 4; j < i + 1000 && j + 4 <= tempHeader.size(); j++) {
                    if (*(uint32_t*)&tempHeader[j] == magic) {
                        start = j + 4;
                        break;
                    }
                }
                if (start != 0) break;
            }
        }

        if (start == 0) { file.close(); continue; }

        file.clear();
        file.seekg(start);

        while (file.good()) {
            uint32_t hash, type, offset, size;
            file.read((char*)&hash, 4);
            file.read((char*)&type, 4);
            file.read((char*)&offset, 4);
            file.read((char*)&size, 4);
            if (!file.good()) break;
            if (type >= 0x1000 || type == 0x0000) break;

            if (size > 4) {
                size_t filePos = file.tellg();
                uint32_t absOffset = (uint32_t)(dataOffset + offset);
                if (absOffset + 4 > fileSize) { file.seekg(filePos); continue; }

                file.seekg(absOffset);
                uint32_t sig = 0;
                file.read((char*)&sig, 4);

                if (file.good() && sig == 0x204D4350) {
                    std::string entryName = dictionary.count(hash) ? StrToLower(dictionary[hash]) : "";
                    if (entryName == "sky_day") {
                        std::vector<uint8_t> skyData(size);
                        file.seekg(absOffset);
                        file.read((char*)skyData.data(), size);

                        float transformMatrix[16] = {0};
                        transformMatrix[0] = -1.0f;
                        transformMatrix[5] = 1.0f;
                        transformMatrix[10] = 1.0f;
                        transformMatrix[15] = 1.0f;

                        AddMeshFromDataWithTransform(skyData, "sky_day", nullptr, path.string(), absOffset, transformMatrix);
                        file.close();
                        return;
                    }
                }
                file.clear();
                file.seekg(filePos);
            }
        }
        file.close();
        break;
    }
}

static void DecodeDXT1Block(const uint8_t* src, uint8_t out[4][4][4]) {
    uint16_t c0 = src[0] | (src[1] << 8), c1 = src[2] | (src[3] << 8);
    uint8_t palette[4][4];
    auto unpack565 = [](uint16_t c, uint8_t* r) {
        r[0] = ((c >> 11) & 0x1F) * 255 / 31;
        r[1] = ((c >> 5) & 0x3F) * 255 / 63;
        r[2] = (c & 0x1F) * 255 / 31;
        r[3] = 255;
    };
    unpack565(c0, palette[0]); unpack565(c1, palette[1]);
    if (c0 > c1) {
        for (int i = 0; i < 3; i++) { palette[2][i] = (2*palette[0][i]+palette[1][i])/3; palette[3][i] = (palette[0][i]+2*palette[1][i])/3; }
        palette[2][3] = palette[3][3] = 255;
    } else {
        for (int i = 0; i < 3; i++) palette[2][i] = (palette[0][i]+palette[1][i])/2;
        palette[2][3] = 255; palette[3][0]=palette[3][1]=palette[3][2]=0; palette[3][3]=0;
    }
    uint32_t bits = src[4]|(src[5]<<8)|(src[6]<<16)|(src[7]<<24);
    for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) {
        int idx = (bits >> ((y*4+x)*2)) & 3;
        memcpy(out[y][x], palette[idx], 4);
    }
}

static bool ConvertDDStoPNG(const std::vector<uint8_t>& dds, const fs::path& pngPath) {
    if (dds.size() < 128) return false;
    if (*(uint32_t*)dds.data() != 0x20534444) return false;

    struct DDSHdr { uint32_t sz,fl,h,w,pitch,dep,mip,rsv[11]; struct{uint32_t sz,fl,fourcc,bits,rM,gM,bM,aM;} pf; uint32_t caps[4],rsv2; };
    const DDSHdr* hdr = (const DDSHdr*)(dds.data()+4);
    int w = hdr->w, h = hdr->h;
    if (w < 1 || h < 1 || w > 8192 || h > 8192) return false;
    const uint8_t* px = dds.data() + 128;
    size_t pxSize = dds.size() - 128;

    std::vector<uint8_t> rgba(w * h * 4);

    if (hdr->pf.fl & 0x4) {
        uint32_t fourCC = hdr->pf.fourcc;
        int blockSize = (fourCC == 0x31545844) ? 8 : 16;
        int bw = (w+3)/4, bh = (h+3)/4;
        if ((size_t)bw*bh*blockSize > pxSize) return false;

        for (int by = 0; by < bh; by++) for (int bx = 0; bx < bw; bx++) {
            const uint8_t* block = px + (by*bw+bx)*blockSize;
            uint8_t decoded[4][4][4];

            if (fourCC == 0x31545844) {
                DecodeDXT1Block(block, decoded);
            } else if (fourCC == 0x33545844) {
                uint64_t alpha = 0; memcpy(&alpha, block, 8);
                DecodeDXT1Block(block+8, decoded);
                for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++)
                    decoded[y][x][3] = ((alpha >> ((y*4+x)*4)) & 0xF) * 17;
            } else if (fourCC == 0x35545844) {
                uint8_t a0 = block[0], a1 = block[1];
                uint8_t aPal[8]; aPal[0]=a0; aPal[1]=a1;
                if (a0 > a1) { for (int i=1;i<7;i++) aPal[i+1]=(uint8_t)(((7-i)*a0+i*a1)/7); }
                else { for (int i=1;i<5;i++) aPal[i+1]=(uint8_t)(((5-i)*a0+i*a1)/5); aPal[6]=0; aPal[7]=255; }
                uint64_t aBits = 0; for (int i = 2; i < 8; i++) aBits |= (uint64_t)block[i] << ((i-2)*8);
                DecodeDXT1Block(block+8, decoded);
                for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++)
                    decoded[y][x][3] = aPal[(aBits >> ((y*4+x)*3)) & 7];
            } else return false;

            for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) {
                int px2 = bx*4+x, py = by*4+y;
                if (px2 < w && py < h) memcpy(&rgba[(py*w+px2)*4], decoded[y][x], 4);
            }
        }
    } else if (hdr->pf.fl & 0x40) {
        if (hdr->pf.bits == 32) {
            for (int i = 0; i < w*h && (size_t)i*4+3 < pxSize; i++) {
                rgba[i*4+0]=px[i*4+2]; rgba[i*4+1]=px[i*4+1]; rgba[i*4+2]=px[i*4+0];
                rgba[i*4+3] = (hdr->pf.aM) ? px[i*4+3] : 255;
            }
        } else if (hdr->pf.bits == 24) {
            for (int i = 0; i < w*h && (size_t)i*3+2 < pxSize; i++) {
                rgba[i*4+0]=px[i*3+2]; rgba[i*4+1]=px[i*3+1]; rgba[i*4+2]=px[i*3+0]; rgba[i*4+3]=255;
            }
        } else return false;
    } else if (hdr->pf.fl & 0x20000) {
        if (hdr->pf.bits == 8) {
            for (int i = 0; i < w*h && (size_t)i < pxSize; i++) {
                rgba[i*4+0]=rgba[i*4+1]=rgba[i*4+2]=px[i]; rgba[i*4+3]=255;
            }
        } else return false;
    } else return false;

    return stbi_write_png(pngPath.string().c_str(), w, h, 4, rgba.data(), w*4) != 0;
}

// Writes a single-mesh GLB. When pngBytes is supplied the image is embedded as a bufferView-backed
// glTF image and wired to baseColorTexture, so the model arrives textured in Blender/UE rather than
// as flat grey with a loose PNG sitting next to it.
static void WriteGLB(const fs::path& path, const RenderMesh& mesh,
                     const std::vector<uint8_t>& pngBytes = {}) {
    std::vector<uint16_t> triangleIndices;
    if (mesh.mode == GL_TRIANGLE_STRIP) {
        for (size_t i = 0; i + 2 < mesh.indices.size(); i++) {
            uint16_t i0 = mesh.indices[i], i1 = mesh.indices[i+1], i2 = mesh.indices[i+2];
            if (i0 == i1 || i1 == i2 || i0 == i2) continue;
            if (i % 2 == 0) { triangleIndices.push_back(i0); triangleIndices.push_back(i1); triangleIndices.push_back(i2); }
            else { triangleIndices.push_back(i0); triangleIndices.push_back(i2); triangleIndices.push_back(i1); }
        }
    } else {
        triangleIndices = mesh.indices;
    }
    if (triangleIndices.empty()) return;

    // Convert the source winding to glTF's convention (counter-clockwise = front-facing), the same
    // correction the model exporter applies. The game draws with D3DCULL_CW, the opposite rule, so
    // passing indices through unchanged yields geometry that reads as inside-out anywhere culling
    // is enforced -- Blender hides it, UE does not. Runs after the strip conversion above so the
    // list is uniform either way.
    for (size_t i = 0; i + 2 < triangleIndices.size(); i += 3) {
        std::swap(triangleIndices[i + 1], triangleIndices[i + 2]);
    }

    int vertexCount = (int)mesh.positions.size() / 3;
    float minP[3] = {1e30f,1e30f,1e30f}, maxP[3] = {-1e30f,-1e30f,-1e30f};
    for (int i = 0; i < vertexCount; i++) {
        for (int a = 0; a < 3; a++) {
            float v = mesh.positions[i*3+a];
            if (v < minP[a]) minP[a] = v;
            if (v > maxP[a]) maxP[a] = v;
        }
    }

    std::vector<uint8_t> bin;
    auto align = [&bin]() { while (bin.size() % 4) bin.push_back(0); };
    auto add = [&bin](const void* d, size_t s) -> int {
        int o = (int)bin.size();
        const uint8_t* p = (const uint8_t*)d;
        bin.insert(bin.end(), p, p + s);
        return o;
    };

    align(); int posOff = add(mesh.positions.data(), mesh.positions.size()*4); int posLen = (int)(mesh.positions.size()*4);
    align(); int nrmOff = add(mesh.normals.data(), mesh.normals.size()*4); int nrmLen = (int)(mesh.normals.size()*4);
    align(); int uvOff = add(mesh.uvs.data(), mesh.uvs.size()*4); int uvLen = (int)(mesh.uvs.size()*4);
    align(); int idxOff = add(triangleIndices.data(), triangleIndices.size()*2); int idxLen = (int)(triangleIndices.size()*2);
    align();

    // Vertex colours carry the world's baked lighting/AO -- the exterior and building shaders are
    // essentially diffuse * vertexColour, so dropping these loses most of the world's shading.
    const bool hasColors = mesh.colors.size() == (size_t)vertexCount * 4;
    int colOff = 0, colLen = 0;
    if (hasColors) {
        colOff = add(mesh.colors.data(), mesh.colors.size() * 4);
        colLen = (int)(mesh.colors.size() * 4);
        align();
    }

    // Image data rides in the same binary chunk as a trailing bufferView.
    const bool hasImage = !pngBytes.empty();
    int imgOff = 0, imgLen = 0;
    if (hasImage) {
        imgOff = add(pngBytes.data(), pngBytes.size());
        imgLen = (int)pngBytes.size();
        align();
    }

    // bufferViews/accessors are numbered in emission order; keep the indices in sync here so the
    // optional colour and image views do not shift each other.
    int nextView = 4;
    const int colView = hasColors ? nextView++ : -1;
    const int imgView = hasImage  ? nextView++ : -1;
    const int colAccessor = hasColors ? 4 : -1;

    std::string name = mesh.meshName;
    for (char& c : name) if (c == '"' || c == '\\') c = '_';

    std::stringstream j;
    j << "{\"asset\":{\"version\":\"2.0\",\"generator\":\"USM\"},";
    j << "\"bufferViews\":[";
    j << "{\"buffer\":0,\"byteOffset\":" << posOff << ",\"byteLength\":" << posLen << ",\"target\":34962},";
    j << "{\"buffer\":0,\"byteOffset\":" << nrmOff << ",\"byteLength\":" << nrmLen << ",\"target\":34962},";
    j << "{\"buffer\":0,\"byteOffset\":" << uvOff << ",\"byteLength\":" << uvLen << ",\"target\":34962},";
    j << "{\"buffer\":0,\"byteOffset\":" << idxOff << ",\"byteLength\":" << idxLen << ",\"target\":34963}";
    if (hasColors) j << ",{\"buffer\":0,\"byteOffset\":" << colOff << ",\"byteLength\":" << colLen << ",\"target\":34962}";
    // No 'target' on the image view: it is not vertex or index data.
    if (hasImage)  j << ",{\"buffer\":0,\"byteOffset\":" << imgOff << ",\"byteLength\":" << imgLen << "}";
    j << "],";
    j << "\"accessors\":[";
    j << "{\"bufferView\":0,\"componentType\":5126,\"count\":" << vertexCount << ",\"type\":\"VEC3\","
      << "\"min\":[" << minP[0] << "," << minP[1] << "," << minP[2] << "],"
      << "\"max\":[" << maxP[0] << "," << maxP[1] << "," << maxP[2] << "]},";
    j << "{\"bufferView\":1,\"componentType\":5126,\"count\":" << vertexCount << ",\"type\":\"VEC3\"},";
    j << "{\"bufferView\":2,\"componentType\":5126,\"count\":" << vertexCount << ",\"type\":\"VEC2\"},";
    j << "{\"bufferView\":3,\"componentType\":5123,\"count\":" << triangleIndices.size() << ",\"type\":\"SCALAR\"}";
    if (hasColors)
        j << ",{\"bufferView\":" << colView << ",\"componentType\":5126,\"count\":" << vertexCount
          << ",\"type\":\"VEC4\"}";
    j << "],";
    if (hasImage) {
        j << "\"images\":[{\"bufferView\":" << imgView << ",\"mimeType\":\"image/png\",\"name\":\""
          << name << "_tex\"}],";
        j << "\"samplers\":[{\"wrapS\":10497,\"wrapT\":10497}],";
        j << "\"textures\":[{\"source\":0,\"sampler\":0}],";
    }
    const char* alphaMode = mesh.isAlphaTest ? "MASK" : (mesh.isTranslucent ? "BLEND" : "OPAQUE");
    j << "\"materials\":[{\"name\":\"" << name << "\",\"doubleSided\":true,"
      << "\"pbrMetallicRoughness\":{";
    if (hasImage) j << "\"baseColorTexture\":{\"index\":0},";
    else          j << "\"baseColorFactor\":[0.8,0.8,0.8,1],";
    j << "\"metallicFactor\":0,\"roughnessFactor\":1},"
      << "\"alphaMode\":\"" << alphaMode << "\"";
    if (mesh.isAlphaTest) j << ",\"alphaCutoff\":0.5";
    j << "}],";
    j << "\"meshes\":[{\"name\":\"" << name << "\",\"primitives\":[{"
      << "\"attributes\":{\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2";
    if (hasColors) j << ",\"COLOR_0\":" << colAccessor;
    j << "},\"indices\":3,\"material\":0}]}],";
    j << "\"nodes\":[{\"name\":\"" << name << "\",\"mesh\":0}],";
    j << "\"scenes\":[{\"nodes\":[0]}],\"scene\":0,";
    j << "\"buffers\":[{\"byteLength\":" << bin.size() << "}]}";

    std::string json = j.str();
    while (json.size() % 4) json += " ";

    std::ofstream out(path, std::ios::binary);
    uint32_t magic = 0x46546C67, version = 2;
    uint32_t totalLen = 12 + 8 + (uint32_t)json.size() + 8 + (uint32_t)bin.size();
    out.write((char*)&magic, 4); out.write((char*)&version, 4); out.write((char*)&totalLen, 4);
    uint32_t jLen = (uint32_t)json.size(), jType = 0x4E4F534A;
    out.write((char*)&jLen, 4); out.write((char*)&jType, 4); out.write(json.c_str(), jLen);
    uint32_t bLen = (uint32_t)bin.size(), bType = 0x004E4942;
    out.write((char*)&bLen, 4); out.write((char*)&bType, 4); out.write((char*)bin.data(), bLen);
    out.close();
}

// Exports the world as a scene: one GLB per *unique* mesh under world/glb/<category>/, the
// textures beside them as PNG, and world/nyc.json holding every placement.
//
// The world contains the same source geometry many times over (a railing appears thousands of
// times), so writing geometry per placement would be enormous and lossy. Meshes are deduplicated
// by source identity (pack + offset + section) and the JSON carries the transforms, which is what
// a scene importer actually needs.
//
// This loads the world itself rather than requiring the caller to have done so, saving and
// restoring whatever was previously in the viewport. The writing is stepped via WorldExportStep
// so the UI can show progress instead of freezing.
void SpiderManTool::ExtractAllWorldMeshes() {
    if (isExportingWorld) return;

    exportItems.clear();
    exportPlacements.clear();
    exportProgress = 0;
    exportTotal = 0;
    exportCurrentItem.clear();

    // Take over the viewport for the duration; restored when the export finishes.
    exportSavedMeshes = std::move(previewMeshes);
    previewMeshes.clear();
    LoadAllWorldGeometries();

    auto categoryFolder = [](WorldMeshKind k) -> const char* {
        switch (k) {
            case WorldMeshKind::Zone:
            case WorldMeshKind::SceneEntity:  return "zone chunks";
            case WorldMeshKind::Interior:     return "interior";
            case WorldMeshKind::Conglomerate: return "conglomerates";
            case WorldMeshKind::Lego:         return "lego";
            default:                          return nullptr;
        }
    };

    const fs::path worldDir = fs::current_path() / "world";
    const fs::path glbDir   = worldDir / "glb";
    fs::create_directories(glbDir);
    for (const char* sub : {"zone chunks", "interior", "conglomerates", "lego"})
        fs::create_directories(glbDir / sub);
    exportRootDir = worldDir.string();

    auto sanitize = [](std::string n) {
        for (char& c : n)
            if (c == '/' || c == '\\' || c == ':' || c == '*' ||
                c == '?' || c == '"'  || c == '<' || c == '>' || c == '|') c = '_';
        if (n.empty()) n = "mesh";
        return n;
    };

    // Identity of the geometry itself, not of where it happened to be loaded from.
    //
    // The same prop is loaded out of many packs and merged into several instance batches, so
    // keying on pack/offset produced one GLB per batch ("obj_cardboard x60" and
    // "obj_cardboard x474" as two files). Keying on the base name plus a geometry signature
    // collapses those into a single mesh whose instances are pooled from every batch.
    auto baseMeshName = [](const std::string& n) {
        // Batched meshes are named "<name> xN"; strip the count so batches of the same prop match.
        const size_t x = n.rfind(" x");
        if (x != std::string::npos && x + 2 < n.size()) {
            bool digits = true;
            for (size_t i = x + 2; i < n.size(); ++i)
                if (!std::isdigit((unsigned char)n[i])) { digits = false; break; }
            if (digits) return n.substr(0, x);
        }
        return n;
    };

    struct MeshKey {
        std::string name;      // base name, count suffix removed
        std::string texture;
        size_t vertexCount;
        size_t indexCount;
        bool operator<(const MeshKey& o) const {
            if (name != o.name) return name < o.name;
            if (texture != o.texture) return texture < o.texture;
            if (vertexCount != o.vertexCount) return vertexCount < o.vertexCount;
            return indexCount < o.indexCount;
        }
    };
    std::map<MeshKey, int> uniqueIds;
    std::map<std::string, int> nameUses;   // keeps filenames unique when names collide

    for (size_t mi = 0; mi < previewMeshes.size(); ++mi) {
        const auto& mesh = previewMeshes[mi];
        const char* folder = categoryFolder(mesh.worldKind);
        if (!folder) continue;
        if (mesh.positions.empty() || mesh.indices.empty()) continue;

        const std::string base = baseMeshName(mesh.meshName.empty() ? "mesh" : mesh.meshName);
        const MeshKey key{ base, StrToLower(mesh.textureName),
                           mesh.positions.size(), mesh.indices.size() };

        auto found = uniqueIds.find(key);
        if (found == uniqueIds.end()) {
            // Group zone chunks by the zone they belong to -- one folder per district pack
            // (BD, CE, AF...), models loose inside it.
            std::string subDir;
            if (std::string(folder) == "zone chunks") {
                std::string zone;
                if (!mesh.sourcePack.empty())
                    zone = StrToLower(fs::path(mesh.sourcePack).stem().string());
                subDir = sanitize(zone.empty() ? std::string("misc") : zone);
            }

            const fs::path destDir = subDir.empty() ? (glbDir / folder)
                                                    : (glbDir / folder / subDir);
            if (!subDir.empty()) fs::create_directories(destDir);

            // Uniquify per destination folder, so the same name in two PCMs keeps its own name.
            std::string fileBase = sanitize(base);
            const std::string useKey = subDir + "/" + fileBase;
            const int use = nameUses[useKey]++;
            if (use > 0) fileBase += "_" + std::to_string(use);

            const std::string rel = subDir.empty()
                ? (std::string("glb/") + folder + "/" + fileBase + ".glb")
                : (std::string("glb/") + folder + "/" + subDir + "/" + fileBase + ".glb");

            WorldExportItem item;
            item.meshIndex   = (int)mi;
            item.name        = fileBase;
            item.category    = folder;
            item.pcmName     = subDir;
            item.relPath     = rel;
            item.outFile     = (destDir / (fileBase + ".glb")).string();
            item.textureName = mesh.textureName;
            item.textureHash = mesh.textureHash;
            item.shaderName  = mesh.materialShaderName;

            found = uniqueIds.emplace(key, (int)exportItems.size()).first;
            exportItems.push_back(std::move(item));
        }
        const int meshId = found->second;

        // Every way a mesh can be positioned: per-instance, a single placement matrix, or origin.
        if (!mesh.instances.empty()) {
            for (size_t ii = 0; ii < mesh.instances.size(); ++ii) {
                const auto& inst = mesh.instances[ii];
                if (inst.isHidden) continue;
                exportPlacements.push_back({ meshId,
                    inst.name.empty() ? (exportItems[meshId].name + "_" + std::to_string(ii))
                                      : inst.name,
                    inst.transform });
            }
        } else if (mesh.hasPlacementTransform) {
            exportPlacements.push_back({ meshId,
                mesh.placementName.empty() ? exportItems[meshId].name : mesh.placementName,
                mesh.placementTransform });
        } else {
            std::array<float, 16> identity{};
            identity[0] = identity[5] = identity[10] = identity[15] = 1.0f;
            exportPlacements.push_back({ meshId, exportItems[meshId].name, identity });
        }
    }

    if (exportItems.empty()) {
        previewMeshes = std::move(exportSavedMeshes);
        exportSavedMeshes.clear();
        ShowNotification("No categorised world meshes found to export.");
        return;
    }

    exportTotal = (int)exportItems.size();
    isExportingWorld = true;
}

// Writes a slice of the pending export. Pumped once per frame from the UI so the progress bar
// updates; when the last item lands it emits nyc.json and hands the viewport back.
void SpiderManTool::WorldExportStep(int itemsThisFrame) {
    if (!isExportingWorld) return;

    const fs::path worldDir(exportRootDir);

    for (int n = 0; n < itemsThisFrame && exportProgress < exportTotal; ++n, ++exportProgress) {
        const auto& item = exportItems[exportProgress];
        if (item.meshIndex < 0 || item.meshIndex >= (int)previewMeshes.size()) continue;
        const auto& mesh = previewMeshes[item.meshIndex];

        exportCurrentItem = item.name;

        // Resolve the texture first so it can be embedded in the GLB. It is also written beside
        // the model as a loose PNG, which keeps the files usable outside a glTF importer.
        std::vector<uint8_t> pngBytes;
        if (!item.textureName.empty()) {
            const fs::path pngPath =
                fs::path(item.outFile).parent_path() / (item.textureName + ".png");

            if (!fs::exists(pngPath)) {
                std::vector<uint8_t> dds;
                auto pull = [&](const auto& index, auto lookup) {
                    auto it = index.find(lookup);
                    if (it == index.end()) return false;
                    std::ifstream tf(it->second.packPath, std::ios::binary);
                    if (!tf.is_open()) return false;
                    tf.seekg(it->second.offset);
                    dds.resize(it->second.size);
                    tf.read((char*)dds.data(), it->second.size);
                    return true;
                };
                if (!pull(globalTextureNameIndex, StrToLower(item.textureName)) &&
                    item.textureHash != 0)
                    pull(globalTextureIndex, item.textureHash);

                if (!dds.empty()) ConvertDDStoPNG(dds, pngPath);
            }

            // Read back whatever ended up on disk (freshly written or from an earlier mesh that
            // shares this texture) and embed those exact bytes.
            std::ifstream pf(pngPath, std::ios::binary | std::ios::ate);
            if (pf.is_open()) {
                const std::streamsize sz = pf.tellg();
                if (sz > 0) {
                    pngBytes.resize((size_t)sz);
                    pf.seekg(0);
                    pf.read((char*)pngBytes.data(), sz);
                }
            }
        }

        WriteGLB(fs::path(item.outFile), mesh, pngBytes);
    }

    if (exportProgress < exportTotal) return;

    auto jsonEscape = [](const std::string& v) {
        std::string o;
        for (char c : v) {
            if (c == '"' || c == '\\') { o += '\\'; o += c; }
            else if ((unsigned char)c < 0x20) continue;
            else o += c;
        }
        return o;
    };

    std::ofstream js(worldDir / "nyc.json");
    if (js.is_open()) {
        js << "{\n";
        js << "  \"name\": \"nyc\",\n";
        js << "  \"generator\": \"Ultimate-Spider-Browser\",\n";
        js << "  \"transform_order\": \"column-major 4x4, glTF convention\",\n";

        js << "  \"meshes\": [\n";
        for (size_t i = 0; i < exportItems.size(); ++i) {
            const auto& it = exportItems[i];
            js << "    {\"id\": " << i
               << ", \"name\": \"" << jsonEscape(it.name) << "\""
               << ", \"category\": \"" << jsonEscape(it.category) << "\""
               << ", \"file\": \"" << jsonEscape(it.relPath) << "\""
               << ", \"texture\": \"" << jsonEscape(it.textureName) << "\""
               << ", \"shader\": \"" << jsonEscape(it.shaderName) << "\""
               << ", \"pcm\": \"" << jsonEscape(it.pcmName) << "\"}"
               << (i + 1 < exportItems.size() ? "," : "") << "\n";
        }
        js << "  ],\n";

        js << "  \"instances\": [\n";
        for (size_t i = 0; i < exportPlacements.size(); ++i) {
            const auto& p = exportPlacements[i];
            js << "    {\"mesh\": " << p.meshId
               << ", \"name\": \"" << jsonEscape(p.name) << "\""
               << ", \"transform\": [";
            for (int k = 0; k < 16; ++k) { js << p.transform[k]; if (k < 15) js << ", "; }
            js << "]}" << (i + 1 < exportPlacements.size() ? "," : "") << "\n";
        }
        js << "  ]\n";
        js << "}\n";
        js.close();
    }

    const size_t uniqueCount = exportItems.size();
    const size_t placeCount  = exportPlacements.size();

    for (auto& m : previewMeshes) {
        if (m.vao) glDeleteVertexArrays(1, &m.vao);
        if (m.vbo) glDeleteBuffers(1, &m.vbo);
        if (m.ebo) glDeleteBuffers(1, &m.ebo);
        if (m.instanceVbo) glDeleteBuffers(1, &m.instanceVbo);
    }
    previewMeshes = std::move(exportSavedMeshes);
    exportSavedMeshes.clear();
    exportItems.clear();
    exportPlacements.clear();
    isExportingWorld = false;
    exportCurrentItem.clear();

    ShowNotification("World exported\n" +
        std::to_string(uniqueCount) + " unique meshes, " +
        std::to_string(placeCount) + " placements\n" +
        worldDir.string());
}
