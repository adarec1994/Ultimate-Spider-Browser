#pragma once
#include "Helpers.h"
#include "PackDirectory.h"
#include "EntityBoneMapping.h"
#include "AnimationStateMachine.h"
#include "Morph.h"
#include "Viseme.h"
#include <filesystem>
#include <map>
#include <array>
#include <glad/glad.h>
#include <functional>
#include <optional>
#include <memory>

struct NalSkeletonData;
struct NalAnimFile;
struct NalAnimEntry;

namespace fs = std::filesystem;

enum NglBlendMode : uint32_t {
    NGLBM_OPAQUE              = 0,
    NGLBM_PUNCHTHROUGH        = 1,
    NGLBM_BLEND               = 2,
    NGLBM_ADDITIVE            = 3,
    NGLBM_SUBTRACTIVE         = 4,
    NGLBM_CONST_BLEND         = 5,
    NGLBM_CONST_ADDITIVE      = 6,
    NGLBM_CONST_SUBTRACTIVE   = 7,
    NGLBM_DESTALPHA_ADDITIVE  = 8,
    NGLBM_MAX_BLEND_MODES_GUARD = 9,
};

struct MaterialDef {
    std::string meshName;
    std::string alphaFlag;
    std::string textureName;
    std::string secondaryTextureName;
    uint16_t serializedSize = 0;
    uint32_t shaderType = 0;
    uint32_t blendMode = NGLBM_OPAQUE;
    bool isTranslucent = false;
    bool isAlphaTest = false;

    bool isColorVolume = false;
    bool isShadowVolume = false;

    bool isPersonMaterial = false;
    std::array<float, 4> personBaseColor = {1.0f, 1.0f, 1.0f, 1.0f};
    uint32_t personLighting = 0;
    uint32_t personEnvA = 0;
    uint32_t personEnvB = 0;
    uint32_t personOutline = 0;
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

// Which of the world loader's four passes produced a mesh. Lets the viewer group and export them
// separately -- they have genuinely different placement semantics (zone/interior geometry is placed
// once, conglomerate members carry a relative po, lego is GPU-instanced).
enum class WorldMeshKind : uint8_t {
    None = 0,      // model preview, not world geometry
    Zone,          // LoadWorldZoneChunks  - city shell, streets, terrain
    Interior,      // LoadWorldInteriorMeshes
    SceneEntity,   // simple single-mesh scene placements
    Conglomerate,  // multi-part scene entities (BuildConglomerateMemberPlacements)
    Lego,          // batched/instanced small props
    Ocean,
    Skybox,
};

struct RenderMesh {
    struct Instance {

        std::array<float, 16> transform{};
        float bboxMin[3] = {0, 0, 0};
        float bboxMax[3] = {0, 0, 0};
        std::string name;
        bool isHidden = false;
    };

    unsigned int vao = 0;
    unsigned int vbo = 0;
    unsigned int ebo = 0;
    unsigned int instanceVbo = 0;
    int indexCount = 0;
    int instanceDrawCount = 0;
    GLenum mode = GL_TRIANGLES;
    unsigned int textureId = 0;
    unsigned int secondaryTextureId = 0;

    std::vector<unsigned int> textureFrames;
    bool isTranslucent = false;
    bool isAlphaTest = false;
    uint32_t blendMode = NGLBM_OPAQUE;
    bool isFakeShadow = false;
    bool isColorVolume = false;
    bool isShadowVolume = false;
    bool isHidden = false;

    bool isDebugTransparent = false;

    bool isWater = false;
    bool skipPicking = false;
    WorldMeshKind worldKind = WorldMeshKind::None;
    uint32_t shaderType = 0;
    bool isPersonMaterial = false;
    std::array<float, 4> personBaseColor = {1.0f, 1.0f, 1.0f, 1.0f};
    uint32_t personLighting = 0;
    uint32_t personEnvA = 0;
    uint32_t personEnvB = 0;
    uint32_t personOutline = 0;

    float bboxMin[3] = {0, 0, 0};
    float bboxMax[3] = {0, 0, 0};

    std::vector<float> positions;
    std::vector<float> normals;
    std::vector<float> uvs;

    std::vector<float> colors;
    std::vector<uint16_t> indices;
    std::vector<uint16_t> bonePalette;
    std::vector<Instance> instances;

    uint32_t sourceSectionIndex = 0;

    std::vector<float> morphVertexData;
    std::vector<std::vector<std::array<float, 3>>> morphPositionDeltas;
    std::vector<std::vector<uint8_t>> morphPositionChanged;

