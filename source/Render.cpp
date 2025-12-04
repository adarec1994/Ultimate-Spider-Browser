#include "SpiderManTool.h"
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include "imgui.h"
#include <fstream>
#include <algorithm>

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
    for(int i=0; i<(int)entries.size(); i++) {
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
    }
    return tex;
}

unsigned int SpiderManTool::LoadTextureByName(const std::string& textureName) {
    if (textureName.empty()) return 0;

    std::string nameLower = StrToLower(textureName);

    if (textureNameCache.count(nameLower)) {
        return textureNameCache[nameLower];
    }

    int foundIdx = -1;
    for (int i = 0; i < (int)entries.size(); i++) {
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
                textureNameCache[nameLower] = tex;
                return tex;
            }
        }
    }

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
                textureNameCache[nameLower] = tex;
                return tex;
            }
        }
    }

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
                textureNameCache[nameLower] = tex;
                return tex;
            }
        }
    }

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

    const char* vShaderCode = "#version 130\n"
        "in vec3 pos;\n"
        "in vec3 normal;\n"
        "in vec2 texCoord;\n"
        "out vec2 TexCoord;\n"
        "out vec3 FragNormal;\n"
        "out vec3 FragPos;\n"
        "uniform mat4 model;\n"
        "uniform mat4 view;\n"
        "uniform mat4 projection;\n"
        "void main() {\n"
        "    TexCoord = texCoord;\n"
        "    FragNormal = mat3(model) * normal;\n"
        "    FragPos = vec3(model * vec4(pos, 1.0));\n"
        "    gl_Position = projection * view * model * vec4(pos, 1.0);\n"
        "}\n";

    const char* fShaderCode = "#version 130\n"
        "in vec2 TexCoord;\n"
        "in vec3 FragNormal;\n"
        "in vec3 FragPos;\n"
        "out vec4 FragColor;\n"
        "uniform sampler2D diffTexture;\n"
        "uniform bool hasTexture;\n"
        "uniform bool isTranslucent;\n"
        "uniform bool isFakeShadow;\n"
        "uniform bool isColorVolume;\n"
        "uniform bool isHighlighted;\n"
        "uniform vec3 viewPos;\n"
        "void main() {\n"
        "    vec3 lightDir = normalize(vec3(0.5, 1.0, 0.3));\n"
        "    vec3 norm = normalize(FragNormal);\n"
        "    float diff = dot(norm, lightDir);\n"
        "    float toon;\n"
        "    if (diff > 0.7) toon = 1.0;\n"
        "    else if (diff > 0.35) toon = 0.7;\n"
        "    else if (diff > 0.0) toon = 0.5;\n"
        "    else toon = 0.3;\n"
        "    vec3 baseColor;\n"
        "    float alpha = 1.0;\n"
        "    if (isFakeShadow || isColorVolume) {\n"
        "        baseColor = vec3(0.0, 0.0, 0.0);\n"
        "        alpha = 0.3;\n"
        "    } else if (hasTexture) {\n"
        "        vec4 texColor = texture(diffTexture, TexCoord);\n"
        "        baseColor = texColor.rgb;\n"
        "    } else {\n"
        "        baseColor = vec3(0.8, 0.8, 0.8);\n"
        "    }\n"
        "    if (isTranslucent && !isFakeShadow && !isColorVolume) {\n"
        "        alpha = 0.5;\n"
        "    }\n"
        "    if (isHighlighted) {\n"
        "        baseColor = mix(baseColor, vec3(0.2, 1.0, 0.3), 0.6);\n"
        "    }\n"
        "    vec3 ambient = 0.2 * baseColor;\n"
        "    vec3 diffuse = toon * baseColor;\n"
        "    vec3 result = ambient + diffuse;\n"
        "    FragColor = vec4(result, alpha);\n"
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
    glBindAttribLocation(modelProgram, 1, "normal");
    glBindAttribLocation(modelProgram, 2, "texCoord");
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
    float aspect = (float)width / (float)height;
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
    glUniform3f(glGetUniformLocation(modelProgram, "viewPos"), camPos[0], camPos[1], camPos[2]);

    GLint locDiffTexture = glGetUniformLocation(modelProgram, "diffTexture");
    GLint locHasTexture = glGetUniformLocation(modelProgram, "hasTexture");
    GLint locIsTranslucent = glGetUniformLocation(modelProgram, "isTranslucent");
    GLint locIsFakeShadow = glGetUniformLocation(modelProgram, "isFakeShadow");
    GLint locIsColorVolume = glGetUniformLocation(modelProgram, "isColorVolume");
    GLint locIsHighlighted = glGetUniformLocation(modelProgram, "isHighlighted");

    auto isInFrustum = [&](const float bboxMin[3], const float bboxMax[3]) -> bool {
        float cx = (bboxMin[0] + bboxMax[0]) * 0.5f;
        float cy = (bboxMin[1] + bboxMax[1]) * 0.5f;
        float cz = (bboxMin[2] + bboxMax[2]) * 0.5f;

        float rx = (bboxMax[0] - bboxMin[0]) * 0.5f;
        float ry = (bboxMax[1] - bboxMin[1]) * 0.5f;
        float rz = (bboxMax[2] - bboxMin[2]) * 0.5f;
        float radius = sqrt(rx*rx + ry*ry + rz*rz);

        float dx = cx - camPos[0];
        float dy = cy - camPos[1];
        float dz = cz - camPos[2];

        float dist = dx * camFront[0] + dy * camFront[1] + dz * camFront[2];

        if (dist < -radius) return false;
        if (dist > 15000.0f + radius) return false;

        return true;
    };

    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);

    // First pass: opaque meshes (not translucent, fake shadow, or color volume)
    for (int i = 0; i < (int)previewMeshes.size(); i++) {
        const auto& m = previewMeshes[i];
        if (m.isHidden) continue;  // Skip hidden meshes
        if (m.indexCount > 0 && !m.isTranslucent && !m.isFakeShadow && !m.isColorVolume) {
            if (!isInFrustum(m.bboxMin, m.bboxMax)) continue;

            if (m.textureId != 0) {
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, m.textureId);
                glUniform1i(locDiffTexture, 0);
                glUniform1i(locHasTexture, 1);
            } else {
                glUniform1i(locHasTexture, 0);
            }
            glUniform1i(locIsTranslucent, 0);
            glUniform1i(locIsFakeShadow, 0);
            glUniform1i(locIsColorVolume, 0);
            glUniform1i(locIsHighlighted, (i == selectedMeshIndex) ? 1 : 0);

            glBindVertexArray(m.vao);
            glDrawElements(m.mode, m.indexCount, GL_UNSIGNED_SHORT, 0);
        }
    }

    // Second pass: translucent meshes with blending (including color volumes)
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);

    for (int i = 0; i < (int)previewMeshes.size(); i++) {
        const auto& m = previewMeshes[i];
        if (m.isHidden) continue;  // Skip hidden meshes
        if (m.indexCount > 0 && (m.isTranslucent || m.isFakeShadow || m.isColorVolume)) {
            if (!isInFrustum(m.bboxMin, m.bboxMax)) continue;

            if (m.textureId != 0 && !m.isFakeShadow && !m.isColorVolume) {
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, m.textureId);
                glUniform1i(locDiffTexture, 0);
                glUniform1i(locHasTexture, 1);
            } else {
                glUniform1i(locHasTexture, 0);
            }
            glUniform1i(locIsTranslucent, m.isTranslucent ? 1 : 0);
            glUniform1i(locIsFakeShadow, m.isFakeShadow ? 1 : 0);
            glUniform1i(locIsColorVolume, m.isColorVolume ? 1 : 0);
            glUniform1i(locIsHighlighted, (i == selectedMeshIndex) ? 1 : 0);

            glBindVertexArray(m.vao);
            glDrawElements(m.mode, m.indexCount, GL_UNSIGNED_SHORT, 0);
        }
    }

    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);

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
    if (index < 0 || index >= (int)entries.size()) return;
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
        return;
    }

    if (!e.isDds) return;

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

    if (format == 0) return;
    uint32_t blockSize = (format == GL_COMPRESSED_RGBA_S3TC_DXT1_EXT) ? 8 : 16;
    uint32_t imageSize = ((ddsWidth + 3) / 4) * ((ddsHeight + 3) / 4) * blockSize;

    glGenTextures(1, &ddsTextureId);
    glBindTexture(GL_TEXTURE_2D, ddsTextureId);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glCompressedTexImage2D(GL_TEXTURE_2D, 0, format, ddsWidth, ddsHeight, 0, imageSize, data + 128);
    glBindTexture(GL_TEXTURE_2D, 0);

    showDdsPopup = true;
}

