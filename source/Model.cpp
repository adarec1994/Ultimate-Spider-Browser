#include "SpiderManTool.h"
#include <glad/glad.h>
#include <sstream>
#include <fstream>

static std::string ReadStringTableEntry(const std::vector<uint8_t>& data, uint32_t offset) {
    if (offset == 0 || offset + 32 > data.size()) return "";
    size_t strStart = offset + 4;
    size_t end = strStart;
    while (end < strStart + 28 && end < data.size() && data[end] != 0) end++;
    return std::string((char*)&data[strStart], end - strStart);
}

void SpiderManTool::ParseMaterialEntries(const std::vector<uint8_t>& pcmData) {
    materialMap.clear();

    BinaryReader br(pcmData);
    if (pcmData.size() < 16) return;

    br.Seek(8);
    uint32_t numEntries = br.Read<uint32_t>();
    uint32_t entryTableOfs = br.Read<uint32_t>();

    if (numEntries > 1000 || entryTableOfs >= pcmData.size()) return;

    br.Seek(entryTableOfs);
    struct EntryInfo { uint16_t type; uint16_t tag; uint32_t dataOffset; uint32_t nameOffset; };
    std::vector<EntryInfo> entries;

    for (uint32_t i = 0; i < numEntries; i++) {
        EntryInfo e;
        e.type = br.Read<uint16_t>();
        e.tag = br.Read<uint16_t>();
        e.dataOffset = br.Read<uint32_t>();
        e.nameOffset = br.Read<uint32_t>();
        entries.push_back(e);
    }

    for (auto& e : entries) {
        if (e.tag != 256) continue;
        if (e.dataOffset + 0x64 > pcmData.size()) continue;

        br.Seek(e.dataOffset);

        uint32_t meshNameOfs = br.Read<uint32_t>();
        uint32_t alphaFlagOfs = br.Read<uint32_t>();

        br.Seek(e.dataOffset + 0x60);
        uint32_t textureNameOfs = br.Read<uint32_t>();

        MaterialDef mat;
        mat.meshName = ReadStringTableEntry(pcmData, meshNameOfs);
        mat.alphaFlag = ReadStringTableEntry(pcmData, alphaFlagOfs);
        mat.textureName = ReadStringTableEntry(pcmData, textureNameOfs);
        mat.isTranslucent = (mat.alphaFlag.find("translucent") != std::string::npos) ||
                            (mat.alphaFlag.find("glass") != std::string::npos);

        if (meshNameOfs != 0) {
            materialMap[meshNameOfs] = mat;
        }
    }
}

MaterialDef SpiderManTool::ResolveMaterialByMeshOffset(uint32_t meshNameOffset) {
    if (materialMap.count(meshNameOffset)) {
        return materialMap[meshNameOffset];
    }
    return MaterialDef();
}

