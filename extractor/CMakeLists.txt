cmake_minimum_required(VERSION 3.14)
project(USM)

if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
endif()

set(CMAKE_CXX_STANDARD 17)

find_package(OpenGL REQUIRED)

include(FetchContent)

# --- 1. Fetch GLFW ---
FetchContent_Declare(
        glfw
        GIT_REPOSITORY https://github.com/glfw/glfw
        GIT_TAG        3.3.8
)
FetchContent_MakeAvailable(glfw)

# --- 2. Fetch ImGui ---
FetchContent_Declare(
        imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui
        GIT_TAG        docking
)
# We manually populate ImGui because we need to build it as a library manually below
FetchContent_GetProperties(imgui)
if(NOT imgui_POPULATED)
    FetchContent_Populate(imgui)
endif()

# --- 3. Fetch ImGuiFileDialog ---
FetchContent_Declare(
        ImGuiFileDialog
        GIT_REPOSITORY https://github.com/aiekick/ImGuiFileDialog
        GIT_TAG        v0.6.7
)
FetchContent_GetProperties(ImGuiFileDialog)
if(NOT imguifiledialog_POPULATED)
    FetchContent_Populate(ImGuiFileDialog)
endif()

# --- Build ImGui Library ---
# Note: We use ${imgui_SOURCE_DIR} variable instead of hardcoded paths
add_library(imgui STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_demo.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)

target_include_directories(imgui PUBLIC
        ${imgui_SOURCE_DIR}
        ${imgui_SOURCE_DIR}/backends
        ${glfw_SOURCE_DIR}/include
)

target_link_libraries(imgui PUBLIC glfw OpenGL::GL)

# --- Main Executable ---
add_executable(USM
        main.cpp
        SpiderManTool.cpp
        Interface.cpp    # Corrected casing here
        ${imguifiledialog_SOURCE_DIR}/ImGuiFileDialog.cpp
)

target_compile_definitions(USM PRIVATE USE_STD_FILESYSTEM)

target_include_directories(USM PRIVATE
        ${imgui_SOURCE_DIR}
        ${imgui_SOURCE_DIR}/backends
        ${glfw_SOURCE_DIR}/include
        ${imguifiledialog_SOURCE_DIR}
)

target_link_libraries(USM PRIVATE imgui glfw OpenGL::GL)

# --- Copy Dictionary File ---
if(EXISTS "${CMAKE_SOURCE_DIR}/string_hash_dictionary.txt")
    configure_file(
            "${CMAKE_SOURCE_DIR}/string_hash_dictionary.txt"
            "${CMAKE_CURRENT_BINARY_DIR}/string_hash_dictionary.txt"
            COPYONLY
    )
endif()