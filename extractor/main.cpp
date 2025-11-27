#include <iostream>
#include <GLFW/glfw3.h>

#include "SpiderManTool.h"
#include "Interface.h"

#include "dependencies/imgui/imgui.h"
#include "dependencies/imgui/backends/imgui_impl_glfw.h"
#include "dependencies/imgui/backends/imgui_impl_opengl3.h"

int main(int, char**) {
    if (!glfwInit()) return 1;
    const char* glsl_version = "#version 130";
    GLFWwindow* window = glfwCreateWindow(1024, 768, "USM Tool", NULL, NULL);
    if (!window) return 1;
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
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