void SpiderManTool::AddMeshFromData(const std::vector<uint8_t>& pcmData, std::string modelName, std::function<unsigned int(uint32_t)> textureResolver, const std::string& sourcePack, uint32_t sourceOffset) {
    ParseMaterialEntries(pcmData);

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
        Info inf; inf.u1 = br.Read<uint16_t>(); inf.type = br.Read<uint16_t>(); inf.offset = br.Read<uint32_t>(); inf.u2 = br.Read<uint32_t>(); infos.push_back(inf);
    }

    struct Vertex { float x,y,z; float nx,ny,nz; float u,v; };

    for(auto& inf : infos) {
        if (inf.type != 512) continue;
        if (inf.offset + 16 > pcmData.size()) continue;

        br.Seek(inf.offset); br.Skip(8);
        uint32_t numSm = br.Read<uint32_t>(); uint32_t infSmOfs = br.Read<uint32_t>();
        if (numSm > 256 || infSmOfs >= pcmData.size()) continue;

        br.Seek(infSmOfs);
        std::vector<uint32_t> smOffsets;
        for(uint32_t s=0; s<numSm; s++) { br.Skip(4); smOffsets.push_back(br.Read<uint32_t>()); }

        for(uint32_t smOfs : smOffsets) {
            if (smOfs + 64 > pcmData.size()) continue;

            br.Seek(smOfs);
            uint32_t meshNameRef = br.Read<uint32_t>();

            // Read mesh name for display
            std::string meshName;
            if (meshNameRef != 0 && meshNameRef + 32 <= pcmData.size()) {
                size_t strStart = meshNameRef + 4;
                size_t end = strStart;
                while (end < strStart + 28 && end < pcmData.size() && pcmData[end] != 0) end++;
                meshName = std::string((char*)&pcmData[strStart], end - strStart);
            }

            MaterialDef mat = ResolveMaterialByMeshOffset(meshNameRef);
            unsigned int tex = 0;

            if (!mat.textureName.empty()) {
                if (textureResolver) {
                    std::string texNameLower = StrToLower(mat.textureName);
                    uint32_t hash1 = CalculateCRC32(texNameLower + ".dds");
                    tex = textureResolver(hash1);
                    if (tex == 0) {
                        uint32_t hash2 = CalculateCRC32(texNameLower);
                        tex = textureResolver(hash2);
                    }
                } else {
                    tex = LoadTextureByName(mat.textureName);
                }
            }

            if (tex == 0 && !modelName.empty()) {
                std::string cleanName = modelName;
                size_t lastDot = cleanName.find_last_of(".");
                if(lastDot != std::string::npos) cleanName = cleanName.substr(0, lastDot);
                cleanName = StrToLower(cleanName);

                if (textureResolver) {
                    uint32_t diffHash = CalculateCRC32(cleanName + "_d.dds");
                    tex = textureResolver(diffHash);
                } else {
                    tex = LoadTextureByName(cleanName + "_d");
                    if (tex == 0) tex = LoadTextureByName(cleanName);
                }
            }

            if (tex == 0 && !mat.meshName.empty()) {
                if (textureResolver) {
                    std::string meshNameLower = StrToLower(mat.meshName);
                    uint32_t hash4 = CalculateCRC32(meshNameLower + ".dds");
                    tex = textureResolver(hash4);
                } else {
                    tex = LoadTextureByName(mat.meshName);
                }
            }

            br.Seek(smOfs + 40);
            uint32_t itype = br.Read<uint32_t>(); uint32_t inum = br.Read<uint32_t>(); uint32_t iofs = br.Read<uint32_t>();
            br.Skip(4); uint32_t vnum = br.Read<uint32_t>(); uint32_t vofs = br.Read<uint32_t>();
            br.Skip(8); uint32_t stride = br.Read<uint32_t>();

            if (vnum > 100000 || inum > 300000 || vofs >= pcmData.size() || iofs >= pcmData.size() || stride == 0) continue;

            br.Seek(vofs);
            std::vector<Vertex> vertices;
            bool valid = true;

            // Initialize bounding box
            float bboxMin[3] = {1e30f, 1e30f, 1e30f};
            float bboxMax[3] = {-1e30f, -1e30f, -1e30f};

            for(uint32_t v=0; v<vnum; v++) {
                size_t startV = br.Tell();
                if (startV + stride > pcmData.size()) { valid = false; break; }
                Vertex vert;
                vert.x = br.Read<float>(); vert.y = br.Read<float>(); vert.z = br.Read<float>();
                vert.nx = 0; vert.ny = 0; vert.nz = 0;
                vert.u = 0; vert.v = 0;

                // Update bounding box
                if (vert.x < bboxMin[0]) bboxMin[0] = vert.x;
                if (vert.y < bboxMin[1]) bboxMin[1] = vert.y;
                if (vert.z < bboxMin[2]) bboxMin[2] = vert.z;
                if (vert.x > bboxMax[0]) bboxMax[0] = vert.x;
                if (vert.y > bboxMax[1]) bboxMax[1] = vert.y;
                if (vert.z > bboxMax[2]) bboxMax[2] = vert.z;

                if (stride == 64) {
                    if (startV + 24 <= pcmData.size()) {
                        br.Seek(startV + 12);
                        vert.nx = br.Read<float>(); vert.ny = br.Read<float>(); vert.nz = br.Read<float>();
                    }
                    if (startV + 32 <= pcmData.size()) {
                        br.Seek(startV + 24);
                        vert.u = br.Read<float>(); vert.v = 1.0f - br.Read<float>();
                    }
                } else if (stride == 24) {
                    if (startV + 20 <= pcmData.size()) {
                        br.Seek(startV + 12);
                        vert.u = br.Read<float>(); vert.v = 1.0f - br.Read<float>();
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

            // Compute normals if not present (stride != 64)
            if (stride != 64 && !vertices.empty() && !indices.empty()) {
                // Initialize all normals to zero
                for (auto& v : vertices) {
                    v.nx = 0; v.ny = 0; v.nz = 0;
                }

                // Accumulate face normals
                if (itype == 4) {
                    // Triangle list
                    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
                        uint16_t i0 = indices[i], i1 = indices[i+1], i2 = indices[i+2];
                        if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size()) continue;

                        float e1[3] = { vertices[i1].x - vertices[i0].x, vertices[i1].y - vertices[i0].y, vertices[i1].z - vertices[i0].z };
                        float e2[3] = { vertices[i2].x - vertices[i0].x, vertices[i2].y - vertices[i0].y, vertices[i2].z - vertices[i0].z };
                        float n[3];
                        Cross(e1, e2, n);

                        vertices[i0].nx += n[0]; vertices[i0].ny += n[1]; vertices[i0].nz += n[2];
                        vertices[i1].nx += n[0]; vertices[i1].ny += n[1]; vertices[i1].nz += n[2];
                        vertices[i2].nx += n[0]; vertices[i2].ny += n[1]; vertices[i2].nz += n[2];
                    }
                } else {
                    // Triangle strip
                    for (size_t i = 0; i + 2 < indices.size(); i++) {
                        uint16_t i0 = indices[i], i1 = indices[i+1], i2 = indices[i+2];
                        if (i0 == i1 || i1 == i2 || i0 == i2) continue;
                        if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size()) continue;

                        float e1[3] = { vertices[i1].x - vertices[i0].x, vertices[i1].y - vertices[i0].y, vertices[i1].z - vertices[i0].z };
                        float e2[3] = { vertices[i2].x - vertices[i0].x, vertices[i2].y - vertices[i0].y, vertices[i2].z - vertices[i0].z };
                        float n[3];
                        Cross(e1, e2, n);

                        // Flip for odd triangles
                        if (i % 2 == 1) { n[0] = -n[0]; n[1] = -n[1]; n[2] = -n[2]; }

                        vertices[i0].nx += n[0]; vertices[i0].ny += n[1]; vertices[i0].nz += n[2];
                        vertices[i1].nx += n[0]; vertices[i1].ny += n[1]; vertices[i1].nz += n[2];
                        vertices[i2].nx += n[0]; vertices[i2].ny += n[1]; vertices[i2].nz += n[2];
                    }
                }

                // Normalize
                for (auto& v : vertices) {
                    float len = sqrt(v.nx*v.nx + v.ny*v.ny + v.nz*v.nz);
                    if (len > 0.0001f) {
                        v.nx /= len; v.ny /= len; v.nz /= len;
                    } else {
                        v.ny = 1.0f; // Default up
                    }
                }
            }

            RenderMesh mesh;
            mesh.indexCount = (int)indices.size();
            mesh.mode = (itype == 4) ? GL_TRIANGLES : GL_TRIANGLE_STRIP;
            mesh.textureId = tex;
            mesh.isTranslucent = mat.isTranslucent;

            // Check for fake_shadow texture
            std::string texNameLower = StrToLower(mat.textureName);
            mesh.isFakeShadow = (texNameLower.find("fake_shadow") != std::string::npos);

            // Store bounding box
            for (int i = 0; i < 3; i++) {
                mesh.bboxMin[i] = bboxMin[i];
                mesh.bboxMax[i] = bboxMax[i];
            }

            // Store source info for hex editor
            mesh.sourcePack = sourcePack;
            mesh.sourceOffset = sourceOffset;
            mesh.sourceSize = (uint32_t)pcmData.size();
            mesh.meshName = meshName.empty() ? modelName : meshName;

            // Check if this mesh should be skipped for picking (sky, ocean, collision volumes)
            std::string meshNameLower = StrToLower(mesh.meshName);
            mesh.skipPicking = (meshNameLower.find("sky") != std::string::npos) ||
                               (meshNameLower.find("ocean") != std::string::npos) ||
                               (meshNameLower.find("colvol") != std::string::npos) ||
                               (meshNameLower.find("shadow") != std::string::npos);

            // Only store vertex data for picking/export if not skipped
            if (!mesh.skipPicking) {
                mesh.positions.reserve(vertices.size() * 3);
                mesh.normals.reserve(vertices.size() * 3);
                mesh.uvs.reserve(vertices.size() * 2);
                for (const auto& v : vertices) {
                    mesh.positions.push_back(v.x);
                    mesh.positions.push_back(v.y);
                    mesh.positions.push_back(v.z);
                    mesh.normals.push_back(v.nx);
                    mesh.normals.push_back(v.ny);
                    mesh.normals.push_back(v.nz);
                    mesh.uvs.push_back(v.u);
                    mesh.uvs.push_back(v.v);
                }
                mesh.indices = indices;
            }

            // Store texture info for export
            mesh.textureName = mat.textureName;
            if (!mat.textureName.empty()) {
                mesh.textureHash = CalculateCRC32(StrToLower(mat.textureName) + ".dds");
            }

            glGenVertexArrays(1, &mesh.vao); glGenBuffers(1, &mesh.vbo); glGenBuffers(1, &mesh.ebo);
            glBindVertexArray(mesh.vao);
            glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo); glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(Vertex), vertices.data(), GL_STATIC_DRAW);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.ebo); glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(uint16_t), indices.data(), GL_STATIC_DRAW);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)0); glEnableVertexAttribArray(0);
            glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)(3*sizeof(float))); glEnableVertexAttribArray(1);
            glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)(6*sizeof(float))); glEnableVertexAttribArray(2);
            glBindVertexArray(0);
            previewMeshes.push_back(mesh);
        }
    }
}

