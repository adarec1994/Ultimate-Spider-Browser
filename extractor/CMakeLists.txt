cmake_minimum_required(VERSION 3.14)
project(USM)

# --- FIX: Suppress FetchContent_Populate warnings ---
if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
endif()

set(CMAKE_CXX_STANDARD 17)

# --- REQUIRED: Find OpenGL on the system ---
find_package(OpenGL REQUIRED)

include(FetchContent)

# ------------------------------------------------------------------------------
# 1. Fetch GLFW
# ------------------------------------------------------------------------------
message(STATUS "Fetching GLFW...")
FetchContent_Declare(
        glfw
        GIT_REPOSITORY https://github.com/glfw/glfw
        GIT_TAG        3.3.8
)
FetchContent_MakeAvailable(glfw)

# ------------------------------------------------------------------------------
# 2. Fetch ImGui (Manual Build) -> INTO /imgui FOLDER
# ------------------------------------------------------------------------------
message(STATUS "Fetching ImGui...")
FetchContent_Declare(
        imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui
        GIT_TAG        docking
        # This forces CMake to download the files into 'imgui' next to your main.cpp
        SOURCE_DIR     "${CMAKE_SOURCE_DIR}/imgui"
)

# Manual populate
FetchContent_GetProperties(imgui)
if(NOT imgui_POPULATED)
    FetchContent_Populate(imgui)
endif()

# Create library
add_library(imgui STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_demo.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR})
target_link_libraries(imgui PUBLIC glfw OpenGL::GL)

# ------------------------------------------------------------------------------
# 3. Fetch ImGuiFileDialog (Manual Build)
# ------------------------------------------------------------------------------
message(STATUS "Fetching ImGuiFileDialog...")
FetchContent_Declare(
        ImGuiFileDialog
        GIT_REPOSITORY https://github.com/aiekick/ImGuiFileDialog
        GIT_TAG        v0.6.7
)
# Manual populate
FetchContent_GetProperties(ImGuiFileDialog)
if(NOT imguifiledialog_POPULATED)
    FetchContent_Populate(ImGuiFileDialog)
endif()

# ------------------------------------------------------------------------------
# 4. Main Executable
# ------------------------------------------------------------------------------
add_executable(USM
        main.cpp
        ${imguifiledialog_SOURCE_DIR}/ImGuiFileDialog.cpp
)

# Definitions
target_compile_definitions(USM PRIVATE USE_STD_FILESYSTEM)

# Includes - Explicitly adding them here helps the IDE find them
target_include_directories(USM PRIVATE
        ${imgui_SOURCE_DIR}
        ${imguifiledialog_SOURCE_DIR}
)

# Link
target_link_libraries(USM PRIVATE imgui glfw OpenGL::GL)

# Dictionary Copy
if(EXISTS "${CMAKE_SOURCE_DIR}/string_hash_dictionary.txt")
    configure_file(
            "${CMAKE_SOURCE_DIR}/string_hash_dictionary.txt"
            "${CMAKE_CURRENT_BINARY_DIR}/string_hash_dictionary.txt"
            COPYONLY
    )
endif()