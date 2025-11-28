#pragma once
#include "Helpers.h"
#include <filesystem>
#include <map>
#include <iostream>
#include <glad/glad.h>
#include "imgui_hex.h"
#include <functional>

namespace fs = std::filesystem;

struct FileEntry {
    uint32_t hash;
    uint32_t type;
    uint32_t offset;
    uint32_t size;
    std::string name;
    bool isPcm;
    bool isDds;
    std::vector<std::string> subItems;
};

struct RenderMesh {
    unsigned int vao;
    unsigned int vbo;
    unsigned int ebo;
    int indexCount;
    GLenum mode;
    unsigned int textureId;
};

struct PCMSkeletonInfo {
    uint32_t count = 0;
    uint32_t offset = 0;
};

struct PCMMeshInfo {
    uint32_t vCount;
    uint32_t vOffset;
    uint32_t iCount;
    uint32_t iOffset;
    uint32_t stride;
    uint32_t primitiveType;
    bool hasUV;
    bool hasBones;
};

struct TextureLocation {
    std::string packPath;
    uint32_t offset;
    uint32_t size;
};

class SpiderManTool {
public:
    enum AppState { STATE_SPLASH, STATE_BROWSER };
    AppState currentState = STATE_SPLASH;

    std::string searchPath = ".";
    std::vector<fs::path> foundPacks;
    int selectedPackIndex = -1;
    int selectedFileIndex = -1;

    std::map<uint32_t, std::string> dictionary;
    std::vector<FileEntry> entries;
    std::string loadedPCPackPath;
    std::vector<uint8_t> pcPackData;
    uint32_t dataOffset = 0;
    std::string logBuffer;

    unsigned int viewportTextureId = 0;
    bool isModelLoaded = false;
    bool isModelPreview = false;


    unsigned int ddsTextureId = 0;
    int ddsWidth = 0;
    int ddsHeight = 0;
    bool showDdsPopup = false;

    bool showHexEditor = false;
    ImGuiHexEditorState hexEditor;

    std::vector<PCMMeshInfo> currentPcmInfos;
    PCMSkeletonInfo currentPcmSkeleton;
    int currentPcmIndex = -1;

    unsigned int modelFbo = 0;
    unsigned int modelRbo = 0;

    unsigned int msFbo = 0;
    unsigned int msColor = 0;
    unsigned int msRbo = 0;

    unsigned int modelProgram = 0;
    std::vector<RenderMesh> previewMeshes;

    std::map<uint32_t, unsigned int> textureCache;
    std::map<uint32_t, TextureLocation> globalTextureIndex;

    bool isWorldMode = false;
    float modelCenter[3] = {0.0f, 0.0f, 0.0f};
    float modelRadius = 1.0f;
    float camPos[3] = {0.0f, 10.0f, 50.0f};
    float camFront[3] = {0.0f, 0.0f, -1.0f};
    float camUp[3] = {0.0f, 1.0f, 0.0f};
    float camYaw = -90.0f;
    float camPitch = 0.0f;
    float camSpeed = 100.0f;

    void Log(const std::string& msg);
    void SaveConfig();
    void LoadConfig();
    void ScanDirectory();
    void LoadDictionary(const std::string& path);
    void OpenPCPack(const std::string& path);
    void ExtractPack(const std::string& packPath, bool convertAll = false);

    void ExtractFile(int index, bool asGlb = false);

    void LoadPreview(int index);
    void CloseDdsPreview();
    void InitModelPreview();
    void LoadModelToGL(int index);
    void RenderModelPreview();
    void UpdateWorldCamera(bool isHovered);
    unsigned int LoadTextureFromHash(uint32_t hash);
    unsigned int LoadTextureFromData(const std::vector<uint8_t>& data);

    void ConvertPCM(const std::vector<uint8_t>& pcmData, const std::string& outPath);
    void AnalyzePCM(int index);

    bool IsWorldPack(const std::string& name);
    bool IsWorldInteriorPack(const std::string& name);

    void AddMeshFromData(const std::vector<uint8_t>& pcmData, std::string modelName = "", std::function<unsigned int(uint32_t)> textureResolver = nullptr);
    void LoadAllWorldGeometries();
};