bool SpiderManTool::RayIntersectAABB(const float rayOrigin[3], const float rayDir[3],
                                      const float bboxMin[3], const float bboxMax[3], float& tMin) {
    float tmax = 1e30f;
    tMin = -1e30f;

    for (int i = 0; i < 3; i++) {
        if (fabs(rayDir[i]) < 1e-8f) {
            if (rayOrigin[i] < bboxMin[i] || rayOrigin[i] > bboxMax[i]) {
                return false;
            }
        } else {
            float ood = 1.0f / rayDir[i];
            float t1 = (bboxMin[i] - rayOrigin[i]) * ood;
            float t2 = (bboxMax[i] - rayOrigin[i]) * ood;
            if (t1 > t2) std::swap(t1, t2);
            if (t1 > tMin) tMin = t1;
            if (t2 < tmax) tmax = t2;
            if (tMin > tmax) return false;
        }
    }

    return tMin >= 0;
}

static bool RayIntersectTriangle(const float rayOrigin[3], const float rayDir[3],
                                  const float v0[3], const float v1[3], const float v2[3],
                                  float& t) {
    const float EPSILON = 1e-7f;

    float edge1[3] = { v1[0] - v0[0], v1[1] - v0[1], v1[2] - v0[2] };
    float edge2[3] = { v2[0] - v0[0], v2[1] - v0[1], v2[2] - v0[2] };

    float h[3];
    Cross(rayDir, edge2, h);
    float a = Dot(edge1, h);

    if (fabs(a) < EPSILON) return false;

    float f = 1.0f / a;
    float s[3] = { rayOrigin[0] - v0[0], rayOrigin[1] - v0[1], rayOrigin[2] - v0[2] };
    float u = f * Dot(s, h);

    if (u < 0.0f || u > 1.0f) return false;

    float q[3];
    Cross(s, edge1, q);
    float v = f * Dot(rayDir, q);

    if (v < 0.0f || u + v > 1.0f) return false;

    t = f * Dot(edge2, q);
    return t > EPSILON;
}

