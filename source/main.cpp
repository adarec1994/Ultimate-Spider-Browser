#include <iostream>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <glad/glad.h>
#include <GLFW/glfw3.h>

#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <windows.h>
#endif

#include "SpiderManTool.h"
#include "Interface.h"
#include "NalIntegration.h"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

static void LoadBestDictionaryForPack(SpiderManTool& tool, const fs::path& packDir) {
    fs::path textDict = packDir / "string_hash_dictionary.txt";
    if (fs::exists(textDict)) {
        tool.LoadDictionary(textDict.string());
    } else if (fs::exists("string_hash_dictionary.txt")) {
        tool.LoadDictionary("string_hash_dictionary.txt");
    }

    if (tool.dictionary.empty()) {
        fs::path binaryDict = packDir / "string_hash_dictionary.bin";
        if (fs::exists(binaryDict)) {
            tool.LoadBinaryDictionary(binaryDict.string());
        } else if (fs::exists("string_hash_dictionary.bin")) {
            tool.LoadBinaryDictionary("string_hash_dictionary.bin");
        }
    }
}

static int ExportPackGlbHeadless(const std::string& packArg) {
    fs::path packPath = fs::absolute(packArg);
    if (!fs::exists(packPath)) {
        std::cerr << "Pack does not exist: " << packPath.string() << std::endl;
        return 2;
    }

    SpiderManTool tool;
    fs::path packDir = packPath.parent_path();
    tool.searchPath = packDir.string();
    LoadBestDictionaryForPack(tool, packDir);

    try {
        for (const auto& entry : fs::recursive_directory_iterator(packDir)) {
            if (!entry.is_regular_file()) continue;
            std::string ext = entry.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
                return (char)std::tolower(c);
            });
            if (ext == ".pcpack") tool.foundPacks.push_back(entry.path());
        }
    } catch (const std::exception& e) {
        std::cerr << "Failed to scan pack folder: " << e.what() << std::endl;
        return 3;
    }

    if (tool.foundPacks.empty()) tool.foundPacks.push_back(packPath);
    std::sort(tool.foundPacks.begin(), tool.foundPacks.end());
    tool.BuildGlobalTextureIndex();
    for (int i = 0; i < (int)tool.foundPacks.size(); ++i) {
        tool.BuildGlobalTextureIndexStep(i);
    }

    tool.ExtractPack(packPath.string(), true);
    return 0;
}

