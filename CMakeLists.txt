if(WIN32)
    set(VCPKG_TARGET_TRIPLET "x64-windows-static")
    set(VCPKG_HOST_TRIPLET "x64-windows-static")
endif()

cmake_minimum_required(VERSION 3.27.0)
project(BetterVR_Layer VERSION 0.9.16 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Only x64 architecture is supported")
endif ()

set(CMAKE_INSTALL_PREFIX "${CMAKE_SOURCE_DIR}/Cemu")
set(DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/Cemu")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

add_compile_definitions(NOMINMAX)

if(WIN32)
    set(CMAKE_VS_INCLUDE_INSTALL_TO_DEFAULT_BUILD 1)
    set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY CMAKE_VS_INCLUDE_INSTALL_TO_DEFAULT_BUILD 1)
    set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT BetterVR_Layer)

    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug>:EditAndContinue>$<$<CONFIG:RelWithDebInfo>:ProgramDatabase>")
    add_compile_options("$<$<C_COMPILER_ID:MSVC>:/utf-8>")
    add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_path(VULKAN_HEADERS_INCLUDE_DIRS "vk_video/vulkan_video_codec_h264std.h")
find_package(OpenXR CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)

if(WIN32)
    find_package(imgui CONFIG REQUIRED)
    find_package(implot CONFIG REQUIRED)
else()
    # On Linux we build imgui + implot from the locally cloned trees under
    # dependencies/ (user-managed; pinned to versions compatible with the
    # bundled dependencies/imgui_impl_vulkan.cpp).
    set(IMGUI_DIR   ${CMAKE_SOURCE_DIR}/dependencies/imgui)
    set(IMPLOT_DIR  ${CMAKE_SOURCE_DIR}/dependencies/implot)

    if(NOT EXISTS ${IMGUI_DIR}/imgui.cpp)
        message(FATAL_ERROR "Missing dependencies/imgui (expected git clone v1.91.6). See README_LINUX.")
    endif()
    if(NOT EXISTS ${IMPLOT_DIR}/implot.cpp)
        message(FATAL_ERROR "Missing dependencies/implot (expected git clone v0.16). See README_LINUX.")
    endif()

    add_library(imgui STATIC
        ${IMGUI_DIR}/imgui.cpp
        ${IMGUI_DIR}/imgui_draw.cpp
        ${IMGUI_DIR}/imgui_tables.cpp
        ${IMGUI_DIR}/imgui_widgets.cpp
        ${IMGUI_DIR}/imgui_demo.cpp
    )
    set_target_properties(imgui PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_include_directories(imgui PUBLIC ${IMGUI_DIR})
    add_library(imgui::imgui ALIAS imgui)

    add_library(implot STATIC
        ${IMPLOT_DIR}/implot.cpp
        ${IMPLOT_DIR}/implot_items.cpp
        ${IMPLOT_DIR}/implot_demo.cpp
    )
    set_target_properties(implot PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_include_directories(implot PUBLIC ${IMPLOT_DIR})
    target_link_libraries(implot PUBLIC imgui)
    add_library(implot::implot ALIAS implot)
endif()

add_subdirectory(src)

if(WIN32)
    # Launcher is Win32-only; the Linux port uses a shell script instead.
    add_subdirectory(launcher)
endif()
