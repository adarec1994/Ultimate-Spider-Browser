cmake_minimum_required(VERSION 3.14)

if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()

project(USM)

if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
endif()

set(CMAKE_CXX_STANDARD 17)

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(USE_MSVC_RUNTIME_LIBRARY_DLL OFF CACHE BOOL "" FORCE)

find_package(OpenGL REQUIRED)

include(FetchContent)

FetchContent_Declare(
        glfw
        GIT_REPOSITORY https://github.com/glfw/glfw
        GIT_TAG        3.3.8
)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
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
        GIT_TAG        v1.89.9-docking
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
        imgui_node_editor
        GIT_REPOSITORY https://github.com/thedmd/imgui-node-editor.git
        GIT_TAG        v0.9.3
)
FetchContent_GetProperties(imgui_node_editor)
if(NOT imgui_node_editor_POPULATED)
    FetchContent_Populate(imgui_node_editor)
endif()

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

add_library(imgui_node_editor STATIC
        ${imgui_node_editor_SOURCE_DIR}/crude_json.cpp
        ${imgui_node_editor_SOURCE_DIR}/imgui_canvas.cpp
        ${imgui_node_editor_SOURCE_DIR}/imgui_node_editor.cpp
        ${imgui_node_editor_SOURCE_DIR}/imgui_node_editor_api.cpp
)

target_include_directories(imgui_node_editor PUBLIC
        ${imgui_node_editor_SOURCE_DIR}
        ${imgui_SOURCE_DIR}
)

target_link_libraries(imgui_node_editor PUBLIC imgui)

set(PLATFORM_SOURCES "")
if(WIN32)
    list(APPEND PLATFORM_SOURCES res/resource.rc)
endif()

add_executable(USM WIN32
        main.cpp
        app/Interface.cpp
        app/AnimationStateMachineView.cpp
        app/SpiderManTool.cpp
        core/Archive.cpp
        formats/AnimationStateMachine.cpp
        render/Render.cpp
        render/Model.cpp
        world/World.cpp
        ${imguifiledialog_SOURCE_DIR}/ImGuiFileDialog.cpp
        ${PLATFORM_SOURCES}
)

target_compile_definitions(USM PRIVATE USE_STD_FILESYSTEM)

if(MSVC)
    target_link_options(USM PRIVATE /ENTRY:mainCRTStartup)
endif()

if(WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/res/string_hash_dictionary.txt")
    set_property(SOURCE res/resource.rc APPEND PROPERTY OBJECT_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/res/string_hash_dictionary.txt")
endif()

target_include_directories(USM PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/app
        ${CMAKE_CURRENT_SOURCE_DIR}/core
        ${CMAKE_CURRENT_SOURCE_DIR}/formats
        ${CMAKE_CURRENT_SOURCE_DIR}/nal
        ${CMAKE_CURRENT_SOURCE_DIR}/render
        ${CMAKE_CURRENT_SOURCE_DIR}/world
        ${CMAKE_CURRENT_SOURCE_DIR}/res
        ${imgui_SOURCE_DIR}
        ${imgui_SOURCE_DIR}/backends
        ${glfw_SOURCE_DIR}/include
        ${imguifiledialog_SOURCE_DIR}
        ${imgui_node_editor_SOURCE_DIR}
        ${stb_SOURCE_DIR}
)

target_link_libraries(USM PRIVATE imgui imgui_node_editor glfw glad OpenGL::GL)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/res/spider-browser.png")
    configure_file(
            "${CMAKE_CURRENT_SOURCE_DIR}/res/spider-browser.png"
            "${CMAKE_CURRENT_BINARY_DIR}/spider-browser.png"
            COPYONLY
    )
endif()
