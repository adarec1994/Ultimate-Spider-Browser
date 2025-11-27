#include "SpiderManTool.h"
#define GL_GLEXT_PROTOTYPES
#include <GLFW/glfw3.h>

// DDS Structs
struct DDS_PIXELFORMAT {
    uint32_t dwSize;
    uint32_t dwFlags;
    uint32_t dwFourCC;
    uint32_t dwRGBBitCount;
    uint32_t dwRBitMask;
    uint32_t dwGBitMask;
    uint32_t dwBBitMask;
    uint32_t dwABitMask;
};

struct DDS_HEADER {
    uint32_t dwSize;
    uint32_t dwFlags;
    uint32_t dwHeight;
    uint32_t dwWidth;
    uint32_t dwPitchOrLinearSize;
    uint32_t dwDepth;
    uint32_t dwMipMapCount;
    uint32_t dwReserved1[11];
    DDS_PIXELFORMAT ddspf;
    uint32_t dwCaps;
    uint32_t dwCaps2;
    uint32_t dwCaps3;
    uint32_t dwCaps4;
    uint32_t dwReserved2;
};

#ifndef GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT 0x83F1
#endif
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT3_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT3_EXT 0x83F2
#endif
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT5_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
#endif

void GLBWriter::AddMeshNode(const std::string& name, int meshIndex) {
    if (nodeCount > 0) nodesJson << ",";
    nodesJson << "{\"name\":\"" << name << "\",\"mesh\":" << meshIndex << "}";
    rootNodes.push_back(nodeCount++);
}

void GLBWriter::AddBoneNode(const std::string& name, const float* matrix) {
    if (nodeCount > 0) nodesJson << ",";
    nodesJson << "{\"name\":\"" << name << "\",\"matrix\":[";
    for(int i=0; i<16; i++) nodesJson << matrix[i] << (i<15?",":"");
    nodesJson << "]}";
    rootNodes.push_back(nodeCount++);
}

int GLBWriter::AddBufferView(const void* data, size_t size, int target) {
    AlignBuffer();
    int offset = (int)buffer.size();
    const uint8_t* ptr = (const uint8_t*)data;
    buffer.insert(buffer.end(), ptr, ptr + size);

    GLBBufferView bv;
    bv.buffer = 0;
    bv.byteOffset = offset;
    bv.byteLength = (int)size;
    bv.target = target;
    bufferViews.push_back(bv);
    return (int)bufferViews.size() - 1;
}

int GLBWriter::AddAccessor(int bufferView, int componentType, int count, const char* type, float* minVal, float* maxVal) {
    GLBAccessor acc;
    acc.bufferView = bufferView;
    acc.componentType = componentType;
    acc.count = count;
    acc.type = type;
    if (minVal) acc.min = { minVal[0], minVal[1], minVal[2] };
    if (maxVal) acc.max = { maxVal[0], maxVal[1], maxVal[2] };
    accessors.push_back(acc);
    return (int)accessors.size() - 1;
}

int GLBWriter::StartMesh(const std::string& name) {
    if (meshCount > 0) meshesJson << ",";
    meshesJson << "{\"name\":\"" << name << "\",\"primitives\":[";
    return meshCount++;
}

void GLBWriter::EndMesh() {
    meshesJson << "]}";
}

void GLBWriter::AddPrimitive(int posAcc, int normAcc, int uvAcc, int indAcc, int jointAcc, int weightAcc) {
    meshesJson << "{\"attributes\":{";
    meshesJson << "\"POSITION\":" << posAcc;
    if (normAcc >= 0) meshesJson << ",\"NORMAL\":" << normAcc;
    if (uvAcc >= 0) meshesJson << ",\"TEXCOORD_0\":" << uvAcc;
    if (jointAcc >= 0) meshesJson << ",\"JOINTS_0\":" << jointAcc;
    if (weightAcc >= 0) meshesJson << ",\"WEIGHTS_0\":" << weightAcc;
    meshesJson << "},\"indices\":" << indAcc << "}";
}

