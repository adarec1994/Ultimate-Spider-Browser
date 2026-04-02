#include "SpiderManTool.h"
#include "NalIntegration.h"
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include "imgui.h"
#include <fstream>
#include <algorithm>
#include <map>

static const int MAX_BONES = 48; // Must stay under GL_MAX_VERTEX_UNIFORM_COMPONENTS (48*16=768 floats)

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
    uint32_t pfFlags = header->ddspf.dwFlags;
    uint32_t fourCC = header->ddspf.dwFourCC;
    uint32_t rgbBits = header->ddspf.dwRGBBitCount;
    const uint8_t* pixelData = data.data() + 128;
    size_t pixelDataSize = data.size() - 128;

    unsigned int tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

    if (pfFlags & 0x4) { // DDPF_FOURCC — compressed DXT
        GLenum format = 0;
        if (fourCC == 0x31545844) format = GL_COMPRESSED_RGBA_S3TC_DXT1_EXT;
        else if (fourCC == 0x33545844) format = GL_COMPRESSED_RGBA_S3TC_DXT3_EXT;
        else if (fourCC == 0x35545844) format = GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
        if (format == 0) { glDeleteTextures(1, &tex); return 0; }
        uint32_t blockSize = (format == GL_COMPRESSED_RGBA_S3TC_DXT1_EXT) ? 8 : 16;
        uint32_t imageSize = ((width + 3) / 4) * ((height + 3) / 4) * blockSize;
        glCompressedTexImage2D(GL_TEXTURE_2D, 0, format, width, height, 0, imageSize, pixelData);
    } else if (pfFlags & 0x40) { // DDPF_RGB — uncompressed
        uint32_t aMask = header->ddspf.dwABitMask;
        if (rgbBits == 32) {
            size_t numPx = (size_t)width * height;
            std::vector<uint8_t> rgba(numPx * 4);
            for (size_t i = 0; i < numPx && i * 4 + 3 < pixelDataSize; i++) {
                rgba[i*4+0] = pixelData[i*4+2]; rgba[i*4+1] = pixelData[i*4+1];
                rgba[i*4+2] = pixelData[i*4+0]; rgba[i*4+3] = aMask ? pixelData[i*4+3] : 255;
            }
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        } else if (rgbBits == 24) {
            size_t numPx = (size_t)width * height;
            std::vector<uint8_t> rgb(numPx * 3);
            for (size_t i = 0; i < numPx && i * 3 + 2 < pixelDataSize; i++) {
                rgb[i*3+0] = pixelData[i*3+2]; rgb[i*3+1] = pixelData[i*3+1]; rgb[i*3+2] = pixelData[i*3+0];
            }
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, rgb.data());
        } else { glDeleteTextures(1, &tex); return 0; }
    } else if (pfFlags & 0x20000) { // DDPF_LUMINANCE
        size_t numPx = (size_t)width * height;
        std::vector<uint8_t> rgba(numPx * 4);
        if (rgbBits == 8) {
            for (size_t i = 0; i < numPx && i < pixelDataSize; i++) {
                rgba[i*4+0]=rgba[i*4+1]=rgba[i*4+2]=pixelData[i]; rgba[i*4+3]=255;
            }
        } else if (rgbBits == 16) {
            for (size_t i = 0; i < numPx && i*2+1 < pixelDataSize; i++) {
                rgba[i*4+0]=rgba[i*4+1]=rgba[i*4+2]=pixelData[i*2]; rgba[i*4+3]=pixelData[i*2+1];
            }
        } else { glDeleteTextures(1, &tex); return 0; }
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    } else { glDeleteTextures(1, &tex); return 0; }

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

    if (modelProgram != 0 && skeletonProgram != 0) return;

    const char* vShaderCode = "#version 130\n"
        "in vec3 pos;\n"
        "in vec3 normal;\n"
        "in vec2 texCoord;\n"
        "in vec4 boneIndices;\n"
        "in vec4 boneWeights;\n"
        "out vec2 TexCoord;\n"
        "out vec3 FragNormal;\n"
        "out vec3 FragPos;\n"
        "uniform mat4 model;\n"
        "uniform mat4 view;\n"
        "uniform mat4 projection;\n"
        "uniform bool useSkinning;\n"
        "uniform mat4 boneMatrices[48];\n"
        "void main() {\n"
        "    vec4 skinnedPos;\n"
        "    vec3 skinnedNorm;\n"
        "    if (useSkinning && (boneWeights.x + boneWeights.y + boneWeights.z + boneWeights.w) > 0.01) {\n"
        "        mat4 skinMat = mat4(0.0);\n"
        "        for (int i = 0; i < 4; i++) {\n"
        "            int idx = int(boneIndices[i]);\n"
        "            if (idx >= 0 && idx < 48 && boneWeights[i] > 0.0) {\n"
        "                skinMat += boneWeights[i] * boneMatrices[idx];\n"
        "            }\n"
        "        }\n"
        "        skinnedPos = skinMat * vec4(pos, 1.0);\n"
        "        skinnedNorm = mat3(skinMat) * normal;\n"
        "    } else {\n"
        "        skinnedPos = vec4(pos, 1.0);\n"
        "        skinnedNorm = normal;\n"
        "    }\n"
        "    TexCoord = texCoord;\n"
        "    FragNormal = mat3(model) * skinnedNorm;\n"
        "    FragPos = vec3(model * skinnedPos);\n"
        "    gl_Position = projection * view * model * skinnedPos;\n"
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
        "        if (isTranslucent) {\n"
        "            // alpha disabled for debugging\n"
        "        }\n"
        "    } else {\n"
        "        baseColor = vec3(0.8, 0.8, 0.8);\n"
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
    { int ok; glGetShaderiv(vertex, GL_COMPILE_STATUS, &ok);
      if (!ok) { char log[512]; glGetShaderInfoLog(vertex, 512, NULL, log); printf("VS ERROR: %s\n", log); } }

    unsigned int fragment = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragment, 1, &fShaderCode, NULL);
    glCompileShader(fragment);
    { int ok; glGetShaderiv(fragment, GL_COMPILE_STATUS, &ok);
      if (!ok) { char log[512]; glGetShaderInfoLog(fragment, 512, NULL, log); printf("FS ERROR: %s\n", log); } }

    modelProgram = glCreateProgram();
    glAttachShader(modelProgram, vertex);
    glAttachShader(modelProgram, fragment);
    glBindAttribLocation(modelProgram, 0, "pos");
    glBindAttribLocation(modelProgram, 1, "normal");
    glBindAttribLocation(modelProgram, 2, "texCoord");
    glBindAttribLocation(modelProgram, 3, "boneIndices");
    glBindAttribLocation(modelProgram, 4, "boneWeights");
    glLinkProgram(modelProgram);
    { int ok; glGetProgramiv(modelProgram, GL_LINK_STATUS, &ok);
      if (!ok) { char log[512]; glGetProgramInfoLog(modelProgram, 512, NULL, log); printf("LINK ERROR: %s\n", log); } }

    glDeleteShader(vertex);
    glDeleteShader(fragment);

    // Skeleton shader: simple solid color, no lighting
    const char* skelVS = "#version 130\n"
        "in vec3 pos;\n"
        "in vec3 color;\n"
        "out vec3 vertColor;\n"
        "uniform mat4 view;\n"
        "uniform mat4 projection;\n"
        "void main() {\n"
        "    vertColor = color;\n"
        "    gl_Position = projection * view * vec4(pos, 1.0);\n"
        "}\n";

    const char* skelFS = "#version 130\n"
        "in vec3 vertColor;\n"
        "out vec4 FragColor;\n"
        "uniform float highlightBone;\n"
        "void main() {\n"
        "    FragColor = vec4(vertColor, 1.0);\n"
        "}\n";

    unsigned int sv = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(sv, 1, &skelVS, NULL);
    glCompileShader(sv);

    unsigned int sf = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(sf, 1, &skelFS, NULL);
    glCompileShader(sf);

    skeletonProgram = glCreateProgram();
    glAttachShader(skeletonProgram, sv);
    glAttachShader(skeletonProgram, sf);
    glBindAttribLocation(skeletonProgram, 0, "pos");
    glBindAttribLocation(skeletonProgram, 1, "color");
    glLinkProgram(skeletonProgram);
    glDeleteShader(sv);
    glDeleteShader(sf);
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
    glDisable(GL_CULL_FACE);  // Disable backface culling globally - all meshes are double-sided

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

    GLint locUseSkinning = glGetUniformLocation(modelProgram, "useSkinning");
    GLint locBoneMatrices = glGetUniformLocation(modelProgram, "boneMatrices");

    // Bone matrix data at function scope so skeleton overlay can access it
    bool skinningActive = false;
    std::vector<float> boneMatData(MAX_BONES * 16, 0.f);
    // Identity = rest pose (vertices are already in bind pose, so identity = no deformation)
    for (int i = 0; i < MAX_BONES; i++) {
        boneMatData[i*16+0] = 1.f; boneMatData[i*16+5] = 1.f;
        boneMatData[i*16+10] = 1.f; boneMatData[i*16+15] = 1.f;
    }

    // For skeleton overlay: track world-space bone positions (animated or bind pose)
    // These are the BIND positions by default, updated if animation is active
    std::vector<float> bonePosWorld(MAX_BONES * 3, 0.f);
    for (int i = 0; i < skeletonBoneCount && i < MAX_BONES; i++) {
        bonePosWorld[i*3+0] = skeletonBones[i].position[0];
        bonePosWorld[i*3+1] = skeletonBones[i].position[1];
        bonePosWorld[i*3+2] = skeletonBones[i].position[2];
    }

    // Compute and upload bone matrices if animation is selected
    if (loadedAnimFile && loadedSkeleton && selectedAnimIndex >= 0 &&
        selectedAnimIndex < (int)loadedAnimFile->animations.size() &&
        skeletonBoneCount > 0) {

        const auto& anim = loadedAnimFile->animations[selectedAnimIndex];
        int frame = currentAnimFrame;

        // --- Step 1: Collect per-bone local rotation quaternions from decoded tracks ---
        // quat as (x,y,z,w), default = identity (0,0,0,1)
        struct BoneAnimQuat { float x=0, y=0, z=0, w=1; bool hasData=false; };
        std::vector<BoneAnimQuat> boneLocalQuats(MAX_BONES);
        // Track position offsets for fakeroot/IK
        float rootOffset[3] = {0,0,0};

        // Helper: find skeleton component by type
        auto findSkelComp = [&](uint32_t type1, uint32_t type2 = 0) -> const NalComponentData* {
            for (const auto& sc : loadedSkeleton->components) {
                if (sc.type_id == type1 || (type2 && sc.type_id == type2))
                    return &sc;
            }
            return nullptr;
        };

        // Helper: set bone quat from track data
        auto setBoneQuat = [&](int boneIdx, float qx, float qy, float qz) {
            if (boneIdx < 0 || boneIdx >= MAX_BONES) return;
            float w2 = 1.f - (qx*qx + qy*qy + qz*qz);
            boneLocalQuats[boneIdx] = {qx, qy, qz, sqrtf(fabsf(w2)), true};
        };

        // Helper: apply masked quats to bone array
        auto applyMaskedQuats = [&](const std::vector<float>& fv, uint32_t mask,
                                     const std::vector<int>& bones, int maxBits) {
            int trackIdx = 0;
            for (int bit = 0; bit < maxBits; bit++) {
                if (!(mask & (1u << bit))) continue;
                if (trackIdx + 2 >= (int)fv.size()) break;
                if (bit < (int)bones.size())
                    setBoneQuat(bones[bit], fv[trackIdx], fv[trackIdx+1], fv[trackIdx+2]);
                trackIdx += 3;
            }
        };

        for (const auto& comp : anim.components) {
            if (frame < 0 || frame >= (int)comp.decoded.frames.size()) continue;
            const auto& fv = comp.decoded.frames[frame];

            if (comp.comp_ix == NalComp::TORSO_HEAD || comp.comp_ix == NalComp::TORSO_HEAD_STD) {
                const auto* sc = findSkelComp(NalCompType::TorsoHead_TwoNeck, NalCompType::TorsoHead_OneNeck);
                if (!sc || sc->bone_indices.size() < 6) continue;
                int roles[] = {TorsoBone::SPINE, TorsoBone::SPINE1, TorsoBone::SPINE2, TorsoBone::NECK, TorsoBone::HEAD};
                int trackIdx = 0;
                for (int qi = 0; qi < 5; qi++) {
                    if (!(comp.mask & (1u << qi))) continue;
                    if (trackIdx + 2 >= (int)fv.size()) break;
                    setBoneQuat(sc->bone_indices[roles[qi]], fv[trackIdx], fv[trackIdx+1], fv[trackIdx+2]);
                    trackIdx += 3;
                }
                if (comp.mask & 0x20 && trackIdx + 5 < (int)fv.size()) {
                    setBoneQuat(sc->bone_indices[TorsoBone::PELVIS], fv[trackIdx], fv[trackIdx+1], fv[trackIdx+2]);
                }
            }
            else if (comp.comp_ix == NalComp::LEGS) {
                const auto* sc = findSkelComp(NalCompType::LegsFeet_Compressed);
                if (sc && sc->bone_indices.size() >= 8) applyMaskedQuats(fv, comp.mask, sc->bone_indices, 8);
            }
            else if (comp.comp_ix == NalComp::LEGS_IK) {
                const auto* sc = findSkelComp(NalCompType::LegsFeet_IK);
                if (!sc || sc->bone_indices.size() < 8) continue;
                int trackIdx = 0;
                for (int bit = 0; bit < 2; bit++) {
                    if (!(comp.mask & (1u << bit))) continue;
                    if (trackIdx + 2 >= (int)fv.size()) break;
                    setBoneQuat(sc->bone_indices[bit], fv[trackIdx], fv[trackIdx+1], fv[trackIdx+2]);
                    trackIdx += 3;
                }
                for (int bit = 2; bit < 4; bit++) {
                    if (!(comp.mask & (1u << bit))) continue;
                    if (trackIdx + 2 >= (int)fv.size()) break;
                    setBoneQuat(sc->bone_indices[bit], fv[trackIdx], fv[trackIdx+1], fv[trackIdx+2]);
                    trackIdx += 7;
                }
            }
            else if (comp.comp_ix == NalComp::ARMS) {
                const auto* sc = findSkelComp(NalCompType::ArmsHands_Compressed);
                if (sc && sc->bone_indices.size() >= 8) applyMaskedQuats(fv, comp.mask, sc->bone_indices, 8);
            }
            else if (comp.comp_ix == NalComp::ARMS_IK) {
                const auto* sc = findSkelComp(NalCompType::ArmsHands_IK);
                if (!sc || sc->bone_indices.size() < 8) continue;
                int trackIdx = 0;
                for (int bit = 0; bit < 2; bit++) {
                    if (!(comp.mask & (1u << bit))) continue;
                    if (trackIdx + 2 >= (int)fv.size()) break;
                    setBoneQuat(sc->bone_indices[bit], fv[trackIdx], fv[trackIdx+1], fv[trackIdx+2]);
                    trackIdx += 3;
                }
                for (int bit = 2; bit < 4; bit++) {
                    if (!(comp.mask & (1u << bit))) continue;
                    if (trackIdx + 2 >= (int)fv.size()) break;
                    setBoneQuat(sc->bone_indices[bit], fv[trackIdx], fv[trackIdx+1], fv[trackIdx+2]);
                    trackIdx += 7;
                }
            }
            else if (comp.comp_ix == NalComp::FAKEROOT_STD) {
                if (comp.mask & 0x1 && fv.size() >= 6) {
                    rootOffset[0] = fv[3]; rootOffset[1] = fv[4]; rootOffset[2] = fv[5];
                }
            }
            else if (comp.comp_ix >= NalComp::FING52 && comp.comp_ix <= NalComp::FING5) {
                const NalComponentData* sc = nullptr;
                for (const auto& s : loadedSkeleton->components) {
                    if (s.type_id == NalCompType::FiveFinger_Top2KnuckleCurl ||
                        s.type_id == NalCompType::FiveFinger_IndividualCurl ||
                        s.type_id == NalCompType::FiveFinger_ReducedAngular ||
                        s.type_id == NalCompType::FiveFinger_FullRotational) { sc = &s; break; }
                }
                if (!sc || sc->bone_indices.size() < 30) continue;
                int trackIdx = 0;
                for (int bi = 0; bi < 30 && bi < (int)sc->bone_indices.size(); bi++) {
                    if (trackIdx + 2 >= (int)fv.size()) break;
                    setBoneQuat(sc->bone_indices[bi], fv[trackIdx], fv[trackIdx+1], fv[trackIdx+2]);
                    trackIdx += 3;
                }
            }
        }

        // --- Step 2: Build world-space animated matrices through hierarchy ---
        // For each bone: worldAnim[bone] = worldAnim[parent] * localRotation * bindLocalOffset
        // bindLocalOffset = invBind[parent] * bind[child]
        // skinMatrix[bone] = worldAnim[bone] * invBind[bone]

        // First build animated world matrices
        std::vector<float> animWorld(MAX_BONES * 16, 0.f);
        for (int i = 0; i < MAX_BONES; i++) {
            animWorld[i*16+0] = 1.f; animWorld[i*16+5] = 1.f;
            animWorld[i*16+10] = 1.f; animWorld[i*16+15] = 1.f;
        }
        // Copy bind matrices as starting point
        for (int i = 0; i < skeletonBoneCount && i < MAX_BONES; i++) {
            memcpy(&animWorld[i*16], skeletonBones[i].bindMatrix, 64);
        }

        // Apply rotations: for each bone with animation data, rotate the bind matrix
        // This is a simplified approach: rotate the bone's bind-pose world matrix
        // by the animation quaternion around the bone's own position
        for (int i = 0; i < skeletonBoneCount && i < MAX_BONES; i++) {
            auto& bq = boneLocalQuats[i];
            if (!bq.hasData) continue;

            float qx=bq.x, qy=bq.y, qz=bq.z, qw=bq.w;
            float xx=qx*qx, yy=qy*qy, zz=qz*qz;
            float xy=qx*qy, xz=qx*qz, yz=qy*qz;
            float wx=qw*qx, wy=qw*qy, wz=qw*qz;

            // Rotation matrix from quaternion
            float R[9] = {
                1.f-2.f*(yy+zz), 2.f*(xy+wz),     2.f*(xz-wy),
                2.f*(xy-wz),     1.f-2.f*(xx+zz), 2.f*(yz+wx),
                2.f*(xz+wy),     2.f*(yz-wx),     1.f-2.f*(xx+yy)
            };

            // Rotate the 3x3 part of the bind matrix: newRot = R * oldRot
            float* m = &animWorld[i*16];
            float oldRot[9] = {m[0],m[1],m[2], m[4],m[5],m[6], m[8],m[9],m[10]};
            m[0]  = R[0]*oldRot[0] + R[1]*oldRot[3] + R[2]*oldRot[6];
            m[1]  = R[0]*oldRot[1] + R[1]*oldRot[4] + R[2]*oldRot[7];
            m[2]  = R[0]*oldRot[2] + R[1]*oldRot[5] + R[2]*oldRot[8];
            m[4]  = R[3]*oldRot[0] + R[4]*oldRot[3] + R[5]*oldRot[6];
            m[5]  = R[3]*oldRot[1] + R[4]*oldRot[4] + R[5]*oldRot[7];
            m[6]  = R[3]*oldRot[2] + R[4]*oldRot[5] + R[5]*oldRot[8];
            m[8]  = R[6]*oldRot[0] + R[7]*oldRot[3] + R[8]*oldRot[6];
            m[9]  = R[6]*oldRot[1] + R[7]*oldRot[4] + R[8]*oldRot[7];
            m[10] = R[6]*oldRot[2] + R[7]*oldRot[5] + R[8]*oldRot[8];
        }

        // Apply root offset from fakeroot
        for (int i = 0; i < skeletonBoneCount && i < MAX_BONES; i++) {
            animWorld[i*16+12] += rootOffset[0];
            animWorld[i*16+13] += rootOffset[1];
            animWorld[i*16+14] += rootOffset[2];
        }

        // --- Step 3: Compute skinning matrices = animWorld * invBind ---
        for (int i = 0; i < skeletonBoneCount && i < MAX_BONES; i++) {
            float* aw = &animWorld[i*16];
            float* ib = skeletonBones[i].invBindMatrix;
            float* out = &boneMatData[i*16];
            for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) {
                out[r*4+c] = aw[r*4+0]*ib[0*4+c] + aw[r*4+1]*ib[1*4+c] +
                             aw[r*4+2]*ib[2*4+c] + aw[r*4+3]*ib[3*4+c];
            }
        }

        // Update bone positions for skeleton overlay
        for (int i = 0; i < skeletonBoneCount && i < MAX_BONES; i++) {
            bonePosWorld[i*3+0] = animWorld[i*16+12];
            bonePosWorld[i*3+1] = animWorld[i*16+13];
            bonePosWorld[i*3+2] = animWorld[i*16+14];
        }

        glUniformMatrix4fv(locBoneMatrices, MAX_BONES, GL_FALSE, boneMatData.data());
        skinningActive = true;
    }
    glUniform1i(locUseSkinning, skinningActive ? 1 : 0);

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
    glDisable(GL_CULL_FACE);  // No backface culling - render both sides of all geometry

    // First pass: render ALL meshes as opaque (alpha disabled)
    for (int i = 0; i < (int)previewMeshes.size(); i++) {
        const auto& m = previewMeshes[i];
        if (m.isHidden) continue;
        if (m.indexCount > 0 && !m.isFakeShadow && !m.isColorVolume) {
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

    // Second pass: DISABLED (all meshes rendered as opaque above)
    // glEnable(GL_BLEND);
    // glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);

    // Skeleton overlay (NAL-based positions set in BuildSkeletonVisual)
    if (showSkeleton && !isWorldMode && skeletonBoneCount > 0) {
        RenderSkeletonOverlay();
    }

    glBindFramebuffer(GL_READ_FRAMEBUFFER, msFbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, modelFbo);
    glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}


// Compute bone world positions from NAL skeleton data
// Primary: use offset_locs chained through hierarchy
// Fallback: generate positions from bone names (always works)
void SpiderManTool::ComputeNALBonePositions() {
    nalBonePositions.clear();
    nalMaxBoneIndex = -1;
    if (!loadedSkeleton || loadedSkeleton->bone_map.empty()) {
        Log("ComputeNALBonePositions: no skeleton data");
        return;
    }

    // --- Fallback: generate humanoid positions from bone names ---
    // This always works as long as we have bone_map and parent_map
    auto nameToPos = [](const std::string& name) -> std::array<float,3> {
        // Humanoid position estimates (Y-up, model centered at origin)
        if (name == "pelvis")       return {0.0f, 2.8f, 0.0f};
        if (name == "spine")        return {0.0f, 3.2f, 0.0f};
        if (name == "spine1")       return {0.0f, 3.6f, 0.0f};
        if (name == "spine2")       return {0.0f, 4.0f, 0.0f};
        if (name == "neck")         return {0.0f, 4.5f, 0.0f};
        if (name == "head")         return {0.0f, 5.0f, 0.0f};
        if (name == "l_clavicle")   return {0.3f, 4.3f, 0.0f};
        if (name == "l_upperarm")   return {0.8f, 4.2f, 0.0f};
        if (name == "l_forearm")    return {1.4f, 4.0f, 0.0f};
        if (name == "l_hand")       return {1.9f, 3.8f, 0.0f};
        if (name == "r_clavicle")   return {-0.3f, 4.3f, 0.0f};
        if (name == "r_upperarm")   return {-0.8f, 4.2f, 0.0f};
        if (name == "r_forearm")    return {-1.4f, 4.0f, 0.0f};
        if (name == "r_hand")       return {-1.9f, 3.8f, 0.0f};
        if (name == "l_thigh")      return {0.3f, 2.5f, 0.0f};
        if (name == "l_calf")       return {0.3f, 1.5f, 0.0f};
        if (name == "l_foot")       return {0.3f, 0.3f, 0.2f};
        if (name == "l_toe")        return {0.3f, 0.0f, 0.5f};
        if (name == "r_thigh")      return {-0.3f, 2.5f, 0.0f};
        if (name == "r_calf")       return {-0.3f, 1.5f, 0.0f};
        if (name == "r_foot")       return {-0.3f, 0.3f, 0.2f};
        if (name == "r_toe")        return {-0.3f, 0.0f, 0.5f};
        if (name == "l_fore_twist0") return {1.5f, 4.0f, 0.0f};
        if (name == "l_fore_twist1") return {1.6f, 3.9f, 0.0f};
        if (name == "r_fore_twist0") return {-1.5f, 4.0f, 0.0f};
        if (name == "r_fore_twist1") return {-1.6f, 3.9f, 0.0f};
        return {0.0f, 3.0f, 0.0f}; // unknown bones near center
    };

    // First try offset_locs from parsed skeleton components
    auto setPos = [&](int idx, float x, float y, float z) {
        if (idx < 0) return;
        nalBonePositions[idx] = {x, y, z};
        if (idx > nalMaxBoneIndex) nalMaxBoneIndex = idx;
    };
    auto getPos = [&](int idx) -> std::array<float,3> {
        if (nalBonePositions.count(idx)) return nalBonePositions[idx];
        return {0,0,0};
    };
    auto chainBone = [&](int childIdx, int parentIdx, const std::array<float,3>& offset) {
        auto pp = getPos(parentIdx);
        setPos(childIdx, pp[0]+offset[0], pp[1]+offset[1], pp[2]+offset[2]);
    };

    // Try offset_locs from torso
    bool hasOffsetLocs = false;
    for (auto& c : loadedSkeleton->components) {
        if (c.type_id != NalCompType::TorsoHead_TwoNeck && c.type_id != NalCompType::TorsoHead_OneNeck) continue;
        if (c.bone_indices.size() >= 6 && c.offset_locs.size() >= 5) {
            // Check if any offset_loc is non-zero
            for (auto& ol : c.offset_locs) {
                if (fabsf(ol[0]) > 0.001f || fabsf(ol[1]) > 0.001f || fabsf(ol[2]) > 0.001f) {
                    hasOffsetLocs = true;
                    break;
                }
            }
        }
        break;
    }

    Log("ComputeNALBonePositions: offset_locs " + std::string(hasOffsetLocs ? "available" : "EMPTY, using name-based fallback"));

    if (hasOffsetLocs) {
        // -- Use offset_locs (exact positions from skeleton file) --
        for (auto& c : loadedSkeleton->components) {
            if (c.type_id != NalCompType::TorsoHead_TwoNeck && c.type_id != NalCompType::TorsoHead_OneNeck) continue;
            if (c.bone_indices.size() < 6 || c.offset_locs.size() < 5) continue;
            std::array<float,3> pelvisPos = {0, 0, 0};
            if (c.default_pose.valid) pelvisPos = c.default_pose.pelvis_pos;
            setPos(c.bone_indices[TorsoBone::PELVIS], pelvisPos[0], pelvisPos[1], pelvisPos[2]);
            chainBone(c.bone_indices[TorsoBone::SPINE],  c.bone_indices[TorsoBone::PELVIS], c.offset_locs[0]);
            chainBone(c.bone_indices[TorsoBone::SPINE1], c.bone_indices[TorsoBone::SPINE],  c.offset_locs[1]);
            chainBone(c.bone_indices[TorsoBone::SPINE2], c.bone_indices[TorsoBone::SPINE1], c.offset_locs[2]);
            chainBone(c.bone_indices[TorsoBone::NECK],   c.bone_indices[TorsoBone::SPINE2], c.offset_locs[3]);
            chainBone(c.bone_indices[TorsoBone::HEAD],   c.bone_indices[TorsoBone::NECK],   c.offset_locs[4]);
            break;
        }
        for (auto& c : loadedSkeleton->components) {
            if (c.type_id != NalCompType::LegsFeet_IK && c.type_id != NalCompType::LegsFeet_Compressed) continue;
            if (c.bone_indices.size() < 8 || c.offset_locs.size() < 8) continue;
            int pelvisIdx = (c.bone_indices.size() > 8) ? c.bone_indices[LegBone::PELVIS] : 0;
            if (!nalBonePositions.count(pelvisIdx)) setPos(pelvisIdx, 0, 0, 0);
            chainBone(c.bone_indices[LegBone::L_THIGH], pelvisIdx, c.offset_locs[LegBone::L_THIGH]);
            chainBone(c.bone_indices[LegBone::L_CALF],  c.bone_indices[LegBone::L_THIGH], c.offset_locs[LegBone::L_CALF]);
            chainBone(c.bone_indices[LegBone::L_FOOT],  c.bone_indices[LegBone::L_CALF],  c.offset_locs[LegBone::L_FOOT]);
            chainBone(c.bone_indices[LegBone::L_TOE],   c.bone_indices[LegBone::L_FOOT],  c.offset_locs[LegBone::L_TOE]);
            chainBone(c.bone_indices[LegBone::R_THIGH], pelvisIdx, c.offset_locs[LegBone::R_THIGH]);
            chainBone(c.bone_indices[LegBone::R_CALF],  c.bone_indices[LegBone::R_THIGH], c.offset_locs[LegBone::R_CALF]);
            chainBone(c.bone_indices[LegBone::R_FOOT],  c.bone_indices[LegBone::R_CALF],  c.offset_locs[LegBone::R_FOOT]);
            chainBone(c.bone_indices[LegBone::R_TOE],   c.bone_indices[LegBone::R_FOOT],  c.offset_locs[LegBone::R_TOE]);
            break;
        }
        for (auto& c : loadedSkeleton->components) {
            if (c.type_id != NalCompType::ArmsHands_IK && c.type_id != NalCompType::ArmsHands_Compressed) continue;
            if (c.offset_locs.size() < 8) continue;
            int armParent = 0;
            for (auto& [idx, name] : loadedSkeleton->bone_map) {
                if (name == "spine2" && nalBonePositions.count(idx)) { armParent = idx; break; }
            }
            int l_clav = c.bone_indices[0], l_upper, l_fore, l_hand, r_clav, r_upper, r_fore, r_hand;
            if (c.type_id == NalCompType::ArmsHands_IK) {
                l_clav = c.bone_indices[ArmIKBone::L_CLAV]; r_clav = c.bone_indices[ArmIKBone::R_CLAV];
                l_upper = c.bone_indices[ArmIKBone::L_UPPER]; r_upper = c.bone_indices[ArmIKBone::R_UPPER];
                l_fore = c.bone_indices[ArmIKBone::L_FORE]; r_fore = c.bone_indices[ArmIKBone::R_FORE];
                l_hand = c.bone_indices[ArmIKBone::L_HAND]; r_hand = c.bone_indices[ArmIKBone::R_HAND];
            } else {
                l_clav = c.bone_indices[ArmBone::L_CLAV]; r_clav = c.bone_indices[ArmBone::R_CLAV];
                l_upper = c.bone_indices[ArmBone::L_UPPER]; r_upper = c.bone_indices[ArmBone::R_UPPER];
                l_fore = c.bone_indices[ArmBone::L_FORE]; r_fore = c.bone_indices[ArmBone::R_FORE];
                l_hand = c.bone_indices[ArmBone::L_HAND]; r_hand = c.bone_indices[ArmBone::R_HAND];
            }
            chainBone(l_clav, armParent, c.offset_locs[0]);
            chainBone(l_upper, l_clav, c.offset_locs[1]);
            chainBone(l_fore, l_upper, c.offset_locs[2]);
            chainBone(l_hand, l_fore, c.offset_locs[3]);
            chainBone(r_clav, armParent, c.offset_locs[4]);
            chainBone(r_upper, r_clav, c.offset_locs[5]);
            chainBone(r_fore, r_upper, c.offset_locs[6]);
            chainBone(r_hand, r_fore, c.offset_locs[7]);
            // Twist bones
            if (c.fore_twist_locs.size() >= 4) {
                int lt0=-1, lt1=-1, rt0=-1, rt1=-1;
                if (c.type_id == NalCompType::ArmsHands_IK && (int)c.bone_indices.size() > ArmIKBone::R_TWIST1) {
                    lt0=c.bone_indices[ArmIKBone::L_TWIST0]; lt1=c.bone_indices[ArmIKBone::L_TWIST1];
                    rt0=c.bone_indices[ArmIKBone::R_TWIST0]; rt1=c.bone_indices[ArmIKBone::R_TWIST1];
                } else if ((int)c.bone_indices.size() > ArmBone::R_TWIST1) {
                    lt0=c.bone_indices[ArmBone::L_TWIST0]; lt1=c.bone_indices[ArmBone::L_TWIST1];
                    rt0=c.bone_indices[ArmBone::R_TWIST0]; rt1=c.bone_indices[ArmBone::R_TWIST1];
                }
                if (lt0>=0) chainBone(lt0, l_fore, c.fore_twist_locs[0]);
                if (lt1>=0 && lt0>=0) chainBone(lt1, lt0, c.fore_twist_locs[1]);
                if (rt0>=0) chainBone(rt0, r_fore, c.fore_twist_locs[2]);
                if (rt1>=0 && rt0>=0) chainBone(rt1, rt0, c.fore_twist_locs[3]);
            }
            break;
        }
    }

    // Fallback: if we got fewer than 3 bones from offset_locs, use name-based positions
    if ((int)nalBonePositions.size() < 3) {
        Log("ComputeNALBonePositions: offset_locs produced " + std::to_string(nalBonePositions.size()) + " bones, using name-based fallback");
        nalBonePositions.clear();
        nalMaxBoneIndex = -1;

        for (const auto& [idx, name] : loadedSkeleton->bone_map) {
            auto pos = nameToPos(name);
            setPos(idx, pos[0], pos[1], pos[2]);
        }
    }

    Log("ComputeNALBonePositions: " + std::to_string(nalBonePositions.size()) + " bones positioned");
}

void SpiderManTool::BuildSkeletonVisual(const std::vector<uint8_t>& pcmData) {
    if (skeletonVao) { glDeleteVertexArrays(1, &skeletonVao); skeletonVao = 0; }
    if (skeletonVbo) { glDeleteBuffers(1, &skeletonVbo); skeletonVbo = 0; }
    skeletonBoneCount = 0;
    skeletonLineVertCount = 0;
    skeletonBones.clear();
    selectedBoneIndex = -1;
    isRotatingBone = false;

    // Also read PCM bone matrices (for GPU skinning later)
    if (pcmData.size() >= 16) {
        BinaryReader br(pcmData);
        br.Seek(8);
        uint32_t numEntries = br.Read<uint32_t>();
        uint32_t entryTableOfs = br.Read<uint32_t>();
        if (numEntries <= 1000 && entryTableOfs < pcmData.size()) {
            uint32_t boneCount = 0, bonesOffset = 0;
            br.Seek(entryTableOfs);
            for (uint32_t i = 0; i < numEntries; i++) {
                uint16_t sz = br.Read<uint16_t>(); uint16_t tag = br.Read<uint16_t>();
                uint32_t dataOfs = br.Read<uint32_t>(); br.Skip(4);
                if (tag == 512 && dataOfs + 24 <= pcmData.size()) {
                    br.Seek(dataOfs + 16);
                    boneCount = br.Read<uint32_t>();
                    bonesOffset = br.Read<uint32_t>();
                    break;
                }
            }
            if (boneCount > 0 && boneCount <= 200 && bonesOffset + boneCount * 64 <= pcmData.size()) {
                skeletonBones.resize(boneCount);
                for (uint32_t i = 0; i < boneCount; i++) {
                    memcpy(skeletonBones[i].bindMatrix, &pcmData[bonesOffset + i * 64], 64);
                    skeletonBones[i].position[0] = skeletonBones[i].bindMatrix[12];
                    skeletonBones[i].position[1] = skeletonBones[i].bindMatrix[13];
                    skeletonBones[i].position[2] = skeletonBones[i].bindMatrix[14];
                    InvertMatrix(skeletonBones[i].bindMatrix, skeletonBones[i].invBindMatrix);
                }
                Log("PCM: " + std::to_string(boneCount) + " bind matrices loaded");
            }
        }
    }

    // Compute NAL bone positions (the Python approach)
    ComputeNALBonePositions();
    if (nalBonePositions.empty()) {
        Log("Skeleton: no NAL bone positions computed");
        return;
    }

    // Build sorted bone index list for VBO ordering
    std::vector<int> sortedIndices;
    for (auto& [idx, pos] : nalBonePositions) sortedIndices.push_back(idx);
    std::sort(sortedIndices.begin(), sortedIndices.end());
    nalBoneVboOrder = sortedIndices; // store for picking/name lookup

    // Map: NAL global index → VBO position
    std::map<int, int> nalToVbo;
    for (int i = 0; i < (int)sortedIndices.size(); i++)
        nalToVbo[sortedIndices[i]] = i;

    skeletonBoneCount = (int)sortedIndices.size();

    struct BoneVert { float x, y, z, r, g, b; };
    std::vector<BoneVert> pointVerts;
    std::vector<BoneVert> lineVerts;

    // Points for each bone (yellow)
    for (int nalIdx : sortedIndices) {
        auto& p = nalBonePositions[nalIdx];
        pointVerts.push_back({p[0], p[1], p[2], 1.0f, 1.0f, 0.0f});
    }

    // Hierarchy lines from parent_map (cyan)
    if (loadedSkeleton) {
        for (const auto& [childIdx, parentIdx] : loadedSkeleton->parent_map) {
            if (parentIdx < 0) continue;
            if (!nalBonePositions.count(childIdx) || !nalBonePositions.count(parentIdx)) continue;
            auto& cp = nalBonePositions[childIdx];
            auto& pp = nalBonePositions[parentIdx];
            lineVerts.push_back({pp[0], pp[1], pp[2], 0.0f, 0.9f, 1.0f});
            lineVerts.push_back({cp[0], cp[1], cp[2], 0.0f, 0.9f, 1.0f});
        }
    }

    skeletonLineVertCount = (int)lineVerts.size();

    std::vector<BoneVert> allVerts;
    allVerts.insert(allVerts.end(), pointVerts.begin(), pointVerts.end());
    allVerts.insert(allVerts.end(), lineVerts.begin(), lineVerts.end());

    glGenVertexArrays(1, &skeletonVao);
    glGenBuffers(1, &skeletonVbo);
    glBindVertexArray(skeletonVao);
    glBindBuffer(GL_ARRAY_BUFFER, skeletonVbo);
    glBufferData(GL_ARRAY_BUFFER, allVerts.size() * sizeof(BoneVert), allVerts.data(), GL_DYNAMIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(BoneVert), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(BoneVert), (void*)(3*sizeof(float)));
    glEnableVertexAttribArray(1);
    glDisableVertexAttribArray(2);
    glDisableVertexAttribArray(3);
    glDisableVertexAttribArray(4);
    glBindVertexArray(0);

    Log("Skeleton: " + std::to_string(skeletonBoneCount) + " NAL bones, " +
        std::to_string(skeletonLineVertCount/2) + " hierarchy lines");
}

void SpiderManTool::RenderSkeletonOverlay() {
    if (!skeletonVao || skeletonBoneCount == 0 || skeletonProgram == 0) return;

    glUseProgram(skeletonProgram);

    float fov = 1.0f;
    float aspect = 3840.0f / 2160.0f;
    float znear = 0.1f, zfar = 20000.0f;
    float proj[16] = {0};
    float tanHalfFov = tan(fov / 2.0f);
    proj[0] = 1.0f / (aspect * tanHalfFov);
    proj[5] = 1.0f / tanHalfFov;
    proj[10] = -(zfar + znear) / (zfar - znear);
    proj[11] = -1.0f;
    proj[14] = -(2.0f * zfar * znear) / (zfar - znear);

    float view[16];
    float target[3] = { camPos[0]+camFront[0], camPos[1]+camFront[1], camPos[2]+camFront[2] };
    LookAt(camPos, target, camUp, view);

    glUniformMatrix4fv(glGetUniformLocation(skeletonProgram, "projection"), 1, GL_FALSE, proj);
    glUniformMatrix4fv(glGetUniformLocation(skeletonProgram, "view"), 1, GL_FALSE, view);

    glBindVertexArray(skeletonVao);
    glDisable(GL_DEPTH_TEST);

    // Hierarchy lines (cyan)
    if (skeletonLineVertCount > 0) {
        glLineWidth(2.0f);
        glDrawArrays(GL_LINES, skeletonBoneCount, skeletonLineVertCount);
    }

    // Bone joints (yellow)
    glPointSize(6.0f);
    glDrawArrays(GL_POINTS, 0, skeletonBoneCount);

    // Selected bone (red, bigger)
    if (selectedBoneIndex >= 0 && selectedBoneIndex < skeletonBoneCount) {
        struct BoneVert { float x, y, z, r, g, b; };
        // selectedBoneIndex is a VBO index here
        glBindBuffer(GL_ARRAY_BUFFER, skeletonVbo);
        BoneVert sel;
        // Read current position from VBO
        glGetBufferSubData(GL_ARRAY_BUFFER, selectedBoneIndex * sizeof(BoneVert), sizeof(BoneVert), &sel);
        sel.r = 1.0f; sel.g = 0.2f; sel.b = 0.2f;
        glBufferSubData(GL_ARRAY_BUFFER, selectedBoneIndex * sizeof(BoneVert), sizeof(BoneVert), &sel);
        glPointSize(12.0f);
        glDrawArrays(GL_POINTS, selectedBoneIndex, 1);
        sel.r = 1.0f; sel.g = 1.0f; sel.b = 0.0f;
        glBufferSubData(GL_ARRAY_BUFFER, selectedBoneIndex * sizeof(BoneVert), sizeof(BoneVert), &sel);
    }

    glEnable(GL_DEPTH_TEST);
    glPointSize(1.0f);
    glLineWidth(1.0f);
    glBindVertexArray(0);
    glUseProgram(modelProgram);
}

int SpiderManTool::PickBoneAtScreenPos(float screenX, float screenY, float vpW, float vpH) {
    if (skeletonBoneCount == 0 || nalBoneVboOrder.empty()) return -1;

    float ndcX = (2.0f * screenX / vpW) - 1.0f;
    float ndcY = 1.0f - (2.0f * screenY / vpH);

    float fov = 1.0f;
    float fbAspect = 3840.0f / 2160.0f;
    float tanHalfFov = tan(fov / 2.0f);

    float right[3]; Cross(camFront, camUp, right); Normalize(right);
    float up[3]; Cross(right, camFront, up); Normalize(up);

    float rayDir[3] = {
        camFront[0] + right[0] * ndcX * tanHalfFov * fbAspect + up[0] * ndcY * tanHalfFov,
        camFront[1] + right[1] * ndcX * tanHalfFov * fbAspect + up[1] * ndcY * tanHalfFov,
        camFront[2] + right[2] * ndcX * tanHalfFov * fbAspect + up[2] * ndcY * tanHalfFov
    };
    Normalize(rayDir);

    int closest = -1;
    float closestDist = 1e30f;
    float pickRadius = 0.05f;

    for (int vboIdx = 0; vboIdx < (int)nalBoneVboOrder.size(); vboIdx++) {
        int nalIdx = nalBoneVboOrder[vboIdx];
        if (!nalBonePositions.count(nalIdx)) continue;
        auto& pos = nalBonePositions[nalIdx];

        float dx = pos[0] - camPos[0];
        float dy = pos[1] - camPos[1];
        float dz = pos[2] - camPos[2];
        float t = dx*rayDir[0] + dy*rayDir[1] + dz*rayDir[2];
        if (t < 0) continue;
        float px = camPos[0] + rayDir[0]*t - pos[0];
        float py = camPos[1] + rayDir[1]*t - pos[1];
        float pz = camPos[2] + rayDir[2]*t - pos[2];
        float dist = sqrt(px*px + py*py + pz*pz);

        float camDist = sqrt(dx*dx + dy*dy + dz*dz);
        float scaledRadius = pickRadius * camDist;

        if (dist < scaledRadius && t < closestDist) {
            closestDist = t;
            closest = vboIdx;
        }
    }
    return closest;
}

void SpiderManTool::ApplyBoneRotation(int boneIdx, float angle, int axis) {
    if (boneIdx < 0 || boneIdx >= skeletonBoneCount) return;

    // Build rotation matrix around the bone's position
    float c = cos(angle), s = sin(angle);
    float R[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    if (axis == 0) { R[5]=c; R[6]=s; R[9]=-s; R[10]=c; }      // X
    else if (axis == 1) { R[0]=c; R[2]=-s; R[8]=s; R[10]=c; }  // Y
    else { R[0]=c; R[1]=s; R[4]=-s; R[5]=c; }                   // Z

    float bx = skeletonBones[boneIdx].position[0];
    float by = skeletonBones[boneIdx].position[1];
    float bz = skeletonBones[boneIdx].position[2];

    // For each submesh, update vertices weighted to this bone
    for (auto& mesh : previewMeshes) {
        if (mesh.positions.empty()) continue;
        int vCount = (int)mesh.positions.size() / 3;

        // Re-upload vertex data — must match VBO layout (pos, norm, uv, boneIdx, boneWgt)
        bool modified = false;
        struct Vertex { float x,y,z,nx,ny,nz,u,v,boneIdx[4],boneWgt[4]; };
        std::vector<Vertex> verts(vCount);
        for (int v = 0; v < vCount; v++) {
            memset(&verts[v], 0, sizeof(Vertex));
            verts[v].x = mesh.positions[v*3]; verts[v].y = mesh.positions[v*3+1]; verts[v].z = mesh.positions[v*3+2];
            verts[v].nx = mesh.normals[v*3]; verts[v].ny = mesh.normals[v*3+1]; verts[v].nz = mesh.normals[v*3+2];
            verts[v].u = mesh.uvs[v*2]; verts[v].v = mesh.uvs[v*2+1];
        }

        // Simple: rotate ALL vertices near this bone by proximity
        for (int v = 0; v < vCount; v++) {
            float vx = verts[v].x - bx;
            float vy = verts[v].y - by;
            float vz = verts[v].z - bz;
            float dist = sqrt(vx*vx + vy*vy + vz*vz);
            if (dist > 0.3f) continue; // Only affect nearby vertices

            float weight = 1.0f - (dist / 0.3f);
            float rx = R[0]*vx + R[4]*vy + R[8]*vz;
            float ry = R[1]*vx + R[5]*vy + R[9]*vz;
            float rz = R[2]*vx + R[6]*vy + R[10]*vz;

            verts[v].x = bx + vx + (rx - vx) * weight;
            verts[v].y = by + vy + (ry - vy) * weight;
            verts[v].z = bz + vz + (rz - vz) * weight;
            modified = true;
        }

        if (modified) {
            glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo);
            glBufferSubData(GL_ARRAY_BUFFER, 0, verts.size() * sizeof(Vertex), verts.data());
        }
    }
}

void SpiderManTool::ResetBoneRotation() {
    // Reload the model to restore original vertex positions
    if (currentPcmIndex >= 0) {
        LoadModelToGL(currentPcmIndex);
    }
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

        if (IsWorldPack(packStem) && (fileStem == packStem + "c" || fileStem == packStem)) {
            isWorldMode = true;
            selectedMeshIndex = -1;
            selectedMeshPcmData.clear();
            showWorldMeshHexEditor = false;

            camPos[0] = 0.0f; camPos[1] = 200.0f; camPos[2] = -600.0f;
            camFront[0] = 0.0f; camFront[1] = -0.3f; camFront[2] = -1.0f;
            camUp[0] = 0.0f; camUp[1] = 1.0f; camUp[2] = 0.0f;
            camYaw = -90.0f;
            camPitch = -15.0f;
            camSpeed = 500.0f;

            float transformMatrix[16] = {0};
            transformMatrix[0] = -1.0f;
            transformMatrix[5] = 1.0f;
            transformMatrix[10] = 1.0f;
            transformMatrix[15] = 1.0f;

            std::vector<uint8_t> pcmData(pcPackData.begin() + e.offset, pcPackData.begin() + e.offset + e.size);
            AddMeshFromDataWithTransform(pcmData, e.name, nullptr, loadedPCPackPath, e.offset, transformMatrix);

            // Load all placed props (entities + orphan PCMs) from this pack
            LoadPackEntities(loadedPCPackPath, transformMatrix);

            LoadSkybox();
        } else {
            LoadModelToGL(index);
        }

        isModelLoaded = true;
        return;
    }

    if (!e.isDds) return;

    CloseDdsPreview();
    isModelPreview = false;

    if (e.offset + e.size > pcPackData.size()) return;
    if (e.size < sizeof(DDS_HEADER) + 4) return;
    const uint8_t* rawData = &pcPackData[e.offset];
    if (*(uint32_t*)rawData != 0x20534444) return;

    const DDS_HEADER* header = (const DDS_HEADER*)(rawData + 4);
    ddsWidth = header->dwWidth;
    ddsHeight = header->dwHeight;

    std::vector<uint8_t> ddsData(rawData, rawData + e.size);
    ddsTextureId = LoadTextureFromData(ddsData);
    if (ddsTextureId == 0) return;
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