void SpiderManTool::LoadBackgroundMeshes() {
    // Load oceanmesh.pcm and sky_day.pcm as background meshes for every preview
    // These are loaded from the global pack files using the texture index

    std::vector<std::pair<std::string, std::string>> backgroundModels = {
        {"oceanmesh", "city_arena"},
        {"sky_day", "city_arena"}
    };

    for (const auto& [modelName, packName] : backgroundModels) {
        // Find the pack file
        std::string packPath;
        for (const auto& path : foundPacks) {
            std::string stem = StrToLower(path.stem().string());
            if (stem == packName) {
                packPath = path.string();
                break;
            }
        }

        if (packPath.empty()) continue;

        // Open and parse the pack to find the PCM
        std::ifstream file(packPath, std::ios::binary | std::ios::ate);
        if (!file.is_open()) continue;

        size_t fileSize = file.tellg();
        if (fileSize < 32) {
            file.close();
            continue;
        }

        file.seekg(24);
        uint32_t headerSize, packDataOffset;
        file.read((char*)&headerSize, 4);
        file.read((char*)&packDataOffset, 4);

        if (!file.good()) {
            file.close();
            continue;
        }

        // Find entry table start
        size_t start = 0;
        const uint32_t magic = 0xE3E3E3E3;
        size_t headerReadSize = std::min((size_t)200000, fileSize);
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
                break;
            }
        }

        if (start == 0) {
            file.close();
            continue;
        }

        // Parse entries to find our target PCM
        file.clear();
        file.seekg(start);

        bool found = false;
        while (file.good() && !found) {
            uint32_t hash, type, offset, size;
            file.read((char*)&hash, 4);
            file.read((char*)&type, 4);
            file.read((char*)&offset, 4);
            file.read((char*)&size, 4);

            if (!file.good()) break;
            if (type >= 0x1000 || type == 0x0000) break;

            if (size > 4) {
                size_t filePos = file.tellg();
                uint32_t absOffset = packDataOffset + offset;

                if (absOffset + 4 > fileSize) {
                    file.seekg(filePos);
                    continue;
                }

                file.seekg(absOffset);
                uint32_t sig = 0;
                file.read((char*)&sig, 4);

                if (file.good() && sig == 0x204D4350) { // "PCM "
                    std::string entryName = "";
                    if (dictionary.count(hash)) entryName = StrToLower(dictionary[hash]);

                    if (entryName == modelName) {
                        // Found it! Load the PCM data
                        file.seekg(absOffset);
                        std::vector<uint8_t> pcmData(size);
                        file.read((char*)pcmData.data(), size);

                        if (file.good()) {
                            AddMeshFromData(pcmData, modelName, nullptr);
                            found = true;
                        }
                    }
                }

                file.clear();
                file.seekg(filePos);
            }
        }

        file.close();
    }
}

void SpiderManTool::LoadModelToGL(int index) {
    isWorldMode = false;
    if (index < 0 || index >= (int)entries.size()) return;
    const auto& e = entries[index];

    for (auto& m : previewMeshes) {
        if (m.vao) glDeleteVertexArrays(1, &m.vao);
        if (m.vbo) glDeleteBuffers(1, &m.vbo);
        if (m.ebo) glDeleteBuffers(1, &m.ebo);
    }
    previewMeshes.clear();

    if (e.offset + e.size > pcPackData.size()) return;

    std::vector<uint8_t> pcmData(pcPackData.begin() + e.offset, pcPackData.begin() + e.offset + e.size);
    AddMeshFromData(pcmData, e.name, nullptr);

    modelCenter[0] = 0; modelCenter[1] = 0; modelCenter[2] = 0;
    modelRadius = 10.0f;

    if(!previewMeshes.empty()) {
        float minP[3] = {1e9, 1e9, 1e9};
        float maxP[3] = {-1e9, -1e9, -1e9};

        BinaryReader br(pcmData);
        br.Seek(8);
        uint32_t num = br.Read<uint32_t>();
        uint32_t ofs = br.Read<uint32_t>();

        if (num < 1000 && ofs < pcmData.size()) {
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

            for(auto& inf : infos) {
                if (inf.type != 512) continue;
                if (inf.offset + 16 > pcmData.size()) continue;
                br.Seek(inf.offset); br.Skip(8);
                uint32_t numSm = br.Read<uint32_t>(); uint32_t infSmOfs = br.Read<uint32_t>();
                if(infSmOfs >= pcmData.size()) continue;

                br.Seek(infSmOfs);
                std::vector<uint32_t> smOffsets;
                for(uint32_t s=0; s<numSm; s++) { br.Skip(4); smOffsets.push_back(br.Read<uint32_t>()); }

                for(uint32_t smOfs : smOffsets) {
                    if (smOfs + 64 > pcmData.size()) continue;
                    br.Seek(smOfs); br.Skip(40);
                    uint32_t itype = br.Read<uint32_t>();
                    uint32_t inum = br.Read<uint32_t>();
                    uint32_t iofs = br.Read<uint32_t>();
                    br.Skip(4);
                    uint32_t vnum = br.Read<uint32_t>();
                    uint32_t vofs = br.Read<uint32_t>();
                    br.Skip(8);
                    uint32_t stride = br.Read<uint32_t>();

                    if (vofs >= pcmData.size() || stride == 0) continue;
                    br.Seek(vofs);
                    for(uint32_t v=0; v<vnum; v++) {
                        size_t sv = br.Tell();
                        float x = br.Read<float>(); float y = br.Read<float>(); float z = br.Read<float>();
                        if(x < minP[0]) minP[0] = x; if(x > maxP[0]) maxP[0] = x;
                        if(y < minP[1]) minP[1] = y; if(y > maxP[1]) maxP[1] = y;
                        if(z < minP[2]) minP[2] = z; if(z > maxP[2]) maxP[2] = z;
                        br.Seek(sv + stride);
                    }
                }
            }
        }

        if (minP[0] != 1e9) {
            float cx = (minP[0] + maxP[0]) * 0.5f;
            float cy = (minP[1] + maxP[1]) * 0.5f;
            float cz = (minP[2] + maxP[2]) * 0.5f;

            float radius = sqrt(pow(maxP[0]-minP[0],2) + pow(maxP[1]-minP[1],2) + pow(maxP[2]-minP[2],2));
            if (radius < 1.0f) radius = 5.0f;

            camPos[0] = cx;
            camPos[1] = cy + radius * 0.5f;
            camPos[2] = cz + radius * 1.5f;

            float target[3] = {cx, cy, cz};
            float dir[3] = {target[0]-camPos[0], target[1]-camPos[1], target[2]-camPos[2]};
            Normalize(dir);
            camFront[0] = dir[0]; camFront[1] = dir[1]; camFront[2] = dir[2];

            camYaw = atan2(camFront[2], camFront[0]) * 180.0f / 3.14159f;
            camPitch = asin(camFront[1]) * 180.0f / 3.14159f;
        }
    }
}

