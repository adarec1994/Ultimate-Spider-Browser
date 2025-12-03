#include <iostream>
#include <glad/glad.h>

#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <windows.h>
#endif

#include <GLFW/glfw3.h>

#include "SpiderManTool.h"
#include "Interface.h"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

int main(int, char**) {
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

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

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