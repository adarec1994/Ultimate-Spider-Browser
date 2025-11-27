cmake_minimum_required(VERSION 3.14)
project(USM)

if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
endif()

set(CMAKE_CXX_STANDARD 17)

find_package(OpenGL REQUIRED)

include(FetchContent)

# Fetch GLFW to /glfw
FetchContent_Declare(
        glfw
        GIT_REPOSITORY https://github.com/glfw/glfw
        GIT_TAG        3.3.8
        SOURCE_DIR     "${CMAKE_SOURCE_DIR}/glfw"
)
FetchContent_GetProperties(glfw)
if(NOT glfw_POPULATED)
    FetchContent_Populate(glfw)
    add_subdirectory(${CMAKE_SOURCE_DIR}/glfw ${CMAKE_BINARY_DIR}/glfw-build)
endif()

# Fetch ImGui to /dependencies/imgui
FetchContent_Declare(
        imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui
        GIT_TAG        docking
        SOURCE_DIR     "${CMAKE_SOURCE_DIR}/dependencies/imgui"
)
FetchContent_GetProperties(imgui)
if(NOT imgui_POPULATED)
    FetchContent_Populate(imgui)
endif()

# Fetch ImGuiFileDialog to /dependencies/ImGuiFileDialog
FetchContent_Declare(
        ImGuiFileDialog
        GIT_REPOSITORY https://github.com/aiekick/ImGuiFileDialog
        GIT_TAG        v0.6.7
        SOURCE_DIR     "${CMAKE_SOURCE_DIR}/dependencies/ImGuiFileDialog"
)
FetchContent_GetProperties(ImGuiFileDialog)
if(NOT imguifiledialog_POPULATED)
    FetchContent_Populate(ImGuiFileDialog)
endif()

# Build ImGui library
add_library(imgui STATIC
        ${CMAKE_SOURCE_DIR}/dependencies/imgui/imgui.cpp
        ${CMAKE_SOURCE_DIR}/dependencies/imgui/imgui_demo.cpp
        ${CMAKE_SOURCE_DIR}/dependencies/imgui/imgui_draw.cpp
        ${CMAKE_SOURCE_DIR}/dependencies/imgui/imgui_tables.cpp
        ${CMAKE_SOURCE_DIR}/dependencies/imgui/imgui_widgets.cpp
        ${CMAKE_SOURCE_DIR}/dependencies/imgui/backends/imgui_impl_glfw.cpp
        ${CMAKE_SOURCE_DIR}/dependencies/imgui/backends/imgui_impl_opengl3.cpp
        SpiderManTool.h
        helpers.h
)
target_include_directories(imgui PUBLIC
        ${CMAKE_SOURCE_DIR}/dependencies/imgui
        ${CMAKE_SOURCE_DIR}/dependencies/imgui/backends
        ${CMAKE_SOURCE_DIR}/glfw/include
)
target_link_libraries(imgui PUBLIC glfw OpenGL::GL)

# Main executable
add_executable(USM
        main.cpp
        ${CMAKE_SOURCE_DIR}/dependencies/ImGuiFileDialog/ImGuiFileDialog.cpp
        SpiderManTool.cpp
        SpiderManTool.h
        helpers.h
        interface.cpp
        Interface.h
)

target_compile_definitions(USM PRIVATE USE_STD_FILESYSTEM)

target_include_directories(USM PRIVATE
        ${CMAKE_SOURCE_DIR}/dependencies/imgui
        ${CMAKE_SOURCE_DIR}/dependencies/imgui/backends
        ${CMAKE_SOURCE_DIR}/glfw/include
        ${CMAKE_SOURCE_DIR}/dependencies/ImGuiFileDialog
)

target_link_libraries(USM PRIVATE imgui glfw OpenGL::GL)

# Copy dictionary file if it exists
if(EXISTS "${CMAKE_SOURCE_DIR}/string_hash_dictionary.txt")
    configure_file(
            "${CMAKE_SOURCE_DIR}/string_hash_dictionary.txt"
            "${CMAKE_CURRENT_BINARY_DIR}/string_hash_dictionary.txt"
            COPYONLY
    )
endif()