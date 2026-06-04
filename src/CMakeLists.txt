add_library(BetterVR_Layer SHARED)

if(WIN32)
    file(GENERATE OUTPUT "$<TARGET_FILE_DIR:BetterVR_Layer>/BetterVR_Layer.json" INPUT "${CMAKE_SOURCE_DIR}/resources/BetterVR_Layer.json")
    file(GENERATE OUTPUT "$<TARGET_FILE_DIR:BetterVR_Layer>/BetterVR_Layer.direct.json" INPUT "${CMAKE_SOURCE_DIR}/resources/BetterVR_Layer.direct.json")
    target_link_options(BetterVR_Layer PRIVATE "$<$<CONFIG:Debug>:/INCREMENTAL>")
else()
    # -Bsymbolic: bind our internal symbol references (both functions AND data)
    # against our OWN symbols before falling back to the global symbol table.
    # Cemu's main binary also links ImGui — without this, both our calls into
    # ImGui AND our access to ImGui's global GImGui pointer get redirected to
    # Cemu's copy (different version, different ABI). The data-side issue
    # surfaces as a NULL-deref in ImGui::CreateContext → InitSettings →
    # GImGui->SettingsHandlers.push_back.
    target_link_options(BetterVR_Layer PRIVATE "-Wl,-Bsymbolic")
    # Some compilers emit __atomic_load (16-byte atomics) which lives in
    # libatomic, not libgcc. Without an explicit link the .so loads but
    # symbol resolution fails on first lazy-bound call.
    target_link_libraries(BetterVR_Layer PRIVATE atomic)
endif()

