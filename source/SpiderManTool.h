#pragma once
#include "Helpers.h"
#include <filesystem>
#include <map>
#include <array>
#include <iostream>
#include <glad/glad.h>
#include "imgui_hex.h"
#include <functional>
#include <optional>
#include <memory>

// Forward declarations for NAL types (full defs in NalIntegration.h)
struct NalSkeletonData;
struct NalAnimFile;
struct NalAnimEntry;

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
    std::vector<uint16_t> bonePalette;


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
    unsigned int skeletonProgram = 0; // Separate shader for skeleton (solid color, no lighting)
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

    // Skeleton visualization & bone manipulation
    bool showSkeleton = false;
    int selectedBoneIndex = -1;
    bool isRotatingBone = false;
    float boneRotationAngle = 0.0f;
    int boneRotationAxis = 1; // 0=X, 1=Y, 2=Z
    std::map<int, std::array<float, 3>> manualBoneRotations;      // NAL/PCM bone index -> XYZ radians
    std::map<int, std::array<float, 3>> boneRotationsBeforeEdit;   // used for Esc cancel while rotating

    unsigned int skeletonVao = 0;
    unsigned int skeletonVbo = 0;
    int skeletonBoneCount = 0;
    int skeletonLineVertCount = 0; // number of line vertices after bone points in VBO

    struct BoneData {
        float bindMatrix[16];     // Original 4x4 matrix from PCM
        float invBindMatrix[16];  // Inverse of bind matrix
        float position[3];       // Model-space position (mat[12..14])
    };
    std::vector<BoneData> skeletonBones;

    void BuildSkeletonVisual(const std::vector<uint8_t>& pcmData);
    void RenderSkeletonOverlay();
    int PickBoneAtScreenPos(float screenX, float screenY, float vpW, float vpH);
    void ApplyBoneRotation(int boneIdx, float angle, int axis);
    void ResetBoneRotation();

    // NAL-computed bone world positions (keyed by global NAL bone index)
    std::map<int, std::array<float,3>> nalBonePositions;
    std::vector<int> nalBoneVboOrder; // VBO index → NAL global index
    int nalMaxBoneIndex = -1;
    void ComputeNALBonePositions();
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
    void AddMeshFromDataWithTransform(const std::vector<uint8_t>& pcmData, std::string modelName = "", std::function<unsigned int(uint32_t)> textureResolver = nullptr, const std::string& sourcePack = "", uint32_t sourceOffset = 0, const float* transform = nullptr, uint32_t onlyMeshOffset = 0xFFFFFFFFu);
    void BatchWorldMeshesByType();
    void LoadBackgroundMeshes();
    void LoadSkybox();
    void LoadAllWorldGeometries();
    void LoadPackEntities(const std::string& packFilePath, const float* baseTransform);


    std::vector<GlobalSearchResult> globalSearchResults;
    int selectedGlobalSearchIndex = -1;
    bool isGlobalSearchMode = false;
    std::string lastGlobalSearchQuery;
    void SearchAllPacks(const std::string& query);
    void SelectGlobalSearchResult(int index);

    void ExportSelectedWorldMesh(bool asGlb);
    void ExtractAllWorldMeshes();

    // === NAL Skeleton / Animation support ===
    std::shared_ptr<NalSkeletonData> loadedSkeleton;
    std::shared_ptr<NalAnimFile>     loadedAnimFile;
    int selectedAnimIndex = -1;
    int currentAnimFrame  = 0;
    bool isAnimPlaying    = false;
    float animPlaybackTime = 0.f;
    std::string loadedSkeletonName;
    std::string loadedAnimName;

    void LoadSkeletonForCurrentPack();
    void LoadAnimationForCurrentPack();
    void UpdateAnimationPlayback(float deltaTime);
    int FindEntryBySignature(uint32_t sig) const;
};