int SpiderManTool::PickMeshAtScreenPos(float screenX, float screenY, float vpWidth, float vpHeight) {
    float ndcX = (2.0f * screenX / vpWidth) - 1.0f;
    float ndcY = 1.0f - (2.0f * screenY / vpHeight);

    const float fbAspect = 3840.0f / 2160.0f;
    float fov = 1.0f;
    float tanHalfFov = tan(fov / 2.0f);

    float right[3];
    Cross(camFront, camUp, right);
    Normalize(right);

    float up[3];
    Cross(right, camFront, up);
    Normalize(up);

    float rayDir[3] = {
        camFront[0] + right[0] * ndcX * tanHalfFov * fbAspect + up[0] * ndcY * tanHalfFov,
        camFront[1] + right[1] * ndcX * tanHalfFov * fbAspect + up[1] * ndcY * tanHalfFov,
        camFront[2] + right[2] * ndcX * tanHalfFov * fbAspect + up[2] * ndcY * tanHalfFov
    };
    Normalize(rayDir);

    float rayOrigin[3] = { camPos[0], camPos[1], camPos[2] };

    int closestMesh = -1;
    float closestT = 1e30f;

    for (int i = 0; i < (int)previewMeshes.size(); i++) {
        const auto& m = previewMeshes[i];

        if (m.skipPicking) continue;
        if (m.isFakeShadow || m.isColorVolume) continue;

        float bboxT;
        if (!RayIntersectAABB(rayOrigin, rayDir, m.bboxMin, m.bboxMax, bboxT)) continue;
        if (bboxT >= closestT) continue;

        if (m.positions.empty() || m.indices.empty()) continue;

        if (m.mode == GL_TRIANGLES) {
            for (size_t j = 0; j + 2 < m.indices.size(); j += 3) {
                uint16_t i0 = m.indices[j];
                uint16_t i1 = m.indices[j + 1];
                uint16_t i2 = m.indices[j + 2];

                if (i0 * 3 + 2 >= m.positions.size() ||
                    i1 * 3 + 2 >= m.positions.size() ||
                    i2 * 3 + 2 >= m.positions.size()) continue;

                float v0[3] = { m.positions[i0*3], m.positions[i0*3+1], m.positions[i0*3+2] };
                float v1[3] = { m.positions[i1*3], m.positions[i1*3+1], m.positions[i1*3+2] };
                float v2[3] = { m.positions[i2*3], m.positions[i2*3+1], m.positions[i2*3+2] };

                float t;
                if (RayIntersectTriangle(rayOrigin, rayDir, v0, v1, v2, t)) {
                    if (t < closestT) {
                        closestT = t;
                        closestMesh = i;
                    }
                }
            }
        } else {
            for (size_t j = 0; j + 2 < m.indices.size(); j++) {
                uint16_t i0 = m.indices[j];
                uint16_t i1 = m.indices[j + 1];
                uint16_t i2 = m.indices[j + 2];

                if (i0 == i1 || i1 == i2 || i0 == i2) continue;

                if (i0 * 3 + 2 >= m.positions.size() ||
                    i1 * 3 + 2 >= m.positions.size() ||
                    i2 * 3 + 2 >= m.positions.size()) continue;

                float v0[3] = { m.positions[i0*3], m.positions[i0*3+1], m.positions[i0*3+2] };
                float v1[3] = { m.positions[i1*3], m.positions[i1*3+1], m.positions[i1*3+2] };
                float v2[3] = { m.positions[i2*3], m.positions[i2*3+1], m.positions[i2*3+2] };

                float t;
                if (j % 2 == 0) {
                    if (RayIntersectTriangle(rayOrigin, rayDir, v0, v1, v2, t)) {
                        if (t < closestT) {
                            closestT = t;
                            closestMesh = i;
                        }
                    }
                } else {
                    if (RayIntersectTriangle(rayOrigin, rayDir, v0, v2, v1, t)) {
                        if (t < closestT) {
                            closestT = t;
                            closestMesh = i;
                        }
                    }
                }
            }
        }
    }

    return closestMesh;
}