    bool hasPlacementTransform = false;
    std::array<float, 16> placementTransform{};
    std::string placementName;
    uint32_t sourceMeshOffset = 0;

    std::string textureName;
    std::string secondaryTextureName;
    std::string materialShaderName;
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

    PackDirectory currentDir;
    uint32_t dataOffset = 0;

    std::vector<UsmAls::File> animationStateMachines;
    bool showAnimationStateMachine = false;
    int selectedAnimationStateMachine = 0;
    int selectedAnimationMachineLayer = 0;
    uint32_t animationStateMachineModelHash = 0;
    std::string animationStateMachineModelName;

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
    unsigned int skeletonProgram = 0;
    std::vector<RenderMesh> previewMeshes;

    std::map<uint32_t, unsigned int> textureCache;
    std::map<std::string, unsigned int> textureNameCache;
    std::map<std::string, std::vector<unsigned int>> textureAnimationCache;
    std::map<uint32_t, TextureLocation> globalTextureIndex;
    std::map<std::string, TextureLocation> globalTextureNameIndex;

    void BuildGlobalTextureIndex();
    void BuildGlobalTextureIndexStep(int packIndex);

    int selectedMeshIndex = -1;
    int selectedMeshInstanceIndex = -1;
    std::vector<uint8_t> selectedMeshPcmData;
    bool showWorldMeshDetails = false;

    bool RayIntersectAABB(const float rayOrigin[3], const float rayDir[3],
                          const float bboxMin[3], const float bboxMax[3], float& tMin);
    int PickMeshAtScreenPos(float screenX, float screenY, float viewportWidth, float viewportHeight,
                            int* instanceIndexOut = nullptr);
    void HandleMeshPicking(float viewportX, float viewportY, float viewportWidth, float viewportHeight);
    void LoadSelectedMeshPcmData();
    void RefreshInstanceBuffer(RenderMesh& mesh);

    bool isWorldMode = false;

    bool showSkeleton = false;
    int selectedBoneIndex = -1;
    bool isRotatingBone = false;
    float boneRotationAngle = 0.0f;
    int boneRotationAxis = 1;
    std::map<int, std::array<float, 3>> manualBoneRotations;
    std::map<int, std::array<float, 3>> boneRotationsBeforeEdit;

    unsigned int skeletonVao = 0;
    unsigned int skeletonVbo = 0;
    int skeletonBoneCount = 0;
    int skeletonLineVertCount = 0;

    struct BoneData {
        float bindMatrix[16];
        float invBindMatrix[16];
        float position[3];
    };
    std::vector<BoneData> skeletonBones;

    RenderMesh proceduralTentacleMesh;

    bool captureEvaluatedSkinMatrices = false;
    bool skipDrawAfterPoseEvaluation = false;
    bool evaluatedSkinningActive = false;
    std::vector<float> evaluatedGlobalBoneMatrices;

    void BuildSkeletonVisual(const std::vector<uint8_t>& pcmData);
    void RenderSkeletonOverlay();

    // Collision overlay. USM has no authored collision spheres: collision_geometry defines only
    // CAPSULE and MESH, and an actor's capsule is *derived* at runtime from the model's bounding
    // sphere (collision_capsule::compute_dimensions). BuildCollisionVisual reproduces that formula
    // exactly, so what's drawn is what the engine would collide with.
    bool showCollision = false;
    unsigned int collisionVao = 0;
    unsigned int collisionVbo = 0;
    int collisionVertCount = 0;

    // World/prop collision: an OBB tree from a 'COLL' resource. Axes are pre-scaled by their
    // half-extent, so the corners are center +/- axisX +/- axisY +/- axisZ.
    struct CollisionObb {
        float center[3];
        float radius;
        float axisX[3];
        float axisY[3];
        float axisZ[3];
    };
    // Collision resources are named "<MESHNAME>" + a 3-digit index (VCL_AMBULANCE -> VCL_AMBULANCE000),
    // so a group is matched back to the mesh it belongs to by that name, and only the groups
    // belonging to currently loaded meshes are drawn.
    struct CollisionGroup {
        std::string name;
        std::vector<CollisionObb> obbs;
    };
    std::vector<CollisionGroup> collisionGroups;
    int collisionObbsDrawn = 0;

    // Stamped onto every mesh the world loader creates; each pass sets it before adding meshes.
    WorldMeshKind currentWorldKind = WorldMeshKind::None;
    void ExportWorldMeshesByKind(const std::vector<WorldMeshKind>& kinds, const std::string& label);

