#include "SpiderManTool.h"
#include <glad/glad.h>
#include <sstream>

// Read a string from string table entry (4-byte hash + 28-byte null-padded string)
static std::string ReadStringTableEntry(const std::vector<uint8_t>& data, uint32_t offset) {
    if (offset == 0 || offset + 32 > data.size()) return "";
    // Skip 4-byte hash, read up to 28 chars
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

    // Parse entry table
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

    // Parse material entries (tag 256)
    for (auto& e : entries) {
        if (e.tag != 256) continue;
        if (e.dataOffset + 0x64 > pcmData.size()) continue;

        br.Seek(e.dataOffset);

        // Material structure:
        // +0x00: mesh_name offset (string table)
        // +0x04: alpha_flag offset (string table) - "smsimple", "smtranslucent", etc.
        // +0x08-0x5F: other data (floats, flags, etc.)
        // +0x60: texture_name offset (string table)

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

        // Key by mesh_name offset so submeshes can look up their material
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

void SpiderManTool::AddMeshFromData(const std::vector<uint8_t>& pcmData, std::string modelName, std::function<unsigned int(uint32_t)> textureResolver) {
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

    struct Vertex { float x,y,z; float u,v; };

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

            // Read mesh_name_ref at +0x00 - this links to material
            br.Seek(smOfs);
            uint32_t meshNameRef = br.Read<uint32_t>();

            MaterialDef mat = ResolveMaterialByMeshOffset(meshNameRef);
            unsigned int tex = 0;

            // Try to load texture using the texture name from material
            if (!mat.textureName.empty()) {
                uint32_t hash1 = CalculateCRC32(mat.textureName + ".dds");
                if (textureResolver) tex = textureResolver(hash1);
                else tex = LoadTextureFromHash(hash1);

                if (tex == 0) {
                    uint32_t hash2 = CalculateCRC32(mat.textureName);
                    if (textureResolver) tex = textureResolver(hash2);
                    else tex = LoadTextureFromHash(hash2);
                }
            }

            // Fallback: try model name based texture
            if (tex == 0 && !modelName.empty()) {
                std::string cleanName = modelName;
                size_t lastDot = cleanName.find_last_of(".");
                if(lastDot != std::string::npos) cleanName = cleanName.substr(0, lastDot);
                cleanName = StrToLower(cleanName);
                std::string diffName = cleanName + "_d.dds";
                uint32_t diffHash = CalculateCRC32(diffName);
                if (textureResolver) tex = textureResolver(diffHash);
                else tex = LoadTextureFromHash(diffHash);
            }

            br.Seek(smOfs + 40);
            uint32_t itype = br.Read<uint32_t>(); uint32_t inum = br.Read<uint32_t>(); uint32_t iofs = br.Read<uint32_t>();
            br.Skip(4); uint32_t vnum = br.Read<uint32_t>(); uint32_t vofs = br.Read<uint32_t>();
            br.Skip(8); uint32_t stride = br.Read<uint32_t>();

            if (vnum > 100000 || inum > 300000 || vofs >= pcmData.size() || iofs >= pcmData.size() || stride == 0) continue;

            br.Seek(vofs);
            std::vector<Vertex> vertices;
            bool valid = true;
            for(uint32_t v=0; v<vnum; v++) {
                size_t startV = br.Tell();
                if (startV + stride > pcmData.size()) { valid = false; break; }
                Vertex vert;
                vert.x = br.Read<float>(); vert.y = br.Read<float>(); vert.z = br.Read<float>();
                vert.u = 0; vert.v = 0;
                if (stride == 64) {
                    if (startV + 24 + 8 <= pcmData.size()) { br.Seek(startV + 24); vert.u = br.Read<float>(); vert.v = 1.0f - br.Read<float>(); }
                } else if (stride == 24) {
                    if (startV + 12 + 8 <= pcmData.size()) { br.Seek(startV + 12); vert.u = br.Read<float>(); vert.v = 1.0f - br.Read<float>(); }
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

            glGenVertexArrays(1, &mesh.vao); glGenBuffers(1, &mesh.vbo); glGenBuffers(1, &mesh.ebo);
            glBindVertexArray(mesh.vao);
            glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo); glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(Vertex), vertices.data(), GL_STATIC_DRAW);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.ebo); glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(uint16_t), indices.data(), GL_STATIC_DRAW);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)0); glEnableVertexAttribArray(0);
            glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)(3*sizeof(float))); glEnableVertexAttribArray(1);
            glBindVertexArray(0);
            previewMeshes.push_back(mesh);
        }
    }
}

void SpiderManTool::LoadModelToGL(int index) {
    isWorldMode = false;
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
    AddMeshFromData(pcmData, e.name, [&](uint32_t hash) -> unsigned int {
        return LoadTextureFromHash(hash);
    });

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

            // Get material info
            MaterialDef mat = ResolveMaterialByMeshOffset(smNameOfs);

            // Add material to GLB
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
    Log("Converted GLB: " + fs::path(outPath).filename().string());
}

void SpiderManTool::AnalyzePCM(int index) {
    if (index == currentPcmIndex && !currentPcmInfos.empty()) return;
    currentPcmInfos.clear();
    currentPcmIndex = index;
    currentPcmSkeleton = PCMSkeletonInfo();

    if (index < 0 || index >= entries.size()) return;
    const auto& e = entries[index];
    if (e.offset + e.size > pcPackData.size()) return;

    std::vector<uint8_t> pcmData(pcPackData.begin() + e.offset, pcPackData.begin() + e.offset + e.size);

    // Parse material entries first
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

            // Get material info by matching mesh_name offset
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

            // Material info from parsed material entries
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