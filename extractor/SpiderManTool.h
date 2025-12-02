#pragma once
#include "Helpers.h"
#include <filesystem>
#include <map>
#include <iostream>
#include <glad/glad.h>
#include "imgui_hex.h"
#include <functional>

namespace fs = std::filesystem;

struct MaterialDef {
    std::string meshName;      // +0x00: mesh name this material applies to
    std::string alphaFlag;     // +0x04: "smsimple", "smtranslucent", etc.
    std::string textureName;   // +0x60: actual texture name
    bool isTranslucent = false;
};

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
    bool isTranslucent = false;
};

struct PCMSkeletonInfo {
    uint32_t count = 0;
    uint32_t offset = 0;
};

struct PCMMeshInfo {
    std::string name;
    uint32_t vCount;
    uint32_t vOffset;
    uint32_t iCount;
    uint32_t iOffset;
    uint32_t stride;
    uint32_t primitiveType;
    bool hasUV;
    bool hasBones;

    // Material info (linked from material entry)
    std::string materialMeshName;  // From material +0x00
    std::string materialAlphaFlag; // From material +0x04
    std::string materialTexture;   // From material +0x60
    bool isTranslucent;
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

    bool showAssetBrowser = true;
    char searchBuffer[256] = "";

    int currentFileFilter = 0;

    std::map<uint32_t, std::string> dictionary;
    std::vector<FileEntry> entries;
    std::string loadedPCPackPath;
    std::vector<uint8_t> pcPackData;
    uint32_t dataOffset = 0;
    std::string logBuffer;

    std::string notificationMsg;
    float notificationTimer = 0.0f;
    const float NOTIFICATION_DURATION = 3.0f;

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

    // Material map keyed by mesh_name offset for linking
    std::map<uint32_t, MaterialDef> materialMap;

    unsigned int modelFbo = 0;
    unsigned int modelRbo = 0;

    unsigned int msFbo = 0;
    unsigned int msColor = 0;
    unsigned int msRbo = 0;

    unsigned int modelProgram = 0;
    std::vector<RenderMesh> previewMeshes;

    std::map<uint32_t, unsigned int> textureCache;
    std::map<std::string, unsigned int> textureNameCache;
    std::map<uint32_t, TextureLocation> globalTextureIndex;
    std::map<std::string, TextureLocation> globalTextureNameIndex;

    void BuildGlobalTextureIndex();

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
    void ShowNotification(const std::string& msg);
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
    unsigned int LoadTextureByName(const std::string& textureName);

    void ConvertPCM(const std::vector<uint8_t>& pcmData, const std::string& outPath);
    void AnalyzePCM(int index);

    void ParseMaterialEntries(const std::vector<uint8_t>& pcmData);
    MaterialDef ResolveMaterialByMeshOffset(uint32_t meshNameOffset);

    bool IsWorldPack(const std::string& name);
    bool IsWorldInteriorPack(const std::string& name);

    void AddMeshFromData(const std::vector<uint8_t>& pcmData, std::string modelName = "", std::function<unsigned int(uint32_t)> textureResolver = nullptr);
    void LoadAllWorldGeometries();
};