void SpiderManTool::HandleMeshPicking(float viewportX, float viewportY, float viewportWidth, float viewportHeight) {
    if (!isWorldMode || !isModelLoaded) return;

    int pickedMesh = PickMeshAtScreenPos(viewportX, viewportY, viewportWidth, viewportHeight);

    if (pickedMesh >= 0) {
        selectedMeshIndex = pickedMesh;
        LoadSelectedMeshPcmData();
        showWorldMeshHexEditor = true;

        const auto& m = previewMeshes[pickedMesh];
        if (!m.meshName.empty()) {
            Log("Selected mesh: " + m.meshName);
        } else {
            Log("Selected mesh index: " + std::to_string(pickedMesh));
        }
    }
}

void SpiderManTool::LoadSelectedMeshPcmData() {
    selectedMeshPcmData.clear();

    if (selectedMeshIndex < 0 || selectedMeshIndex >= (int)previewMeshes.size()) return;

    const auto& m = previewMeshes[selectedMeshIndex];
    if (m.sourcePack.empty() || m.sourceSize == 0) return;

    std::ifstream file(m.sourcePack, std::ios::binary);
    if (!file.is_open()) return;

    file.seekg(m.sourceOffset);
    if (!file.good()) return;

    selectedMeshPcmData.resize(m.sourceSize);
    file.read((char*)selectedMeshPcmData.data(), m.sourceSize);
    file.close();
}