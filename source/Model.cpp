#include "SpiderManTool.h"
#include "NalIntegration.h"
#include <glad/glad.h>
#include <sstream>
#include <fstream>
#include <cmath>
#include <algorithm>
#include <map>

static constexpr int PREVIEW_MAX_BONES = 64;

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
    struct EntryInfo { uint16_t size; uint16_t tag; uint32_t dataOffset; uint32_t nameOffset; };
    std::vector<EntryInfo> entries;

    for (uint32_t i = 0; i < numEntries; i++) {
        EntryInfo e;
        e.size = br.Read<uint16_t>();
        e.tag = br.Read<uint16_t>();
        e.dataOffset = br.Read<uint32_t>();
        e.nameOffset = br.Read<uint32_t>();
        entries.push_back(e);
    }

    for (auto& e : entries) {
        if (e.tag != 256) continue;
        if (e.dataOffset + e.size > pcmData.size()) continue;

        br.Seek(e.dataOffset);

        uint32_t meshNameOfs = br.Read<uint32_t>();
        uint32_t alphaFlagOfs = br.Read<uint32_t>();

        br.Seek(e.dataOffset + 0x10);
        uint32_t shaderType = br.Read<uint32_t>();

        uint32_t textureNameOfs = 0;

        if (e.size == 80 || e.size == 88) {
            // Character materials: diffuse texture at +0x18 (NOT +0x20 which is spheremap)
            br.Seek(e.dataOffset + 0x18);
            textureNameOfs = br.Read<uint32_t>();
        }
        else {
            br.Seek(e.dataOffset + 0x60);
            textureNameOfs = br.Read<uint32_t>();
        }

        MaterialDef mat;
        mat.meshName = ReadStringTableEntry(pcmData, meshNameOfs);
        mat.alphaFlag = ReadStringTableEntry(pcmData, alphaFlagOfs);
        mat.textureName = ReadStringTableEntry(pcmData, textureNameOfs);
        mat.shaderType = shaderType;
        // Only "translucent" alpha flags use alpha blending (per openusm shader system)
        std::string alphaLower = StrToLower(mat.alphaFlag);
        mat.isTranslucent = (alphaLower.find("translucent") != std::string::npos);

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


static bool IsColorVolumeMesh(const std::string& meshName) {
    (void)meshName;
    return false;
}


static bool ShouldHideMesh(const std::string& meshName) {
    (void)meshName;
    return false;
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

    struct Vertex { float x,y,z; float nx,ny,nz; float u,v; float boneIdx[4]; float boneWgt[4]; };

    bool loadedFirstLod = false;
    for(auto& inf : infos) {
        if (inf.type != 512) continue;
        if (loadedFirstLod) break;
        loadedFirstLod = true;

        if (inf.offset + 16 > pcmData.size()) continue;

        br.Seek(inf.offset); br.Skip(8);
        uint32_t numSm = br.Read<uint32_t>(); uint32_t infSmOfs = br.Read<uint32_t>();
        if (numSm > 256 || infSmOfs >= pcmData.size()) continue;

        br.Seek(infSmOfs);
        std::vector<std::pair<uint32_t, uint32_t>> smRefs;
        for(uint32_t s=0; s<numSm; s++) {
            uint32_t matRef = br.Read<uint32_t>();
            uint32_t smOfs = br.Read<uint32_t>();
            smRefs.push_back({matRef, smOfs});
        }

        for(auto& [matRef, smOfs] : smRefs) {
            if (smOfs + 96 > pcmData.size()) continue;

            br.Seek(smOfs);
            uint32_t meshNameRef = br.Read<uint32_t>();
            uint32_t ptrShaderRef = br.Read<uint32_t>();

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

            // Read per-section bone palette (maps local bone indices to global)
            // nglMeshSection: NBones at offset +8, BonesIdx at offset +12
            PCMSectionBonePalette bonePalette;
            bonePalette.load(pcmData, smOfs);

            br.Seek(vofs);
            std::vector<Vertex> vertices;
            bool valid = true;


            float bboxMin[3] = {1e30f, 1e30f, 1e30f};
            float bboxMax[3] = {-1e30f, -1e30f, -1e30f};

            for(uint32_t v=0; v<vnum; v++) {
                size_t startV = br.Tell();
                if (startV + stride > pcmData.size()) { valid = false; break; }
                Vertex vert;
                memset(&vert, 0, sizeof(vert));
                vert.x = br.Read<float>(); vert.y = br.Read<float>(); vert.z = br.Read<float>();


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
                        vert.u = br.Read<float>(); vert.v = br.Read<float>();
                    }
                    // Read bone indices and weights (4 floats each, at offset 32 and 48)
                    if (startV + 64 <= pcmData.size()) {
                        br.Seek(startV + 32);
                        for (int bi = 0; bi < 4; bi++) vert.boneIdx[bi] = br.Read<float>();
                        for (int bi = 0; bi < 4; bi++) vert.boneWgt[bi] = br.Read<float>();
                        // OpenUSM keeps blend indices local to this section palette.
                        for (int bi = 0; bi < 4; bi++) {
                            int localIdx = (int)(vert.boneIdx[bi] + 0.5f);
                            int localLimit = bonePalette.palette.empty()
                                ? PREVIEW_MAX_BONES
                                : std::min(PREVIEW_MAX_BONES, (int)bonePalette.palette.size());
                            if (localIdx >= 0 && localIdx < localLimit && vert.boneWgt[bi] > 0.f) {
                                vert.boneIdx[bi] = (float)localIdx;
                            } else {
                                vert.boneIdx[bi] = 0.f;
                                vert.boneWgt[bi] = 0.f;
                            }
                        }
                        // Normalize weights
                        float wTotal = vert.boneWgt[0] + vert.boneWgt[1] + vert.boneWgt[2] + vert.boneWgt[3];
                        if (wTotal > 1e-8f) {
                            float inv = 1.f / wTotal;
                            for (int bi = 0; bi < 4; bi++) vert.boneWgt[bi] *= inv;
                        } else {
                            for (int bi = 0; bi < 4; bi++) vert.boneWgt[bi] = 0.f;
                        }
                    }
                } else if (stride == 24) {
                    if (startV + 20 <= pcmData.size()) {
                        br.Seek(startV + 12);
                        vert.u = br.Read<float>(); vert.v = br.Read<float>();
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


            if (stride != 64 && !vertices.empty() && !indices.empty()) {

                for (auto& v : vertices) {
                    v.nx = 0; v.ny = 0; v.nz = 0;
                }


                if (itype == 4) {

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

                    for (size_t i = 0; i + 2 < indices.size(); i++) {
                        uint16_t i0 = indices[i], i1 = indices[i+1], i2 = indices[i+2];
                        if (i0 == i1 || i1 == i2 || i0 == i2) continue;
                        if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size()) continue;

                        float e1[3] = { vertices[i1].x - vertices[i0].x, vertices[i1].y - vertices[i0].y, vertices[i1].z - vertices[i0].z };
                        float e2[3] = { vertices[i2].x - vertices[i0].x, vertices[i2].y - vertices[i0].y, vertices[i2].z - vertices[i0].z };
                        float n[3];
                        Cross(e1, e2, n);


                        if (i % 2 == 1) { n[0] = -n[0]; n[1] = -n[1]; n[2] = -n[2]; }

                        vertices[i0].nx += n[0]; vertices[i0].ny += n[1]; vertices[i0].nz += n[2];
                        vertices[i1].nx += n[0]; vertices[i1].ny += n[1]; vertices[i1].nz += n[2];
                        vertices[i2].nx += n[0]; vertices[i2].ny += n[1]; vertices[i2].nz += n[2];
                    }
                }


                for (auto& v : vertices) {
                    float len = sqrt(v.nx*v.nx + v.ny*v.ny + v.nz*v.nz);
                    if (len > 0.0001f) {
                        v.nx /= len; v.ny /= len; v.nz /= len;
                    } else {
                        v.ny = 1.0f;
                    }
                }
            }

            RenderMesh mesh;
            mesh.indexCount = (int)indices.size();
            mesh.mode = (itype == 4) ? GL_TRIANGLES : GL_TRIANGLE_STRIP;
            mesh.textureId = tex;
            mesh.isTranslucent = mat.isTranslucent;
            mesh.shaderType = mat.shaderType;
            mesh.bonePalette = bonePalette.palette;


            std::string texNameLower = StrToLower(mat.textureName);
            mesh.isFakeShadow = (texNameLower.find("fake_shadow") != std::string::npos);


            std::string meshNameLower = StrToLower(meshName.empty() ? modelName : meshName);
            mesh.isColorVolume = IsColorVolumeMesh(meshNameLower);


            mesh.isHidden = ShouldHideMesh(meshNameLower);


            for (int i = 0; i < 3; i++) {
                mesh.bboxMin[i] = bboxMin[i];
                mesh.bboxMax[i] = bboxMax[i];
            }


            mesh.sourcePack = sourcePack;
            mesh.sourceOffset = sourceOffset;
            mesh.sourceSize = (uint32_t)pcmData.size();
            mesh.meshName = meshName.empty() ? modelName : meshName;


            mesh.skipPicking = (meshNameLower.find("sky") != std::string::npos) ||
                               (meshNameLower.find("ocean") != std::string::npos) ||
                               (meshNameLower.find("colvol") != std::string::npos) ||
                               (meshNameLower.find("shadow") != std::string::npos) ||
                               mesh.isColorVolume;


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
            glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)(8*sizeof(float))); glEnableVertexAttribArray(3);  // boneIdx
            glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)(12*sizeof(float))); glEnableVertexAttribArray(4); // boneWgt
            glBindVertexArray(0);
            previewMeshes.push_back(mesh);
        }
    }
}


