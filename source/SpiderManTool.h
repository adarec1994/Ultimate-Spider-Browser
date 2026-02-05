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
    std::string meshName;
    std::string alphaFlag;
    std::string textureName;
    uint32_t shaderType = 0;
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
    bool isFakeShadow = false;
    bool isColorVolume = false;
    bool isHidden = false;
    bool skipPicking = false;
    uint32_t shaderType = 0;


    float bboxMin[3] = {0, 0, 0};
    float bboxMax[3] = {0, 0, 0};


    std::vector<float> positions;
    std::vector<float> normals;
    std::vector<float> uvs;
    std::vector<uint16_t> indices;


    std::string textureName;
    uint32_t textureHash = 0;


    std::string sourcePack;
    uint32_t sourceOffset = 0;
    uint32_t sourceSize = 0;
    std::string meshName;
};

struct PCMSkeletonInfo {
    uint32_t count = 0;
    uint32_t offset = 0;
};


struct PCMBoneInfo {
    int index;
    float posX, posY, posZ;
    int parentIndex;
    std::string inferredRole;
};


struct PCMLodInfo {
    std::string name;
    uint32_t nameOffset;
    uint32_t submeshCount;
    uint32_t boneCount;
    uint32_t bonesOffset;
    float lodDistance;
    uint32_t nextLodOffset;


    float boundsMin[3];
    float boundsMax[3];
};


struct PCMSubmeshInfo {
    std::string name;
    uint32_t nameOffset;


    uint32_t vertexCount;
    uint32_t vertexOffset;
    uint32_t indexCount;
    uint32_t indexOffset;
    uint32_t stride;
    uint32_t primitiveType;


    bool hasNormals;
    bool hasUV;
    bool hasBones;


    float boundingRadius;
    uint32_t vertexBufferSize;


    uint32_t boneMapOffset;
    uint32_t boneMapCount;


    std::string materialMeshName;
    std::string materialAlphaFlag;
    std::string materialTexture;
    bool isTranslucent;


    std::string shaderType;
    uint32_t shaderSize;
};


struct PCMMaterialInfo {
    std::string name;
    uint32_t nameOffset;
    std::string meshName;
    std::string alphaFlag;
    std::string textureName;
    uint32_t shaderSize;
    bool isTranslucent;
};


struct PCMFileInfo {

    uint32_t fileSize;
    uint32_t numEntries;
    uint32_t entryTableOffset;
    uint32_t stringTableOffset;


    uint32_t materialCount;
    uint32_t lodCount;
    uint32_t totalSubmeshes;
    uint32_t totalVertices;
    uint32_t totalIndices;


    std::vector<PCMMaterialInfo> materials;
    std::vector<PCMLodInfo> lods;
    std::vector<PCMSubmeshInfo> submeshes;
    std::vector<PCMBoneInfo> bones;


    uint32_t boneCount;
    uint32_t bonesOffset;
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


    std::string materialMeshName;
    std::string materialAlphaFlag;
    std::string materialTexture;
    bool isTranslucent;
};

struct TextureLocation {
    std::string packPath;
    uint32_t offset;
    uint32_t size;
};

struct GlobalSearchResult {
    int packIndex;
    std::string packName;
    std::string fileName;
    uint32_t hash;
    uint32_t offset;
    uint32_t size;
    bool isPcm;
    bool isDds;
};

class SpiderManTool {
public:
    enum AppState { STATE_SPLASH, STATE_BROWSER, STATE_LOADING };
    AppState currentState = STATE_SPLASH;


    bool isIndexing = false;
    int indexingProgress = 0;
    int indexingTotal = 0;
    std::string indexingCurrentPack;

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


    PCMFileInfo currentPcmDetails;
    bool showPcmDetailsPanel = false;


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
    void BuildGlobalTextureIndexStep(int packIndex);


    int selectedMeshIndex = -1;
    std::vector<uint8_t> selectedMeshPcmData;
    bool showWorldMeshHexEditor = false;
    ImGuiHexEditorState worldMeshHexEditor;


    bool RayIntersectAABB(const float rayOrigin[3], const float rayDir[3],
                          const float bboxMin[3], const float bboxMax[3], float& tMin);
    int PickMeshAtScreenPos(float screenX, float screenY, float viewportWidth, float viewportHeight);
    void HandleMeshPicking(float viewportX, float viewportY, float viewportWidth, float viewportHeight);
    void LoadSelectedMeshPcmData();

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
    void LoadBinaryDictionary(const std::string& path);
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
    void AnalyzePCMDetailed(const std::vector<uint8_t>& pcmData);

    void ParseMaterialEntries(const std::vector<uint8_t>& pcmData);
    MaterialDef ResolveMaterialByMeshOffset(uint32_t meshNameOffset);

    bool IsWorldPack(const std::string& name);
    bool IsWorldInteriorPack(const std::string& name);

    void AddMeshFromData(const std::vector<uint8_t>& pcmData, std::string modelName = "", std::function<unsigned int(uint32_t)> textureResolver = nullptr, const std::string& sourcePack = "", uint32_t sourceOffset = 0);
    void AddMeshFromDataWithTransform(const std::vector<uint8_t>& pcmData, std::string modelName = "", std::function<unsigned int(uint32_t)> textureResolver = nullptr, const std::string& sourcePack = "", uint32_t sourceOffset = 0, const float* transform = nullptr);
    void LoadBackgroundMeshes();
    void LoadSkybox();
    void LoadAllWorldGeometries();


    std::vector<GlobalSearchResult> globalSearchResults;
    int selectedGlobalSearchIndex = -1;
    bool isGlobalSearchMode = false;
    std::string lastGlobalSearchQuery;
    void SearchAllPacks(const std::string& query);
    void SelectGlobalSearchResult(int index);

    void ExportSelectedWorldMesh(bool asGlb);
};