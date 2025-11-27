#pragma once
#include "Helpers.h"
#include <filesystem>
#include <map>
#include <iostream>

namespace fs = std::filesystem;

struct FileEntry {
    uint32_t hash;
    uint32_t type;
    uint32_t offset;
    uint32_t size;
    std::string name;
    bool isPcm;
    bool isDds;
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

    void Log(const std::string& msg);
    void SaveConfig();
    void LoadConfig();
    void ScanDirectory();
    void LoadDictionary(const std::string& path);
    void OpenPCPack(const std::string& path);
    void ExtractPack(const std::string& packPath, bool convertAll = false);
    void ExtractFile(int index);
    void LoadPreview(int index);
    void ClosePreview();
    void ConvertPCM(const std::vector<uint8_t>& pcmData, const std::string& outPath);
};