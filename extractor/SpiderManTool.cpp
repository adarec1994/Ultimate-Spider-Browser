#include "SpiderManTool.h"
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <sstream>

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

// --- Helper: Matrix Inversion for 4x4 ---
// GLTF requires Inverse Bind Matrices, but file likely provides Bind Pose.
bool InvertMatrix(const float m[16], float invOut[16]) {
    float inv[16], det;
    int i;

    inv[0] = m[5]  * m[10] * m[15] - m[5]  * m[11] * m[14] - m[9]  * m[6]  * m[15] + m[9]  * m[7]  * m[14] + m[13] * m[6]  * m[11] - m[13] * m[7]  * m[10];
    inv[4] = -m[4]  * m[10] * m[15] + m[4]  * m[11] * m[14] + m[8]  * m[6]  * m[15] - m[8]  * m[7]  * m[14] - m[12] * m[6]  * m[11] + m[12] * m[7]  * m[10];
    inv[8] = m[4]  * m[9] * m[15] - m[4]  * m[11] * m[13] - m[8]  * m[5] * m[15] + m[8]  * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4]  * m[9] * m[14] + m[4]  * m[10] * m[13] + m[8]  * m[5] * m[14] - m[8]  * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1]  * m[10] * m[15] + m[1]  * m[11] * m[14] + m[9]  * m[2] * m[15] - m[9]  * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0]  * m[10] * m[15] - m[0]  * m[11] * m[14] - m[8]  * m[2] * m[15] + m[8]  * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0]  * m[9] * m[15] + m[0]  * m[11] * m[13] + m[8]  * m[1] * m[15] - m[8]  * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0]  * m[9] * m[14] - m[0]  * m[10] * m[13] - m[8]  * m[1] * m[14] + m[8]  * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1]  * m[6] * m[15] - m[1]  * m[7] * m[14] - m[5]  * m[2] * m[15] + m[5]  * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0]  * m[6] * m[15] + m[0]  * m[7] * m[14] + m[4]  * m[2] * m[15] - m[4]  * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0]  * m[5] * m[15] - m[0]  * m[7] * m[13] - m[4]  * m[1] * m[15] + m[4]  * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0]  * m[5] * m[14] + m[0]  * m[6] * m[13] + m[4]  * m[1] * m[14] - m[4]  * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];

    det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (det == 0) return false;

    det = 1.0f / det;
    for (i = 0; i < 16; i++) invOut[i] = inv[i] * det;
    return true;
}

// --- Local Extended GLBWriter for Skinning ---
// Replaces the basic one from Helpers.h just for this file to support Skins/Joints
class SkinningGLBWriter {
    struct Accessor {
        int bufferView; int componentType; int count; std::string type;
        std::vector<float> min; std::vector<float> max;
    };
    struct BufferView { int byteOffset; int byteLength; int target; };

    std::vector<uint8_t> buffer;
    std::vector<Accessor> accessors;
    std::vector<BufferView> bufferViews;
    std::stringstream nodesJson;
    std::stringstream meshesJson;
    std::vector<int> rootNodes;
    std::vector<int> jointIndices;
    int meshCount = 0;
    int nodeCount = 0;

    void AlignBuffer() { while (buffer.size() % 4 != 0) buffer.push_back(0); }

public:
    void AddMeshNode(const std::string& name, int meshIndex) {
        if (nodeCount > 0) nodesJson << ",";
        // Attach skin 0 if joints exist
        nodesJson << "{\"name\":\"" << name << "\",\"mesh\":" << meshIndex;
        if (!jointIndices.empty()) nodesJson << ",\"skin\":0";
        nodesJson << "}";
        rootNodes.push_back(nodeCount++);
    }

    void AddBoneNode(const std::string& name, const float* matrix) {
        if (nodeCount > 0) nodesJson << ",";
        nodesJson << "{\"name\":\"" << name << "\",\"matrix\":[";
        for(int i=0; i<16; i++) nodesJson << matrix[i] << (i<15?",":"");
        nodesJson << "]}";
        jointIndices.push_back(nodeCount); // Track this as a bone
        rootNodes.push_back(nodeCount++);
    }

    int AddBufferView(const void* data, size_t size, int target) {
        AlignBuffer();
        int offset = (int)buffer.size();
        const uint8_t* ptr = (const uint8_t*)data;
        buffer.insert(buffer.end(), ptr, ptr + size);
        bufferViews.push_back({offset, (int)size, target});
        return (int)bufferViews.size() - 1;
    }

    int AddAccessor(int bufferView, int componentType, int count, const char* type, float* minVal = nullptr, float* maxVal = nullptr) {
        Accessor acc = {bufferView, componentType, count, type};
        if (minVal) acc.min = { minVal[0], minVal[1], minVal[2] };
        if (maxVal) acc.max = { maxVal[0], maxVal[1], maxVal[2] };
        accessors.push_back(acc);
        return (int)accessors.size() - 1;
    }

