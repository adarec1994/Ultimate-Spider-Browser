#include "SpiderManTool.h"
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include "imgui.h"

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

unsigned int SpiderManTool::LoadTextureFromData(const std::vector<uint8_t>& data) {
    if (data.size() < sizeof(DDS_HEADER) + 4) return 0;

    uint32_t magic = *(uint32_t*)data.data();
    if (magic != 0x20534444) return 0;

    const DDS_HEADER* header = (const DDS_HEADER*)(data.data() + 4);
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
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glCompressedTexImage2D(GL_TEXTURE_2D, 0, format, width, height, 0, imageSize, data.data() + 128);
    glBindTexture(GL_TEXTURE_2D, 0);
    return tex;
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
    if (e.offset + e.size > pcPackData.size()) return 0;

    std::vector<uint8_t> ddsData(pcPackData.begin() + e.offset, pcPackData.begin() + e.offset + e.size);
    unsigned int tex = LoadTextureFromData(ddsData);

    if (tex != 0) {
        textureCache[hash] = tex;
        Log("Loaded texture by hash: 0x" + std::to_string(hash));
    }
    return tex;
}

unsigned int SpiderManTool::LoadTextureByName(const std::string& textureName) {
    if (textureName.empty()) return 0;

    std::string nameLower = StrToLower(textureName);

    // Check name cache first
    if (textureNameCache.count(nameLower)) {
        return textureNameCache[nameLower];
    }

    // First try: Search current pack's entries by name
    int foundIdx = -1;
    for (int i = 0; i < entries.size(); i++) {
        if (!entries[i].isDds) continue;

        std::string entryName = StrToLower(entries[i].name);
        std::string entryBase = entryName;
        if (entryBase.size() > 4 && entryBase.substr(entryBase.size() - 4) == ".dds") {
            entryBase = entryBase.substr(0, entryBase.size() - 4);
        }

        if (entryBase == nameLower || entryName == nameLower ||
            entryBase == nameLower + ".dds" || entryName == nameLower + ".dds") {
            foundIdx = i;
            break;
        }
    }

    if (foundIdx != -1) {
        const auto& e = entries[foundIdx];
        if (e.offset + e.size <= pcPackData.size()) {
            std::vector<uint8_t> ddsData(pcPackData.begin() + e.offset, pcPackData.begin() + e.offset + e.size);
            unsigned int tex = LoadTextureFromData(ddsData);
            if (tex != 0) {
                Log("Loaded texture from current pack: " + textureName);
                textureNameCache[nameLower] = tex;
                return tex;
            }
        }
    }

    // Second try: Search global texture name index (all packs)
    if (globalTextureNameIndex.count(nameLower)) {
        auto& loc = globalTextureNameIndex[nameLower];
        std::ifstream texFile(loc.packPath, std::ios::binary);
        if (texFile.is_open()) {
            texFile.seekg(loc.offset);
            std::vector<uint8_t> ddsData(loc.size);
            texFile.read((char*)ddsData.data(), loc.size);
            texFile.close();

            unsigned int tex = LoadTextureFromData(ddsData);
            if (tex != 0) {
                Log("Loaded texture from global index: " + textureName + " (" + fs::path(loc.packPath).filename().string() + ")");
                textureNameCache[nameLower] = tex;
                return tex;
            }
        }
    }

    // Third try: Hash-based lookup in global index
    uint32_t hash1 = CalculateCRC32(nameLower + ".dds");
    if (globalTextureIndex.count(hash1)) {
        auto& loc = globalTextureIndex[hash1];
        std::ifstream texFile(loc.packPath, std::ios::binary);
        if (texFile.is_open()) {
            texFile.seekg(loc.offset);
            std::vector<uint8_t> ddsData(loc.size);
            texFile.read((char*)ddsData.data(), loc.size);
            texFile.close();

            unsigned int tex = LoadTextureFromData(ddsData);
            if (tex != 0) {
                Log("Loaded texture by hash from global index: " + textureName);
                textureNameCache[nameLower] = tex;
                return tex;
            }
        }
    }

    uint32_t hash2 = CalculateCRC32(nameLower);
    if (globalTextureIndex.count(hash2)) {
        auto& loc = globalTextureIndex[hash2];
        std::ifstream texFile(loc.packPath, std::ios::binary);
        if (texFile.is_open()) {
            texFile.seekg(loc.offset);
            std::vector<uint8_t> ddsData(loc.size);
            texFile.read((char*)ddsData.data(), loc.size);
            texFile.close();

            unsigned int tex = LoadTextureFromData(ddsData);
            if (tex != 0) {
                Log("Loaded texture by hash (no ext) from global index: " + textureName);
                textureNameCache[nameLower] = tex;
                return tex;
            }
        }
    }

    // Not found
    textureNameCache[nameLower] = 0;
    return 0;
}

