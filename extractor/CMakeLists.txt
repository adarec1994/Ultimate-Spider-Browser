cmake_minimum_required(VERSION 3.14)
project(USM)

# Suppress warnings about FetchContent_Populate
cmake_policy(SET CMP0169 OLD)

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
# 2. Fetch ImGui
# ------------------------------------------------------------------------------
message(STATUS "Fetching ImGui...")
FetchContent_Declare(
        imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui
        GIT_TAG        docking
)
# Manually download source
FetchContent_GetProperties(imgui)
if(NOT imgui_POPULATED)
    FetchContent_Populate(imgui)
endif()

# Create the ImGui library target
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
# 3. Fetch ImGuiFileDialog
# ------------------------------------------------------------------------------
message(STATUS "Fetching ImGuiFileDialog...")
FetchContent_Declare(
        ImGuiFileDialog
        GIT_REPOSITORY https://github.com/aiekick/ImGuiFileDialog
        GIT_TAG        v0.6.7
)
# Manually download source
FetchContent_GetProperties(ImGuiFileDialog)
if(NOT imguifiledialog_POPULATED)
    FetchContent_Populate(ImGuiFileDialog)
endif()

# ------------------------------------------------------------------------------
# 4. Main Executable
# ------------------------------------------------------------------------------
add_executable(USM
        main.cpp
        # Compile the file dialog source directly into our app
        ${imguifiledialog_SOURCE_DIR}/ImGuiFileDialog.cpp
)

# Crucial: Tell ImGuiFileDialog to use standard C++17 filesystem instead of dirent.h
target_compile_definitions(USM PRIVATE USE_STD_FILESYSTEM)

# Add Include Paths
target_include_directories(USM PRIVATE
        ${imguifiledialog_SOURCE_DIR}
)

# Link everything together
target_link_libraries(USM PRIVATE imgui glfw OpenGL::GL)

# Copy the dictionary file to the build folder if it exists
if(EXISTS "${CMAKE_SOURCE_DIR}/string_hash_dictionary.txt")
    configure_file(
            "${CMAKE_SOURCE_DIR}/string_hash_dictionary.txt"
            "${CMAKE_CURRENT_BINARY_DIR}/string_hash_dictionary.txt"
            COPYONLY
    )
endif()