    int StartMesh(const std::string& name) {
        if (meshCount > 0) meshesJson << ",";
        meshesJson << "{\"name\":\"" << name << "\",\"primitives\":[";
        return meshCount++;
    }

    void EndMesh() { meshesJson << "]}"; }

    void AddPrimitive(int posAcc, int normAcc, int uvAcc, int indAcc, int jointAcc, int weightAcc) {
        meshesJson << "{\"attributes\":{";
        meshesJson << "\"POSITION\":" << posAcc;
        if (normAcc >= 0) meshesJson << ",\"NORMAL\":" << normAcc;
        if (uvAcc >= 0) meshesJson << ",\"TEXCOORD_0\":" << uvAcc;
        if (jointAcc >= 0) meshesJson << ",\"JOINTS_0\":" << jointAcc;
        if (weightAcc >= 0) meshesJson << ",\"WEIGHTS_0\":" << weightAcc;
        meshesJson << "},\"indices\":" << indAcc << "}";
    }

    // Pass ibmAccessor index to write the skin definition
    void WriteToFile(const std::string& path, int ibmAccessor = -1) {
        AlignBuffer();
        std::stringstream json;
        json << "{\"asset\":{\"version\":\"2.0\"},";

        // Scene
        json << "\"scene\":0,\"scenes\":[{\"nodes\":[";
        for (size_t i = 0; i < rootNodes.size(); i++) json << rootNodes[i] << (i < rootNodes.size() - 1 ? "," : "");
        json << "]}],";

        // Skins
        if (!jointIndices.empty()) {
            json << "\"skins\":[{\"inverseBindMatrices\":" << ibmAccessor << ",\"joints\":[";
            for (size_t i = 0; i < jointIndices.size(); i++) json << jointIndices[i] << (i < jointIndices.size() - 1 ? "," : "");
            json << "]}],";
        }

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
            json << "{\"buffer\":0,\"byteOffset\":" << bv.byteOffset
                 << ",\"byteLength\":" << bv.byteLength << ",\"target\":" << bv.target << "}"
                 << (i < bufferViews.size() - 1 ? "," : "");
        }
        json << "],";

        json << "\"buffers\":[{\"byteLength\":" << buffer.size() << "}]}";

        std::string jsonStr = json.str();
        while (jsonStr.size() % 4 != 0) jsonStr += " ";
        uint32_t totalLen = 12 + 8 + (uint32_t)jsonStr.size() + 8 + (uint32_t)buffer.size();

        std::ofstream out(path, std::ios::binary);
        uint32_t magic = 0x46546C67; uint32_t version = 2;
        out.write((char*)&magic, 4); out.write((char*)&version, 4); out.write((char*)&totalLen, 4);
        uint32_t chunkLen = (uint32_t)jsonStr.size(); uint32_t chunkType = 0x4E4F534A;
        out.write((char*)&chunkLen, 4); out.write((char*)&chunkType, 4); out.write(jsonStr.c_str(), chunkLen);
        chunkLen = (uint32_t)buffer.size(); chunkType = 0x004E4942;
        out.write((char*)&chunkLen, 4); out.write((char*)&chunkType, 4);
        if (chunkLen > 0) out.write((char*)buffer.data(), chunkLen);
        out.close();
    }
};

// ... [GLBWriter from Helpers.h implementation removed or kept, doesn't matter since we use local class,
//      but to prevent link errors for other files, we keep the original GLBWriter stubs if needed.
//      For this specific file, we will just use the new SkinningGLBWriter in ConvertPCM.]