void SpiderManTool::InitModelPreview() {
    if (viewportTextureId == 0) {
        if (msFbo != 0) glDeleteFramebuffers(1, &msFbo);
        if (msColor != 0) glDeleteTextures(1, &msColor);
        if (msRbo != 0) glDeleteRenderbuffers(1, &msRbo);
        if (modelFbo != 0) glDeleteFramebuffers(1, &modelFbo);

        if (viewportTextureId != 0) glDeleteTextures(1, &viewportTextureId);

        int width = 3840;
        int height = 2160;

        glGenFramebuffers(1, &msFbo);
        glBindFramebuffer(GL_FRAMEBUFFER, msFbo);

        glGenTextures(1, &msColor);
        glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, msColor);
        glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, 4, GL_RGBA, width, height, GL_TRUE);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D_MULTISAMPLE, msColor, 0);

        glGenRenderbuffers(1, &msRbo);
        glBindRenderbuffer(GL_RENDERBUFFER, msRbo);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, 4, GL_DEPTH24_STENCIL8, width, height);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, msRbo);

        glGenFramebuffers(1, &modelFbo);
        glBindFramebuffer(GL_FRAMEBUFFER, modelFbo);

        glGenTextures(1, &viewportTextureId);
        glBindTexture(GL_TEXTURE_2D, viewportTextureId);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, viewportTextureId, 0);

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    if (modelProgram != 0) return;

    // Updated shaders with proper alpha handling
    const char* vShaderCode = "#version 130\n"
        "in vec3 pos;\n"
        "in vec2 texCoord;\n"
        "out vec2 TexCoord;\n"
        "uniform mat4 model;\n"
        "uniform mat4 view;\n"
        "uniform mat4 projection;\n"
        "void main() {\n"
        "    TexCoord = texCoord;\n"
        "    gl_Position = projection * view * model * vec4(pos, 1.0);\n"
        "}\n";

    const char* fShaderCode = "#version 130\n"
        "in vec2 TexCoord;\n"
        "out vec4 FragColor;\n"
        "uniform sampler2D diffTexture;\n"
        "uniform bool hasTexture;\n"
        "uniform bool isTranslucent;\n"
        "void main() {\n"
        "    if (hasTexture) {\n"
        "        vec4 texColor = texture(diffTexture, TexCoord);\n"
        "        if (isTranslucent) {\n"
        "            // Use alpha channel for translucent materials\n"
        "            if (texColor.a < 0.01) discard;\n"
        "            FragColor = texColor;\n"
        "        } else {\n"
        "            // Opaque materials - discard very low alpha\n"
        "            if (texColor.a < 0.1) discard;\n"
        "            FragColor = vec4(texColor.rgb, 1.0);\n"
        "        }\n"
        "    } else {\n"
        "        FragColor = vec4(0.8, 0.8, 0.8, 1.0);\n"
        "    }\n"
        "}\n";

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