void SpiderManTool::ConvertPCM(const std::vector<uint8_t>& pcmData, const std::string& outPath) {
    ParseMaterialEntries(pcmData);

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

    // Collect all submeshes and their data
    struct SubmeshData {
        std::string name;
        std::string textureName;
        bool isTranslucent;
        std::vector<float> pos, norm, uvs, weights;
        std::vector<uint16_t> joints, indices;
        float minP[3], maxP[3];
    };
    std::vector<SubmeshData> submeshes;
    std::vector<float> allIBMs;
    uint32_t totalBones = 0;
    std::string modelName = "Model";

    auto ReadName = [&](uint32_t offset) -> std::string {
        return ReadStringTableEntry(pcmData, offset);
    };

    for(auto& inf : infos) {
        if (inf.type != 512) continue;
        if (inf.offset + 20 > pcmData.size()) continue;

        br.Seek(inf.offset);
        uint32_t mdlNameOfs = br.Read<uint32_t>();
        std::string mdlName = ReadName(mdlNameOfs);
        if (!mdlName.empty()) modelName = mdlName;

        br.Skip(4);
        uint32_t numSm = br.Read<uint32_t>();
        uint32_t infSmOfs = br.Read<uint32_t>();
        uint32_t numBn = br.Read<uint32_t>();
        uint32_t ofsBn = br.Read<uint32_t>();

        if (numBn > 0 && ofsBn > 0 && ofsBn + numBn * 64 <= pcmData.size() && allIBMs.empty()) {
            totalBones = numBn;
            br.Seek(ofsBn);
            for (uint32_t b = 0; b < numBn; b++) {
                float invMtx[16];
                for (int mi = 0; mi < 16; mi++) invMtx[mi] = br.Read<float>();
                float ibm[16];
                if (InvertMatrix(invMtx, ibm)) {
                    for (int mi = 0; mi < 16; mi++) allIBMs.push_back(ibm[mi]);
                } else {
                    for (int mi = 0; mi < 16; mi++) allIBMs.push_back(invMtx[mi]);
                }
            }
        }

        if (infSmOfs >= pcmData.size()) continue;
        br.Seek(infSmOfs);
        std::vector<uint32_t> smOffsets;
        for (uint32_t s = 0; s < numSm; s++) { br.Skip(4); smOffsets.push_back(br.Read<uint32_t>()); }

        for (size_t smIdx = 0; smIdx < smOffsets.size(); smIdx++) {
            uint32_t smOfs = smOffsets[smIdx];
            if (smOfs + 80 > pcmData.size()) continue;

            br.Seek(smOfs);
            uint32_t smNameOfs = br.Read<uint32_t>();
            std::string smName = ReadName(smNameOfs);
            if (smName.empty()) smName = "Submesh_" + std::to_string(smIdx);

            MaterialDef mat = ResolveMaterialByMeshOffset(smNameOfs);

            br.Seek(smOfs + 40);
            uint32_t itype = br.Read<uint32_t>();
            uint32_t inum = br.Read<uint32_t>();
            uint32_t iofs = br.Read<uint32_t>();
            br.Skip(4);
            uint32_t vnum = br.Read<uint32_t>();
            uint32_t vofs = br.Read<uint32_t>();
            br.Skip(8);
            uint32_t stride = br.Read<uint32_t>();

            if (vnum == 0 || inum == 0 || vofs >= pcmData.size() || iofs >= pcmData.size()) continue;

            SubmeshData sm;
            sm.name = smName;
            sm.textureName = mat.textureName;
            sm.isTranslucent = mat.isTranslucent;

            br.Seek(vofs);
            for (uint32_t v = 0; v < vnum; v++) {
                size_t startV = br.Tell();
                if (startV + stride > pcmData.size()) break;
                sm.pos.push_back(br.Read<float>()); sm.pos.push_back(br.Read<float>()); sm.pos.push_back(br.Read<float>());

                if (stride == 64) {
                    br.Seek(startV + 12);
                    sm.norm.push_back(br.Read<float>()); sm.norm.push_back(br.Read<float>()); sm.norm.push_back(br.Read<float>());
                    br.Seek(startV + 24);
                    sm.uvs.push_back(br.Read<float>()); sm.uvs.push_back(1.0f - br.Read<float>());
                    br.Seek(startV + 32);
                    for(int k=0; k<4; k++) {
                        float val = br.Read<float>();
                        int idx = (int)val;
                        if (idx < 0 || (totalBones > 0 && idx >= (int)totalBones)) idx = 0;
                        sm.joints.push_back((uint16_t)idx);
                    }
                    br.Seek(startV + 48);
                    sm.weights.push_back(br.Read<float>()); sm.weights.push_back(br.Read<float>());
                    sm.weights.push_back(br.Read<float>()); sm.weights.push_back(br.Read<float>());
                } else if (stride == 24) {
                    br.Seek(startV + 12);
                    sm.uvs.push_back(br.Read<float>()); sm.uvs.push_back(1.0f - br.Read<float>());
                }
                br.Seek(startV + stride);
            }

            if (sm.pos.empty()) continue;

            br.Seek(iofs);
            std::vector<uint16_t> rawIndices;
            if (iofs + inum * 2 > pcmData.size()) continue;

            for(uint32_t i=0; i<inum; i++) rawIndices.push_back(br.Read<uint16_t>());
            if (itype != 4) {
                for (size_t k = 0; k < rawIndices.size(); k++) {
                    if (k + 2 >= rawIndices.size()) break;
                    uint16_t v1 = rawIndices[k], v2 = rawIndices[k+1], v3 = rawIndices[k+2];
                    if (v1==v2||v2==v3||v1==v3) continue;
                    if (k%2==0) { sm.indices.push_back(v1); sm.indices.push_back(v2); sm.indices.push_back(v3); }
                    else { sm.indices.push_back(v1); sm.indices.push_back(v3); sm.indices.push_back(v2); }
                }
            } else {
                sm.indices = rawIndices;
            }

            if (sm.indices.empty()) continue;

            // Calculate bounds
            sm.minP[0] = sm.minP[1] = sm.minP[2] = 1e9f;
            sm.maxP[0] = sm.maxP[1] = sm.maxP[2] = -1e9f;
            for(size_t i=0; i<sm.pos.size(); i+=3) {
                for(int k=0; k<3; k++) {
                    if(sm.pos[i+k] < sm.minP[k]) sm.minP[k] = sm.pos[i+k];
                    if(sm.pos[i+k] > sm.maxP[k]) sm.maxP[k] = sm.pos[i+k];
                }
            }

            submeshes.push_back(sm);
        }
    }

    if (submeshes.empty()) return;

    // Collect unique textures and load them
    std::map<std::string, int> textureIndices;
    std::vector<std::vector<uint8_t>> textureDataList;

    for (auto& sm : submeshes) {
        if (sm.textureName.empty()) continue;
        std::string texNameLower = StrToLower(sm.textureName);
        if (textureIndices.count(texNameLower)) continue;

        std::vector<uint8_t> texData;
        bool found = false;

        // Try global texture name index
        if (globalTextureNameIndex.count(texNameLower)) {
            auto& loc = globalTextureNameIndex[texNameLower];
            std::ifstream texFile(loc.packPath, std::ios::binary);
            if (texFile.is_open()) {
                texFile.seekg(loc.offset);
                texData.resize(loc.size);
                texFile.read((char*)texData.data(), loc.size);
                texFile.close();
                found = true;
            }
        }

        // Try by hash
        if (!found) {
            uint32_t hash = CalculateCRC32(texNameLower + ".dds");
            if (globalTextureIndex.count(hash)) {
                auto& loc = globalTextureIndex[hash];
                std::ifstream texFile(loc.packPath, std::ios::binary);
                if (texFile.is_open()) {
                    texFile.seekg(loc.offset);
                    texData.resize(loc.size);
                    texFile.read((char*)texData.data(), loc.size);
                    texFile.close();
                    found = true;
                }
            }
        }

        // Try in current pack
        if (!found && !pcPackData.empty()) {
            for (const auto& e : entries) {
                if (!e.isDds) continue;
                std::string entryName = StrToLower(e.name);
                if (entryName.find(texNameLower) != std::string::npos ||
                    entryName == texNameLower + ".dds") {
                    if (e.offset + e.size <= pcPackData.size()) {
                        texData.assign(pcPackData.begin() + e.offset, pcPackData.begin() + e.offset + e.size);
                        found = true;
                        break;
                    }
                }
            }
        }

        if (found && !texData.empty()) {
            textureIndices[texNameLower] = (int)textureDataList.size();
            textureDataList.push_back(texData);
        }
    }

    // Build GLB with textures
    std::vector<uint8_t> binBuffer;
    auto alignBuffer = [&binBuffer]() { while (binBuffer.size() % 4 != 0) binBuffer.push_back(0); };
    auto addToBuffer = [&binBuffer](const void* data, size_t size) -> int {
        int offset = (int)binBuffer.size();
        const uint8_t* ptr = (const uint8_t*)data;
        binBuffer.insert(binBuffer.end(), ptr, ptr + size);
        return offset;
    };

    // Track buffer views and accessors
    struct BufView { int offset, length, target; };
    std::vector<BufView> bufferViews;

    struct Accessor { int bufView, compType, count; std::string type; float min[3], max[3]; bool hasMinMax; };
    std::vector<Accessor> accessors;

    auto addBufferView = [&](const void* data, size_t size, int target) -> int {
        alignBuffer();
        int offset = addToBuffer(data, size);
        bufferViews.push_back({offset, (int)size, target});
        return (int)bufferViews.size() - 1;
    };

    auto addAccessor = [&](int bv, int compType, int count, const char* type, float* minV = nullptr, float* maxV = nullptr) -> int {
        Accessor acc = {bv, compType, count, type, {0,0,0}, {0,0,0}, minV != nullptr};
        if (minV) { acc.min[0] = minV[0]; acc.min[1] = minV[1]; acc.min[2] = minV[2]; }
        if (maxV) { acc.max[0] = maxV[0]; acc.max[1] = maxV[1]; acc.max[2] = maxV[2]; }
        accessors.push_back(acc);
        return (int)accessors.size() - 1;
    };

    // Add geometry data for each submesh
    struct MeshInfo { int posAcc, normAcc, uvAcc, indAcc, jointAcc, weightAcc, matIdx; std::string name; };
    std::vector<MeshInfo> meshInfos;

    for (size_t i = 0; i < submeshes.size(); i++) {
        auto& sm = submeshes[i];
        MeshInfo mi;
        mi.name = sm.name;

        int vnum = (int)(sm.pos.size() / 3);
        mi.posAcc = addAccessor(addBufferView(sm.pos.data(), sm.pos.size()*4, 34962), 5126, vnum, "VEC3", sm.minP, sm.maxP);
        mi.normAcc = sm.norm.empty() ? -1 : addAccessor(addBufferView(sm.norm.data(), sm.norm.size()*4, 34962), 5126, vnum, "VEC3");
        mi.uvAcc = sm.uvs.empty() ? -1 : addAccessor(addBufferView(sm.uvs.data(), sm.uvs.size()*4, 34962), 5126, vnum, "VEC2");
        mi.indAcc = addAccessor(addBufferView(sm.indices.data(), sm.indices.size()*2, 34963), 5123, (int)sm.indices.size(), "SCALAR");
        mi.jointAcc = sm.joints.empty() ? -1 : addAccessor(addBufferView(sm.joints.data(), sm.joints.size()*2, 34963), 5123, vnum, "VEC4");
        mi.weightAcc = sm.weights.empty() ? -1 : addAccessor(addBufferView(sm.weights.data(), sm.weights.size()*4, 34962), 5126, vnum, "VEC4");

        // Find texture index for this submesh
        mi.matIdx = (int)i;  // Each submesh gets its own material

        meshInfos.push_back(mi);
    }

    // Add IBM data if we have bones
    int ibmAccIdx = -1;
    if (!allIBMs.empty()) {
        ibmAccIdx = addAccessor(addBufferView(allIBMs.data(), allIBMs.size()*4, 0), 5126, (int)allIBMs.size()/16, "MAT4");
    }

    // Add texture data
    std::vector<int> textureBufferViews;
    for (auto& texData : textureDataList) {
        alignBuffer();
        int offset = addToBuffer(texData.data(), texData.size());
        bufferViews.push_back({offset, (int)texData.size(), 0});
        textureBufferViews.push_back((int)bufferViews.size() - 1);
    }

    alignBuffer();

    // Build JSON
    std::stringstream json;
    json << "{\"asset\":{\"version\":\"2.0\",\"generator\":\"USM\"},";

    // Buffer views
    json << "\"bufferViews\":[";
    for (size_t i = 0; i < bufferViews.size(); i++) {
        auto& bv = bufferViews[i];
        json << "{\"buffer\":0,\"byteOffset\":" << bv.offset << ",\"byteLength\":" << bv.length;
        if (bv.target != 0) json << ",\"target\":" << bv.target;
        json << "}" << (i < bufferViews.size()-1 ? "," : "");
    }
    json << "],";

    // Accessors
    json << "\"accessors\":[";
    for (size_t i = 0; i < accessors.size(); i++) {
        auto& acc = accessors[i];
        json << "{\"bufferView\":" << acc.bufView << ",\"componentType\":" << acc.compType
             << ",\"count\":" << acc.count << ",\"type\":\"" << acc.type << "\"";
        if (acc.hasMinMax) {
            json << ",\"min\":[" << acc.min[0] << "," << acc.min[1] << "," << acc.min[2] << "]";
            json << ",\"max\":[" << acc.max[0] << "," << acc.max[1] << "," << acc.max[2] << "]";
        }
        json << "}" << (i < accessors.size()-1 ? "," : "");
    }
    json << "],";

    // Images
    if (!textureBufferViews.empty()) {
        json << "\"images\":[";
        for (size_t i = 0; i < textureBufferViews.size(); i++) {
            json << "{\"bufferView\":" << textureBufferViews[i] << ",\"mimeType\":\"image/vnd-ms.dds\"}";
            json << (i < textureBufferViews.size()-1 ? "," : "");
        }
        json << "],";

        json << "\"samplers\":[{\"magFilter\":9729,\"minFilter\":9729,\"wrapS\":10497,\"wrapT\":10497}],";

        json << "\"textures\":[";
        for (size_t i = 0; i < textureBufferViews.size(); i++) {
            json << "{\"sampler\":0,\"source\":" << i << "}";
            json << (i < textureBufferViews.size()-1 ? "," : "");
        }
        json << "],";
    }

    // Materials
    json << "\"materials\":[";
    for (size_t i = 0; i < submeshes.size(); i++) {
        auto& sm = submeshes[i];
        std::string texNameLower = StrToLower(sm.textureName);
        int texIdx = textureIndices.count(texNameLower) ? textureIndices[texNameLower] : -1;

        json << "{\"name\":\"" << sm.name << "\",\"doubleSided\":true,\"pbrMetallicRoughness\":{";
        if (texIdx >= 0) {
            json << "\"baseColorTexture\":{\"index\":" << texIdx << "},";
        } else {
            json << "\"baseColorFactor\":[0.8,0.8,0.8,1],";
        }
        json << "\"metallicFactor\":0,\"roughnessFactor\":1},";
        json << "\"alphaMode\":\"" << (sm.isTranslucent ? "BLEND" : "OPAQUE") << "\"}";
        json << (i < submeshes.size()-1 ? "," : "");
    }
    json << "],";

    // Meshes
    json << "\"meshes\":[";
    for (size_t i = 0; i < meshInfos.size(); i++) {
        auto& mi = meshInfos[i];
        json << "{\"name\":\"" << mi.name << "\",\"primitives\":[{\"attributes\":{\"POSITION\":" << mi.posAcc;
        if (mi.normAcc >= 0) json << ",\"NORMAL\":" << mi.normAcc;
        if (mi.uvAcc >= 0) json << ",\"TEXCOORD_0\":" << mi.uvAcc;
        if (mi.jointAcc >= 0) json << ",\"JOINTS_0\":" << mi.jointAcc;
        if (mi.weightAcc >= 0) json << ",\"WEIGHTS_0\":" << mi.weightAcc;
        json << "},\"indices\":" << mi.indAcc << ",\"material\":" << mi.matIdx << "}]}";
        json << (i < meshInfos.size()-1 ? "," : "");
    }
    json << "],";

    // Nodes
    json << "\"nodes\":[";
    int nodeIdx = 0;

    // Bone nodes if we have skinning
    if (totalBones > 0) {
        for (uint32_t b = 0; b < totalBones; b++) {
            json << "{\"name\":\"Bone_" << b << "\"},";
            nodeIdx++;
        }
    }

    // Mesh nodes
    std::vector<int> meshNodeIndices;
    for (size_t i = 0; i < meshInfos.size(); i++) {
        json << "{\"name\":\"" << meshInfos[i].name << "\",\"mesh\":" << i;
        if (totalBones > 0) json << ",\"skin\":0";
        json << "}";
        meshNodeIndices.push_back(nodeIdx++);
        json << (i < meshInfos.size()-1 ? "," : "");
    }
    json << "],";

    // Skin if we have bones
    if (totalBones > 0) {
        json << "\"skins\":[{\"inverseBindMatrices\":" << ibmAccIdx << ",\"joints\":[";
        for (uint32_t b = 0; b < totalBones; b++) {
            json << b << (b < totalBones-1 ? "," : "");
        }
        json << "]}],";
    }

    // Scene
    json << "\"scenes\":[{\"nodes\":[";
    for (size_t i = 0; i < meshNodeIndices.size(); i++) {
        json << meshNodeIndices[i] << (i < meshNodeIndices.size()-1 ? "," : "");
    }
    json << "]}],\"scene\":0,";

    // Buffer
    json << "\"buffers\":[{\"byteLength\":" << binBuffer.size() << "}]}";

    std::string jsonStr = json.str();
    while (jsonStr.size() % 4 != 0) jsonStr += " ";

    // Write GLB file
    std::ofstream out(outPath, std::ios::binary);
    uint32_t magic = 0x46546C67;
    uint32_t version = 2;
    uint32_t totalLen = 12 + 8 + (uint32_t)jsonStr.size() + 8 + (uint32_t)binBuffer.size();

    out.write((char*)&magic, 4);
    out.write((char*)&version, 4);
    out.write((char*)&totalLen, 4);

    uint32_t jsonLen = (uint32_t)jsonStr.size();
    uint32_t jsonType = 0x4E4F534A;
    out.write((char*)&jsonLen, 4);
    out.write((char*)&jsonType, 4);
    out.write(jsonStr.c_str(), jsonLen);

    uint32_t binLen = (uint32_t)binBuffer.size();
    uint32_t binType = 0x004E4942;
    out.write((char*)&binLen, 4);
    out.write((char*)&binType, 4);
    out.write((char*)binBuffer.data(), binLen);

    out.close();

    // Also export textures as separate DDS files
    fs::path outDir = fs::path(outPath).parent_path();
    int texExported = 0;
    for (auto& [texName, texIdx] : textureIndices) {
        fs::path ddsPath = outDir / (texName + ".dds");
        std::ofstream ddsOut(ddsPath, std::ios::binary);
        if (ddsOut.is_open()) {
            ddsOut.write((char*)textureDataList[texIdx].data(), textureDataList[texIdx].size());
            ddsOut.close();
            texExported++;
        }
    }

    if (texExported > 0) {
        Log("Exported " + std::to_string(texExported) + " textures to " + outDir.string());
    }
}