// Re-implementing original GLBWriter methods to satisfy linker if Helpers.h declares them
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
    GLBBufferView bv; bv.buffer = 0; bv.byteOffset = offset; bv.byteLength = (int)size; bv.target = target;
    bufferViews.push_back(bv);
    return (int)bufferViews.size() - 1;
}
int GLBWriter::AddAccessor(int bufferView, int componentType, int count, const char* type, float* minVal, float* maxVal) {
    GLBAccessor acc; acc.bufferView = bufferView; acc.componentType = componentType; acc.count = count; acc.type = type;
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
void GLBWriter::EndMesh() { meshesJson << "]}"; }
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
    // ... Existing implementation ...
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
    uint32_t magic = 0x46546C67; uint32_t version = 2;
    out.write((char*)&magic, 4); out.write((char*)&version, 4); out.write((char*)&totalLen, 4);
    uint32_t chunkLen = (uint32_t)jsonStr.size(); uint32_t chunkType = 0x4E4F534A;
    out.write((char*)&chunkLen, 4); out.write((char*)&chunkType, 4); out.write(jsonStr.c_str(), chunkLen);
    chunkLen = (uint32_t)buffer.size(); chunkType = 0x004E4942;
    out.write((char*)&chunkLen, 4); out.write((char*)&chunkType, 4);
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

    ClosePreview();
    selectedFileIndex = -1;

    for (auto& t : textureCache) {
        if (t.second != 0) glDeleteTextures(1, &t.second);
    }
    textureCache.clear();

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

        if (fsize > 4 && e.offset + 4 <= pcPackData.size()) {
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

        std::string originalName = e.name;
        if (nameCounts.count(originalName)) {
            int idx = nameCounts[originalName];
            fs::path p(originalName);
            std::string stem = p.stem().string();
            std::string ext = p.extension().string();
            e.name = stem + "_" + std::to_string(idx) + ext;
            nameCounts[originalName]++;
        } else {
            nameCounts[originalName] = 0;
        }

        if (e.isPcm && e.size > 16) {
             size_t pcmBase = e.offset;
             if (pcmBase + 16 <= pcPackData.size()) {
                 br.Seek(pcmBase + 8);
                 uint32_t num = br.Read<uint32_t>();
                 uint32_t dirOfs = br.Read<uint32_t>();

                 size_t dirAddr = pcmBase + dirOfs;
                 if (dirAddr < pcPackData.size()) {
                     for(uint32_t k=0; k<num; k++) {
                         if (dirAddr + k*12 + 12 > pcPackData.size()) break;

                         br.Seek(dirAddr + k*12 + 2);
                         uint16_t itemType = br.Read<uint16_t>();
                         uint32_t objOfs = br.Read<uint32_t>();

                         if (itemType == 512) {
                             if (pcmBase + objOfs + 4 <= pcPackData.size()) {
                                 br.Seek(pcmBase + objOfs);
                                 uint32_t nameOfs = br.Read<uint32_t>();

                                 if (nameOfs != 0 && pcmBase + nameOfs < pcPackData.size()) {
                                     size_t strAddr = pcmBase + nameOfs;
                                     const char* strStart = (const char*)&pcPackData[strAddr];
                                     size_t maxLen = pcPackData.size() - strAddr;
                                     size_t sLen = 0;
                                     while(sLen < maxLen && strStart[sLen] != 0) sLen++;
                                     e.subItems.push_back(std::string(strStart, sLen));
                                 } else {
                                     e.subItems.push_back("Model_" + std::to_string(e.subItems.size()));
                                 }
                             }
                         }
                     }
                 }
             }
        }
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
    fs::path outDir = fs::current_path() / "extracted" / p.stem();
    fs::create_directories(outDir);

    Log("Extracting to: " + outDir.string());

    for(auto& e : entries) {
        fs::path fullFilePath = outDir / e.name;
        if (fullFilePath.has_parent_path()) {
            fs::create_directories(fullFilePath.parent_path());
        }

        if (e.isPcm && convertAll) {
            fs::path glbPath = fullFilePath;
            glbPath.replace_extension(".glb");
            std::vector<uint8_t> pcmData(pcPackData.begin() + e.offset, pcPackData.begin() + e.offset + e.size);
            ConvertPCM(pcmData, glbPath.string());
        } else {
            std::ofstream out(fullFilePath, std::ios::binary);
            if (out.is_open()) {
                out.write((char*)&pcPackData[e.offset], e.size);
                out.close();
            } else {
                Log("Failed to write file: " + fullFilePath.string());
                continue;
            }
        }
    }
    Log("Extraction complete.");
}

void SpiderManTool::ExtractFile(int index, bool asGlb) {
    if (index < 0 || index >= entries.size()) return;
    if (pcPackData.empty()) return;

    const auto& e = entries[index];
    fs::path p(loadedPCPackPath);
    fs::path outDir = fs::current_path() / "extracted" / p.stem();
    fs::path fullFilePath = outDir / e.name;

    if (fullFilePath.has_parent_path()) {
        fs::create_directories(fullFilePath.parent_path());
    }

    if (e.isPcm && asGlb) {
        fs::path glbPath = fullFilePath;
        glbPath.replace_extension(".glb");
        std::vector<uint8_t> pcmData(pcPackData.begin() + e.offset, pcPackData.begin() + e.offset + e.size);
        ConvertPCM(pcmData, glbPath.string());
        Log("Extracted GLB: " + glbPath.filename().string());
    } else {
        std::ofstream out(fullFilePath, std::ios::binary);
        if (out.is_open()) {
            out.write((char*)&pcPackData[e.offset], e.size);
            out.close();
            Log("Extracted: " + e.name);
        } else {
            Log("Failed to write file: " + fullFilePath.string());
        }
    }
}

unsigned int SpiderManTool::LoadTextureFromHash(uint32_t hash) {
    if (textureCache.count(hash)) return textureCache[hash];

    int foundIdx = -1;
    for(int i=0; i<entries.size(); i++) {
        if (entries[i].hash == hash && entries[i].isDds) {
            foundIdx = i;
            break;
        }
    }

    if (foundIdx == -1) {
        textureCache[hash] = 0;
        return 0;
    }

    const auto& e = entries[foundIdx];
    const uint8_t* data = &pcPackData[e.offset];
    if (e.size < sizeof(DDS_HEADER) + 4) return 0;
    uint32_t magic = *(uint32_t*)data;
    if (magic != 0x20534444) return 0;

    const DDS_HEADER* header = (const DDS_HEADER*)(data + 4);
    int width = header->dwWidth;
    int height = header->dwHeight;
    uint32_t fourCC = header->ddspf.dwFourCC;
    GLenum format = 0;
    if (fourCC == 0x31545844) format = GL_COMPRESSED_RGBA_S3TC_DXT1_EXT;
    else if (fourCC == 0x33545844) format = GL_COMPRESSED_RGBA_S3TC_DXT3_EXT;
    else if (fourCC == 0x35545844) format = GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;

    if (format == 0) return 0;

    uint32_t blockSize = (format == GL_COMPRESSED_RGBA_S3TC_DXT1_EXT) ? 8 : 16;
    uint32_t imageSize = ((width + 3) / 4) * ((height + 3) / 4) * blockSize;

    unsigned int tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glCompressedTexImage2D(GL_TEXTURE_2D, 0, format, width, height, 0, imageSize, data + 128);
    glBindTexture(GL_TEXTURE_2D, 0);

    textureCache[hash] = tex;
    Log("Loaded texture hash: " + std::to_string(hash));
    return tex;
}

void SpiderManTool::InitModelPreview() {
    if (previewTextureId == 0) {
        if (modelFbo != 0) glDeleteFramebuffers(1, &modelFbo);
        if (modelRbo != 0) glDeleteRenderbuffers(1, &modelRbo);
        modelFbo = 0;
        modelRbo = 0;

        glGenFramebuffers(1, &modelFbo);
        glGenTextures(1, &previewTextureId);
        glGenRenderbuffers(1, &modelRbo);

        glBindFramebuffer(GL_FRAMEBUFFER, modelFbo);

        glBindTexture(GL_TEXTURE_2D, previewTextureId);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 800, 600, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, previewTextureId, 0);

        glBindRenderbuffer(GL_RENDERBUFFER, modelRbo);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, 800, 600);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, modelRbo);

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    if (modelProgram != 0) return;

    const char* vShaderCode = "#version 130\n"
        "in vec3 pos; in vec2 texCoord; out vec2 TexCoord; uniform mat4 model; uniform mat4 view; uniform mat4 projection; void main(){ TexCoord = texCoord; gl_Position = projection * view * model * vec4(pos,1.0); }";
    const char* fShaderCode = "#version 130\n"
        "in vec2 TexCoord; out vec4 color; uniform sampler2D diffTexture; uniform bool hasTexture; void main(){ if(hasTexture) { vec4 texColor = texture(diffTexture, TexCoord); color = vec4(texColor.rgb, 1.0); } else { color = vec4(1.0, 1.0, 1.0, 1.0); } }";

    unsigned int vertex = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vertex, 1, &vShaderCode, NULL);
    glCompileShader(vertex);

    unsigned int fragment = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragment, 1, &fShaderCode, NULL);
    glCompileShader(fragment);

    modelProgram = glCreateProgram();
    glAttachShader(modelProgram, vertex);
    glAttachShader(modelProgram, fragment);
    glBindAttribLocation(modelProgram, 0, "pos");
    glBindAttribLocation(modelProgram, 1, "texCoord");
    glLinkProgram(modelProgram);

    glDeleteShader(vertex);
    glDeleteShader(fragment);
}

