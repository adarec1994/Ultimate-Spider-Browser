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

struct GLBAccessor {
    int bufferView;
    int componentType;
    int count;
    std::string type;
    std::vector<float> min;
    std::vector<float> max;
};

struct GLBBufferView {
    int buffer;
    int byteOffset;
    int byteLength;
    int target;
};

class GLBWriter {
    std::vector<uint8_t> buffer;
    std::vector<GLBAccessor> accessors;
    std::vector<GLBBufferView> bufferViews;
    std::stringstream nodesJson;
    std::stringstream meshesJson;
    std::vector<int> rootNodes;
    int meshCount = 0;
    int nodeCount = 0;

    void AlignBuffer() {
        while (buffer.size() % 4 != 0) buffer.push_back(0);
    }

public:
    void AddMeshNode(const std::string& name, int meshIndex);
    void AddBoneNode(const std::string& name, const float* matrix);
    int AddBufferView(const void* data, size_t size, int target);
    int AddAccessor(int bufferView, int componentType, int count, const char* type, float* minVal = nullptr, float* maxVal = nullptr);
    int StartMesh(const std::string& name);
    void EndMesh();
    void AddPrimitive(int posAcc, int normAcc, int uvAcc, int indAcc, int jointAcc = -1, int weightAcc = -1);
    void WriteToFile(const std::string& path);
};