void SpiderManTool::AnalyzePCM(int index) {
    if (index == currentPcmIndex && !currentPcmInfos.empty()) return;
    currentPcmInfos.clear();
    currentPcmIndex = index;
    currentPcmSkeleton = PCMSkeletonInfo();

    if (index < 0 || index >= (int)entries.size()) return;
    const auto& e = entries[index];
    if (e.offset + e.size > pcPackData.size()) return;

    std::vector<uint8_t> pcmData(pcPackData.begin() + e.offset, pcPackData.begin() + e.offset + e.size);

    ParseMaterialEntries(pcmData);

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
        Info inf;
        inf.u1 = br.Read<uint16_t>();
        inf.type = br.Read<uint16_t>();
        inf.offset = br.Read<uint32_t>();
        inf.u2 = br.Read<uint32_t>();
        infos.push_back(inf);
    }

    for(auto& inf : infos) {
        if (inf.type != 512) continue;
        if (inf.offset + 20 > pcmData.size()) continue;

        br.Seek(inf.offset);
        uint32_t nameOfs = br.Read<uint32_t>();
        br.Skip(4);
        uint32_t numSm = br.Read<uint32_t>();
        uint32_t infSmOfs = br.Read<uint32_t>();

        uint32_t numBn = br.Read<uint32_t>();
        uint32_t ofsBn = br.Read<uint32_t>();

        if (currentPcmSkeleton.count == 0 && numBn > 0) {
            currentPcmSkeleton.count = numBn;
            currentPcmSkeleton.offset = ofsBn;
        }

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
            uint32_t smNameOfs = br.Read<uint32_t>();
            std::string smName = ReadStringTableEntry(pcmData, smNameOfs);

            MaterialDef mat = ResolveMaterialByMeshOffset(smNameOfs);

            br.Seek(smOfs + 40);

            uint32_t itype = br.Read<uint32_t>();
            uint32_t inum = br.Read<uint32_t>();
            uint32_t iofs = br.Read<uint32_t>();
            br.Skip(4);
            uint32_t vnum = br.Read<uint32_t>();
            uint32_t vofs = br.Read<uint32_t>();
            br.Skip(8);
            uint32_t stride = br.Read<uint32_t>();

            PCMMeshInfo info;
            info.name = smName;
            info.primitiveType = itype;
            info.iCount = inum;
            info.iOffset = iofs;
            info.vCount = vnum;
            info.vOffset = vofs;
            info.stride = stride;

            info.materialMeshName = mat.meshName;
            info.materialAlphaFlag = mat.alphaFlag;
            info.materialTexture = mat.textureName;
            info.isTranslucent = mat.isTranslucent;

            if (stride == 64) {
                info.hasUV = true;
                info.hasBones = true;
            } else if (stride == 24) {
                info.hasUV = true;
                info.hasBones = false;
            } else {
                info.hasUV = false;
                info.hasBones = false;
            }

            currentPcmInfos.push_back(info);
        }
    }
}