void SpiderManTool::LoadModelToGL(int index) {
    if (index < 0 || index >= entries.size()) return;
    const auto& e = entries[index];

    for (auto& m : previewMeshes) {
        if (m.vao) glDeleteVertexArrays(1, &m.vao);
        if (m.vbo) glDeleteBuffers(1, &m.vbo);
        if (m.ebo) glDeleteBuffers(1, &m.ebo);
    }
    previewMeshes.clear();

    if (e.offset + e.size > pcPackData.size()) { Log("Model data out of bounds"); return; }
    std::vector<uint8_t> pcmData(pcPackData.begin() + e.offset, pcPackData.begin() + e.offset + e.size);
    BinaryReader br(pcmData);

    if (8 + 4 > pcmData.size()) return;
    br.Seek(8);
    uint32_t num = br.Read<uint32_t>();
    uint32_t ofs = br.Read<uint32_t>();

    if (num > 1000) return;
    if (ofs >= pcmData.size()) return;

    br.Seek(ofs);
    struct Info { uint16_t u1, type; uint32_t offset, u2; };
    std::vector<Info> infos;
    for(uint32_t i=0; i<num; i++) {
        Info inf;
        inf.u1 = br.Read<uint16_t>();
        inf.type = br.Read<uint16_t>();
        inf.offset = br.Read<uint32_t>();
        inf.u2 = br.Read<uint32_t>();
        infos.push_back(inf);
    }

    struct Vertex { float x,y,z; float u,v; };

    float minX = 1e9f, minY = 1e9f, minZ = 1e9f;
    float maxX = -1e9f, maxY = -1e9f, maxZ = -1e9f;
    bool hasVerts = false;

    for(auto& inf : infos) {
        if (inf.type != 512) continue;
        if (inf.offset + 16 > pcmData.size()) continue;

        br.Seek(inf.offset);
        br.Skip(8);
        uint32_t numSm = br.Read<uint32_t>();
        uint32_t infSmOfs = br.Read<uint32_t>();

        if (numSm > 256) continue;
        if (infSmOfs >= pcmData.size()) continue;

        br.Seek(infSmOfs);
        std::vector<uint32_t> smOffsets;
        for(uint32_t s=0; s<numSm; s++) {
            br.Skip(4);
            smOffsets.push_back(br.Read<uint32_t>());
        }

        for(uint32_t smOfs : smOffsets) {
            if (smOfs + 64 > pcmData.size()) continue;

            br.Seek(smOfs);
            uint32_t nameOfs = br.Read<uint32_t>();
            uint32_t zero1 = br.Read<uint32_t>();
            uint32_t unk0 = br.Read<uint32_t>();
            uint32_t unk0_ofs = br.Read<uint32_t>();
            br.Skip(16); // 4 floats
            uint32_t unk_uint = br.Read<uint32_t>();
            uint32_t zero2 = br.Read<uint32_t>();
            uint32_t itype = br.Read<uint32_t>();
            uint32_t inum = br.Read<uint32_t>();
            uint32_t iofs = br.Read<uint32_t>();
            uint32_t zero3 = br.Read<uint32_t>();
            uint32_t vnum = br.Read<uint32_t>();
            uint32_t vofs = br.Read<uint32_t>();

            std::vector<uint32_t> unks;
            for(int k=0;k<8;k++) unks.push_back(br.Read<uint32_t>());
            uint32_t stride = unks[2];

            unsigned int tex = 0;
            for (uint32_t u : unks) {
                if (u > 0) {
                    tex = LoadTextureFromHash(u);
                    if (tex != 0) break;
                }
            }

            if (vnum > 100000 || inum > 300000) continue;
            if (vofs >= pcmData.size() || iofs >= pcmData.size()) continue;
            if (stride == 0) continue;

            br.Seek(vofs);
            std::vector<Vertex> vertices;
            bool valid = true;
            for(uint32_t v=0; v<vnum; v++) {
                size_t startV = br.Tell();
                if (startV + stride > pcmData.size()) { valid = false; break; }

                Vertex vert;
                vert.x = br.Read<float>(); vert.y = br.Read<float>(); vert.z = br.Read<float>();
                vert.u = 0; vert.v = 0;

                if (vert.x < minX) minX = vert.x; if (vert.x > maxX) maxX = vert.x;
                if (vert.y < minY) minY = vert.y; if (vert.y > maxY) maxY = vert.y;
                if (vert.z < minZ) minZ = vert.z; if (vert.z > maxZ) maxZ = vert.z;
                hasVerts = true;

                if (stride == 64) {
                    if (startV + 24 + 8 <= pcmData.size()) {
                        br.Seek(startV + 24);
                        vert.u = br.Read<float>();
                        vert.v = 1.0f - br.Read<float>();
                    }
                }
                else if (stride == 24) {
                    if (startV + 12 + 8 <= pcmData.size()) {
                        br.Seek(startV + 12);
                        vert.u = br.Read<float>();
                        vert.v = 1.0f - br.Read<float>();
                    }
                }

                vertices.push_back(vert);
                br.Seek(startV + stride);
            }
            if (!valid) continue;

            br.Seek(iofs);
            std::vector<uint16_t> indices;

            if (iofs + inum * 2 > pcmData.size()) continue;

            for(uint32_t i=0; i<inum; i++) indices.push_back(br.Read<uint16_t>());

            if (vertices.empty() || indices.empty()) continue;

            RenderMesh mesh;
            mesh.indexCount = (int)indices.size();
            mesh.mode = (itype == 4) ? GL_TRIANGLES : GL_TRIANGLE_STRIP;
            mesh.textureId = tex;

            glGenVertexArrays(1, &mesh.vao);
            glGenBuffers(1, &mesh.vbo);
            glGenBuffers(1, &mesh.ebo);

            glBindVertexArray(mesh.vao);
            glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo);
            glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(Vertex), vertices.data(), GL_STATIC_DRAW);

            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.ebo);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(uint16_t), indices.data(), GL_STATIC_DRAW);

            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)0);
            glEnableVertexAttribArray(0);

            glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)(3*sizeof(float)));
            glEnableVertexAttribArray(1);

            glBindVertexArray(0);
            previewMeshes.push_back(mesh);
            Log("Loaded Mesh. Verts: " + std::to_string(vnum) + " Mode: " + std::to_string(itype));
        }
    }

    if (hasVerts) {
        modelCenter[0] = (minX + maxX) * 0.5f;
        modelCenter[1] = (minY + maxY) * 0.5f;
        modelCenter[2] = (minZ + maxZ) * 0.5f;
        float dx = maxX - minX; float dy = maxY - minY; float dz = maxZ - minZ;
        modelRadius = sqrt(dx*dx + dy*dy + dz*dz) * 0.5f;
        if (modelRadius < 0.1f) modelRadius = 1.0f;
    } else {
        modelCenter[0] = 0; modelCenter[1] = 0; modelCenter[2] = 0;
        modelRadius = 10.0f;
    }
    modelZoom = 1.0f;
}

