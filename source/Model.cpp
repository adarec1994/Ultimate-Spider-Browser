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
                vert.nx = 0; vert.ny = 1; vert.nz = 0;
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

            // Store positions for triangle picking
            mesh.positions.reserve(vertices.size() * 3);
            for (const auto& v : vertices) {
                mesh.positions.push_back(v.x);
                mesh.positions.push_back(v.y);
                mesh.positions.push_back(v.z);
            }
            mesh.indices = indices;

            // Store source info for hex editor
            mesh.sourcePack = sourcePack;
            mesh.sourceOffset = sourceOffset;
            mesh.sourceSize = (uint32_t)pcmData.size();
            mesh.meshName = meshName.empty() ? modelName : meshName;

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

    SkinningGLBWriter glb;
    std::vector<float> allIBMs;

    auto ReadName = [&](uint32_t offset) -> std::string {
        return ReadStringTableEntry(pcmData, offset);
    };

    for(auto& inf : infos) {
        if (inf.type != 512) continue;
        if (inf.offset + 20 > pcmData.size()) continue;

        br.Seek(inf.offset);
        uint32_t mdlNameOfs = br.Read<uint32_t>();
        std::string modelName = ReadName(mdlNameOfs);
        if (modelName.empty()) modelName = "Model";

        br.Skip(4);
        uint32_t numSm = br.Read<uint32_t>();
        uint32_t infSmOfs = br.Read<uint32_t>();
        uint32_t numBn = br.Read<uint32_t>();
        uint32_t ofsBn = br.Read<uint32_t>();

        if (numBn > 0 && ofsBn > 0 && ofsBn + numBn * 64 <= pcmData.size() && allIBMs.empty()) {
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

            for (uint32_t b = 0; b < numBn; b++) {
                int nodeIdx = glb.AddNode("Bone_" + std::to_string(b));
                glb.AddJoint(nodeIdx);
            }

            int armatureNode = glb.AddNode("Armature");
            glb.AddToScene(armatureNode);
        }

        if (infSmOfs >= pcmData.size()) continue;
        br.Seek(infSmOfs);
        std::vector<uint32_t> smOffsets;
        for (uint32_t s = 0; s < numSm; s++) { br.Skip(4); smOffsets.push_back(br.Read<uint32_t>()); }

        std::vector<int> currentModelChildren;

        for (size_t smIdx = 0; smIdx < smOffsets.size(); smIdx++) {
            uint32_t smOfs = smOffsets[smIdx];
            if (smOfs + 80 > pcmData.size()) continue;

            br.Seek(smOfs);
            uint32_t smNameOfs = br.Read<uint32_t>();
            std::string smName = ReadName(smNameOfs);
            if (smName.empty()) smName = "Submesh_" + std::to_string(smIdx);

            MaterialDef mat = ResolveMaterialByMeshOffset(smNameOfs);

            int matIdx = glb.AddMaterial(
                mat.textureName.empty() ? smName : mat.textureName,
                mat.isTranslucent
            );

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

            br.Seek(vofs);
            std::vector<float> pos, norm, uvs, weights;
            std::vector<uint16_t> joints;

            for (uint32_t v = 0; v < vnum; v++) {
                size_t startV = br.Tell();
                if (startV + stride > pcmData.size()) break;
                pos.push_back(br.Read<float>()); pos.push_back(br.Read<float>()); pos.push_back(br.Read<float>());

                if (stride == 64) {
                    br.Seek(startV + 12);
                    float nx = br.Read<float>(); float ny = br.Read<float>(); float nz = br.Read<float>();
                    norm.push_back(nx); norm.push_back(ny); norm.push_back(nz);
                    br.Seek(startV + 24);
                    uvs.push_back(br.Read<float>()); uvs.push_back(1.0f - br.Read<float>());
                    br.Seek(startV + 32);
                    for(int k=0; k<4; k++) {
                        float val = br.Read<float>();
                        int idx = (int)val;
                        if (idx < 0 || (numBn > 0 && idx >= (int)numBn)) idx = 0;
                        joints.push_back((uint16_t)idx);
                    }
                    br.Seek(startV + 48);
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
            if(!joints.empty()) jointAcc = glb.AddAccessor(glb.AddBufferView(joints.data(), joints.size()*2, 34963), 5123, vnum, "VEC4");
            if(!weights.empty()) weightAcc = glb.AddAccessor(glb.AddBufferView(weights.data(), weights.size()*4, 34962), 5126, vnum, "VEC4");

            int meshIdx = glb.StartMesh(smName);
            glb.AddPrimitive(posAcc, normAcc, uvAcc, indAcc, jointAcc, weightAcc, matIdx);
            glb.EndMesh();

            int meshNodeIdx = glb.AddNode(smName, meshIdx, (glb.HasJoints() ? 0 : -1));
            currentModelChildren.push_back(meshNodeIdx);
        }

        if (!currentModelChildren.empty()) {
            int containerIdx = glb.AddNode(modelName, -1, -1, nullptr, currentModelChildren);
            glb.AddToScene(containerIdx);
        }
    }

    int ibmAccIndex = -1;
    if (!allIBMs.empty()) {
        ibmAccIndex = glb.AddAccessor(glb.AddBufferView(allIBMs.data(), allIBMs.size()*4, 0), 5126, (int)allIBMs.size() / 16, "MAT4");
    }
    glb.WriteToFile(outPath, ibmAccIndex);
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
    if (mesh.sourcePack.empty() || mesh.sourceSize == 0) {
        Log("No source data for selected mesh");
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

    fs::path outDir = fs::current_path() / "extracted" / "world_meshes";
    fs::create_directories(outDir);

    std::string baseName = mesh.meshName.empty() ? ("mesh_" + std::to_string(selectedMeshIndex)) : mesh.meshName;

    if (asGlb) {
        fs::path glbPath = outDir / (baseName + ".glb");
        ConvertPCM(pcmData, glbPath.string());
        ShowNotification("Exported GLB to:\n" + glbPath.string());
    } else {
        fs::path pcmPath = outDir / (baseName + ".pcm");
        std::ofstream out(pcmPath, std::ios::binary);
        if (out.is_open()) {
            out.write((char*)pcmData.data(), pcmData.size());
            out.close();
            ShowNotification("Exported PCM to:\n" + pcmPath.string());
        }
    }
}