void SpiderManTool::ExportSelectedWorldMesh(bool asGlb) {
    if (selectedMeshIndex < 0 || selectedMeshIndex >= (int)previewMeshes.size()) return;

    const auto& mesh = previewMeshes[selectedMeshIndex];

    fs::path outDir = fs::current_path() / "extracted" / "world_meshes";
    fs::create_directories(outDir);

    std::string baseName = mesh.meshName.empty() ? ("mesh_" + std::to_string(selectedMeshIndex)) : mesh.meshName;

    if (asGlb) {
        // Export just this single mesh to GLB with texture
        if (mesh.positions.empty() || mesh.indices.empty()) {
            Log("No geometry data for selected mesh");
            return;
        }

        size_t vertexCount = mesh.positions.size() / 3;

        // Calculate bounding box
        float minP[3] = {1e9f, 1e9f, 1e9f};
        float maxP[3] = {-1e9f, -1e9f, -1e9f};
        for (size_t i = 0; i < mesh.positions.size(); i += 3) {
            for (int k = 0; k < 3; k++) {
                if (mesh.positions[i + k] < minP[k]) minP[k] = mesh.positions[i + k];
                if (mesh.positions[i + k] > maxP[k]) maxP[k] = mesh.positions[i + k];
            }
        }

        // Convert indices for triangle strips
        std::vector<uint16_t> triangleIndices;
        if (mesh.mode == GL_TRIANGLE_STRIP) {
            for (size_t i = 0; i + 2 < mesh.indices.size(); i++) {
                uint16_t i0 = mesh.indices[i];
                uint16_t i1 = mesh.indices[i + 1];
                uint16_t i2 = mesh.indices[i + 2];
                if (i0 == i1 || i1 == i2 || i0 == i2) continue;
                if (i % 2 == 0) {
                    triangleIndices.push_back(i0);
                    triangleIndices.push_back(i1);
                    triangleIndices.push_back(i2);
                } else {
                    triangleIndices.push_back(i0);
                    triangleIndices.push_back(i2);
                    triangleIndices.push_back(i1);
                }
            }
        } else {
            triangleIndices = mesh.indices;
        }

        if (triangleIndices.empty()) {
            Log("No valid triangles in mesh");
            return;
        }

        // Try to load the texture data
        std::vector<uint8_t> textureData;
        bool hasTexture = false;

        if (!mesh.textureName.empty()) {
            // Try to find texture in global index by name
            std::string texNameLower = StrToLower(mesh.textureName);
            if (globalTextureNameIndex.count(texNameLower)) {
                auto& loc = globalTextureNameIndex[texNameLower];
                std::ifstream texFile(loc.packPath, std::ios::binary);
                if (texFile.is_open()) {
                    texFile.seekg(loc.offset);
                    textureData.resize(loc.size);
                    texFile.read((char*)textureData.data(), loc.size);
                    texFile.close();
                    hasTexture = true;
                    Log("Found texture: " + mesh.textureName);
                }
            }
            // Try with hash
            if (!hasTexture && mesh.textureHash != 0 && globalTextureIndex.count(mesh.textureHash)) {
                auto& loc = globalTextureIndex[mesh.textureHash];
                std::ifstream texFile(loc.packPath, std::ios::binary);
                if (texFile.is_open()) {
                    texFile.seekg(loc.offset);
                    textureData.resize(loc.size);
                    texFile.read((char*)textureData.data(), loc.size);
                    texFile.close();
                    hasTexture = true;
                    Log("Found texture by hash: " + mesh.textureName);
                }
            }
        }

        // Build GLB manually with texture support
        std::vector<uint8_t> binBuffer;
        auto alignBuffer = [&binBuffer]() {
            while (binBuffer.size() % 4 != 0) binBuffer.push_back(0);
        };
        auto addToBuffer = [&binBuffer](const void* data, size_t size) -> int {
            int offset = (int)binBuffer.size();
            const uint8_t* ptr = (const uint8_t*)data;
            binBuffer.insert(binBuffer.end(), ptr, ptr + size);
            return offset;
        };

        // Add position data
        alignBuffer();
        int posOffset = addToBuffer(mesh.positions.data(), mesh.positions.size() * sizeof(float));
        int posLength = (int)(mesh.positions.size() * sizeof(float));

        // Add normal data
        alignBuffer();
        int normOffset = addToBuffer(mesh.normals.data(), mesh.normals.size() * sizeof(float));
        int normLength = (int)(mesh.normals.size() * sizeof(float));

        // Add UV data
        alignBuffer();
        int uvOffset = addToBuffer(mesh.uvs.data(), mesh.uvs.size() * sizeof(float));
        int uvLength = (int)(mesh.uvs.size() * sizeof(float));

        // Add index data
        alignBuffer();
        int indOffset = addToBuffer(triangleIndices.data(), triangleIndices.size() * sizeof(uint16_t));
        int indLength = (int)(triangleIndices.size() * sizeof(uint16_t));

        // Add texture data if available
        int texOffset = 0, texLength = 0;
        if (hasTexture && !textureData.empty()) {
            alignBuffer();
            texOffset = addToBuffer(textureData.data(), textureData.size());
            texLength = (int)textureData.size();
        }

        alignBuffer();

        // Build JSON
        std::stringstream json;
        json << "{\"asset\":{\"version\":\"2.0\",\"generator\":\"USM\"},";

        // Buffer views
        json << "\"bufferViews\":[";
        json << "{\"buffer\":0,\"byteOffset\":" << posOffset << ",\"byteLength\":" << posLength << ",\"target\":34962},";  // 0: positions
        json << "{\"buffer\":0,\"byteOffset\":" << normOffset << ",\"byteLength\":" << normLength << ",\"target\":34962},"; // 1: normals
        json << "{\"buffer\":0,\"byteOffset\":" << uvOffset << ",\"byteLength\":" << uvLength << ",\"target\":34962},";     // 2: UVs
        json << "{\"buffer\":0,\"byteOffset\":" << indOffset << ",\"byteLength\":" << indLength << ",\"target\":34963}";    // 3: indices
        if (hasTexture) {
            json << ",{\"buffer\":0,\"byteOffset\":" << texOffset << ",\"byteLength\":" << texLength << "}";  // 4: texture
        }
        json << "],";

        // Accessors
        json << "\"accessors\":[";
        json << "{\"bufferView\":0,\"componentType\":5126,\"count\":" << vertexCount << ",\"type\":\"VEC3\","
             << "\"min\":[" << minP[0] << "," << minP[1] << "," << minP[2] << "],"
             << "\"max\":[" << maxP[0] << "," << maxP[1] << "," << maxP[2] << "]},";  // 0: position
        json << "{\"bufferView\":1,\"componentType\":5126,\"count\":" << vertexCount << ",\"type\":\"VEC3\"},";  // 1: normal
        json << "{\"bufferView\":2,\"componentType\":5126,\"count\":" << vertexCount << ",\"type\":\"VEC2\"},";  // 2: UV
        json << "{\"bufferView\":3,\"componentType\":5123,\"count\":" << triangleIndices.size() << ",\"type\":\"SCALAR\"}"; // 3: indices
        json << "],";

        // Images, samplers, textures (if we have a texture)
        if (hasTexture) {
            json << "\"images\":[{\"bufferView\":4,\"mimeType\":\"image/vnd-ms.dds\"}],";
            json << "\"samplers\":[{\"magFilter\":9729,\"minFilter\":9729,\"wrapS\":10497,\"wrapT\":10497}],";
            json << "\"textures\":[{\"sampler\":0,\"source\":0}],";
        }

        // Materials
        json << "\"materials\":[{\"name\":\"" << baseName << "\",\"doubleSided\":true,";
        if (hasTexture) {
            json << "\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0},\"metallicFactor\":0,\"roughnessFactor\":1},";
        } else {
            json << "\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.8,0.8,0.8,1],\"metallicFactor\":0,\"roughnessFactor\":1},";
        }
        json << "\"alphaMode\":\"" << (mesh.isTranslucent ? "BLEND" : "OPAQUE") << "\"}],";

        // Meshes
        json << "\"meshes\":[{\"name\":\"" << baseName << "\",\"primitives\":[{"
             << "\"attributes\":{\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2},"
             << "\"indices\":3,\"material\":0}]}],";

        // Nodes
        json << "\"nodes\":[{\"name\":\"" << baseName << "\",\"mesh\":0}],";

        // Scene
        json << "\"scenes\":[{\"nodes\":[0]}],\"scene\":0,";

        // Buffer
        json << "\"buffers\":[{\"byteLength\":" << binBuffer.size() << "}]}";

        std::string jsonStr = json.str();
        while (jsonStr.size() % 4 != 0) jsonStr += " ";

        // Write GLB file
        fs::path glbPath = outDir / (baseName + ".glb");
        std::ofstream out(glbPath, std::ios::binary);

        uint32_t magic = 0x46546C67;  // glTF
        uint32_t version = 2;
        uint32_t totalLen = 12 + 8 + (uint32_t)jsonStr.size() + 8 + (uint32_t)binBuffer.size();

        out.write((char*)&magic, 4);
        out.write((char*)&version, 4);
        out.write((char*)&totalLen, 4);

        // JSON chunk
        uint32_t jsonLen = (uint32_t)jsonStr.size();
        uint32_t jsonType = 0x4E4F534A;  // JSON
        out.write((char*)&jsonLen, 4);
        out.write((char*)&jsonType, 4);
        out.write(jsonStr.c_str(), jsonLen);

        // Binary chunk
        uint32_t binLen = (uint32_t)binBuffer.size();
        uint32_t binType = 0x004E4942;  // BIN
        out.write((char*)&binLen, 4);
        out.write((char*)&binType, 4);
        out.write((char*)binBuffer.data(), binLen);

        out.close();

        if (hasTexture) {
            // Also export the DDS separately for easier use
            fs::path ddsPath = outDir / (baseName + ".dds");
            std::ofstream ddsOut(ddsPath, std::ios::binary);
            ddsOut.write((char*)textureData.data(), textureData.size());
            ddsOut.close();
            ShowNotification("Exported GLB + DDS to:\n" + outDir.string());
        } else {
            ShowNotification("Exported GLB to:\n" + glbPath.string() + "\n(no texture found)");
        }
    } else {
        // For PCM export, we still need the source data
        if (mesh.sourcePack.empty() || mesh.sourceSize == 0) {
            Log("No source PCM data for selected mesh");
            return;
        }

        std::ifstream file(mesh.sourcePack, std::ios::binary);
        if (!file.is_open()) {
            Log("Failed to open source pack");
            return;
        }

        file.seekg(mesh.sourceOffset);
        std::vector<uint8_t> pcmData(mesh.sourceSize);
        file.read((char*)pcmData.data(), mesh.sourceSize);
        file.close();

        fs::path pcmPath = outDir / (baseName + ".pcm");
        std::ofstream out(pcmPath, std::ios::binary);
        if (out.is_open()) {
            out.write((char*)pcmData.data(), pcmData.size());
            out.close();
            ShowNotification("Exported PCM to:\n" + pcmPath.string());
        }
    }
}