void SpiderManTool::RenderModelPreview() {
    glBindFramebuffer(GL_FRAMEBUFFER, modelFbo);
    glViewport(0, 0, 800, 600);
    // Teal background
    glClearColor(0.0f, 0.5f, 0.5f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    if (previewMeshes.empty()) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return;
    }

    glEnable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

    glUseProgram(modelProgram);

    float fov = 1.0f;
    float aspect = 800.0f / 600.0f;
    float znear = modelRadius * 0.01f;
    float zfar = modelRadius * 20.0f;

    if (znear < 0.1f) znear = 0.1f;
    if (zfar < 100.0f) zfar = 100.0f;

    float proj[16] = {0};
    float tanHalfFov = tan(fov / 2.0f);
    proj[0] = 1.0f / (aspect * tanHalfFov);
    proj[5] = 1.0f / tanHalfFov;
    proj[10] = -(zfar + znear) / (zfar - znear);
    proj[11] = -1.0f;
    proj[14] = -(2.0f * zfar * znear) / (zfar - znear);

    float dist = modelRadius * 2.5f * (1.0f / modelZoom);
    float tx = -modelCenter[0];
    float ty = -modelCenter[1];
    float tz = -modelCenter[2] - dist;

    float view[16] = {
        1,0,0,0,
        0,1,0,0,
        0,0,1,0,
        0,0,0,1
    };
    view[12] = tx;
    view[13] = ty;
    view[14] = tz;

    float cx = cos(modelRotX), sx = sin(modelRotX);
    float cy = cos(modelRotY), sy = sin(modelRotY);

    float model[16] = {
        cy, sx*sy, -cx*sy, 0,
        0, cx, sx, 0,
        sy, -sx*cy, cx*cy, 0,
        0, 0, 0, 1
    };

    glUniformMatrix4fv(glGetUniformLocation(modelProgram, "projection"), 1, GL_FALSE, proj);
    glUniformMatrix4fv(glGetUniformLocation(modelProgram, "view"), 1, GL_FALSE, view);
    glUniformMatrix4fv(glGetUniformLocation(modelProgram, "model"), 1, GL_FALSE, model);

    for (const auto& m : previewMeshes) {
        if (m.indexCount > 0) {
            if (m.textureId != 0) {
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, m.textureId);
                glUniform1i(glGetUniformLocation(modelProgram, "diffTexture"), 0);
                glUniform1i(glGetUniformLocation(modelProgram, "hasTexture"), 1);
            } else {
                glUniform1i(glGetUniformLocation(modelProgram, "hasTexture"), 0);
            }

            glBindVertexArray(m.vao);
            glDrawElements(m.mode, m.indexCount, GL_UNSIGNED_SHORT, 0);
        }
    }
    glBindVertexArray(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void SpiderManTool::LoadPreview(int index) {
    if (index < 0 || index >= entries.size()) return;
    if (pcPackData.empty()) return;
    const auto& e = entries[index];

    if (e.isPcm) {
        if (previewTextureId != 0 && !isModelPreview) ClosePreview();
        isModelPreview = true;
        InitModelPreview();
        LoadModelToGL(index);
        previewWidth = 800;
        previewHeight = 600;
        showPreview = true;
        Log("Model loaded: " + e.name);
        return;
    }

    if (!e.isDds) { Log("Not a DDS/PCM file."); return; }
    if (previewTextureId != 0) ClosePreview();
    isModelPreview = false;

    const uint8_t* data = &pcPackData[e.offset];
    if (e.size < sizeof(DDS_HEADER) + 4) return;
    uint32_t magic = *(uint32_t*)data;
    if (magic != 0x20534444) return;

    const DDS_HEADER* header = (const DDS_HEADER*)(data + 4);
    previewWidth = header->dwWidth;
    previewHeight = header->dwHeight;

    uint32_t fourCC = header->ddspf.dwFourCC;
    GLenum format = 0;
    if (fourCC == 0x31545844) format = GL_COMPRESSED_RGBA_S3TC_DXT1_EXT;
    else if (fourCC == 0x33545844) format = GL_COMPRESSED_RGBA_S3TC_DXT3_EXT;
    else if (fourCC == 0x35545844) format = GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;

    if (format == 0) { Log("Unsupported DXT format."); return; }
    uint32_t blockSize = (format == GL_COMPRESSED_RGBA_S3TC_DXT1_EXT) ? 8 : 16;
    uint32_t imageSize = ((previewWidth + 3) / 4) * ((previewHeight + 3) / 4) * blockSize;

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
    if (previewTextureId != 0 && !isModelPreview) {
        glDeleteTextures(1, &previewTextureId);
        previewTextureId = 0;
    }
    showPreview = false;
}

void SpiderManTool::ConvertPCM(const std::vector<uint8_t>& pcmData, const std::string& outPath) {
    BinaryReader br(pcmData);
    if (8 + 4 > pcmData.size()) return;
    br.Seek(8);
    uint32_t num = br.Read<uint32_t>();
    uint32_t ofs = br.Read<uint32_t>();
    if (ofs >= pcmData.size()) return;
    br.Seek(ofs);
    struct Info { uint16_t u1, type; uint32_t offset, u2; };
    std::vector<Info> infos;
    for(uint32_t i=0; i<num; i++) {
        Info inf; inf.u1 = br.Read<uint16_t>(); inf.type = br.Read<uint16_t>(); inf.offset = br.Read<uint32_t>(); inf.u2 = br.Read<uint32_t>(); infos.push_back(inf);
    }

    SkinningGLBWriter glb;
    std::vector<float> allIBMs; // Accumulate all IBMs here

    for(auto& inf : infos) {
        if (inf.type != 512) continue;
        if (inf.offset + 20 > pcmData.size()) continue;

        br.Seek(inf.offset);
        uint32_t nameOfs = br.Read<uint32_t>();
        br.Skip(4); uint32_t numSm = br.Read<uint32_t>(); uint32_t infSmOfs = br.Read<uint32_t>();

        // --- READ BONES (Skeleton) ---
        uint32_t numBn = br.Read<uint32_t>();
        uint32_t ofsBn = br.Read<uint32_t>();
        br.Skip(24);

        if (numBn > 0 && ofsBn < pcmData.size()) {
             br.Seek(ofsBn);
             for(uint32_t b=0; b<numBn; b++) {
                 float mat[16];
                 for(int m=0; m<16; m++) mat[m] = br.Read<float>();

                 // Add bone node (stores the bind pose matrix as node transform)
                 std::stringstream bnName; bnName << "bone_" << b;
                 glb.AddBoneNode(bnName.str(), mat);

                 // GLTF Needs Inverse Bind Matrices
                 float invMat[16];
                 if (InvertMatrix(mat, invMat)) {
                     for(int k=0; k<16; k++) allIBMs.push_back(invMat[k]);
                 } else {
                     // Fallback identity if inversion fails
                     float id[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
                     for(int k=0; k<16; k++) allIBMs.push_back(id[k]);
                 }
             }
        }

        if (infSmOfs >= pcmData.size()) continue;
        br.Seek(infSmOfs);
        std::vector<uint32_t> smOffsets;
        for(uint32_t s=0; s<numSm; s++) { br.Skip(4); smOffsets.push_back(br.Read<uint32_t>()); }
        for(uint32_t smOfs : smOffsets) {
            if (smOfs + 64 > pcmData.size()) continue;
            br.Seek(smOfs);
            uint32_t smNameOfs = br.Read<uint32_t>();

            // Fixed skip: 36 bytes for header
            br.Skip(36);

            uint32_t itype = br.Read<uint32_t>(); uint32_t inum = br.Read<uint32_t>(); uint32_t iofs = br.Read<uint32_t>();
            br.Skip(4); uint32_t vnum = br.Read<uint32_t>(); uint32_t vofs = br.Read<uint32_t>();
            br.Skip(8); uint32_t stride = br.Read<uint32_t>();
            std::string smName = "mesh";

            if (vofs >= pcmData.size() || iofs >= pcmData.size()) continue;

            br.Seek(vofs);
            std::vector<float> pos, norm, uvs, weights;
            std::vector<uint16_t> joints;

            for(uint32_t v=0; v<vnum; v++) {
                size_t startV = br.Tell();
                if (startV + stride > pcmData.size()) break;

                pos.push_back(br.Read<float>()); pos.push_back(br.Read<float>()); pos.push_back(br.Read<float>());
                if (stride == 64) {
                    br.Seek(startV + 12); norm.push_back(br.Read<float>()); norm.push_back(br.Read<float>()); norm.push_back(br.Read<float>());
                    br.Seek(startV + 24); uvs.push_back(br.Read<float>()); uvs.push_back(1.0f - br.Read<float>());

                    // --- READ SKINNING DATA ---
                    br.Seek(startV + 32); // Joints
                    // Clamp indices to valid range (fixes 65535 crash)
                    for(int k=0; k<4; k++) {
                        float val = br.Read<float>();
                        int idx = (int)val;
                        if (idx < 0 || (numBn > 0 && idx >= (int)numBn)) idx = 0;
                        joints.push_back((uint16_t)idx);
                    }

                    br.Seek(startV + 48); // Weights
                    weights.push_back(br.Read<float>()); weights.push_back(br.Read<float>());
                    weights.push_back(br.Read<float>()); weights.push_back(br.Read<float>());

                } else if (stride == 24) {
                    br.Seek(startV + 12); uvs.push_back(br.Read<float>()); uvs.push_back(1.0f - br.Read<float>());
                }
                br.Seek(startV + stride);
            }

            if (pos.empty()) continue;

            br.Seek(iofs);
            std::vector<uint16_t> indices;
            std::vector<uint16_t> rawIndices;
            if (iofs + inum * 2 > pcmData.size()) continue;

            for(uint32_t i=0; i<inum; i++) rawIndices.push_back(br.Read<uint16_t>());
            if (itype != 4) {
                 for (size_t k = 0; k < rawIndices.size(); k++) {
                    if (k + 2 >= rawIndices.size()) break;
                    uint16_t v1 = rawIndices[k], v2 = rawIndices[k+1], v3 = rawIndices[k+2];
                    if (v1==v2||v2==v3||v1==v3) continue;
                    if (k%2==0) { indices.push_back(v1); indices.push_back(v2); indices.push_back(v3); }
                    else { indices.push_back(v1); indices.push_back(v3); indices.push_back(v2); }
                }
            } else indices = rawIndices;

            if (indices.empty()) continue;

            float minP[3]={1e9,1e9,1e9}, maxP[3]={-1e9,-1e9,-1e9};
            for(size_t i=0; i<pos.size(); i+=3) for(int k=0;k<3;k++) { if(pos[i+k]<minP[k]) minP[k]=pos[i+k]; if(pos[i+k]>maxP[k]) maxP[k]=pos[i+k]; }
            int posAcc = glb.AddAccessor(glb.AddBufferView(pos.data(), pos.size()*4, 34962), 5126, vnum, "VEC3", minP, maxP);
            int indAcc = glb.AddAccessor(glb.AddBufferView(indices.data(), indices.size()*2, 34963), 5123, (int)indices.size(), "SCALAR");
            int normAcc = -1, uvAcc = -1, jointAcc = -1, weightAcc = -1;

            if(!norm.empty()) normAcc = glb.AddAccessor(glb.AddBufferView(norm.data(), norm.size()*4, 34962), 5126, vnum, "VEC3");
            if(!uvs.empty()) uvAcc = glb.AddAccessor(glb.AddBufferView(uvs.data(), uvs.size()*4, 34962), 5126, vnum, "VEC2");

            // Add Skinning Accessors
            if(!joints.empty()) jointAcc = glb.AddAccessor(glb.AddBufferView(joints.data(), joints.size()*2, 34963), 5123, vnum, "VEC4"); // 5123 = Unsigned Short
            if(!weights.empty()) weightAcc = glb.AddAccessor(glb.AddBufferView(weights.data(), weights.size()*4, 34962), 5126, vnum, "VEC4");

            int meshIdx = glb.StartMesh(smName);
            glb.AddPrimitive(posAcc, normAcc, uvAcc, indAcc, jointAcc, weightAcc);
            glb.EndMesh();
            glb.AddMeshNode(smName, meshIdx);
        }
    }
    // Write File with IBM Accessor Index (needed for skinning definition)
    int ibmAccIndex = -1;
    if (!allIBMs.empty()) {
        ibmAccIndex = glb.AddAccessor(glb.AddBufferView(allIBMs.data(), allIBMs.size()*4, 0), 5126, (int)allIBMs.size() / 16, "MAT4");
    }
    glb.WriteToFile(outPath, ibmAccIndex);
    Log("Converted GLB (Skinned): " + fs::path(outPath).filename().string());
}