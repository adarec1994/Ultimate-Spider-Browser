cmake_minimum_required(VERSION 3.14)
project(USM)

if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
endif()

set(CMAKE_CXX_STANDARD 17)

find_package(OpenGL REQUIRED)

include(FetchContent)

FetchContent_Declare(
        glfw
        GIT_REPOSITORY https://github.com/glfw/glfw
        GIT_TAG        3.3.8
)
FetchContent_MakeAvailable(glfw)

FetchContent_Declare(
        glad
        GIT_REPOSITORY https://github.com/libigl/libigl-glad.git
        GIT_TAG        09b4969c56779f7ddf8e6176ec1873184aec890f
)
FetchContent_GetProperties(glad)
if(NOT glad_POPULATED)
    FetchContent_Populate(glad)
endif()

add_library(glad STATIC ${glad_SOURCE_DIR}/src/glad.c)
target_include_directories(glad PUBLIC ${glad_SOURCE_DIR}/include)

FetchContent_Declare(
        stb
        GIT_REPOSITORY https://github.com/nothings/stb.git
        GIT_TAG        master
)
FetchContent_GetProperties(stb)
if(NOT stb_POPULATED)
    FetchContent_Populate(stb)
endif()

FetchContent_Declare(
        imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui
        GIT_TAG        docking
)
FetchContent_GetProperties(imgui)
if(NOT imgui_POPULATED)
    FetchContent_Populate(imgui)
endif()

FetchContent_Declare(
        ImGuiFileDialog
        GIT_REPOSITORY https://github.com/aiekick/ImGuiFileDialog
        GIT_TAG        v0.6.7
)
FetchContent_GetProperties(ImGuiFileDialog)
if(NOT imguifiledialog_POPULATED)
    FetchContent_Populate(ImGuiFileDialog)
endif()

FetchContent_Declare(
        imgui_hex_editor
        GIT_REPOSITORY https://github.com/Teselka/imgui_hex_editor
        GIT_TAG        main
)
FetchContent_MakeAvailable(imgui_hex_editor)

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

target_link_libraries(imgui PUBLIC glfw glad OpenGL::GL)

add_executable(USM
        main.cpp
        Interface.cpp
        SpiderManTool.cpp
        Archive.cpp
        Render.cpp
        Model.cpp
        World.cpp
        ${imguifiledialog_SOURCE_DIR}/ImGuiFileDialog.cpp
        ${imgui_hex_editor_SOURCE_DIR}/imgui_hex.cpp
        resource.rc
)

target_compile_definitions(USM PRIVATE USE_STD_FILESYSTEM)

target_include_directories(USM PRIVATE
        ${imgui_SOURCE_DIR}
        ${imgui_SOURCE_DIR}/backends
        ${glfw_SOURCE_DIR}/include
        ${imguifiledialog_SOURCE_DIR}
        ${imgui_hex_editor_SOURCE_DIR}
        ${stb_SOURCE_DIR}
)

target_link_libraries(USM PRIVATE imgui glfw glad OpenGL::GL)

if(EXISTS "${CMAKE_SOURCE_DIR}/string_hash_dictionary.txt")
    configure_file(
            "${CMAKE_SOURCE_DIR}/string_hash_dictionary.txt"
            "${CMAKE_CURRENT_BINARY_DIR}/string_hash_dictionary.txt"
            COPYONLY
    )
endif()

if(EXISTS "${CMAKE_SOURCE_DIR}/spider-browser.png")
    configure_file(
            "${CMAKE_SOURCE_DIR}/spider-browser.png"
            "${CMAKE_CURRENT_BINARY_DIR}/spider-browser.png"
            COPYONLY
    )
endif()