void GLBWriter::WriteToFile(const std::string& path) {
    AlignBuffer();
    std::stringstream json;
    json << "{\"asset\":{\"version\":\"2.0\"},";
    json << "\"scene\":0,\"scenes\":[{\"nodes\":[";
    for (size_t i = 0; i < rootNodes.size(); i++) json << rootNodes[i] << (i < rootNodes.size() - 1 ? "," : "");
    json << "]}],";
    json << "\"nodes\":[" << nodesJson.str() << "],";
    json << "\"meshes\":[" << meshesJson.str() << "],";
    json << "\"accessors\":[";
    for (size_t i = 0; i < accessors.size(); i++) {
        auto& acc = accessors[i];
        json << "{\"bufferView\":" << acc.bufferView << ",\"componentType\":" << acc.componentType
             << ",\"count\":" << acc.count << ",\"type\":\"" << acc.type << "\"";
        if (!acc.min.empty()) json << ",\"min\":[" << acc.min[0] << "," << acc.min[1] << "," << acc.min[2] << "],\"max\":[" << acc.max[0] << "," << acc.max[1] << "," << acc.max[2] << "]";
        json << "}" << (i < accessors.size() - 1 ? "," : "");
    }
    json << "],";
    json << "\"bufferViews\":[";
    for (size_t i = 0; i < bufferViews.size(); i++) {
        auto& bv = bufferViews[i];
        json << "{\"buffer\":" << bv.buffer << ",\"byteOffset\":" << bv.byteOffset
             << ",\"byteLength\":" << bv.byteLength << ",\"target\":" << bv.target << "}"
             << (i < bufferViews.size() - 1 ? "," : "");
    }
    json << "],";
    json << "\"buffers\":[{\"byteLength\":" << buffer.size() << "}]}";

    std::string jsonStr = json.str();
    while (jsonStr.size() % 4 != 0) jsonStr += " ";
    uint32_t totalLen = 12 + 8 + (uint32_t)jsonStr.size() + 8 + (uint32_t)buffer.size();

    std::ofstream out(path, std::ios::binary);
    uint32_t magic = 0x46546C67;
    uint32_t version = 2;
    out.write((char*)&magic, 4);
    out.write((char*)&version, 4);
    out.write((char*)&totalLen, 4);
    uint32_t chunkLen = (uint32_t)jsonStr.size();
    uint32_t chunkType = 0x4E4F534A;
    out.write((char*)&chunkLen, 4);
    out.write((char*)&chunkType, 4);
    out.write(jsonStr.c_str(), chunkLen);
    chunkLen = (uint32_t)buffer.size();
    chunkType = 0x004E4942;
    out.write((char*)&chunkLen, 4);
    out.write((char*)&chunkType, 4);
    if (chunkLen > 0) out.write((char*)buffer.data(), chunkLen);
    out.close();
}

void SpiderManTool::Log(const std::string& msg) {
    logBuffer += msg + "\n";
    std::cout << msg << std::endl;
}

void SpiderManTool::SaveConfig() {
    std::ofstream f("usm_config.txt");
    if (f.is_open()) {
        f << searchPath;
        f.close();
    }
}

void SpiderManTool::LoadConfig() {
    std::ifstream f("usm_config.txt");
    if (f.is_open()) {
        std::string line;
        if (std::getline(f, line) && !line.empty()) {
            if (fs::exists(line)) {
                searchPath = line;
            }
        }
        f.close();
    }
}

void SpiderManTool::ScanDirectory() {
    foundPacks.clear();
    Log("Scanning " + searchPath + "...");
    try {
        if (!fs::exists(searchPath)) {
            Log("Path does not exist!");
            return;
        }

        SaveConfig();

        for (auto& p : fs::recursive_directory_iterator(searchPath)) {
            auto ext = p.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext == ".pcpack") {
                foundPacks.push_back(p.path());
            }
        }
        Log("Found " + std::to_string(foundPacks.size()) + " .pcpack files.");
        if (!foundPacks.empty()) currentState = STATE_BROWSER;
    } catch (const std::exception& e) {
        Log(std::string("Error scanning: ") + e.what());
    }
}

void SpiderManTool::LoadDictionary(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) return;
    std::string line;
    std::getline(file, line); std::getline(file, line);
    while (std::getline(file, line)) {
        std::stringstream ss(line);
        std::string hashStr, name;
        ss >> hashStr;
        std::getline(ss, name);
        size_t first = name.find_first_not_of(" \t");
        if (first != std::string::npos) name = name.substr(first);
        try { dictionary[std::stoul(hashStr, nullptr, 16)] = name; } catch (...) {}
    }
    Log("Loaded dictionary.");
}