void SpiderManTool::UpdateWorldCamera(bool isHovered) {
    GLFWwindow* window = glfwGetCurrentContext();
    if (!window) return;

    bool isCapturing = (glfwGetInputMode(window, GLFW_CURSOR) == GLFW_CURSOR_DISABLED);
    if (!isHovered && !isCapturing && !ImGui::IsMouseDown(ImGuiMouseButton_Right)) return;

    float dt = ImGui::GetIO().DeltaTime;

    if (ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);

        float xoffset = ImGui::GetIO().MouseDelta.x;
        float yoffset = ImGui::GetIO().MouseDelta.y;

        float sensitivity = 0.2f;
        xoffset *= sensitivity;
        yoffset *= sensitivity;

        camYaw += xoffset;
        camPitch -= yoffset;

        if (camPitch > 89.0f) camPitch = 89.0f;
        if (camPitch < -89.0f) camPitch = -89.0f;

        float front[3];
        front[0] = cos(camYaw * 3.14159f / 180.0f) * cos(camPitch * 3.14159f / 180.0f);
        front[1] = sin(camPitch * 3.14159f / 180.0f);
        front[2] = sin(camYaw * 3.14159f / 180.0f) * cos(camPitch * 3.14159f / 180.0f);
        Normalize(front);
        camFront[0] = front[0]; camFront[1] = front[1]; camFront[2] = front[2];

        float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0) {
            float multiplier = 1.0f + (0.2f * wheel);
            camSpeed *= multiplier;
            if (camSpeed < 0.1f) camSpeed = 0.1f;
            if (camSpeed > 20000.0f) camSpeed = 20000.0f;
        }

        float velocity = camSpeed * dt;

        if (ImGui::IsKeyDown(ImGuiKey_W)) {
            camPos[0] += camFront[0] * velocity;
            camPos[1] += camFront[1] * velocity;
            camPos[2] += camFront[2] * velocity;
        }
        if (ImGui::IsKeyDown(ImGuiKey_S)) {
            camPos[0] -= camFront[0] * velocity;
            camPos[1] -= camFront[1] * velocity;
            camPos[2] -= camFront[2] * velocity;
        }

        float right[3];
        Cross(camFront, camUp, right);
        Normalize(right);

        if (ImGui::IsKeyDown(ImGuiKey_A)) {
            camPos[0] -= right[0] * velocity;
            camPos[1] -= right[1] * velocity;
            camPos[2] -= right[2] * velocity;
        }
        if (ImGui::IsKeyDown(ImGuiKey_D)) {
            camPos[0] += right[0] * velocity;
            camPos[1] += right[1] * velocity;
            camPos[2] += right[2] * velocity;
        }

        if (ImGui::IsKeyDown(ImGuiKey_Z)) {
            camPos[1] += velocity;
        }
        if (ImGui::IsKeyDown(ImGuiKey_X)) {
            camPos[1] -= velocity;
        }

    } else {
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
    }
}

