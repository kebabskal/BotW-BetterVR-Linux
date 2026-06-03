add_library(BetterVR_Layer SHARED)

if(WIN32)
    file(GENERATE OUTPUT "$<TARGET_FILE_DIR:BetterVR_Layer>/BetterVR_Layer.json" INPUT "${CMAKE_SOURCE_DIR}/resources/BetterVR_Layer.json")
    file(GENERATE OUTPUT "$<TARGET_FILE_DIR:BetterVR_Layer>/BetterVR_Layer.direct.json" INPUT "${CMAKE_SOURCE_DIR}/resources/BetterVR_Layer.direct.json")
    target_link_options(BetterVR_Layer PRIVATE "$<$<CONFIG:Debug>:/INCREMENTAL>")
endif()

target_precompile_headers(BetterVR_Layer PRIVATE ${CMAKE_SOURCE_DIR}/include/pch.h)
target_include_directories(BetterVR_Layer BEFORE PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_include_directories(BetterVR_Layer AFTER PRIVATE ${CMAKE_SOURCE_DIR}/include)

if(UNIX AND NOT APPLE)
    # Phase 2A Linux smoke test: build only the minimal layer entry. The full
    # BetterVR rendering pipeline is still tied to D3D12 and needs the OpenXR
    # binding rewrite (Phase 2B) before it can build on Linux.
    target_sources(BetterVR_Layer PRIVATE
        ${CMAKE_SOURCE_DIR}/src/layer_linux.cpp
        ${CMAKE_SOURCE_DIR}/dependencies/imgui_impl_vulkan.cpp)
    # On Linux, allow ImGui's Vulkan backend to use prototypes (linked
    # against libvulkan.so.1). NO_PROTOTYPES is now gated to non-Linux above
    # so we just need to add the Vulkan link.
    find_package(Vulkan REQUIRED)
    target_link_libraries(BetterVR_Layer PRIVATE Vulkan::Vulkan)
else()
target_sources(BetterVR_Layer PUBLIC
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

# D3D12-specific sources only on Windows.
target_sources(BetterVR_Layer PUBLIC
    ${CMAKE_SOURCE_DIR}/src/utils/d3d12_utils.h
    ${CMAKE_SOURCE_DIR}/src/rendering/d3d12.cpp
    ${CMAKE_SOURCE_DIR}/src/rendering/d3d12.h
)
endif() # UNIX-vs-WIN32 source-set split

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

target_link_libraries(BetterVR_Layer PRIVATE OpenXR::headers OpenXR::openxr_loader)
target_include_directories(BetterVR_Layer SYSTEM PRIVATE ${VULKAN_HEADERS_INCLUDE_DIRS})
target_link_libraries(BetterVR_Layer PRIVATE glm::glm)
target_link_libraries(BetterVR_Layer PRIVATE imgui::imgui)
target_link_libraries(BetterVR_Layer PRIVATE implot::implot)

if(NOT (UNIX AND NOT APPLE))
    target_compile_definitions(BetterVR_Layer PRIVATE IMGUI_IMPL_VULKAN_NO_PROTOTYPES)
endif()
target_sources(BetterVR_Layer PRIVATE ${CMAKE_SOURCE_DIR}/dependencies/imgui_impl_vulkan.cpp)
target_include_directories(BetterVR_Layer PRIVATE ${CMAKE_SOURCE_DIR}/dependencies)

# Install/Debugger related stuff. This is really only used for direct calls
set(BETTERVR_GRAPHIC_PACK_INSTALL_DIR "${CMAKE_INSTALL_PREFIX}/graphicPacks/BreathOfTheWild_BetterVR")

if(CMAKE_INSTALL_PREFIX STREQUAL "${CMAKE_SOURCE_DIR}/Cemu")
    install(CODE [[
        file(REMOVE_RECURSE "${CMAKE_INSTALL_PREFIX}/graphicPacks/BreathOfTheWild_BetterVR")
    ]])
endif()

install(TARGETS BetterVR_Layer
    RUNTIME DESTINATION "${CMAKE_INSTALL_PREFIX}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_PREFIX}"
)
if(WIN32)
    install(FILES "$<TARGET_FILE_DIR:BetterVR_Layer>/BetterVR_Layer.direct.json" DESTINATION "${CMAKE_INSTALL_PREFIX}")
    install(FILES $<TARGET_PDB_FILE:BetterVR_Layer> DESTINATION "${CMAKE_INSTALL_PREFIX}" CONFIGURATIONS Debug RelWithDebInfo OPTIONAL)
endif()

install(DIRECTORY "${CMAKE_SOURCE_DIR}/resources/BreathOfTheWild_BetterVR/"
    DESTINATION "${BETTERVR_GRAPHIC_PACK_INSTALL_DIR}"
    CONFIGURATIONS Debug
)
install(DIRECTORY "${CMAKE_SOURCE_DIR}/resources/BreathOfTheWild_BetterVR/"
    DESTINATION "${BETTERVR_GRAPHIC_PACK_INSTALL_DIR}"
    CONFIGURATIONS Release RelWithDebInfo MinSizeRel
    PATTERN "patch_debug_*.asm" EXCLUDE
)

if(WIN32)
    set_target_properties(BetterVR_Layer PROPERTIES
        VS_DEBUGGER_COMMAND "${CMAKE_INSTALL_PREFIX}/Cemu.exe"
        VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_INSTALL_PREFIX}"
        VS_DEBUGGER_ENVIRONMENT "ENABLE_BETTERVR_MOD=1"
    )
endif()

if(UNIX AND NOT APPLE)
    # Vulkan loader expects layer .so files named libVkLayer_*.so by convention
    set_target_properties(BetterVR_Layer PROPERTIES
        OUTPUT_NAME "VkLayer_BetterVR"
        PREFIX "lib"
    )
    target_link_libraries(BetterVR_Layer PRIVATE ${CMAKE_DL_LIBS})
endif()