void SpiderManTool::OpenPCPack(const std::string& path) {
    if (loadedPCPackPath == path) return;

    ClosePreview(); // Close existing preview
    selectedFileIndex = -1; // Reset selection

    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) { Log("Failed to open " + path); return; }

    size_t size = file.tellg();
    file.seekg(0);
    pcPackData.resize(size);
    file.read((char*)pcPackData.data(), size);
    loadedPCPackPath = path;

    BinaryReader br(pcPackData);
    br.Skip(24);
    uint32_t headerSize = br.Read<uint32_t>();
    dataOffset = br.Read<uint32_t>();

    size_t start = 0;
    bool found = false;
    const uint32_t magic = 0xE3E3E3E3;
    for(size_t i=0; i<size-4; i++) {
        if (*(uint32_t*)&pcPackData[i] == magic) {
            for(size_t j=i+4; j<size-4; j++) {
                if (*(uint32_t*)&pcPackData[j] == magic) {
                    start = j + 4;
                    found = true;
                    break;
                }
            }
            break;
        }
    }

    entries.clear();
    if (!found) { Log("Invalid PCPACK header."); return; }

    // Map to track duplicate names: Name -> Count
    std::map<std::string, int> nameCounts;

    br.Seek(start);
    int counter = 0;
    while (true) {
        uint32_t hash = br.Read<uint32_t>();
        uint32_t type = br.Read<uint32_t>();
        uint32_t offset = br.Read<uint32_t>();
        uint32_t fsize = br.Read<uint32_t>();

        if (type >= 0x1000 || type == 0x0000) break;

        FileEntry e;
        e.hash = hash; e.type = type; e.offset = offset + dataOffset; e.size = fsize;
        e.isPcm = false;
        e.isDds = false;

        if (dictionary.count(hash)) e.name = dictionary[hash];
        else { std::stringstream ss; ss << "Unknown_" << std::hex << hash; e.name = ss.str(); }

        if (fsize > 4) {
            const char* magicSig = (const char*)&pcPackData[e.offset];
            if (strncmp(magicSig, "PCM ", 4) == 0) {
                e.name += ".pcm";
                e.isPcm = true;
            }
            else if (strncmp(magicSig, "DDS ", 4) == 0) {
                e.name += ".dds";
                e.isDds = true;
            }
            else e.name += ".dat";
        }

        // --- Handle Duplicates ---
        std::string originalName = e.name;
        if (nameCounts.count(originalName)) {
            int idx = nameCounts[originalName];

            // Insert suffix before extension
            fs::path p(originalName);
            std::string stem = p.stem().string();
            std::string ext = p.extension().string();

            e.name = stem + "_" + std::to_string(idx) + ext;
            nameCounts[originalName]++;
        } else {
            nameCounts[originalName] = 0;
        }
        // -------------------------

        entries.push_back(e);
        br.Seek(start + (counter + 1) * 16);
        counter++;
    }
    Log("Opened " + fs::path(path).filename().string());
}

void SpiderManTool::ExtractPack(const std::string& packPath, bool convertAll) {
    OpenPCPack(packPath);
    if (entries.empty()) return;

    fs::path p(packPath);
    std::string folderName = p.stem().string() + "_extracted";
    fs::path outDir = p.parent_path() / folderName;
    fs::create_directories(outDir);

    Log("Extracting to: " + outDir.string());

    for(auto& e : entries) {
        std::string outFilePath = (outDir / e.name).string();
        std::ofstream out(outFilePath, std::ios::binary);
        out.write((char*)&pcPackData[e.offset], e.size);
        out.close();

        if (e.isPcm && convertAll) {
            std::vector<uint8_t> pcmData(pcPackData.begin() + e.offset, pcPackData.begin() + e.offset + e.size);
            ConvertPCM(pcmData, outFilePath + ".glb");
        }
    }
    Log("Extraction complete.");
}

void SpiderManTool::ExtractFile(int index) {
    if (index < 0 || index >= entries.size()) return;
    if (pcPackData.empty()) return;

    const auto& e = entries[index];
    fs::path p(loadedPCPackPath);
    std::string folderName = p.stem().string() + "_extracted";
    fs::path outDir = p.parent_path() / folderName;
    fs::create_directories(outDir);

    std::string outFilePath = (outDir / e.name).string();
    std::ofstream out(outFilePath, std::ios::binary);
    out.write((char*)&pcPackData[e.offset], e.size);
    out.close();

    if (e.isPcm) {
        std::vector<uint8_t> pcmData(pcPackData.begin() + e.offset, pcPackData.begin() + e.offset + e.size);
        ConvertPCM(pcmData, outFilePath + ".glb");
        Log("Extracted & Converted: " + e.name);
    } else {
        Log("Extracted: " + e.name);
    }
}