static void TransformVertex(float& x, float& y, float& z, const float* m) {
    float ox = x, oy = y, oz = z;
    x = m[0]*ox + m[4]*oy + m[8]*oz + m[12];
    y = m[1]*ox + m[5]*oy + m[9]*oz + m[13];
    z = m[2]*ox + m[6]*oy + m[10]*oz + m[14];
}


static void TransformNormal(float& nx, float& ny, float& nz, const float* m) {
    float ox = nx, oy = ny, oz = nz;
    nx = m[0]*ox + m[4]*oy + m[8]*oz;
    ny = m[1]*ox + m[5]*oy + m[9]*oz;
    nz = m[2]*ox + m[6]*oy + m[10]*oz;

    float len = sqrt(nx*nx + ny*ny + nz*nz);
    if (len > 0.0001f) { nx /= len; ny /= len; nz /= len; }
}

void SpiderManTool::BatchWorldMeshesByType() {
    if (!isWorldMode || previewMeshes.empty()) return;

    struct BatchVertex {
        float x, y, z;
        float nx, ny, nz;
        float u, v;
        float boneIdx[4];
        float boneWgt[4];
    };

    auto canBatch = [](const RenderMesh& m) {
        return !m.isHidden &&
               !m.skipPicking &&
               !m.isFakeShadow &&
               !m.isColorVolume &&
               m.indexCount > 0 &&
               !m.positions.empty() &&
               !m.indices.empty();
    };

    auto makeKey = [](const RenderMesh& m) {
        std::ostringstream ss;
        ss << m.sourcePack << '|'
           << m.sourceOffset << '|'
           << m.sourceSize << '|'
           << m.meshName << '|'
           << m.textureId << '|'
           << m.textureName << '|'
           << m.textureHash << '|'
           << m.shaderType << '|'
           << m.isTranslucent;
        return ss.str();
    };

    auto appendTriangles = [](const RenderMesh& m, std::vector<uint16_t>& out) {
        size_t vertexCount = m.positions.size() / 3;
        if (m.mode == GL_TRIANGLES) {
            for (size_t i = 0; i + 2 < m.indices.size(); i += 3) {
                uint16_t i0 = m.indices[i];
                uint16_t i1 = m.indices[i + 1];
                uint16_t i2 = m.indices[i + 2];
                if (i0 == i1 || i1 == i2 || i0 == i2) continue;
                if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount) continue;
                out.push_back(i0);
                out.push_back(i1);
                out.push_back(i2);
            }
        } else if (m.mode == GL_TRIANGLE_STRIP) {
            for (size_t i = 0; i + 2 < m.indices.size(); i++) {
                uint16_t i0 = m.indices[i];
                uint16_t i1 = m.indices[i + 1];
                uint16_t i2 = m.indices[i + 2];
                if (i0 == i1 || i1 == i2 || i0 == i2) continue;
                if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount) continue;
                if ((i & 1) == 0) {
                    out.push_back(i0);
                    out.push_back(i1);
                    out.push_back(i2);
                } else {
                    out.push_back(i1);
                    out.push_back(i0);
                    out.push_back(i2);
                }
            }
        }
    };

    std::map<std::string, std::vector<int>> groups;
    for (int i = 0; i < (int)previewMeshes.size(); i++) {
        if (canBatch(previewMeshes[i])) {
            groups[makeKey(previewMeshes[i])].push_back(i);
        }
    }

    std::vector<char> consumed(previewMeshes.size(), 0);
    std::vector<RenderMesh> batchedMeshes;
    int sourceMeshCount = 0;
    int batchCount = 0;

    auto uploadBatch = [&](const RenderMesh& proto,
                           const std::vector<BatchVertex>& vertices,
                           const std::vector<uint16_t>& indices,
                           const float bboxMin[3],
                           const float bboxMax[3],
                           int mergedCount,
                           int chunkIndex) {
        if (vertices.empty() || indices.empty()) return;

        RenderMesh mesh = proto;
        mesh.vao = 0;
        mesh.vbo = 0;
        mesh.ebo = 0;
        mesh.mode = GL_TRIANGLES;
        mesh.indexCount = (int)indices.size();
        mesh.bonePalette.clear();
        mesh.positions.clear();
        mesh.normals.clear();
        mesh.uvs.clear();
        mesh.indices.clear();
        mesh.meshName = proto.meshName + " x" + std::to_string(mergedCount);
        if (chunkIndex > 0) mesh.meshName += " #" + std::to_string(chunkIndex + 1);
        for (int axis = 0; axis < 3; axis++) {
            mesh.bboxMin[axis] = bboxMin[axis];
            mesh.bboxMax[axis] = bboxMax[axis];
        }

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

        glGenVertexArrays(1, &mesh.vao);
        glGenBuffers(1, &mesh.vbo);
        glGenBuffers(1, &mesh.ebo);
        glBindVertexArray(mesh.vao);
        glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo);
        glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(BatchVertex), vertices.data(), GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(uint16_t), indices.data(), GL_STATIC_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(BatchVertex), (void*)0);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(BatchVertex), (void*)(3 * sizeof(float)));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(BatchVertex), (void*)(6 * sizeof(float)));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, sizeof(BatchVertex), (void*)(8 * sizeof(float)));
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, sizeof(BatchVertex), (void*)(12 * sizeof(float)));
        glEnableVertexAttribArray(4);
        glBindVertexArray(0);

        batchedMeshes.push_back(std::move(mesh));
        batchCount++;
    };

    for (const auto& [key, indicesInGroup] : groups) {
        if (indicesInGroup.size() < 2) continue;

        const RenderMesh& proto = previewMeshes[indicesInGroup[0]];
        std::vector<BatchVertex> vertices;
        std::vector<uint16_t> indices;
        int mergedInChunk = 0;
        int chunkIndex = 0;
        float bboxMin[3] = {1e30f, 1e30f, 1e30f};
        float bboxMax[3] = {-1e30f, -1e30f, -1e30f};

        auto flushChunk = [&]() {
            uploadBatch(proto, vertices, indices, bboxMin, bboxMax, mergedInChunk, chunkIndex);
            vertices.clear();
            indices.clear();
            mergedInChunk = 0;
            chunkIndex++;
            bboxMin[0] = bboxMin[1] = bboxMin[2] = 1e30f;
            bboxMax[0] = bboxMax[1] = bboxMax[2] = -1e30f;
        };

        for (int meshIdx : indicesInGroup) {
            const RenderMesh& m = previewMeshes[meshIdx];
            size_t vertexCount = m.positions.size() / 3;
            if (vertexCount == 0 || vertexCount > 65535) continue;

            std::vector<uint16_t> meshTriangles;
            appendTriangles(m, meshTriangles);
            if (meshTriangles.empty()) continue;

            if (!vertices.empty() && vertices.size() + vertexCount > 65535) {
                flushChunk();
            }

            uint16_t baseVertex = (uint16_t)vertices.size();
            for (size_t v = 0; v < vertexCount; v++) {
                BatchVertex bv{};
                bv.x = m.positions[v * 3 + 0];
                bv.y = m.positions[v * 3 + 1];
                bv.z = m.positions[v * 3 + 2];
                if (v * 3 + 2 < m.normals.size()) {
                    bv.nx = m.normals[v * 3 + 0];
                    bv.ny = m.normals[v * 3 + 1];
                    bv.nz = m.normals[v * 3 + 2];
                } else {
                    bv.ny = 1.0f;
                }
                if (v * 2 + 1 < m.uvs.size()) {
                    bv.u = m.uvs[v * 2 + 0];
                    bv.v = m.uvs[v * 2 + 1];
                }
                vertices.push_back(bv);
            }

            for (uint16_t idx : meshTriangles) {
                indices.push_back((uint16_t)(baseVertex + idx));
            }

            for (int axis = 0; axis < 3; axis++) {
                bboxMin[axis] = std::min(bboxMin[axis], m.bboxMin[axis]);
                bboxMax[axis] = std::max(bboxMax[axis], m.bboxMax[axis]);
            }

            consumed[meshIdx] = 1;
            mergedInChunk++;
            sourceMeshCount++;
        }

        if (mergedInChunk > 0) flushChunk();
    }

    if (sourceMeshCount == 0) return;

    std::vector<RenderMesh> rebuilt;
    rebuilt.reserve(previewMeshes.size() - sourceMeshCount + batchedMeshes.size());
    for (size_t i = 0; i < previewMeshes.size(); i++) {
        if (consumed[i]) {
            if (previewMeshes[i].vao) glDeleteVertexArrays(1, &previewMeshes[i].vao);
            if (previewMeshes[i].vbo) glDeleteBuffers(1, &previewMeshes[i].vbo);
            if (previewMeshes[i].ebo) glDeleteBuffers(1, &previewMeshes[i].ebo);
            continue;
        }
        rebuilt.push_back(std::move(previewMeshes[i]));
    }
    for (auto& mesh : batchedMeshes) {
        rebuilt.push_back(std::move(mesh));
    }

    int oldCount = (int)previewMeshes.size();
    previewMeshes = std::move(rebuilt);
    selectedMeshIndex = -1;
    Log("World batching: " + std::to_string(sourceMeshCount) + " repeated meshes -> "
        + std::to_string(batchCount) + " batches (" + std::to_string(oldCount)
        + " -> " + std::to_string(previewMeshes.size()) + " draw meshes)");
}