    void LoadCollisionMeshesForCurrentPack();
    void LoadCollisionMeshesForWorld();
    void AppendCollisionGroups(const std::vector<uint8_t>& packData, const PackDirectory& dir);
    void BuildCollisionVisual();
    void RenderCollisionOverlay();
    int PickBoneAtScreenPos(float screenX, float screenY, float vpW, float vpH);
    void ApplyBoneRotation(int boneIdx, float angle, int axis);
    void ResetBoneRotation();

    std::map<int, std::array<float,3>> nalBonePositions;
    std::vector<int> nalBoneVboOrder;
    int nalMaxBoneIndex = -1;
    void ComputeNALBonePositions();
    float modelCenter[3] = {0.0f, 0.0f, 0.0f};
    float modelRadius = 1.0f;
    // Orbit/zoom focal point. For a full-body character this is the head, so zooming in
    // homes in on the face; for compact meshes it falls back to the model centre.
    float modelHeadTarget[3] = {0.0f, 0.0f, 0.0f};
    // Persistent orbit radius, so the zoom-driven centre<->head focal pan stays stable.
    float orbitDistance = 0.0f;
    float camPos[3] = {0.0f, 10.0f, 50.0f};
    float camFront[3] = {0.0f, 0.0f, -1.0f};
    float camUp[3] = {0.0f, 1.0f, 0.0f};
    float camYaw = -90.0f;
    float camPitch = 0.0f;
    float camSpeed = 100.0f;
    bool isFlyCameraMouseLocked = false;
    double flyCameraLockX = 0.0;
    double flyCameraLockY = 0.0;

    void ShowNotification(const std::string& msg);
    void SaveConfig();
    void LoadConfig();
    void ScanDirectory();
    void LoadDictionary(const std::string& path);
    void LoadBinaryDictionary(const std::string& path);
    void OpenPCPack(const std::string& path);
    void LoadAnimationStateMachinesForCurrentPack();
    bool PcmModelHasSkeleton(int entryIndex) const;
    bool CanViewAnimationStateMachine(int entryIndex) const;
    void OpenAnimationStateMachineForModel(int entryIndex);
    void ExtractPack(const std::string& packPath, bool convertAll = false);

    void ExtractFile(int index, bool asGlb = false);

    void LoadPreview(int index);
    void CloseDdsPreview();
    void InitModelPreview();
    void LoadModelToGL(int index);
    void RenderModelPreview();
    void UpdateWorldCamera(bool isHovered);
    void UpdateModelOrbitCamera(bool isHovered);
    unsigned int LoadTextureFromHash(uint32_t hash);
    unsigned int LoadTextureFromData(const std::vector<uint8_t>& data);
    unsigned int LoadTextureByName(const std::string& textureName);

    void ConvertPCM(const std::vector<uint8_t>& pcmData, const std::string& outPath, uint32_t meshHash = 0);
    void AnalyzePCM(int index);
    void AnalyzePCMDetailed(const std::vector<uint8_t>& pcmData);

    void ParseMaterialEntries(const std::vector<uint8_t>& pcmData);
    MaterialDef ResolveMaterialByMeshOffset(uint32_t meshNameOffset);

    bool IsWorldPack(const std::string& name);
    bool IsWorldInteriorPack(const std::string& name);

    void AddMeshFromData(const std::vector<uint8_t>& pcmData, std::string modelName = "", std::function<unsigned int(uint32_t)> textureResolver = nullptr, const std::string& sourcePack = "", uint32_t sourceOffset = 0);
    void AddMeshFromDataWithTransform(const std::vector<uint8_t>& pcmData, std::string modelName = "", std::function<unsigned int(uint32_t)> textureResolver = nullptr, const std::string& sourcePack = "", uint32_t sourceOffset = 0, const float* transform = nullptr, uint32_t onlyMeshOffset = 0xFFFFFFFFu);
    void AddMeshInstancesFromDataBatched(const std::vector<uint8_t>& pcmData, std::string modelName, std::function<unsigned int(uint32_t)> textureResolver, const std::string& sourcePack, uint32_t sourceOffset, const std::vector<std::array<float, 16>>& transforms, uint32_t onlyMeshOffset = 0xFFFFFFFFu);
    struct PendingWorldInstanceBatch {
        // Which loader pass queued this. Captured at queue time because the batch is materialised
        // later, when currentWorldKind no longer reflects the pass that produced it.
        WorldMeshKind kind = WorldMeshKind::None;
        std::vector<uint8_t> pcmData;
        std::string modelName;
        std::string sourcePack;
        uint32_t sourceOffset = 0;
        uint32_t onlyMeshOffset = 0xFFFFFFFFu;
        std::vector<std::array<float, 16>> transforms;
        std::vector<std::string> instanceNames;
    };
    bool collectWholeWorldInstances = false;
    std::map<std::string, PendingWorldInstanceBatch> pendingWholeWorldInstances;
    void FlushPendingWholeWorldInstances();
    void InstanceWholeWorldMeshes();
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

