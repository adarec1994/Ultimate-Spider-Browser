#pragma once
#define _CRT_SECURE_NO_WARNINGS
#include <vector>
#include <string>
#include <cstdint>
#include <algorithm>
#include <sstream>
#include <fstream>
#include <iomanip>
#include <cstring>
#include <cmath>

inline std::string StrToLower(const std::string& str) {
    std::string lower = str;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    return lower;
}

inline uint32_t CalculateCRC32(const std::string& str) {
    std::string lower = StrToLower(str);
    uint32_t crc = 0xFFFFFFFF;
    for (char c : lower) {
        crc ^= (uint8_t)c;
        for (int i = 0; i < 8; i++) {
            if (crc & 1) crc = (crc >> 1) ^ 0xEDB88320;
            else crc >>= 1;
        }
    }
    return ~crc;
}

inline void Normalize(float* v) {
    float len = sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
    if (len > 0) { v[0]/=len; v[1]/=len; v[2]/=len; }
}

inline void Cross(const float* a, const float* b, float* out) {
    out[0] = a[1]*b[2] - a[2]*b[1];
    out[1] = a[2]*b[0] - a[0]*b[2];
    out[2] = a[0]*b[1] - a[1]*b[0];
}

inline float Dot(const float* a, const float* b) {
    return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
}

inline void LookAt(const float* eye, const float* center, const float* up, float* dest) {
    float f[3] = { center[0]-eye[0], center[1]-eye[1], center[2]-eye[2] };
    Normalize(f);
    float s[3];
    Cross(f, up, s);
    Normalize(s);
    float u[3];
    Cross(s, f, u);
    dest[0] = s[0];  dest[4] = s[1];  dest[8] = s[2];  dest[12] = -Dot(s, eye);
    dest[1] = u[0];  dest[5] = u[1];  dest[9] = u[2];  dest[13] = -Dot(u, eye);
    dest[2] = -f[0]; dest[6] = -f[1]; dest[10]= -f[2]; dest[14] = Dot(f, eye);
    dest[3] = 0;     dest[7] = 0;     dest[11]= 0;     dest[15] = 1;
}

inline bool InvertMatrix(const float m[16], float invOut[16]) {
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

class BinaryReader {
public:
    const uint8_t* data;
    size_t size;
    size_t pos;

    BinaryReader(const std::vector<uint8_t>& buffer) : data(buffer.data()), size(buffer.size()), pos(0) {}

    void Seek(size_t offset) { if (offset <= size) pos = offset; }
    size_t Tell() const { return pos; }
    void Skip(size_t n) { Seek(pos + n); }

    template<typename T>
    T Read() {
        if (pos + sizeof(T) > size) return T();
        T val = *reinterpret_cast<const T*>(&data[pos]);
        pos += sizeof(T);
        return val;
    }

    std::string ReadString(size_t len) {
        if (pos + len > size) return "";
        std::string s(reinterpret_cast<const char*>(&data[pos]), len);
        pos += len;
        s.erase(std::find(s.begin(), s.end(), '\0'), s.end());
        return s;
    }

    std::vector<uint8_t> ReadBytes(size_t len) {
        if (pos + len > size) return {};
        std::vector<uint8_t> res(data + pos, data + pos + len);
        pos += len;
        return res;
    }
};

struct GLBMaterial {
    std::string name;
    bool doubleSided;
    std::string alphaMode;
};

class SkinningGLBWriter {
    struct Accessor {
        int bufferView; int componentType; int count; std::string type;
        std::vector<float> min; std::vector<float> max;
    };
    struct BufferView { int byteOffset; int byteLength; int target; };

    std::vector<uint8_t> buffer;
    std::vector<Accessor> accessors;
    std::vector<BufferView> bufferViews;
    std::vector<GLBMaterial> materials;
    std::stringstream nodesJson;
    std::stringstream meshesJson;
    std::vector<int> rootNodes;
    std::vector<int> jointIndices;
    int meshCount = 0;
    int nodeCount = 0;

    void AlignBuffer() { while (buffer.size() % 4 != 0) buffer.push_back(0); }

public:
    int AddNode(const std::string& name, int meshIndex = -1, int skinIndex = -1, const float* matrix = nullptr, const std::vector<int>& children = {}) {
        if (nodeCount > 0) nodesJson << ",";
        nodesJson << "{\"name\":\"" << name << "\"";

        if (meshIndex >= 0) nodesJson << ",\"mesh\":" << meshIndex;
        if (skinIndex >= 0) nodesJson << ",\"skin\":" << skinIndex;

        if (matrix) {
            nodesJson << ",\"matrix\":[";
            for(int i=0; i<16; i++) nodesJson << matrix[i] << (i<15?",":"");
            nodesJson << "]";
        }

        if (!children.empty()) {
            nodesJson << ",\"children\":[";
            for(size_t i=0; i<children.size(); i++) nodesJson << children[i] << (i<children.size()-1?",":"");
            nodesJson << "]";
        }

        nodesJson << "}";
        return nodeCount++;
    }

    void AddToScene(int nodeIndex) {
        rootNodes.push_back(nodeIndex);
    }

    void AddJoint(int nodeIndex) {
        jointIndices.push_back(nodeIndex);
    }

    int AddMaterial(const std::string& name, bool translucent) {
        GLBMaterial m;
        m.name = name;
        m.doubleSided = true;
        m.alphaMode = translucent ? "BLEND" : "OPAQUE";
        materials.push_back(m);
        return (int)materials.size() - 1;
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

    void AddPrimitive(int posAcc, int normAcc, int uvAcc, int indAcc, int jointAcc, int weightAcc, int matIdx = -1) {
        meshesJson << "{\"attributes\":{";
        meshesJson << "\"POSITION\":" << posAcc;
        if (normAcc >= 0) meshesJson << ",\"NORMAL\":" << normAcc;
        if (uvAcc >= 0) meshesJson << ",\"TEXCOORD_0\":" << uvAcc;
        if (jointAcc >= 0) meshesJson << ",\"JOINTS_0\":" << jointAcc;
        if (weightAcc >= 0) meshesJson << ",\"WEIGHTS_0\":" << weightAcc;
        meshesJson << "},\"indices\":" << indAcc;

        if (matIdx >= 0) {
            meshesJson << ",\"material\":" << matIdx;
        }
        meshesJson << "}";
    }

    void WriteToFile(const std::string& path, int ibmAccessor = -1) {
        AlignBuffer();
        std::stringstream json;
        json << "{\"asset\":{\"version\":\"2.0\"},";

        json << "\"scene\":0,\"scenes\":[{\"nodes\":[";
        for (size_t i = 0; i < rootNodes.size(); i++) json << rootNodes[i] << (i < rootNodes.size() - 1 ? "," : "");
        json << "]}],";

        if (!jointIndices.empty()) {
            json << "\"skins\":[{\"inverseBindMatrices\":" << ibmAccessor << ",\"joints\":[";
            for (size_t i = 0; i < jointIndices.size(); i++) json << jointIndices[i] << (i < jointIndices.size() - 1 ? "," : "");
            json << "]}],";
        }

        if (!materials.empty()) {
            json << "\"materials\":[";
            for(size_t i=0; i<materials.size(); i++) {
                json << "{\"name\":\"" << materials[i].name << "\",";
                json << "\"alphaMode\":\"" << materials[i].alphaMode << "\",";
                json << "\"doubleSided\":true";
                json << "}" << (i < materials.size()-1 ? "," : "");
            }
            json << "],";
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

    bool HasJoints() const { return !jointIndices.empty(); }
};