void SpiderManTool::AddMeshFromDataWithTransform(const std::vector<uint8_t>& pcmData, std::string modelName, std::function<unsigned int(uint32_t)> textureResolver, const std::string& sourcePack, uint32_t sourceOffset, const float* transform, uint32_t onlyMeshOffset) {
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

    struct Vertex { float x,y,z; float nx,ny,nz; float u,v; float boneIdx[4]; float boneWgt[4]; };

    bool loadedFirstLod = false;
    for(auto& inf : infos) {
        if (inf.type != 512) continue;
        if (onlyMeshOffset != 0xFFFFFFFFu && inf.offset != onlyMeshOffset) continue;
        if (!isWorldMode && loadedFirstLod) break;
        loadedFirstLod = true;

        if (inf.offset + 16 > pcmData.size()) continue;

        br.Seek(inf.offset); br.Skip(8);
        uint32_t numSm = br.Read<uint32_t>(); uint32_t infSmOfs = br.Read<uint32_t>();
        if (numSm > 256 || infSmOfs >= pcmData.size()) continue;

        br.Seek(infSmOfs);
        std::vector<std::pair<uint32_t, uint32_t>> smRefs;
        for(uint32_t s=0; s<numSm; s++) {
            uint32_t matRef = br.Read<uint32_t>();
            uint32_t smOfs = br.Read<uint32_t>();
            smRefs.push_back({matRef, smOfs});
        }

        for(auto& [matRef, smOfs] : smRefs) {
            if (smOfs + 96 > pcmData.size()) continue;

            br.Seek(smOfs);
            uint32_t meshNameRef = br.Read<uint32_t>();
            uint32_t ptrShaderRef = br.Read<uint32_t>();

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

            // Read per-section bone palette
            PCMSectionBonePalette bonePalette;
            bonePalette.load(pcmData, smOfs);

            br.Seek(vofs);
            std::vector<Vertex> vertices;
            bool valid = true;

            float bboxMin[3] = {1e30f, 1e30f, 1e30f};
            float bboxMax[3] = {-1e30f, -1e30f, -1e30f};

            for(uint32_t v=0; v<vnum; v++) {
                size_t startV = br.Tell();
                if (startV + stride > pcmData.size()) { valid = false; break; }
                Vertex vert;
                memset(&vert, 0, sizeof(vert));
                vert.x = br.Read<float>(); vert.y = br.Read<float>(); vert.z = br.Read<float>();

                if (stride == 64) {
                    if (startV + 24 <= pcmData.size()) {
                        br.Seek(startV + 12);
                        vert.nx = br.Read<float>(); vert.ny = br.Read<float>(); vert.nz = br.Read<float>();
                    }
                    if (startV + 32 <= pcmData.size()) {
                        br.Seek(startV + 24);
                        vert.u = br.Read<float>(); vert.v = br.Read<float>();
                    }
                    // Read bone indices and weights
                    if (startV + 64 <= pcmData.size()) {
                        br.Seek(startV + 32);
                        for (int bi = 0; bi < 4; bi++) vert.boneIdx[bi] = br.Read<float>();
                        for (int bi = 0; bi < 4; bi++) vert.boneWgt[bi] = br.Read<float>();
                        for (int bi = 0; bi < 4; bi++) {
                            int localIdx = (int)(vert.boneIdx[bi] + 0.5f);
                            int localLimit = bonePalette.palette.empty()
                                ? PREVIEW_MAX_BONES
                                : std::min(PREVIEW_MAX_BONES, (int)bonePalette.palette.size());
                            if (localIdx >= 0 && localIdx < localLimit && vert.boneWgt[bi] > 0.f) {
                                vert.boneIdx[bi] = (float)localIdx;
                            } else {
                                vert.boneIdx[bi] = 0.f;
                                vert.boneWgt[bi] = 0.f;
                            }
                        }
                        float wTotal = vert.boneWgt[0] + vert.boneWgt[1] + vert.boneWgt[2] + vert.boneWgt[3];
                        if (wTotal > 1e-8f) {
                            float inv = 1.f / wTotal;
                            for (int bi = 0; bi < 4; bi++) vert.boneWgt[bi] *= inv;
                        } else {
                            for (int bi = 0; bi < 4; bi++) vert.boneWgt[bi] = 0.f;
                        }
                    }
                } else if (stride == 24) {
                    if (startV + 20 <= pcmData.size()) {
                        br.Seek(startV + 12);
                        vert.u = br.Read<float>(); vert.v = br.Read<float>();
                    }
                }


                if (transform) {
                    TransformVertex(vert.x, vert.y, vert.z, transform);
                    if (stride == 64) {
                        TransformNormal(vert.nx, vert.ny, vert.nz, transform);
                    }
                }


                if (vert.x < bboxMin[0]) bboxMin[0] = vert.x;
                if (vert.y < bboxMin[1]) bboxMin[1] = vert.y;
                if (vert.z < bboxMin[2]) bboxMin[2] = vert.z;
                if (vert.x > bboxMax[0]) bboxMax[0] = vert.x;
                if (vert.y > bboxMax[1]) bboxMax[1] = vert.y;
                if (vert.z > bboxMax[2]) bboxMax[2] = vert.z;

                vertices.push_back(vert);
                br.Seek(startV + stride);
            }
            if (!valid) continue;

            br.Seek(iofs);
            std::vector<uint16_t> indices;
            if (iofs + inum * 2 > pcmData.size()) continue;
            for(uint32_t i=0; i<inum; i++) indices.push_back(br.Read<uint16_t>());
            if (vertices.empty() || indices.empty()) continue;


            if (stride != 64 && !vertices.empty() && !indices.empty()) {
                for (auto& v : vertices) { v.nx = 0; v.ny = 0; v.nz = 0; }

                if (itype == 4) {
                    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
                        uint16_t i0 = indices[i], i1 = indices[i+1], i2 = indices[i+2];
                        if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size()) continue;
                        float e1[3] = { vertices[i1].x - vertices[i0].x, vertices[i1].y - vertices[i0].y, vertices[i1].z - vertices[i0].z };
                        float e2[3] = { vertices[i2].x - vertices[i0].x, vertices[i2].y - vertices[i0].y, vertices[i2].z - vertices[i0].z };
                        float n[3]; Cross(e1, e2, n);
                        vertices[i0].nx += n[0]; vertices[i0].ny += n[1]; vertices[i0].nz += n[2];
                        vertices[i1].nx += n[0]; vertices[i1].ny += n[1]; vertices[i1].nz += n[2];
                        vertices[i2].nx += n[0]; vertices[i2].ny += n[1]; vertices[i2].nz += n[2];
                    }
                } else {
                    for (size_t i = 0; i + 2 < indices.size(); i++) {
                        uint16_t i0 = indices[i], i1 = indices[i+1], i2 = indices[i+2];
                        if (i0 == i1 || i1 == i2 || i0 == i2) continue;
                        if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size()) continue;
                        float e1[3] = { vertices[i1].x - vertices[i0].x, vertices[i1].y - vertices[i0].y, vertices[i1].z - vertices[i0].z };
                        float e2[3] = { vertices[i2].x - vertices[i0].x, vertices[i2].y - vertices[i0].y, vertices[i2].z - vertices[i0].z };
                        float n[3]; Cross(e1, e2, n);
                        if (i % 2 == 1) { n[0] = -n[0]; n[1] = -n[1]; n[2] = -n[2]; }
                        vertices[i0].nx += n[0]; vertices[i0].ny += n[1]; vertices[i0].nz += n[2];
                        vertices[i1].nx += n[0]; vertices[i1].ny += n[1]; vertices[i1].nz += n[2];
                        vertices[i2].nx += n[0]; vertices[i2].ny += n[1]; vertices[i2].nz += n[2];
                    }
                }

                for (auto& v : vertices) {
                    float len = sqrt(v.nx*v.nx + v.ny*v.ny + v.nz*v.nz);
                    if (len > 0.0001f) { v.nx /= len; v.ny /= len; v.nz /= len; }
                    else { v.ny = 1.0f; }
                }
            }

            RenderMesh mesh;
            mesh.indexCount = (int)indices.size();
            mesh.mode = (itype == 4) ? GL_TRIANGLES : GL_TRIANGLE_STRIP;
            mesh.textureId = tex;
            mesh.isTranslucent = mat.isTranslucent;
            mesh.shaderType = mat.shaderType;
            mesh.bonePalette = bonePalette.palette;

            std::string texNameLower = StrToLower(mat.textureName);
            mesh.isFakeShadow = (texNameLower.find("fake_shadow") != std::string::npos);

            std::string meshNameLower = StrToLower(meshName.empty() ? modelName : meshName);
            mesh.isColorVolume = IsColorVolumeMesh(meshNameLower);
            mesh.isHidden = ShouldHideMesh(meshNameLower);

            for (int i = 0; i < 3; i++) {
                mesh.bboxMin[i] = bboxMin[i];
                mesh.bboxMax[i] = bboxMax[i];
            }

            mesh.sourcePack = sourcePack;
            mesh.sourceOffset = sourceOffset;
            mesh.sourceSize = (uint32_t)pcmData.size();
            mesh.meshName = meshName.empty() ? modelName : meshName;

            mesh.skipPicking = (meshNameLower.find("sky") != std::string::npos) ||
                               (meshNameLower.find("ocean") != std::string::npos) ||
                               (meshNameLower.find("colvol") != std::string::npos) ||
                               (meshNameLower.find("shadow") != std::string::npos) ||
                               mesh.isColorVolume;

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
            glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)(8*sizeof(float))); glEnableVertexAttribArray(3);
            glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)(12*sizeof(float))); glEnableVertexAttribArray(4);
            glBindVertexArray(0);
            previewMeshes.push_back(mesh);
        }
    }
}