    // World scene export. Runs stepped so the UI can show progress: ExtractAllWorldMeshes() loads
    // the world itself (the caller does not need to have it loaded) and builds the work list, then
    // WorldExportStep() is pumped once per frame until exportProgress == exportTotal.
    void ExtractAllWorldMeshes();
    void WorldExportStep(int itemsThisFrame);

    struct WorldExportItem {
        int meshIndex = -1;          // index into previewMeshes
        std::string relPath;         // "glb/<category>/<name>.glb"
        std::string outFile;         // absolute path
        std::string name;
        std::string category;
        std::string textureName;
        uint32_t textureHash = 0;
        // Material's shader name ("usstreet", "smsimple", "usperson"...). Read from material
        // offset +0x04; it selects which of the engine's shaders drew this mesh, so it is the
        // right hint for picking a matching material on the importing side.
        std::string shaderName;
        // Zone (district pack) this mesh came from; zone chunks are grouped into one folder
        // per zone.
        std::string pcmName;
    };
    struct WorldExportPlacement {
        int meshId = 0;
        std::string name;
        std::array<float, 16> transform{};
    };

    bool isExportingWorld = false;
    int exportProgress = 0;
    int exportTotal = 0;
    std::string exportCurrentItem;
    std::vector<WorldExportItem> exportItems;
    std::vector<WorldExportPlacement> exportPlacements;
    std::vector<RenderMesh> exportSavedMeshes;
    std::string exportRootDir;

    std::shared_ptr<NalSkeletonData> loadedSkeleton;
    std::shared_ptr<NalAnimFile>     loadedAnimFile;
    UsmMorph::File loadedMorphFile;
    std::vector<float> morphTargetWeights;
    // One-shot request (consumed by the UI): when a model with morph targets is
    // loaded or restored, pop the Animations panel to the front on its Morphs tab.
    bool focusMorphsTab = false;
    std::vector<UsmViseme::Stream> loadedVisemeStreams;
    int selectedVisemeIndex = -1;
    int selectedAnimIndex = -1;
    int currentAnimFrame  = 0;
    float animFrameFraction = 0.f;
    bool isAnimPlaying    = false;
    float animPlaybackTime = 0.f;
    std::string loadedSkeletonName;
    std::string loadedAnimName;

    struct SkeletonCandidate {
        std::shared_ptr<NalSkeletonData> data;
        std::string name;
        uint32_t    hash = 0;
        int         entryIndex = -1;
    };
    std::vector<SkeletonCandidate> skeletonCandidates;
    int activeSkeletonCandidate = -1;

    struct SkeletonLocation { std::string packPath; uint32_t offset = 0; uint32_t size = 0; };
    std::map<uint32_t, SkeletonLocation> globalSkeletonIndex;
    bool globalSkeletonIndexBuilt = false;
    void BuildGlobalSkeletonIndex();
    std::shared_ptr<NalSkeletonData> LoadSkeletonFromLocation(const SkeletonLocation& loc);

    // Face morphs are keyed by mesh hash but may ship in a different pack than the mesh
    // (e.g. a CH_VWR_* character viewer holds the head mesh, while the morph only lives in
    // a cutscene/IGC pack). This index lets LoadMorphForCurrentModel resolve a morph from
    // any pack by mesh hash. Built lazily on first use.
    struct MorphLocation { std::string packPath; uint32_t offset = 0; uint32_t size = 0; };
    std::map<uint32_t, MorphLocation> globalMorphIndex;
    bool globalMorphIndexBuilt = false;
    void BuildGlobalMorphIndex();

    // Animations, like skeletons, may live in a different pack than the mesh: a character loaded
    // from a cutscene pack has no anim file; its clips ship in its CH_VWR viewer/costume pack.
    // Maps a skeleton hash -> anim-file locations in packs that also contain that skeleton, so a
    // model can pull its animations cross-pack when the current pack has none.
    struct AnimFileLocation { std::string packPath; uint32_t offset = 0; uint32_t size = 0; };
    std::map<uint32_t, std::vector<AnimFileLocation>> globalAnimIndex;
    bool globalAnimIndexBuilt = false;
    void BuildGlobalAnimIndex();