target_precompile_headers(BetterVR_Layer PRIVATE ${CMAKE_SOURCE_DIR}/include/pch.h)
target_include_directories(BetterVR_Layer BEFORE PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_include_directories(BetterVR_Layer AFTER PRIVATE ${CMAKE_SOURCE_DIR}/include)

# ---- Source files ----
# Files common to both platforms. The rendering backend is split:
#   Windows: d3d12.{h,cpp} (D3D12 composer) + vulkan.cpp (Cemu-side hooks)
#   Linux:   vk_composer.{h,cpp} (Vulkan composer) + vulkan.cpp (Cemu-side hooks)
# vulkan_imgui.cpp is shared (it's already Vulkan-based).
set(COMMON_SOURCES
    ${CMAKE_SOURCE_DIR}/src/instance.h
    ${CMAKE_SOURCE_DIR}/src/shader.h
    ${CMAKE_SOURCE_DIR}/src/utils/vulkan_utils.h
    ${CMAKE_SOURCE_DIR}/src/utils/game_utils.h
    ${CMAKE_SOURCE_DIR}/src/utils/logger.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/logger.h
    ${CMAKE_SOURCE_DIR}/src/utils/mod_settings.h
    ${CMAKE_SOURCE_DIR}/src/utils/profiler.h
    ${CMAKE_SOURCE_DIR}/src/utils/debug_draw.h
    ${CMAKE_SOURCE_DIR}/src/utils/debug_draw.cpp
    ${CMAKE_SOURCE_DIR}/src/hooking/framebuffer.cpp
    ${CMAKE_SOURCE_DIR}/src/hooking/framebuffer.h
    ${CMAKE_SOURCE_DIR}/src/hooking/layer.cpp
    ${CMAKE_SOURCE_DIR}/src/hooking/layer.h
    ${CMAKE_SOURCE_DIR}/src/hooking/cemu_hooks.h
    ${CMAKE_SOURCE_DIR}/src/hooking/camera.cpp
    ${CMAKE_SOURCE_DIR}/src/hooking/game_state.cpp
    ${CMAKE_SOURCE_DIR}/src/hooking/settings.cpp
    ${CMAKE_SOURCE_DIR}/src/hooking/profiler_hooks.cpp
    ${CMAKE_SOURCE_DIR}/src/hooking/weapon.h
    ${CMAKE_SOURCE_DIR}/src/hooking/weapon.cpp
    ${CMAKE_SOURCE_DIR}/src/hooking/imgui_menus.h
    ${CMAKE_SOURCE_DIR}/src/hooking/imgui_menus.cpp
    ${CMAKE_SOURCE_DIR}/src/hooking/controls.cpp
    ${CMAKE_SOURCE_DIR}/src/hooking/entity_debugger.h
    ${CMAKE_SOURCE_DIR}/src/hooking/entity_debugger.cpp
    ${CMAKE_SOURCE_DIR}/src/hooking/rumble.cpp
    ${CMAKE_SOURCE_DIR}/src/hooking/rumble.h
    ${CMAKE_SOURCE_DIR}/src/hooking/skeleton.cpp
    ${CMAKE_SOURCE_DIR}/src/hooking/openxr_motion_bridge.h
    ${CMAKE_SOURCE_DIR}/src/hooking/entity_controller.cpp
    ${CMAKE_SOURCE_DIR}/src/rendering/renderer.cpp
    ${CMAKE_SOURCE_DIR}/src/rendering/renderer.h
    ${CMAKE_SOURCE_DIR}/src/rendering/openxr.cpp
    ${CMAKE_SOURCE_DIR}/src/rendering/openxr.h
    ${CMAKE_SOURCE_DIR}/src/rendering/swapchain.cpp
    ${CMAKE_SOURCE_DIR}/src/rendering/swapchain.h
    ${CMAKE_SOURCE_DIR}/src/rendering/texture.cpp
    ${CMAKE_SOURCE_DIR}/src/rendering/texture.h
    ${CMAKE_SOURCE_DIR}/src/rendering/vulkan.cpp
    ${CMAKE_SOURCE_DIR}/src/rendering/vulkan.h
    ${CMAKE_SOURCE_DIR}/src/rendering/vulkan_imgui.cpp
)

if(WIN32)
    target_sources(BetterVR_Layer PUBLIC
        ${COMMON_SOURCES}
        ${CMAKE_SOURCE_DIR}/src/utils/d3d12_utils.h
        ${CMAKE_SOURCE_DIR}/src/rendering/d3d12.cpp
        ${CMAKE_SOURCE_DIR}/src/rendering/d3d12.h
    )
else()
    target_sources(BetterVR_Layer PUBLIC
        ${COMMON_SOURCES}
        ${CMAKE_SOURCE_DIR}/src/rendering/vk_composer.cpp
        ${CMAKE_SOURCE_DIR}/src/rendering/vk_composer.h
        ${CMAKE_SOURCE_DIR}/src/utils/shader_utils.h
    )
endif()

# Graphic-pack resource files (display-only in IDE).
file(GLOB_RECURSE GRAPHIC_PACK_FILES "${CMAKE_SOURCE_DIR}/resources/BreathOfTheWild_BetterVR/*")
if(GRAPHIC_PACK_FILES)
    foreach(GRAPHIC_PACK_FILE ${GRAPHIC_PACK_FILES})
        file(RELATIVE_PATH REL_PATH "${CMAKE_SOURCE_DIR}/resources/BreathOfTheWild_BetterVR" "${GRAPHIC_PACK_FILE}")
        string(REPLACE "\\" "/" REL_PATH "${REL_PATH}")
        source_group("BreathOfTheWild_BetterVR" FILES "${GRAPHIC_PACK_FILE}")
        set_source_files_properties("${GRAPHIC_PACK_FILE}" PROPERTIES HEADER_FILE_ONLY TRUE)
        list(APPEND GRAPHIC_PACK_HEADER_FILES "${GRAPHIC_PACK_FILE}")
    endforeach()
    target_sources(BetterVR_Layer PRIVATE ${GRAPHIC_PACK_HEADER_FILES})
endif()

# ---- Libraries ----
target_link_libraries(BetterVR_Layer PRIVATE OpenXR::headers OpenXR::openxr_loader)
target_include_directories(BetterVR_Layer SYSTEM PRIVATE ${VULKAN_HEADERS_INCLUDE_DIRS})
target_link_libraries(BetterVR_Layer PRIVATE glm::glm)

if(WIN32)
    target_link_libraries(BetterVR_Layer PRIVATE imgui::imgui)
    target_link_libraries(BetterVR_Layer PRIVATE implot::implot)
    target_compile_definitions(BetterVR_Layer PRIVATE IMGUI_IMPL_VULKAN_NO_PROTOTYPES)
else()
    # Required on Linux too: without this, imgui_impl_vulkan.cpp uses static
    # prototypes (vkGetPhysicalDeviceMemoryProperties etc.) that bypass our
    # function-pointer table. Those calls go through the loader's terminator,
    # which rejects our layer-side VkPhysicalDevice handle.
    # Apply only to imgui_impl_vulkan.cpp + vulkan_imgui.cpp so the rest of our
    # code can keep using direct vulkan.h prototypes (texture.cpp, vk_composer
    # etc. expect them globally).
    set_source_files_properties(
        ${CMAKE_SOURCE_DIR}/dependencies/imgui_impl_vulkan.cpp
        PROPERTIES
            COMPILE_DEFINITIONS "IMGUI_IMPL_VULKAN_NO_PROTOTYPES"
            SKIP_PRECOMPILE_HEADERS TRUE
    )
    # Vendored ImGui + ImPlot under dependencies/.
    target_sources(BetterVR_Layer PRIVATE
        ${CMAKE_SOURCE_DIR}/dependencies/imgui/imgui.cpp
        ${CMAKE_SOURCE_DIR}/dependencies/imgui/imgui_draw.cpp
        ${CMAKE_SOURCE_DIR}/dependencies/imgui/imgui_tables.cpp
        ${CMAKE_SOURCE_DIR}/dependencies/imgui/imgui_widgets.cpp
    )
    if(EXISTS ${CMAKE_SOURCE_DIR}/dependencies/implot/implot.cpp)
        target_sources(BetterVR_Layer PRIVATE
            ${CMAKE_SOURCE_DIR}/dependencies/implot/implot.cpp
            ${CMAKE_SOURCE_DIR}/dependencies/implot/implot_items.cpp
        )
        target_include_directories(BetterVR_Layer PRIVATE ${CMAKE_SOURCE_DIR}/dependencies/implot)
    endif()
    target_include_directories(BetterVR_Layer PRIVATE ${CMAKE_SOURCE_DIR}/dependencies/imgui)
    # ImGui-Vulkan backend uses the loader's prototypes (we link libvulkan).
    target_link_libraries(BetterVR_Layer PRIVATE Vulkan::Vulkan Threads::Threads)
    # Runtime GLSL compilation via shaderc.
    # Prefer shaderc_shared (.so on Arch) — has glslang/SPIRV linked transitively.
    # Fall back to shaderc_combined (.a) plus explicit glslang/SPIRV deps.
    find_library(SHADERC_SHARED_LIB shaderc_shared)
    if(SHADERC_SHARED_LIB)
        target_link_libraries(BetterVR_Layer PRIVATE ${SHADERC_SHARED_LIB})
    else()
        find_library(SHADERC_COMBINED_LIB shaderc_combined)
        if(SHADERC_COMBINED_LIB)
            find_library(GLSLANG_LIB glslang REQUIRED)
            find_library(GLSLANG_RES_LIB glslang-default-resource-limits REQUIRED)
            find_library(SPIRV_LIB SPIRV REQUIRED)
            find_library(SPIRV_TOOLS_LIB SPIRV-Tools REQUIRED)
            find_library(SPIRV_TOOLS_OPT_LIB SPIRV-Tools-opt REQUIRED)
            target_link_libraries(BetterVR_Layer PRIVATE
                ${SHADERC_COMBINED_LIB}
                ${GLSLANG_LIB}
                ${GLSLANG_RES_LIB}
                ${SPIRV_LIB}
                ${SPIRV_TOOLS_OPT_LIB}
                ${SPIRV_TOOLS_LIB})
        else()
            message(FATAL_ERROR "shaderc library not found. Install Arch package 'shaderc'.")
        endif()
    endif()
endif()

target_sources(BetterVR_Layer PRIVATE ${CMAKE_SOURCE_DIR}/dependencies/imgui_impl_vulkan.cpp)
target_include_directories(BetterVR_Layer PRIVATE ${CMAKE_SOURCE_DIR}/dependencies)

# ---- Install + debugger config ----
set(BETTERVR_GRAPHIC_PACK_INSTALL_DIR "${CMAKE_INSTALL_PREFIX}/graphicPacks/BreathOfTheWild_BetterVR")

if(WIN32)
    if(CMAKE_INSTALL_PREFIX STREQUAL "${CMAKE_SOURCE_DIR}/Cemu")
        install(CODE [[
            file(REMOVE_RECURSE "${CMAKE_INSTALL_PREFIX}/graphicPacks/BreathOfTheWild_BetterVR")
        ]])
    endif()

    install(TARGETS BetterVR_Layer
        RUNTIME DESTINATION "${CMAKE_INSTALL_PREFIX}"
    )
    install(FILES "$<TARGET_FILE_DIR:BetterVR_Layer>/BetterVR_Layer.direct.json" DESTINATION "${CMAKE_INSTALL_PREFIX}")
    install(FILES $<TARGET_PDB_FILE:BetterVR_Layer> DESTINATION "${CMAKE_INSTALL_PREFIX}" CONFIGURATIONS Debug RelWithDebInfo OPTIONAL)

    install(DIRECTORY "${CMAKE_SOURCE_DIR}/resources/BreathOfTheWild_BetterVR/"
        DESTINATION "${BETTERVR_GRAPHIC_PACK_INSTALL_DIR}"
        CONFIGURATIONS Debug
    )
    install(DIRECTORY "${CMAKE_SOURCE_DIR}/resources/BreathOfTheWild_BetterVR/"
        DESTINATION "${BETTERVR_GRAPHIC_PACK_INSTALL_DIR}"
        CONFIGURATIONS Release RelWithDebInfo MinSizeRel
        PATTERN "patch_debug_*.asm" EXCLUDE
    )

    set_target_properties(BetterVR_Layer PROPERTIES
        VS_DEBUGGER_COMMAND "${CMAKE_INSTALL_PREFIX}/Cemu.exe"
        VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_INSTALL_PREFIX}"
        VS_DEBUGGER_ENVIRONMENT "ENABLE_BETTERVR_MOD=1"
    )
else()
    # On Linux the layer becomes an implicit Vulkan layer. Install into the
    # XDG vulkan implicit_layer.d after build (manual step to avoid
    # CMake-time root permissions); CMakeLists.txt prints the command.
    set_target_properties(BetterVR_Layer PROPERTIES
        OUTPUT_NAME "VkLayer_BetterVR"
        PREFIX "lib")
endif()