void SpiderManTool::LoadPreview(int index) {
    if (index < 0 || index >= entries.size()) return;
    if (pcPackData.empty()) return;

    const auto& e = entries[index];
    if (!e.isDds) {
        Log("Not a DDS file.");
        return;
    }

    if (previewTextureId != 0) ClosePreview();

    const uint8_t* data = &pcPackData[e.offset];
    if (e.size < sizeof(DDS_HEADER) + 4) return;

    uint32_t magic = *(uint32_t*)data;
    if (magic != 0x20534444) return; // "DDS "

    const DDS_HEADER* header = (const DDS_HEADER*)(data + 4);
    previewWidth = header->dwWidth;
    previewHeight = header->dwHeight;

    uint32_t fourCC = header->ddspf.dwFourCC;
    GLenum format = 0;

    // DXT1 = "DXT1", DXT3 = "DXT3", DXT5 = "DXT5"
    if (fourCC == 0x31545844) format = GL_COMPRESSED_RGBA_S3TC_DXT1_EXT;
    else if (fourCC == 0x33545844) format = GL_COMPRESSED_RGBA_S3TC_DXT3_EXT;
    else if (fourCC == 0x35545844) format = GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;

    if (format == 0) {
        Log("Unsupported DXT format.");
        return;
    }

    uint32_t blockSize = (format == GL_COMPRESSED_RGBA_S3TC_DXT1_EXT) ? 8 : 16;
    uint32_t imageSize = ((previewWidth + 3) / 4) * ((previewHeight + 3) / 4) * blockSize;

    if (e.size < 128 + imageSize) {
        Log("DDS data too small.");
        return;
    }

    glGenTextures(1, &previewTextureId);
    glBindTexture(GL_TEXTURE_2D, previewTextureId);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glCompressedTexImage2D(GL_TEXTURE_2D, 0, format, previewWidth, previewHeight, 0, imageSize, data + 128);
    glBindTexture(GL_TEXTURE_2D, 0);

    showPreview = true;
    Log("Preview loaded: " + e.name);
}

void SpiderManTool::ClosePreview() {
    if (previewTextureId != 0) {
        glDeleteTextures(1, &previewTextureId);
        previewTextureId = 0;
    }
    showPreview = false;
}

