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

// Helper function to check if a mesh name indicates a color volume
static bool IsColorVolumeMesh(const std::string& meshName) {
    std::string lower = StrToLower(meshName);

    // Check for various color volume naming patterns
    if (lower.find("col vol") != std::string::npos) return true;
    if (lower.find("col_vol") != std::string::npos) return true;
    if (lower.find("colvol") != std::string::npos) return true;
    if (lower.find("color volume") != std::string::npos) return true;
    if (lower.find("color_volume") != std::string::npos) return true;
    if (lower.find("colorvolume") != std::string::npos) return true;
    if (lower.find("colorvol") != std::string::npos) return true;
    if (lower.find("cv_alley") != std::string::npos) return true;
    if (lower.find("cv_") != std::string::npos) return true;  // cv_ prefix pattern

    return false;
}

// Helper function to check if a mesh should be completely hidden (not rendered at all)
static bool ShouldHideMesh(const std::string& meshName) {
    std::string lower = StrToLower(meshName);

    if (lower.find("gen_white") != std::string::npos) return true;

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
                        vert.u = br.Read<float>(); vert.v = br.Read<float>();  // Don't flip V
                    }
                } else if (stride == 24) {
                    if (startV + 20 <= pcmData.size()) {
                        br.Seek(startV + 12);
                        vert.u = br.Read<float>(); vert.v = br.Read<float>();  // Don't flip V
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

            // Check for color volume meshes
            std::string meshNameLower = StrToLower(meshName.empty() ? modelName : meshName);
            mesh.isColorVolume = IsColorVolumeMesh(meshNameLower);

            // Check if mesh should be hidden (gen_white, etc)
            mesh.isHidden = ShouldHideMesh(meshNameLower);

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

            // Check if this mesh should be skipped for picking (sky, ocean, collision volumes, color volumes)
            mesh.skipPicking = (meshNameLower.find("sky") != std::string::npos) ||
                               (meshNameLower.find("ocean") != std::string::npos) ||
                               (meshNameLower.find("colvol") != std::string::npos) ||
                               (meshNameLower.find("shadow") != std::string::npos) ||
                               mesh.isColorVolume;

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

// Helper to transform a vertex position by a 4x4 row-major matrix
static void TransformVertex(float& x, float& y, float& z, const float* m) {
    float ox = x, oy = y, oz = z;
    x = m[0]*ox + m[4]*oy + m[8]*oz + m[12];
    y = m[1]*ox + m[5]*oy + m[9]*oz + m[13];
    z = m[2]*ox + m[6]*oy + m[10]*oz + m[14];
}

// Helper to transform a normal by a 4x4 matrix (ignore translation)
static void TransformNormal(float& nx, float& ny, float& nz, const float* m) {
    float ox = nx, oy = ny, oz = nz;
    nx = m[0]*ox + m[4]*oy + m[8]*oz;
    ny = m[1]*ox + m[5]*oy + m[9]*oz;
    nz = m[2]*ox + m[6]*oy + m[10]*oz;
    // Renormalize
    float len = sqrt(nx*nx + ny*ny + nz*nz);
    if (len > 0.0001f) { nx /= len; ny /= len; nz /= len; }
}

void SpiderManTool::AddMeshFromDataWithTransform(const std::vector<uint8_t>& pcmData, std::string modelName, std::function<unsigned int(uint32_t)> textureResolver, const std::string& sourcePack, uint32_t sourceOffset, const float* transform) {
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

            float bboxMin[3] = {1e30f, 1e30f, 1e30f};
            float bboxMax[3] = {-1e30f, -1e30f, -1e30f};

            for(uint32_t v=0; v<vnum; v++) {
                size_t startV = br.Tell();
                if (startV + stride > pcmData.size()) { valid = false; break; }
                Vertex vert;
                vert.x = br.Read<float>(); vert.y = br.Read<float>(); vert.z = br.Read<float>();
                vert.nx = 0; vert.ny = 0; vert.nz = 0;
                vert.u = 0; vert.v = 0;

                if (stride == 64) {
                    if (startV + 24 <= pcmData.size()) {
                        br.Seek(startV + 12);
                        vert.nx = br.Read<float>(); vert.ny = br.Read<float>(); vert.nz = br.Read<float>();
                    }
                    if (startV + 32 <= pcmData.size()) {
                        br.Seek(startV + 24);
                        vert.u = br.Read<float>(); vert.v = br.Read<float>();
                    }
                } else if (stride == 24) {
                    if (startV + 20 <= pcmData.size()) {
                        br.Seek(startV + 12);
                        vert.u = br.Read<float>(); vert.v = br.Read<float>();
                    }
                }

                // Apply transform if provided
                if (transform) {
                    TransformVertex(vert.x, vert.y, vert.z, transform);
                    if (stride == 64) {
                        TransformNormal(vert.nx, vert.ny, vert.nz, transform);
                    }
                }

                // Update bounding box after transform
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

            // Compute normals if not present
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

                if (file.good() && sig == 0x204D4350) { // PCM
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
        br.Skip(8);
        uint32_t numSm = br.Read<uint32_t>();
        uint32_t infoSmOfs = br.Read<uint32_t>();

        if (numSm > 256 || infoSmOfs >= pcmData.size()) continue;

        br.Seek(infoSmOfs);
        std::vector<uint32_t> smOffsets;
        for(uint32_t s=0; s<numSm; s++) {
            br.Skip(4);
            smOffsets.push_back(br.Read<uint32_t>());
        }

        for(uint32_t smOfs : smOffsets) {
            if (smOfs + 80 > pcmData.size()) continue;

            br.Seek(smOfs);
            uint32_t meshNameRef = br.Read<uint32_t>();

            MaterialDef mat = ResolveMaterialByMeshOffset(meshNameRef);

            br.Seek(smOfs + 40);
            uint32_t itype = br.Read<uint32_t>();
            uint32_t inum = br.Read<uint32_t>();
            uint32_t iofs = br.Read<uint32_t>();
            br.Skip(4);
            uint32_t vnum = br.Read<uint32_t>();
            uint32_t vofs = br.Read<uint32_t>();
            br.Skip(8);
            uint32_t stride = br.Read<uint32_t>();

            if (vnum > 100000 || inum > 300000 || vofs >= pcmData.size() || iofs >= pcmData.size() || stride == 0) continue;

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
                    u = br.Read<float>(); v2 = br.Read<float>();  // Don't flip V for export
                } else if (stride == 24) {
                    br.Seek(startV + 12);
                    u = br.Read<float>(); v2 = br.Read<float>();  // Don't flip V for export
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

    SkinningGLBWriter writer;

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

        std::string meshName = sm.name.empty() ? "submesh_" + std::to_string(si) : sm.name;
        int meshIdx = writer.StartMesh(meshName);
        writer.AddPrimitive(posAcc, normAcc, uvAcc, indAcc, -1, -1, matIdx);
        writer.EndMesh();

        int nodeIdx = writer.AddNode(meshName, meshIdx);
        writer.AddToScene(nodeIdx);
    }

    writer.WriteToFile(outPath);
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

void SpiderManTool::ExportSelectedWorldMesh(bool asGlb) {
    if (selectedMeshIndex < 0 || selectedMeshIndex >= (int)previewMeshes.size()) {
        Log("No mesh selected");
        return;
    }

    const auto& mesh = previewMeshes[selectedMeshIndex];

    // Create output directory
    fs::path outDir = fs::current_path() / "extracted" / "world_meshes";
    fs::create_directories(outDir);

    // Generate a base name from the mesh
    std::string baseName = mesh.meshName;
    if (baseName.empty()) {
        baseName = "mesh_" + std::to_string(selectedMeshIndex);
    }
    // Clean up the name
    for (char& c : baseName) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
            c = '_';
        }
    }

    if (asGlb) {
        // Export as GLB with embedded texture if available
        if (mesh.positions.empty() || mesh.indices.empty()) {
            Log("Selected mesh has no geometry data for export");
            return;
        }

        // Convert triangle strip to triangle list if needed
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

        // Calculate bounds
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

        // Try to get texture data
        std::vector<uint8_t> textureData;
        bool hasTexture = false;

        if (!mesh.textureName.empty()) {
            std::string texNameLower = StrToLower(mesh.textureName);
            // Try without .dds extension first
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