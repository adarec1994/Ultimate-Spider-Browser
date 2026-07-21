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

                        RecordWorldMeshPlacementDebug("unique pcms", "sky_day",
                                                      path.string(), absOffset,
                                                      transformMatrix);
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

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  LoadPackEntities â€” load all placed props from a single PCPACK
//  Called from LoadPreview when viewing a zone mesh in world mode.
//  Handles: named entity instances (0x0A block) + orphan PCM placement records

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

static void WriteGLB(const fs::path& path, const RenderMesh& mesh) {
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

    std::string name = mesh.meshName;
    for (char& c : name) if (c == '"' || c == '\\') c = '_';

    std::stringstream j;
    j << "{\"asset\":{\"version\":\"2.0\",\"generator\":\"USM\"},";
    j << "\"bufferViews\":[";
    j << "{\"buffer\":0,\"byteOffset\":" << posOff << ",\"byteLength\":" << posLen << ",\"target\":34962},";
    j << "{\"buffer\":0,\"byteOffset\":" << nrmOff << ",\"byteLength\":" << nrmLen << ",\"target\":34962},";
    j << "{\"buffer\":0,\"byteOffset\":" << uvOff << ",\"byteLength\":" << uvLen << ",\"target\":34962},";
    j << "{\"buffer\":0,\"byteOffset\":" << idxOff << ",\"byteLength\":" << idxLen << ",\"target\":34963}],";
    j << "\"accessors\":[";
    j << "{\"bufferView\":0,\"componentType\":5126,\"count\":" << vertexCount << ",\"type\":\"VEC3\","
      << "\"min\":[" << minP[0] << "," << minP[1] << "," << minP[2] << "],"
      << "\"max\":[" << maxP[0] << "," << maxP[1] << "," << maxP[2] << "]},";
    j << "{\"bufferView\":1,\"componentType\":5126,\"count\":" << vertexCount << ",\"type\":\"VEC3\"},";
    j << "{\"bufferView\":2,\"componentType\":5126,\"count\":" << vertexCount << ",\"type\":\"VEC2\"},";
    j << "{\"bufferView\":3,\"componentType\":5123,\"count\":" << triangleIndices.size() << ",\"type\":\"SCALAR\"}],";
    const char* alphaMode = mesh.isAlphaTest ? "MASK" : (mesh.isTranslucent ? "BLEND" : "OPAQUE");
    j << "\"materials\":[{\"name\":\"" << name << "\",\"doubleSided\":true,"
      << "\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.8,0.8,0.8,1],\"metallicFactor\":0,\"roughnessFactor\":1},"
      << "\"alphaMode\":\"" << alphaMode << "\"";
    if (mesh.isAlphaTest) j << ",\"alphaCutoff\":0.5";
    j << "}],";
    j << "\"meshes\":[{\"name\":\"" << name << "\",\"primitives\":[{"
      << "\"attributes\":{\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2},"
      << "\"indices\":3,\"material\":0}]}],";
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

void SpiderManTool::ExtractAllWorldMeshes() {
    if (foundPacks.empty()) return;

    fs::path levelsDir = fs::current_path() / "extracted" / "Levels";
    fs::path propsDir = fs::current_path() / "extracted" / "Props";
    fs::create_directories(levelsDir);
    fs::create_directories(propsDir);

    auto savedMeshes = std::move(previewMeshes);
    previewMeshes.clear();

    std::set<uint32_t> exported;
    int levelCount = 0, propCount = 0;

    for (const auto& packPath : foundPacks) {
        std::string stem = packPath.stem().string();
        std::string stemLower = StrToLower(stem);
        bool isCityArena = (stemLower == "city_arena");
        if (stem.length() != 2 && !isCityArena) continue;

        std::ifstream file(packPath, std::ios::binary);
        if (!file.is_open()) continue;

        file.seekg(0, std::ios::end);
        size_t fileSize = file.tellg();
        if (fileSize < 64) { file.close(); continue; }

        file.seekg(24);
        uint32_t headerSize, dataOffset;
        file.read((char*)&headerSize, 4);
        file.read((char*)&dataOffset, 4);

        size_t hdrRead = std::min((size_t)dataOffset, std::min(fileSize, (size_t)0x100000));
        std::vector<uint8_t> hdr(hdrRead);
        file.seekg(0);
        file.read((char*)hdr.data(), hdrRead);

        size_t tocStart = FindTocStart(hdr, hdrRead);
        if (tocStart == 0) { file.close(); continue; }

        struct TocEntry { uint32_t hash, type, absOffset, size; };
        std::vector<TocEntry> toc;
        size_t pos = tocStart;
        while (pos + 16 <= hdrRead) {
            uint32_t h, t, o, s;
            memcpy(&h, &hdr[pos], 4); memcpy(&t, &hdr[pos+4], 4);
            memcpy(&o, &hdr[pos+8], 4); memcpy(&s, &hdr[pos+12], 4);
            if (t >= 0x1000 || t == 0) break;
            toc.push_back({h, t, dataOffset + o, s});
            pos += 16;
        }

        uint32_t zoneBaseHash = 0;
        if (!isCityArena) {
            std::string zoneBaseName = stemLower + "c";
            zoneBaseHash = HashString33(zoneBaseName);
            uint32_t largestHash = 0, largestSize = 0;
            for (auto& te : toc) {
                if (te.type == 0x15 && te.size > largestSize) {
                    largestSize = te.size; largestHash = te.hash;
                }
            }
            if (largestHash != 0 && largestHash != zoneBaseHash) zoneBaseHash = largestHash;
        }

        for (auto& te : toc) {
            if (te.type != 0x15) continue;
            if (te.size < 64) continue;
            if (exported.count(te.hash)) continue;
            exported.insert(te.hash);

            std::string meshName = dictionary.count(te.hash) ? dictionary[te.hash] : "";
            if (meshName.empty()) {
                char buf[32]; snprintf(buf, 32, "0x%08X", te.hash);
                meshName = buf;
            }

            bool isLevel = (!isCityArena && te.hash == zoneBaseHash);
            fs::path destDir = isLevel ? levelsDir : propsDir;

            if (te.absOffset + te.size > fileSize) continue;
            std::vector<uint8_t> pcmData(te.size);
            file.seekg(te.absOffset);
            file.read((char*)pcmData.data(), te.size);

            previewMeshes.clear();
            AddMeshFromData(pcmData, meshName, nullptr, packPath.string(), te.absOffset);

            for (size_t mi = 0; mi < previewMeshes.size(); mi++) {
                auto& m = previewMeshes[mi];
                if (m.positions.empty() || m.indices.empty()) continue;

                std::string baseName = meshName;
                if (previewMeshes.size() > 1) baseName += "_" + std::to_string(mi);
                for (char& c : baseName) if (c=='/'||c=='\\'||c==':'||c=='*'||c=='?'||c=='"'||c=='<'||c=='>'||c=='|') c = '_';

                WriteGLB(destDir / (baseName + ".glb"), m);

                if (!m.textureName.empty()) {
                    std::string texLower = StrToLower(m.textureName);
                    std::vector<uint8_t> texData;
                    bool hasTex = false;
                    if (globalTextureNameIndex.count(texLower)) {
                        auto& loc = globalTextureNameIndex[texLower];
                        std::ifstream tf(loc.packPath, std::ios::binary);
                        if (tf.is_open()) {
                            tf.seekg(loc.offset); texData.resize(loc.size);
                            tf.read((char*)texData.data(), loc.size); tf.close();
                            hasTex = true;
                        }
                    }
                    if (!hasTex && m.textureHash != 0 && globalTextureIndex.count(m.textureHash)) {
                        auto& loc = globalTextureIndex[m.textureHash];
                        std::ifstream tf(loc.packPath, std::ios::binary);
                        if (tf.is_open()) {
                            tf.seekg(loc.offset); texData.resize(loc.size);
                            tf.read((char*)texData.data(), loc.size); tf.close();
                            hasTex = true;
                        }
                    }
                    if (hasTex) {
                        std::string texBaseName = m.textureName;
                        for (char& c : texBaseName) if (c=='/'||c=='\\'||c==':'||c=='*'||c=='?'||c=='"'||c=='<'||c=='>'||c=='|') c = '_';
                        fs::path ddsPath = destDir / (texBaseName + ".dds");
                        fs::path pngPath = destDir / (texBaseName + ".png");
                        if (!fs::exists(ddsPath)) {
                            std::ofstream dout(ddsPath, std::ios::binary);
                            dout.write((char*)texData.data(), texData.size());
                            dout.close();
                        }
                        if (!fs::exists(pngPath)) {
                            ConvertDDStoPNG(texData, pngPath);
                        }
                    }
                }
            }

            for (auto& m : previewMeshes) {
                if (m.vao) glDeleteVertexArrays(1, &m.vao);
                if (m.vbo) glDeleteBuffers(1, &m.vbo);
                if (m.ebo) glDeleteBuffers(1, &m.ebo);
                if (m.instanceVbo) glDeleteBuffers(1, &m.instanceVbo);
            }
            previewMeshes.clear();
            if (isLevel) levelCount++; else propCount++;
        }

        file.close();
    }

    previewMeshes = std::move(savedMeshes);
    ShowNotification("Extracted " + std::to_string(levelCount) + " levels, " + std::to_string(propCount) + " props\n"
        + levelsDir.string() + "\n" + propsDir.string());
}