void SpiderManTool::ConvertPCM(const std::vector<uint8_t>& pcmData, const std::string& outPath) {
    BinaryReader br(pcmData);
    br.Seek(8);
    uint32_t num = br.Read<uint32_t>();
    uint32_t ofs = br.Read<uint32_t>();
    br.Seek(ofs);
    struct Info { uint16_t u1, type; uint32_t offset, u2; };
    std::vector<Info> infos;
    for(uint32_t i=0; i<num; i++) {
        Info inf; inf.u1 = br.Read<uint16_t>(); inf.type = br.Read<uint16_t>(); inf.offset = br.Read<uint32_t>(); inf.u2 = br.Read<uint32_t>(); infos.push_back(inf);
    }

    GLBWriter glb;
    for(auto& inf : infos) {
        if (inf.type != 512) continue;
        br.Seek(inf.offset);
        uint32_t nameOfs = br.Read<uint32_t>();
        br.Skip(4); uint32_t numSm = br.Read<uint32_t>(); uint32_t infSmOfs = br.Read<uint32_t>(); uint32_t numBn = br.Read<uint32_t>(); uint32_t ofsBn = br.Read<uint32_t>();
        br.Skip(24);
        br.Seek(ofsBn);
        for(uint32_t b=0; b<numBn; b++) {
            std::vector<float> mat(16);
            for(int m=0; m<16; m++) mat[m] = br.Read<float>();
            std::stringstream bnName; bnName << "bone_" << b;
            glb.AddBoneNode(bnName.str(), mat.data());
        }
        br.Seek(infSmOfs);
        std::vector<uint32_t> smOffsets;
        for(uint32_t s=0; s<numSm; s++) { br.Skip(4); smOffsets.push_back(br.Read<uint32_t>()); }
        for(uint32_t smOfs : smOffsets) {
            br.Seek(smOfs);
            uint32_t smNameOfs = br.Read<uint32_t>();
            br.Skip(28); uint32_t itype = br.Read<uint32_t>(); uint32_t inum = br.Read<uint32_t>(); uint32_t iofs = br.Read<uint32_t>();
            br.Skip(4); uint32_t vnum = br.Read<uint32_t>(); uint32_t vofs = br.Read<uint32_t>();
            br.Skip(8); uint32_t stride = br.Read<uint32_t>();
            std::string smName = "mesh";
            br.Seek(vofs);
            std::vector<float> pos, norm, uvs, weights; std::vector<uint16_t> joints;
            for(uint32_t v=0; v<vnum; v++) {
                size_t startV = br.Tell();
                pos.push_back(br.Read<float>()); pos.push_back(br.Read<float>()); pos.push_back(br.Read<float>());
                if (stride == 64) {
                    br.Seek(startV + 12); norm.push_back(br.Read<float>()); norm.push_back(br.Read<float>()); norm.push_back(br.Read<float>());
                    br.Seek(startV + 32); joints.push_back((uint16_t)br.Read<float>()); joints.push_back((uint16_t)br.Read<float>()); joints.push_back((uint16_t)br.Read<float>()); joints.push_back((uint16_t)br.Read<float>());
                    br.Seek(startV + 48); weights.push_back(br.Read<float>()); weights.push_back(br.Read<float>()); weights.push_back(br.Read<float>()); weights.push_back(br.Read<float>());
                } else if (stride == 24) {
                    br.Seek(startV + 12); uvs.push_back(br.Read<float>()); uvs.push_back(1.0f - br.Read<float>());
                }
                br.Seek(startV + stride);
            }
            br.Seek(iofs);
            std::vector<uint16_t> indices;
            std::vector<uint16_t> rawIndices;
            for(uint32_t i=0; i<inum; i++) rawIndices.push_back(br.Read<uint16_t>());
            if (itype != 4) {
                 for (size_t k = 0; k < rawIndices.size() - 2; k++) {
                    uint16_t v1 = rawIndices[k], v2 = rawIndices[k+1], v3 = rawIndices[k+2];
                    if (v1==v2||v2==v3||v1==v3) continue;
                    if (k%2==0) { indices.push_back(v1); indices.push_back(v2); indices.push_back(v3); }
                    else { indices.push_back(v1); indices.push_back(v3); indices.push_back(v2); }
                }
            } else indices = rawIndices;

            float minP[3]={1e9,1e9,1e9}, maxP[3]={-1e9,-1e9,-1e9};
            for(size_t i=0; i<pos.size(); i+=3) for(int k=0;k<3;k++) { if(pos[i+k]<minP[k]) minP[k]=pos[i+k]; if(pos[i+k]>maxP[k]) maxP[k]=pos[i+k]; }
            int posAcc = glb.AddAccessor(glb.AddBufferView(pos.data(), pos.size()*4, 34962), 5126, vnum, "VEC3", minP, maxP);
            int indAcc = glb.AddAccessor(glb.AddBufferView(indices.data(), indices.size()*2, 34963), 5123, (int)indices.size(), "SCALAR");
            int normAcc = -1, uvAcc = -1, jointAcc = -1, weightAcc = -1;
            if(!norm.empty()) normAcc = glb.AddAccessor(glb.AddBufferView(norm.data(), norm.size()*4, 34962), 5126, vnum, "VEC3");
            if(!uvs.empty()) uvAcc = glb.AddAccessor(glb.AddBufferView(uvs.data(), uvs.size()*4, 34962), 5126, vnum, "VEC2");
            if(!joints.empty()) {
                jointAcc = glb.AddAccessor(glb.AddBufferView(joints.data(), joints.size()*2, 34962), 5123, vnum, "VEC4");
                weightAcc = glb.AddAccessor(glb.AddBufferView(weights.data(), weights.size()*4, 34962), 5126, vnum, "VEC4");
            }
            int meshIdx = glb.StartMesh(smName);
            glb.AddPrimitive(posAcc, normAcc, uvAcc, indAcc, jointAcc, weightAcc);
            glb.EndMesh();
            glb.AddMeshNode(smName, meshIdx);
        }
    }
    glb.WriteToFile(outPath);
    Log("Converted: " + fs::path(outPath).filename().string());
}