void SpiderManTool::LoadBackgroundMeshes() {



    std::vector<std::pair<std::string, std::string>> backgroundModels = {
        {"oceanmesh", "city_arena"},
        {"sky_day", "city_arena"}
    };

    for (const auto& [modelName, packName] : backgroundModels) {

        std::string packPath;
        for (const auto& path : foundPacks) {
            std::string stem = StrToLower(path.stem().string());
            if (stem == packName) {
                packPath = path.string();
                break;
            }
        }

        if (packPath.empty()) continue;


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
                uint32_t absOffset = (uint32_t)(packDataOffset + offset);

                if (absOffset + 4 > fileSize) {
                    file.seekg(filePos);
                    continue;
                }

                file.seekg(absOffset);
                uint32_t sig = 0;
                file.read((char*)&sig, 4);

                if (file.good() && sig == 0x204D4350) {
                    std::string entryName = "";
                    if (dictionary.count(hash)) entryName = StrToLower(dictionary[hash]);
                    if (entryName == modelName) {
                        Log("Loading background mesh: " + modelName);
                        file.seekg(absOffset);
                        std::vector<uint8_t> pcmData(size);
                        file.read((char*)pcmData.data(), size);
                        AddMeshFromData(pcmData, modelName, nullptr);
                        file.close();
                        break;
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

    // Build skeleton visualization from bone data
    BuildSkeletonVisual(pcmData);

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

    bool loadedFirstLod = false;
    for(auto& inf : infos) {
        if (inf.type != 512) continue;
        if (loadedFirstLod) break;
        loadedFirstLod = true;

        if (inf.offset + 20 > pcmData.size()) continue;

        br.Seek(inf.offset);
        br.Skip(8);
        uint32_t numSm = br.Read<uint32_t>();
        uint32_t infoSmOfs = br.Read<uint32_t>();

        if (numSm > 256 || infoSmOfs >= pcmData.size()) continue;

        br.Seek(infoSmOfs);
        std::vector<std::pair<uint32_t, uint32_t>> smRefs;
        for(uint32_t s=0; s<numSm; s++) {
            uint32_t matRef = br.Read<uint32_t>();
            uint32_t smOfs = br.Read<uint32_t>();
            smRefs.push_back({matRef, smOfs});
        }

        for(auto& [matRef, smOfs] : smRefs) {
            if (smOfs + 96 > pcmData.size()) continue;

            br.Seek(smOfs);
            uint32_t meshNameRef = br.Read<uint32_t>();

            MaterialDef mat = ResolveMaterialByMeshOffset(meshNameRef);

            br.Seek(smOfs + 0x28);
            uint32_t itype = br.Read<uint32_t>();
            uint32_t inum = br.Read<uint32_t>();
            uint32_t iofs = br.Read<uint32_t>();
            br.Skip(4);
            uint32_t vnum = br.Read<uint32_t>();
            uint32_t vofs = br.Read<uint32_t>();
            br.Skip(8);
            uint32_t stride = br.Read<uint32_t>();

            if (vnum > 100000 || inum > 300000 || vofs >= pcmData.size() || iofs >= pcmData.size() || stride == 0) continue;

            // Read bone palette for this section
            PCMSectionBonePalette bonePalette;
            bonePalette.load(pcmData, smOfs);

            SubmeshData sm;
            sm.name = ReadName(meshNameRef);
            sm.textureName = mat.textureName;
            sm.isTranslucent = mat.isTranslucent;

            for (int i = 0; i < 3; i++) {
                sm.minP[i] = 1e9f;
                sm.maxP[i] = -1e9f;
            }

            br.Seek(vofs);
            for(uint32_t v=0; v<vnum; v++) {
                size_t startV = br.Tell();
                if (startV + stride > pcmData.size()) break;

                float x = br.Read<float>(); float y = br.Read<float>(); float z = br.Read<float>();
                sm.pos.push_back(x); sm.pos.push_back(y); sm.pos.push_back(z);

                if (x < sm.minP[0]) sm.minP[0] = x;
                if (y < sm.minP[1]) sm.minP[1] = y;
                if (z < sm.minP[2]) sm.minP[2] = z;
                if (x > sm.maxP[0]) sm.maxP[0] = x;
                if (y > sm.maxP[1]) sm.maxP[1] = y;
                if (z > sm.maxP[2]) sm.maxP[2] = z;

                float nx = 0, ny = 1, nz = 0;
                float u = 0, v2 = 0;

                if (stride == 64) {
                    br.Seek(startV + 12);
                    nx = br.Read<float>(); ny = br.Read<float>(); nz = br.Read<float>();
                    u = br.Read<float>(); v2 = br.Read<float>();

                    // Read bone indices + weights (4 floats each at offsets 32, 48)
                    if (startV + 64 <= pcmData.size()) {
                        br.Seek(startV + 32);
                        float bIdx[4], bWgt[4];
                        for (int bi = 0; bi < 4; bi++) bIdx[bi] = br.Read<float>();
                        for (int bi = 0; bi < 4; bi++) bWgt[bi] = br.Read<float>();

                        // Map through palette
                        for (int bi = 0; bi < 4; bi++) {
                            int localIdx = (int)bIdx[bi];
                            if (localIdx >= 0 && bWgt[bi] > 0.f && !bonePalette.palette.empty())
                                bIdx[bi] = (float)bonePalette.mapIndex(localIdx);
                            else if (bWgt[bi] <= 0.f)
                                bIdx[bi] = 0.f;
                        }
                        // Normalize weights
                        float wTotal = bWgt[0] + bWgt[1] + bWgt[2] + bWgt[3];
                        if (wTotal > 1e-8f) {
                            float inv = 1.f / wTotal;
                            for (int bi = 0; bi < 4; bi++) bWgt[bi] *= inv;
                        } else {
                            bWgt[0] = 1.f;
                        }

                        for (int bi = 0; bi < 4; bi++) sm.joints.push_back((uint16_t)bIdx[bi]);
                        for (int bi = 0; bi < 4; bi++) sm.weights.push_back(bWgt[bi]);
                    }
                } else if (stride == 24) {
                    br.Seek(startV + 12);
                    u = br.Read<float>(); v2 = br.Read<float>();
                }

                sm.norm.push_back(nx); sm.norm.push_back(ny); sm.norm.push_back(nz);
                sm.uvs.push_back(u); sm.uvs.push_back(v2);

                br.Seek(startV + stride);
            }

            br.Seek(iofs);
            for(uint32_t i=0; i<inum; i++) {
                sm.indices.push_back(br.Read<uint16_t>());
            }

            if (itype != 4 && !sm.indices.empty()) {
                std::vector<uint16_t> newIndices;
                for(size_t i=0; i+2<sm.indices.size(); i++) {
                    uint16_t i0 = sm.indices[i], i1 = sm.indices[i+1], i2 = sm.indices[i+2];
                    if (i0 == i1 || i1 == i2 || i0 == i2) continue;
                    if (i % 2 == 0) {
                        newIndices.push_back(i0); newIndices.push_back(i1); newIndices.push_back(i2);
                    } else {
                        newIndices.push_back(i0); newIndices.push_back(i2); newIndices.push_back(i1);
                    }
                }
                sm.indices = newIndices;
            }

            if (!sm.pos.empty() && !sm.indices.empty()) {
                submeshes.push_back(sm);
            }
        }
    }

    if (submeshes.empty()) return;

    // Check if any submesh has bone data
    bool hasSkinning = false;
    for (auto& sm : submeshes) {
        if (!sm.joints.empty() && !sm.weights.empty()) {
            hasSkinning = true;
            break;
        }
    }

    // Read bone matrices from PCM for inverse bind matrices
    // Look for mesh entry to get bone matrices offset
    PCMBoneMatrices boneMats;
    for (auto& inf : infos) {
        if (inf.type != 512) continue;
        boneMats.load(pcmData, inf.offset);
        break;
    }

    SkinningGLBWriter writer;

    // If we have skinning data and bone matrices, set up skin nodes
    int ibmAccessor = -1;
    if (hasSkinning && !boneMats.matrices.empty()) {
        totalBones = (uint32_t)boneMats.matrices.size();

        // Add bone nodes
        for (uint32_t bi = 0; bi < totalBones; bi++) {
            std::string boneName = "bone_" + std::to_string(bi);
            int boneNode = writer.AddNode(boneName);
            writer.AddJoint(boneNode);
        }

        // Write inverse bind matrices
        // The PCM stores bind matrices; we use them as-is for the IBM
        // (Blender/viewers will compute the actual inverse)
        std::vector<float> ibmData;
        for (auto& mat : boneMats.matrices) {
            // Convert row-major 4x4 to column-major for glTF
            for (int c = 0; c < 4; c++)
                for (int r = 0; r < 4; r++)
                    ibmData.push_back(mat[r * 4 + c]);
        }
        int ibmView = writer.AddBufferView(ibmData.data(), ibmData.size() * sizeof(float), 0);
        ibmAccessor = writer.AddAccessor(ibmView, 5126, (int)totalBones, "MAT4");
    }

    for (size_t si = 0; si < submeshes.size(); si++) {
        auto& sm = submeshes[si];

        int matIdx = writer.AddMaterial(sm.name.empty() ? "material_" + std::to_string(si) : sm.name, sm.isTranslucent);

        int posView = writer.AddBufferView(sm.pos.data(), sm.pos.size() * sizeof(float), 34962);
        int posAcc = writer.AddAccessor(posView, 5126, (int)sm.pos.size() / 3, "VEC3", sm.minP, sm.maxP);

        int normView = writer.AddBufferView(sm.norm.data(), sm.norm.size() * sizeof(float), 34962);
        int normAcc = writer.AddAccessor(normView, 5126, (int)sm.norm.size() / 3, "VEC3");

        int uvView = writer.AddBufferView(sm.uvs.data(), sm.uvs.size() * sizeof(float), 34962);
        int uvAcc = writer.AddAccessor(uvView, 5126, (int)sm.uvs.size() / 2, "VEC2");

        int indView = writer.AddBufferView(sm.indices.data(), sm.indices.size() * sizeof(uint16_t), 34963);
        int indAcc = writer.AddAccessor(indView, 5123, (int)sm.indices.size(), "SCALAR");

        int jointAcc = -1, weightAcc = -1;
        if (!sm.joints.empty() && !sm.weights.empty()) {
            int jointView = writer.AddBufferView(sm.joints.data(), sm.joints.size() * sizeof(uint16_t), 34962);
            jointAcc = writer.AddAccessor(jointView, 5123, (int)sm.joints.size() / 4, "VEC4");

            int weightView = writer.AddBufferView(sm.weights.data(), sm.weights.size() * sizeof(float), 34962);
            weightAcc = writer.AddAccessor(weightView, 5126, (int)sm.weights.size() / 4, "VEC4");
        }

        std::string meshName = sm.name.empty() ? "submesh_" + std::to_string(si) : sm.name;
        int meshIdx = writer.StartMesh(meshName);
        writer.AddPrimitive(posAcc, normAcc, uvAcc, indAcc, jointAcc, weightAcc, matIdx);
        writer.EndMesh();

        int skinIdx = (hasSkinning && jointAcc >= 0) ? 0 : -1;
        int nodeIdx = writer.AddNode(meshName, meshIdx, skinIdx);
        writer.AddToScene(nodeIdx);
    }

    writer.WriteToFile(outPath, ibmAccessor);
    Log("Converted to: " + outPath);
}

void SpiderManTool::AnalyzePCM(int index) {
    if (index < 0 || index >= (int)entries.size()) return;
    const auto& e = entries[index];
    if (e.offset + e.size > pcPackData.size()) return;

    currentPcmInfos.clear();
    currentPcmSkeleton = {0, 0};
    currentPcmIndex = index;

    std::vector<uint8_t> pcmData(pcPackData.begin() + e.offset, pcPackData.begin() + e.offset + e.size);


    AnalyzePCMDetailed(pcmData);


    BinaryReader br(pcmData);

    if (pcmData.size() < 16) return;

    br.Seek(8);
    uint32_t numEntries = br.Read<uint32_t>();
    uint32_t entryTableOfs = br.Read<uint32_t>();

    if (numEntries > 1000 || entryTableOfs >= pcmData.size()) return;

    br.Seek(entryTableOfs);
    struct EntryInfo { uint16_t type; uint16_t tag; uint32_t dataOffset; uint32_t nameOffset; };
    std::vector<EntryInfo> entryInfos;

    for (uint32_t i = 0; i < numEntries; i++) {
        EntryInfo ei;
        ei.type = br.Read<uint16_t>();
        ei.tag = br.Read<uint16_t>();
        ei.dataOffset = br.Read<uint32_t>();
        ei.nameOffset = br.Read<uint32_t>();
        entryInfos.push_back(ei);
    }

    for (auto& ei : entryInfos) {
        if (ei.tag == 512) {
            if (ei.dataOffset + 20 > pcmData.size()) continue;

            br.Seek(ei.dataOffset);
            br.Skip(8);
            uint32_t numSubMeshes = br.Read<uint32_t>();
            uint32_t subMeshInfoOfs = br.Read<uint32_t>();

            if (numSubMeshes > 256 || subMeshInfoOfs >= pcmData.size()) continue;

            br.Seek(subMeshInfoOfs);
            std::vector<uint32_t> smOffsets;
            for (uint32_t s = 0; s < numSubMeshes; s++) {
                br.Skip(4);
                smOffsets.push_back(br.Read<uint32_t>());
            }

            for (uint32_t smOfs : smOffsets) {
                if (smOfs + 80 > pcmData.size()) continue;

                br.Seek(smOfs);
                uint32_t meshNameRef = br.Read<uint32_t>();

                std::string meshName;
                if (meshNameRef != 0 && meshNameRef + 32 <= pcmData.size()) {
                    size_t strStart = meshNameRef + 4;
                    size_t end = strStart;
                    while (end < strStart + 28 && end < pcmData.size() && pcmData[end] != 0) end++;
                    meshName = std::string((char*)&pcmData[strStart], end - strStart);
                }

                br.Seek(smOfs + 40);
                uint32_t primType = br.Read<uint32_t>();
                uint32_t iCount = br.Read<uint32_t>();
                uint32_t iOffset = br.Read<uint32_t>();
                br.Skip(4);
                uint32_t vCount = br.Read<uint32_t>();
                uint32_t vOffset = br.Read<uint32_t>();
                br.Skip(8);
                uint32_t stride = br.Read<uint32_t>();

                PCMMeshInfo info;
                info.name = meshName;
                info.vCount = vCount;
                info.vOffset = vOffset;
                info.iCount = iCount;
                info.iOffset = iOffset;
                info.stride = stride;
                info.primitiveType = primType;
                info.hasUV = (stride >= 24);
                info.hasBones = (stride >= 44);

                currentPcmInfos.push_back(info);
            }
        }
        else if (ei.tag == 768) {
            br.Seek(ei.dataOffset);
            uint32_t boneCount = br.Read<uint32_t>();
            currentPcmSkeleton.count = boneCount;
            currentPcmSkeleton.offset = ei.dataOffset;
        }
    }
}


void SpiderManTool::AnalyzePCMDetailed(const std::vector<uint8_t>& pcmData) {
    currentPcmDetails = PCMFileInfo();

    if (pcmData.size() < 32) return;

    BinaryReader br(pcmData);










    br.Seek(8);
    currentPcmDetails.numEntries = br.Read<uint32_t>();
    currentPcmDetails.entryTableOffset = br.Read<uint32_t>();
    currentPcmDetails.fileSize = (uint32_t)pcmData.size();

    if (currentPcmDetails.numEntries > 1000 || currentPcmDetails.entryTableOffset >= pcmData.size()) return;








    struct AssetEntry {
        uint16_t size;
        uint16_t tag;
        uint32_t dataOffset;
        uint32_t nameOffset;
    };

    std::vector<AssetEntry> entries;
    br.Seek(currentPcmDetails.entryTableOffset);

    for (uint32_t i = 0; i < currentPcmDetails.numEntries; i++) {
        AssetEntry e;
        e.size = br.Read<uint16_t>();
        e.tag = br.Read<uint16_t>();
        e.dataOffset = br.Read<uint32_t>();
        e.nameOffset = br.Read<uint32_t>();
        entries.push_back(e);
    }

    auto readString = [&](uint32_t offset) -> std::string {
        if (offset == 0 || offset + 32 > pcmData.size()) return "";
        size_t strStart = offset + 4;
        size_t end = strStart;
        while (end < strStart + 60 && end < pcmData.size() && pcmData[end] != 0) end++;
        return std::string((char*)&pcmData[strStart], end - strStart);
    };


    for (const auto& e : entries) {
        if (e.tag != 256) continue;
        if (e.dataOffset + 0x64 > pcmData.size()) continue;

        PCMMaterialInfo mat;
        mat.nameOffset = e.nameOffset;
        mat.name = readString(e.nameOffset);
        mat.shaderSize = e.size;

        br.Seek(e.dataOffset);
        uint32_t meshNameOfs = br.Read<uint32_t>();
        uint32_t alphaFlagOfs = br.Read<uint32_t>();

        uint32_t textureNameOfs = 0;
        if (e.size == 80 || e.size == 88) {
            br.Seek(e.dataOffset + 0x18);
            textureNameOfs = br.Read<uint32_t>();
        } else {
            br.Seek(e.dataOffset + 0x60);
            textureNameOfs = br.Read<uint32_t>();
        }

        mat.meshName = readString(meshNameOfs);
        mat.alphaFlag = readString(alphaFlagOfs);
        mat.textureName = readString(textureNameOfs);
        mat.isTranslucent = (mat.alphaFlag.find("translucent") != std::string::npos) ||
                            (mat.alphaFlag.find("glass") != std::string::npos);

        currentPcmDetails.materials.push_back(mat);
    }
    currentPcmDetails.materialCount = (uint32_t)currentPcmDetails.materials.size();


    for (const auto& e : entries) {
        if (e.tag != 512) continue;
        if (e.dataOffset + 48 > pcmData.size()) continue;

        PCMLodInfo lod;
        lod.nameOffset = e.nameOffset;
        lod.name = readString(e.nameOffset);













        br.Seek(e.dataOffset);
        br.Skip(8);
        lod.submeshCount = br.Read<uint32_t>();
        uint32_t submeshRefsOfs = br.Read<uint32_t>();
        lod.boneCount = br.Read<uint32_t>();
        lod.bonesOffset = br.Read<uint32_t>();
        br.Skip(4);
        lod.nextLodOffset = br.Read<uint32_t>();



        br.Seek(e.dataOffset + 0x20);
        lod.boundsMin[0] = br.Read<float>();
        lod.boundsMin[1] = br.Read<float>();
        lod.boundsMin[2] = br.Read<float>();
        br.Skip(4);


        lod.lodDistance = 0.0f;
        if (lod.nextLodOffset > 0 && lod.nextLodOffset + 8 <= pcmData.size()) {
            br.Seek(lod.nextLodOffset + 4);
            lod.lodDistance = br.Read<float>();
        }

        currentPcmDetails.lods.push_back(lod);
        currentPcmDetails.boneCount = lod.boneCount;
        currentPcmDetails.bonesOffset = lod.bonesOffset;


        if (lod.submeshCount > 256 || submeshRefsOfs >= pcmData.size()) continue;





        br.Seek(submeshRefsOfs);
        std::vector<std::pair<uint32_t, uint32_t>> submeshRefs;
        for (uint32_t s = 0; s < lod.submeshCount; s++) {
            uint32_t matNameOfs = br.Read<uint32_t>();
            uint32_t smInfoOfs = br.Read<uint32_t>();
            submeshRefs.push_back({matNameOfs, smInfoOfs});
        }

        for (const auto& [matNameOfs, smInfoOfs] : submeshRefs) {
            if (smInfoOfs + 96 > pcmData.size()) continue;

            PCMSubmeshInfo sm;





















            br.Seek(smInfoOfs);
            sm.nameOffset = br.Read<uint32_t>();
            sm.name = readString(sm.nameOffset);

            uint32_t ptrShaderRef = br.Read<uint32_t>();

            br.Seek(smInfoOfs + 0x0C);
            sm.boneMapOffset = br.Read<uint32_t>();
            sm.boneMapCount = br.Read<uint32_t>();

            br.Seek(smInfoOfs + 0x20);
            sm.boundingRadius = br.Read<float>();

            br.Seek(smInfoOfs + 0x28);
            sm.primitiveType = br.Read<uint32_t>();
            sm.indexCount = br.Read<uint32_t>();
            sm.indexOffset = br.Read<uint32_t>();
            br.Skip(4);
            sm.vertexCount = br.Read<uint32_t>();
            sm.vertexOffset = br.Read<uint32_t>();
            sm.vertexBufferSize = br.Read<uint32_t>();
            br.Skip(4);
            sm.stride = br.Read<uint32_t>();


            sm.hasNormals = (sm.stride >= 24);
            sm.hasUV = (sm.stride >= 32);
            sm.hasBones = (sm.stride >= 44);


            for (const auto& mat : currentPcmDetails.materials) {
                uint32_t matMeshOfs = 0;

                for (const auto& me : entries) {
                    if (me.tag == 256 && readString(me.nameOffset) == mat.name) {
                        br.Seek(me.dataOffset);
                        matMeshOfs = br.Read<uint32_t>();
                        break;
                    }
                }
                if (matMeshOfs == sm.nameOffset) {
                    sm.materialMeshName = mat.meshName;
                    sm.materialAlphaFlag = mat.alphaFlag;
                    sm.materialTexture = mat.textureName;
                    sm.isTranslucent = mat.isTranslucent;
                    sm.shaderSize = mat.shaderSize;


                    switch (mat.shaderSize) {
                        case 80: sm.shaderType = "CHARACTER"; break;
                        case 88: sm.shaderType = "CHARACTER_EXT"; break;
                        case 128: sm.shaderType = "STREET"; break;
                        case 132: sm.shaderType = "WORLD"; break;
                        case 136: sm.shaderType = "TRANSLUCENT"; break;
                        default: sm.shaderType = "UNKNOWN_" + std::to_string(mat.shaderSize); break;
                    }
                    break;
                }
            }

            currentPcmDetails.submeshes.push_back(sm);
            currentPcmDetails.totalVertices += sm.vertexCount;
            currentPcmDetails.totalIndices += sm.indexCount;
        }
    }

    currentPcmDetails.lodCount = (uint32_t)currentPcmDetails.lods.size();
    currentPcmDetails.totalSubmeshes = (uint32_t)currentPcmDetails.submeshes.size();


    if (currentPcmDetails.boneCount > 0 && currentPcmDetails.bonesOffset > 0 &&
        currentPcmDetails.bonesOffset + currentPcmDetails.boneCount * 64 <= pcmData.size()) {

        for (uint32_t i = 0; i < currentPcmDetails.boneCount; i++) {
            PCMBoneInfo bone;
            bone.index = (int)i;



            uint32_t boneOfs = currentPcmDetails.bonesOffset + i * 64;
            br.Seek(boneOfs + 48);
            bone.posX = br.Read<float>();
            bone.posY = br.Read<float>();
            bone.posZ = br.Read<float>();


            // Parent hierarchy is in nalCompSkeleton (not in PCM bone data)
            bone.parentIndex = -1;
            bone.inferredRole = "";

            currentPcmDetails.bones.push_back(bone);
        }
    }


    showPcmDetailsPanel = true;
}

void SpiderManTool::ExportSelectedWorldMesh(bool asGlb) {
    if (selectedMeshIndex < 0 || selectedMeshIndex >= (int)previewMeshes.size()) {
        Log("No mesh selected");
        return;
    }

    const auto& mesh = previewMeshes[selectedMeshIndex];


    fs::path outDir = fs::current_path() / "extracted" / "world_meshes";
    fs::create_directories(outDir);


    std::string baseName = mesh.meshName;
    if (baseName.empty()) {
        baseName = "mesh_" + std::to_string(selectedMeshIndex);
    }

    for (char& c : baseName) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
            c = '_';
        }
    }

    if (asGlb) {

        if (mesh.positions.empty() || mesh.indices.empty()) {
            Log("Selected mesh has no geometry data for export");
            return;
        }


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


        float minP[3] = {1e30f, 1e30f, 1e30f};
        float maxP[3] = {-1e30f, -1e30f, -1e30f};
        int vertexCount = (int)mesh.positions.size() / 3;
        for (int i = 0; i < vertexCount; i++) {
            float x = mesh.positions[i * 3];
            float y = mesh.positions[i * 3 + 1];
            float z = mesh.positions[i * 3 + 2];
            if (x < minP[0]) minP[0] = x;
            if (y < minP[1]) minP[1] = y;
            if (z < minP[2]) minP[2] = z;
            if (x > maxP[0]) maxP[0] = x;
            if (y > maxP[1]) maxP[1] = y;
            if (z > maxP[2]) maxP[2] = z;
        }


        std::vector<uint8_t> textureData;
        bool hasTexture = false;

        if (!mesh.textureName.empty()) {
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


        alignBuffer();
        int posOffset = addToBuffer(mesh.positions.data(), mesh.positions.size() * sizeof(float));
        int posLength = (int)(mesh.positions.size() * sizeof(float));


        alignBuffer();
        int normOffset = addToBuffer(mesh.normals.data(), mesh.normals.size() * sizeof(float));
        int normLength = (int)(mesh.normals.size() * sizeof(float));


        alignBuffer();
        int uvOffset = addToBuffer(mesh.uvs.data(), mesh.uvs.size() * sizeof(float));
        int uvLength = (int)(mesh.uvs.size() * sizeof(float));


        alignBuffer();
        int indOffset = addToBuffer(triangleIndices.data(), triangleIndices.size() * sizeof(uint16_t));
        int indLength = (int)(triangleIndices.size() * sizeof(uint16_t));


        int texOffset = 0, texLength = 0;
        if (hasTexture && !textureData.empty()) {
            alignBuffer();
            texOffset = addToBuffer(textureData.data(), textureData.size());
            texLength = (int)textureData.size();
        }

        alignBuffer();


        std::stringstream json;
        json << "{\"asset\":{\"version\":\"2.0\",\"generator\":\"USM\"},";


        json << "\"bufferViews\":[";
        json << "{\"buffer\":0,\"byteOffset\":" << posOffset << ",\"byteLength\":" << posLength << ",\"target\":34962},";
        json << "{\"buffer\":0,\"byteOffset\":" << normOffset << ",\"byteLength\":" << normLength << ",\"target\":34962},";
        json << "{\"buffer\":0,\"byteOffset\":" << uvOffset << ",\"byteLength\":" << uvLength << ",\"target\":34962},";
        json << "{\"buffer\":0,\"byteOffset\":" << indOffset << ",\"byteLength\":" << indLength << ",\"target\":34963}";
        if (hasTexture) {
            json << ",{\"buffer\":0,\"byteOffset\":" << texOffset << ",\"byteLength\":" << texLength << "}";
        }
        json << "],";


        json << "\"accessors\":[";
        json << "{\"bufferView\":0,\"componentType\":5126,\"count\":" << vertexCount << ",\"type\":\"VEC3\","
             << "\"min\":[" << minP[0] << "," << minP[1] << "," << minP[2] << "],"
             << "\"max\":[" << maxP[0] << "," << maxP[1] << "," << maxP[2] << "]},";
        json << "{\"bufferView\":1,\"componentType\":5126,\"count\":" << vertexCount << ",\"type\":\"VEC3\"},";
        json << "{\"bufferView\":2,\"componentType\":5126,\"count\":" << vertexCount << ",\"type\":\"VEC2\"},";
        json << "{\"bufferView\":3,\"componentType\":5123,\"count\":" << triangleIndices.size() << ",\"type\":\"SCALAR\"}";
        json << "],";


        if (hasTexture) {
            json << "\"images\":[{\"bufferView\":4,\"mimeType\":\"image/vnd-ms.dds\"}],";
            json << "\"samplers\":[{\"magFilter\":9729,\"minFilter\":9729,\"wrapS\":10497,\"wrapT\":10497}],";
            json << "\"textures\":[{\"sampler\":0,\"source\":0}],";
        }


        json << "\"materials\":[{\"name\":\"" << baseName << "\",\"doubleSided\":true,";
        if (hasTexture) {
            json << "\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0},\"metallicFactor\":0,\"roughnessFactor\":1},";
        } else {
            json << "\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.8,0.8,0.8,1],\"metallicFactor\":0,\"roughnessFactor\":1},";
        }
        json << "\"alphaMode\":\"" << (mesh.isTranslucent ? "BLEND" : "OPAQUE") << "\"}],";


        json << "\"meshes\":[{\"name\":\"" << baseName << "\",\"primitives\":[{"
             << "\"attributes\":{\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2},"
             << "\"indices\":3,\"material\":0}]}],";


        json << "\"nodes\":[{\"name\":\"" << baseName << "\",\"mesh\":0}],";


        json << "\"scenes\":[{\"nodes\":[0]}],\"scene\":0,";


        json << "\"buffers\":[{\"byteLength\":" << binBuffer.size() << "}]}";

        std::string jsonStr = json.str();
        while (jsonStr.size() % 4 != 0) jsonStr += " ";


        fs::path glbPath = outDir / (baseName + ".glb");
        std::ofstream out(glbPath, std::ios::binary);

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

        if (hasTexture) {

            fs::path ddsPath = outDir / (baseName + ".dds");
            std::ofstream ddsOut(ddsPath, std::ios::binary);
            ddsOut.write((char*)textureData.data(), textureData.size());
            ddsOut.close();
            ShowNotification("Exported GLB + DDS to:\n" + outDir.string());
        } else {
            ShowNotification("Exported GLB to:\n" + glbPath.string() + "\n(no texture found)");
        }
    } else {

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
            ShowNotification("Exported PCM to:\n"     + pcmPath.string());
        }
    }
}