    struct EntityLocation { std::string packPath; uint32_t offset = 0; uint32_t size = 0; };
    std::map<uint32_t, std::vector<EntityLocation>> globalEntityIndex;
    bool globalEntityIndexBuilt = false;
    EntityBoneMapping activeBoneMapping;
    uint32_t activeBoneMappingHash = 0;
    void BuildGlobalEntityIndex();
    bool SelectBoneMappingForMesh(uint32_t meshHash);

    struct PreviewTabState {
        uint64_t id = 0;
        std::string key;
        std::string label;
        std::string packPath;
        uint32_t resourceHash = 0;
        uint32_t resourceOffset = 0;
        bool requestSelect = false;
        bool hasCachedState = false;

        bool modelLoaded = false;
        bool modelPreview = false;
        bool worldMode = false;
        std::vector<RenderMesh> meshes;

        int selectedMeshIndex = -1;
        int selectedMeshInstanceIndex = -1;
        std::vector<uint8_t> selectedMeshPcmData;
        bool showWorldMeshDetails = false;

        bool showSkeleton = false;
        int selectedBoneIndex = -1;
        bool isRotatingBone = false;
        float boneRotationAngle = 0.0f;
        int boneRotationAxis = 1;
        std::map<int, std::array<float, 3>> manualBoneRotations;
        std::map<int, std::array<float, 3>> boneRotationsBeforeEdit;
        unsigned int skeletonVao = 0;
        unsigned int skeletonVbo = 0;
        int skeletonBoneCount = 0;
        int skeletonLineVertCount = 0;
        std::vector<BoneData> skeletonBones;
        RenderMesh proceduralTentacleMesh;
        std::map<int, std::array<float, 3>> nalBonePositions;
        std::vector<int> nalBoneVboOrder;
        int nalMaxBoneIndex = -1;

        std::array<float, 3> modelCenter = {0.0f, 0.0f, 0.0f};
        std::array<float, 3> modelHeadTarget = {0.0f, 0.0f, 0.0f};
        float modelRadius = 1.0f;
        float orbitDistance = 0.0f;
        std::array<float, 3> camPos = {0.0f, 10.0f, 50.0f};
        std::array<float, 3> camFront = {0.0f, 0.0f, -1.0f};
        std::array<float, 3> camUp = {0.0f, 1.0f, 0.0f};
        float camYaw = -90.0f;
        float camPitch = 0.0f;
        float camSpeed = 100.0f;

        std::shared_ptr<NalSkeletonData> loadedSkeleton;
        std::shared_ptr<NalAnimFile> loadedAnimFile;
        UsmMorph::File loadedMorphFile;
        std::vector<float> morphTargetWeights;
        std::vector<UsmViseme::Stream> loadedVisemeStreams;
        int selectedVisemeIndex = -1;
        int selectedAnimIndex = -1;
        int currentAnimFrame = 0;
        float animFrameFraction = 0.0f;
        bool isAnimPlaying = false;
        float animPlaybackTime = 0.0f;
        std::string loadedSkeletonName;
        std::string loadedAnimName;
        std::vector<SkeletonCandidate> skeletonCandidates;
        int activeSkeletonCandidate = -1;
        EntityBoneMapping activeBoneMapping;
        uint32_t activeBoneMappingHash = 0;
    };

    std::vector<PreviewTabState> previewTabs;
    int activePreviewTab = -1;
    uint64_t nextPreviewTabId = 1;

    void OpenPcmPreviewTab(int entryIndex);
    void OpenGlobalSearchPcmTab(int resultIndex);
    void ActivatePreviewTab(int tabIndex);
    void ClosePreviewTab(int tabIndex);
    void StoreActivePreviewTab();
    void RestorePreviewTab(int tabIndex);
    void DestroyPreviewTabResources(PreviewTabState& tab);

    void LoadSkeletonForCurrentPack();
    void SelectSkeletonForMesh(const std::string& meshName, uint32_t meshHash = 0);
    void ActivateSkeletonCandidate(int candidateIndex);
    void LoadAnimationForCurrentPack();
    void LoadVisemeStreamsForCurrentPack();
    void LoadMorphForCurrentModel(uint32_t meshHash);
    // Side-effect-free morph resolver (current pack, then global index) shared by the viewer
    // loader and the GLB exporter. Returns the raw .pcmorph bytes for a mesh hash.
    bool FetchMorphBytesForHash(uint32_t meshHash, std::vector<uint8_t>& out);
    void ApplyMorphTargets();
    void UpdateAnimationPlayback(float deltaTime);
    int FindEntryBySignature(uint32_t sig) const;
};