// Exercises the same directory, skeleton, animation, entropy-codec, PCM, and
// GLB construction paths used by the application without retaining extracted
// assets. The single scratch GLB is deleted after every PCM so a corpus run has
// bounded disk usage.
static int ValidateAllPacksHeadless(const std::string& packDirArg) {
    fs::path packDir = fs::absolute(packDirArg);
    if (!fs::is_directory(packDir)) {
        std::cerr << "Pack directory does not exist: " << packDir.string() << std::endl;
        return 2;
    }

    SpiderManTool tool;
    tool.searchPath = packDir.string();
    LoadBestDictionaryForPack(tool, packDir);

    try {
        for (const auto& entry : fs::directory_iterator(packDir)) {
            if (!entry.is_regular_file()) continue;
            std::string ext = entry.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
                return (char)std::tolower(c);
            });
            if (ext == ".pcpack") tool.foundPacks.push_back(entry.path());
        }
    } catch (const std::exception& e) {
        std::cerr << "Failed to scan pack folder: " << e.what() << std::endl;
        return 3;
    }
    std::sort(tool.foundPacks.begin(), tool.foundPacks.end());
    if (tool.foundPacks.empty()) {
        std::cerr << "No PCPACK files found in: " << packDir.string() << std::endl;
        return 4;
    }

    tool.BuildGlobalTextureIndex();
    for (int i = 0; i < (int)tool.foundPacks.size(); ++i)
        tool.BuildGlobalTextureIndexStep(i);
    tool.BuildGlobalSkeletonIndex();

    const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    fs::path scratchDir = fs::temp_directory_path() /
        ("usm_pack_validation_" + std::to_string(stamp));
    std::error_code fsError;
    fs::create_directories(scratchDir, fsError);
    if (fsError) {
        std::cerr << "Failed to create validation directory: " << fsError.message() << std::endl;
        return 5;
    }

    uint64_t resourceCount = 0;
    uint64_t skeletonCount = 0;
    uint64_t animationCount = 0;
    uint64_t componentCount = 0;
    uint64_t decodedFrameCount = 0;
    uint64_t decodedValueCount = 0;
    uint64_t pcmCount = 0;
    uint64_t convertedPcmCount = 0;
    uint64_t nonRenderablePcmCount = 0;
    uint64_t warningCount = 0;
    uint64_t failureCount = 0;

    for (size_t packIndex = 0; packIndex < tool.foundPacks.size(); ++packIndex) {
        const fs::path& packPath = tool.foundPacks[packIndex];
        std::cout << "[VALIDATE " << (packIndex + 1) << "/" << tool.foundPacks.size()
                  << "] " << packPath.filename().string() << std::endl;
        try {
            tool.OpenPCPack(packPath.string());
            if (!tool.currentDir.valid) {
                std::cerr << "[FAIL] invalid directory: " << packPath.filename().string()
                          << " (" << tool.currentDir.error << ")" << std::endl;
                ++failureCount;
                continue;
            }

            resourceCount += tool.currentDir.resources.size();
            for (const auto& resource : tool.currentDir.resources) {
                if ((uint64_t)resource.offset + resource.size > tool.pcPackData.size()) {
                    std::cerr << "[FAIL] resource outside pack: " << packPath.filename().string()
                              << " offset=" << resource.offset << " size=" << resource.size << std::endl;
                    ++failureCount;
                }
            }

            skeletonCount += tool.skeletonCandidates.size();
            for (const auto& candidate : tool.skeletonCandidates) {
                if (!candidate.data) {
                    ++failureCount;
                    continue;
                }
                warningCount += candidate.data->warnings.size();
            }

            if (tool.loadedAnimFile) {
                warningCount += tool.loadedAnimFile->warnings.size();
                animationCount += tool.loadedAnimFile->animations.size();
                for (const auto& animation : tool.loadedAnimFile->animations) {
                    warningCount += animation.warnings.size();
                    componentCount += animation.components.size();
                    if (animation.is_gen_anim()) {
                        if (!animation.generic_decoded.complete) {
                            std::cerr << "[FAIL] generic animation did not decode: "
                                      << packPath.filename().string() << " / "
                                      << animation.name << std::endl;
                            for (const auto& warning : animation.generic_decoded.warnings)
                                std::cerr << "       " << warning << std::endl;
                            ++failureCount;
                        }
                        decodedFrameCount += animation.generic_decoded.world_frames.size();
                        for (const auto& frame : animation.generic_decoded.world_frames) {
                            for (const auto& matrix : frame) {
                                decodedValueCount += matrix.size();
                                for (float value : matrix) {
                                    if (!std::isfinite(value)) {
                                        std::cerr << "[FAIL] non-finite generic matrix: "
                                                  << packPath.filename().string() << " / "
                                                  << animation.name << std::endl;
                                        ++failureCount;
                                        break;
                                    }
                                }
                            }
                        }
                    }
                    for (const auto& component : animation.components) {
                        if (!component.decode_error.empty()) {
                            std::cerr << "[FAIL] " << packPath.filename().string() << " / "
                                      << animation.name << " component " << component.comp_ix
                                      << ": " << component.decode_error << std::endl;
                            ++failureCount;
                        }
                        decodedFrameCount += component.decoded.frames.size();
                        for (const auto& frame : component.decoded.frames) {
                            decodedValueCount += frame.size();
                            for (float value : frame) {
                                if (!std::isfinite(value)) {
                                    std::cerr << "[FAIL] non-finite decoded value: "
                                              << packPath.filename().string() << " / "
                                              << animation.name << std::endl;
                                    ++failureCount;
                                    break;
                                }
                            }
                        }
                    }
                }
            }

            for (const auto& fileEntry : tool.entries) {
                if (!fileEntry.isPcm) continue;
                ++pcmCount;
                if ((uint64_t)fileEntry.offset + fileEntry.size > tool.pcPackData.size()) {
                    std::cerr << "[FAIL] PCM outside pack: " << packPath.filename().string()
                              << " / " << fileEntry.name << std::endl;
                    ++failureCount;
                    continue;
                }

                tool.SelectSkeletonForMesh(fileEntry.name, fileEntry.hash);
                fs::path probePath = scratchDir / fs::path(fileEntry.name).filename();
                probePath.replace_extension(".glb");
                fs::remove(probePath, fsError);
                fsError.clear();

                std::vector<uint8_t> pcmData(
                    tool.pcPackData.begin() + fileEntry.offset,
                    tool.pcPackData.begin() + fileEntry.offset + fileEntry.size);
                tool.ConvertPCM(pcmData, probePath.string());

                if (fs::exists(probePath, fsError) && fs::file_size(probePath, fsError) >= 20) {
                    ++convertedPcmCount;
                } else {
                    // Some PCM resources contain helpers/collision data with no
                    // renderable type-512 LOD. They still passed bounds parsing.
                    ++nonRenderablePcmCount;
                }
                fs::remove(probePath, fsError);
                fsError.clear();
            }
        } catch (const std::exception& e) {
            std::cerr << "[FAIL] " << packPath.filename().string() << ": " << e.what() << std::endl;
            ++failureCount;
        } catch (...) {
            std::cerr << "[FAIL] " << packPath.filename().string() << ": unknown exception" << std::endl;
            ++failureCount;
        }
    }

    fs::remove(scratchDir, fsError);
    std::cout << "VALIDATION_SUMMARY"
              << " packs=" << tool.foundPacks.size()
              << " resources=" << resourceCount
              << " skeletons=" << skeletonCount
              << " animations=" << animationCount
              << " components=" << componentCount
              << " decoded_frames=" << decodedFrameCount
              << " decoded_values=" << decodedValueCount
              << " pcms=" << pcmCount
              << " converted_pcms=" << convertedPcmCount
              << " nonrenderable_pcms=" << nonRenderablePcmCount
              << " warnings=" << warningCount
              << " failures=" << failureCount
              << std::endl;
    return failureCount == 0 ? 0 : 10;
}

