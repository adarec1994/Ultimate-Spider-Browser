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

static int TestPreviewTabCacheHeadless() {
    SpiderManTool tool;
    tool.previewTabs.resize(2);
    tool.previewTabs[0].id = 1;
    tool.previewTabs[0].label = "first.pcm";
    tool.previewTabs[1].id = 2;
    tool.previewTabs[1].label = "second.pcm";
    tool.activePreviewTab = 0;

    RenderMesh firstMesh{};
    firstMesh.vao = 101;
    firstMesh.vbo = 102;
    firstMesh.ebo = 103;
    tool.previewMeshes.push_back(firstMesh);
    tool.isModelLoaded = true;
    tool.isModelPreview = true;
    tool.camPos[0] = 11.0f;
    tool.animPlaybackTime = 3.25f;
    tool.loadedSkeletonName = "first_skeleton";
    tool.StoreActivePreviewTab();

    RenderMesh secondMesh{};
    secondMesh.vao = 201;
    secondMesh.vbo = 202;
    secondMesh.ebo = 203;
    auto& second = tool.previewTabs[1];
    second.hasCachedState = true;
    second.modelLoaded = true;
    second.modelPreview = true;
    second.meshes.push_back(secondMesh);
    second.camPos[0] = 22.0f;
    second.animPlaybackTime = 7.5f;
    second.loadedSkeletonName = "second_skeleton";

    tool.ActivatePreviewTab(1);
    const bool secondRestored = tool.previewMeshes.size() == 1 &&
        tool.previewMeshes[0].vao == 201 && tool.camPos[0] == 22.0f &&
        tool.animPlaybackTime == 7.5f && tool.loadedSkeletonName == "second_skeleton";

    tool.camPos[0] = 23.0f;
    tool.animPlaybackTime = 8.25f;
    tool.ActivatePreviewTab(0);
    const bool firstRestored = tool.previewMeshes.size() == 1 &&
        tool.previewMeshes[0].vao == 101 && tool.camPos[0] == 11.0f &&
        tool.animPlaybackTime == 3.25f && tool.loadedSkeletonName == "first_skeleton";

    tool.ActivatePreviewTab(1);
    const bool secondPreserved = tool.previewMeshes.size() == 1 &&
        tool.previewMeshes[0].vao == 201 && tool.camPos[0] == 23.0f &&
        tool.animPlaybackTime == 8.25f;

    const bool ok = secondRestored && firstRestored && secondPreserved;
    std::cout << "PREVIEW_TAB_CACHE_SUMMARY switches=3 reloads=0 result="
              << (ok ? "pass" : "fail") << std::endl;
    return ok ? 0 : 11;
}

int main(int argc, char** argv) {
    if (argc >= 3 && std::string(argv[1]) == "--export-pack-glb") {
        return ExportPackGlbHeadless(argv[2]);
    }
    if (argc >= 3 && std::string(argv[1]) == "--validate-all-packs") {
        return ValidateAllPacksHeadless(argv[2]);
    }
    if (argc >= 2 && std::string(argv[1]) == "--test-preview-tab-cache") {
        return TestPreviewTabCacheHeadless();
    }

    const bool testWorldInstancingGl =
        argc >= 2 && std::string(argv[1]) == "--test-world-instancing-gl";

    if (!glfwInit()) return 1;
    if (testWorldInstancingGl) glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
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

    if (testWorldInstancingGl) {
        tool.InitModelPreview();

        GLint shaderLinked = GL_FALSE;
        glGetProgramiv(tool.modelProgram, GL_LINK_STATUS, &shaderLinked);
        const bool instancingFunctionsLoaded =
            glDrawElementsInstanced != nullptr && glVertexAttribDivisor != nullptr;

        RenderMesh mesh;
        struct TestVertex { float x, y, z; float r, g, b, a; };
        const TestVertex vertices[3] = {
            {-0.5f, -0.5f, 0.0f, 1, 1, 1, 1},
            { 0.5f, -0.5f, 0.0f, 1, 1, 1, 1},
            { 0.0f,  0.5f, 0.0f, 1, 1, 1, 1}
        };
        const uint16_t indices[3] = {0, 1, 2};
        glGenVertexArrays(1, &mesh.vao);
        glGenBuffers(1, &mesh.vbo);
        glGenBuffers(1, &mesh.ebo);
        glBindVertexArray(mesh.vao);
        glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(TestVertex), (void*)0);
        glEnableVertexAttribArray(5);
        glVertexAttribPointer(5, 4, GL_FLOAT, GL_FALSE, sizeof(TestVertex),
                              (void*)(sizeof(float) * 3));
        glBindVertexArray(0);
        mesh.indexCount = 3;

        for (int i = 0; i < 2; ++i) {
            RenderMesh::Instance instance;
            instance.transform[0] = instance.transform[5] =
                instance.transform[10] = instance.transform[15] = 1.0f;
            instance.transform[12] = (float)i;
            mesh.instances.push_back(instance);
        }

        GLint instanceBufferBytes = 0;
        GLenum drawError = GL_INVALID_OPERATION;
        if (instancingFunctionsLoaded) {
            tool.RefreshInstanceBuffer(mesh);
            glBindBuffer(GL_ARRAY_BUFFER, mesh.instanceVbo);
            glGetBufferParameteriv(GL_ARRAY_BUFFER, GL_BUFFER_SIZE, &instanceBufferBytes);

            float identity[16] = {0};
            identity[0] = identity[5] = identity[10] = identity[15] = 1.0f;
            glUseProgram(tool.modelProgram);
            glUniformMatrix4fv(glGetUniformLocation(tool.modelProgram, "model"), 1, GL_FALSE, identity);
            glUniformMatrix4fv(glGetUniformLocation(tool.modelProgram, "view"), 1, GL_FALSE, identity);
            glUniformMatrix4fv(glGetUniformLocation(tool.modelProgram, "projection"), 1, GL_FALSE, identity);
            glUniform1i(glGetUniformLocation(tool.modelProgram, "useSkinning"), 0);
            glUniform1i(glGetUniformLocation(tool.modelProgram, "useInstancing"), 1);
            glUniform1i(glGetUniformLocation(tool.modelProgram, "hasTexture"), 0);
            glUniform1i(glGetUniformLocation(tool.modelProgram, "isWater"), 0);
            glUniform1i(glGetUniformLocation(tool.modelProgram, "debugTransparent"), 0);
            glUniform1f(glGetUniformLocation(tool.modelProgram, "selectedInstanceIndex"), 1.0f);
            while (glGetError() != GL_NO_ERROR) {}
            glBindVertexArray(mesh.vao);
            glDrawElementsInstanced(GL_TRIANGLES, 3, GL_UNSIGNED_SHORT, nullptr, 2);
            drawError = glGetError();
        }

        const bool bufferOk = instanceBufferBytes == (GLint)(2 * (17 * sizeof(float)));
        const bool ok = shaderLinked == GL_TRUE && instancingFunctionsLoaded &&
                        mesh.instanceDrawCount == 2 && bufferOk && drawError == GL_NO_ERROR;
        std::cout << "WORLD_INSTANCING_GL_SUMMARY shader="
                  << (shaderLinked == GL_TRUE ? "pass" : "fail")
                  << " functions=" << (instancingFunctionsLoaded ? "pass" : "fail")
                  << " instances=" << mesh.instanceDrawCount
                  << " buffer_bytes=" << instanceBufferBytes
                  << " draw=" << (drawError == GL_NO_ERROR ? "pass" : "fail")
                  << " result=" << (ok ? "pass" : "fail") << std::endl;
        return ok ? 0 : 12;
    }

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
