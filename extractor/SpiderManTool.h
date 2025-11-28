#pragma once
#include "Helpers.h"
#include <filesystem>
#include <map>
#include <iostream>
#include <glad/glad.h>
#include "imgui_hex.h"

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

    unsigned int previewTextureId = 0;
    int previewWidth = 0;
    int previewHeight = 0;
    bool showPreview = false;
    bool isModelPreview = false;

    // --- Hex Editor State ---
    bool showHexEditor = false;
    ImGuiHexEditorState hexEditor;

    unsigned int modelFbo = 0;
    unsigned int modelRbo = 0;
    unsigned int modelProgram = 0;
    std::vector<RenderMesh> previewMeshes;
    std::map<uint32_t, unsigned int> textureCache;

    float modelRotX = 0.0f;
    float modelRotY = 0.0f;
    float modelZoom = 1.0f;
    float modelCenter[3] = {0.0f, 0.0f, 0.0f};
    float modelRadius = 1.0f;

    void Log(const std::string& msg);
    void SaveConfig();
    void LoadConfig();
    void ScanDirectory();
    void LoadDictionary(const std::string& path);
    void OpenPCPack(const std::string& path);
    void ExtractPack(const std::string& packPath, bool convertAll = false);

    void ExtractFile(int index, bool asGlb = false);

    void LoadPreview(int index);
    void ClosePreview();
    void InitModelPreview();
    void LoadModelToGL(int index);
    void RenderModelPreview();
    unsigned int LoadTextureFromHash(uint32_t hash);

    void ConvertPCM(const std::vector<uint8_t>& pcmData, const std::string& outPath);
};