int main(int argc, char** argv) {
    if (argc >= 3 && std::string(argv[1]) == "--export-pack-glb") {
        return ExportPackGlbHeadless(argv[2]);
    }
    if (argc >= 3 && std::string(argv[1]) == "--validate-all-packs") {
        return ValidateAllPacksHeadless(argv[2]);
    }

    if (!glfwInit()) return 1;
    const char* glsl_version = "#version 130";
    GLFWwindow* window = glfwCreateWindow(1024, 768, "Ultimate Spider-Browser", NULL, NULL);
    if (!window) return 1;

#ifdef _WIN32
    HWND hwnd = glfwGetWin32Window(window);
    HICON hIcon = (HICON)LoadImage(GetModuleHandle(NULL), "IDI_ICON1", IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED);
    if (hIcon) {
        SendMessage(hwnd, WM_SETICON, ICON_BIG, (LPARAM)hIcon);
        SendMessage(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hIcon);
    }
#endif

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        return -1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    ImGui::StyleColorsDark();

    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 8.0f;
    style.ChildRounding = 8.0f;
    style.FrameRounding = 8.0f;
    style.GrabRounding = 8.0f;
    style.PopupRounding = 8.0f;
    style.ScrollbarRounding = 8.0f;

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    SpiderManTool tool;
    tool.LoadConfig();

    if (fs::exists("string_hash_dictionary.txt")) tool.LoadDictionary("string_hash_dictionary.txt");
    if (tool.dictionary.empty() && fs::exists("string_hash_dictionary.bin")) tool.LoadBinaryDictionary("string_hash_dictionary.bin");

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // Update animation playback
        tool.UpdateAnimationPlayback(ImGui::GetIO().DeltaTime);

        RenderUI(tool);

        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