void SpiderManTool::RenderModelPreview() {
    glBindFramebuffer(GL_FRAMEBUFFER, msFbo);
    int width = 3840;
    int height = 2160;
    glViewport(0, 0, width, height);
    glClearColor(0.15f, 0.15f, 0.2f, 1.0f);
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
    float znear = 0.1f;
    float zfar = 20000.0f;

    float proj[16] = {0};
    float tanHalfFov = tan(fov / 2.0f);
    proj[0] = 1.0f / (aspect * tanHalfFov);
    proj[5] = 1.0f / tanHalfFov;
    proj[10] = -(zfar + znear) / (zfar - znear);
    proj[11] = -1.0f;
    proj[14] = -(2.0f * zfar * znear) / (zfar - znear);

    float view[16];
    float model[16];

    float target[3] = { camPos[0] + camFront[0], camPos[1] + camFront[1], camPos[2] + camFront[2] };
    LookAt(camPos, target, camUp, view);

    memset(model, 0, sizeof(model));
    model[0] = 1; model[5] = 1; model[10] = 1; model[15] = 1;

    glUniformMatrix4fv(glGetUniformLocation(modelProgram, "projection"), 1, GL_FALSE, proj);
    glUniformMatrix4fv(glGetUniformLocation(modelProgram, "view"), 1, GL_FALSE, view);
    glUniformMatrix4fv(glGetUniformLocation(modelProgram, "model"), 1, GL_FALSE, model);

    // First pass: Render opaque meshes
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);

    for (const auto& m : previewMeshes) {
        if (m.isTranslucent) continue;

        if (m.indexCount > 0) {
            if (m.textureId != 0) {
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, m.textureId);
                glUniform1i(glGetUniformLocation(modelProgram, "diffTexture"), 0);
                glUniform1i(glGetUniformLocation(modelProgram, "hasTexture"), 1);
            } else {
                glUniform1i(glGetUniformLocation(modelProgram, "hasTexture"), 0);
            }
            glUniform1i(glGetUniformLocation(modelProgram, "isTranslucent"), 0);

            glBindVertexArray(m.vao);
            glDrawElements(m.mode, m.indexCount, GL_UNSIGNED_SHORT, 0);
        }
    }

    // Second pass: Render translucent meshes with alpha blending
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);  // Don't write to depth buffer for translucent objects

    for (const auto& m : previewMeshes) {
        if (!m.isTranslucent) continue;

        if (m.indexCount > 0) {
            if (m.textureId != 0) {
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, m.textureId);
                glUniform1i(glGetUniformLocation(modelProgram, "diffTexture"), 0);
                glUniform1i(glGetUniformLocation(modelProgram, "hasTexture"), 1);
            } else {
                glUniform1i(glGetUniformLocation(modelProgram, "hasTexture"), 0);
            }
            glUniform1i(glGetUniformLocation(modelProgram, "isTranslucent"), 1);

            glBindVertexArray(m.vao);
            glDrawElements(m.mode, m.indexCount, GL_UNSIGNED_SHORT, 0);
        }
    }

    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);

    // Resolve multisampled buffer
    glBindFramebuffer(GL_READ_FRAMEBUFFER, msFbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, modelFbo);
    glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void SpiderManTool::CloseDdsPreview() {
    if (ddsTextureId != 0) {
        glDeleteTextures(1, &ddsTextureId);
        ddsTextureId = 0;
    }
    showDdsPopup = false;
}

void SpiderManTool::LoadPreview(int index) {
    if (index < 0 || index >= entries.size()) return;
    if (pcPackData.empty()) return;
    const auto& e = entries[index];

    if (e.isPcm) {
        InitModelPreview();
        isModelPreview = true;

        fs::path p(loadedPCPackPath);
        std::string packStem = StrToLower(p.stem().string());
        std::string fileStem = StrToLower(fs::path(e.name).stem().string());

        if (IsWorldPack(packStem) && fileStem.find(packStem) == 0) {
             LoadAllWorldGeometries();
        } else {
             LoadModelToGL(index);
        }

        isModelLoaded = true;
        Log("Model loaded: " + e.name);
        return;
    }

    if (!e.isDds) { Log("Not a DDS/PCM file."); return; }

    CloseDdsPreview();
    isModelPreview = false;

    const uint8_t* data = &pcPackData[e.offset];
    if (e.size < sizeof(DDS_HEADER) + 4) return;
    uint32_t magic = *(uint32_t*)data;
    if (magic != 0x20534444) return;

    const DDS_HEADER* header = (const DDS_HEADER*)(data + 4);
    ddsWidth = header->dwWidth;
    ddsHeight = header->dwHeight;

    uint32_t fourCC = header->ddspf.dwFourCC;
    GLenum format = 0;
    if (fourCC == 0x31545844) format = GL_COMPRESSED_RGBA_S3TC_DXT1_EXT;
    else if (fourCC == 0x33545844) format = GL_COMPRESSED_RGBA_S3TC_DXT3_EXT;
    else if (fourCC == 0x35545844) format = GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;

    if (format == 0) { Log("Unsupported DXT format."); return; }
    uint32_t blockSize = (format == GL_COMPRESSED_RGBA_S3TC_DXT1_EXT) ? 8 : 16;
    uint32_t imageSize = ((ddsWidth + 3) / 4) * ((ddsHeight + 3) / 4) * blockSize;

    glGenTextures(1, &ddsTextureId);
    glBindTexture(GL_TEXTURE_2D, ddsTextureId);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glCompressedTexImage2D(GL_TEXTURE_2D, 0, format, ddsWidth, ddsHeight, 0, imageSize, data + 128);
    glBindTexture(GL_TEXTURE_2D, 0);

    showDdsPopup = true;
    Log("Preview loaded: " + e.name);
}