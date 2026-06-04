#include "pch.h"

#include "openxr.h"
#include "instance.h"

static XrBool32 XR_DebugUtilsMessengerCallback(XrDebugUtilsMessageSeverityFlagsEXT messageSeverity, XrDebugUtilsMessageTypeFlagsEXT messageType, const XrDebugUtilsMessengerCallbackDataEXT* callbackData, void* userData) {
    Log::print<XR_DEBUGUTILS>("[XR Debug Utils] Function {}: {}", callbackData->functionName, callbackData->message);
    return XR_FALSE;
}

OpenXR::OpenXR() {
    Log::print<INFO>("[BVR-trace] OpenXR ctor: enter");
    uint32_t xrExtensionCount = 0;
    xrEnumerateInstanceExtensionProperties(NULL, 0, &xrExtensionCount, NULL);
    std::vector<XrExtensionProperties> instanceExtensions;
    instanceExtensions.resize(xrExtensionCount, { XR_TYPE_EXTENSION_PROPERTIES, NULL });
    {
        XrResult result = xrEnumerateInstanceExtensionProperties(NULL, xrExtensionCount, &xrExtensionCount, instanceExtensions.data());
        if (result == XR_ERROR_RUNTIME_FAILURE) {
            Log::print<ERROR>("Couldn't enumerate OpenXR extensions! Is the OpenXR runtime installed and set to the correct runtime? Restarting might help, or going to SteamVR/Oculus Link's Settings and making sure OpenXR is enabled.");
        }
        checkXRResult(result, "Couldn't enumerate OpenXR extensions!");
    }

    // Create instance with required extensions
    bool d3d12Supported = false;
    bool depthSupported = false;
    bool timeConvSupported = false;
    bool debugUtilsSupported = false;
    for (XrExtensionProperties& extensionProperties : instanceExtensions) {
        Log::print<VERBOSE>("Found available OpenXR extension: {}", extensionProperties.extensionName);
        if (strcmp(extensionProperties.extensionName, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME) == 0) {
            d3d12Supported = true;
        }
        if (strcmp(extensionProperties.extensionName, XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME) == 0) {
            depthSupported = true;
        }
        else if (strcmp(extensionProperties.extensionName, "XR_KHR_disabled_linux") == 0) {
            timeConvSupported = true;
        }
        else if (strcmp(extensionProperties.extensionName, XR_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0) {
#ifdef _DEBUG
            debugUtilsSupported = Log::isLogTypeEnabled<XR_DEBUGUTILS>();
#endif
        }
    }

    if (!d3d12Supported) {
        Log::print<ERROR>("OpenXR runtime doesn't support D3D12 (XR_KHR_D3D12_ENABLE)!");
        throw std::runtime_error("Current OpenXR runtime doesn't support Direct3D 12 (XR_KHR_D3D12_ENABLE). See the Github page's troubleshooting section for a solution!");
    }
    if (!depthSupported) {
        Log::print<ERROR>("OpenXR runtime doesn't support depth composition layers (XR_KHR_COMPOSITION_LAYER_DEPTH)!");
        throw std::runtime_error("Current OpenXR runtime doesn't support depth composition layers (XR_KHR_COMPOSITION_LAYER_DEPTH). See the Github page's troubleshooting section for a solution!");
    }
    if (!timeConvSupported) {
        Log::print<WARNING>("OpenXR runtime doesn't support converting time from/to XrTime (XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME). Not required, as of this version.");
    }
    if (!debugUtilsSupported && Log::isLogTypeEnabled<XR_DEBUGUTILS>()) {
        Log::print<INFO>("OpenXR runtime doesn't support debug utils (XR_EXT_DEBUG_UTILS)! Errors/debug information will no longer be able to be shown!");
    }

    std::vector<const char*> enabledExtensions = { XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME, XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME };
    if (debugUtilsSupported) enabledExtensions.emplace_back(XR_EXT_DEBUG_UTILS_EXTENSION_NAME);
    Log::print<INFO>("[BVR-trace] before xrCreateInstance");

    XrInstanceCreateInfo xrInstanceCreateInfo = { XR_TYPE_INSTANCE_CREATE_INFO };
    xrInstanceCreateInfo.createFlags = 0;
    xrInstanceCreateInfo.enabledExtensionCount = (uint32_t)enabledExtensions.size();
    xrInstanceCreateInfo.enabledExtensionNames = enabledExtensions.data();
    xrInstanceCreateInfo.enabledApiLayerCount = 0;
    xrInstanceCreateInfo.enabledApiLayerNames = NULL;
    xrInstanceCreateInfo.applicationInfo = { "BetterVR", 1, "Cemu", 1, XR_API_VERSION_1_0 };
    {
        XrResult result = XR_ERROR_RUNTIME_FAILURE;
        for (int i = 0; i < 3; i++) {
             result = xrCreateInstance(&xrInstanceCreateInfo, &m_instance);
             if (XR_SUCCEEDED(result)) {
                 break;
             }
             std::this_thread::sleep_for(std::chrono::seconds(2));
        }

        if (result == XR_ERROR_RUNTIME_FAILURE) {
            Log::print<ERROR>("Failed to create OpenXR instance! Is the OpenXR runtime installed and set to the correct runtime? Restarting might help, or going to SteamVR/Oculus Link's Settings and making sure OpenXR is enabled.");
        }
        checkXRResult(result, "Failed to initialize the OpenXR instance!");
    }

    // Load extension pointers for this XrInstance
    xrGetInstanceProcAddr(m_instance, "xrGetVulkanGraphicsRequirements2KHR", (PFN_xrVoidFunction*)&func_xrGetVulkanGraphicsRequirements2KHR);
    // (Win32 perf-counter time conversion not used on Linux — XrTime is in
    // nanoseconds and our timing uses CLOCK_MONOTONIC throughout.)
    (void)timeConvSupported;
    if (debugUtilsSupported) {
        xrGetInstanceProcAddr(m_instance, "xrCreateDebugUtilsMessengerEXT", (PFN_xrVoidFunction*)&func_xrCreateDebugUtilsMessengerEXT);
        xrGetInstanceProcAddr(m_instance, "xrDestroyDebugUtilsMessengerEXT", (PFN_xrVoidFunction*)&func_xrDestroyDebugUtilsMessengerEXT);
    }

    // Create debug utils messenger
    if (debugUtilsSupported) {
        XrDebugUtilsMessengerCreateInfoEXT utilsMessengerCreateInfo = { XR_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
        utilsMessengerCreateInfo.messageSeverities = XR_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT | XR_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT | XR_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | XR_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        utilsMessengerCreateInfo.messageTypes = XR_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | XR_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | XR_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT | XR_DEBUG_UTILS_MESSAGE_TYPE_CONFORMANCE_BIT_EXT;
        utilsMessengerCreateInfo.userCallback = &XR_DebugUtilsMessengerCallback;
        func_xrCreateDebugUtilsMessengerEXT(m_instance, &utilsMessengerCreateInfo, &m_debugMessengerHandle);
    }

    // Get system information
    Log::print<INFO>("[BVR-trace] before xrGetSystem");
    XrSystemGetInfo xrSystemGetInfo = { XR_TYPE_SYSTEM_GET_INFO };
    xrSystemGetInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    checkXRResult(xrGetSystem(m_instance, &xrSystemGetInfo, &m_systemId), "No (available) head mounted display found!");
    Log::print<INFO>("[BVR-trace] after xrGetSystem -> systemId=0x{:x}", (uint64_t)m_systemId);

    XrSystemProperties xrSystemProperties = { XR_TYPE_SYSTEM_PROPERTIES };
    checkXRResult(xrGetSystemProperties(m_instance, m_systemId, &xrSystemProperties), "Couldn't get system properties of the given VR headset!");
    Log::print<INFO>("[BVR-trace] after xrGetSystemProperties");
    m_capabilities.supportsOrientational = xrSystemProperties.trackingProperties.orientationTracking;
    m_capabilities.supportsPositional = xrSystemProperties.trackingProperties.positionTracking;

    XrInstanceProperties properties = { XR_TYPE_INSTANCE_PROPERTIES };
    checkXRResult(xrGetInstanceProperties(m_instance, &properties), "Failed to get runtime details using xrGetInstanceProperties!");

    XrViewConfigurationProperties stereoViewConfiguration = { XR_TYPE_VIEW_CONFIGURATION_PROPERTIES };
    checkXRResult(xrGetViewConfigurationProperties(m_instance, m_systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, &stereoViewConfiguration), "There's no VR headset available that allows stereo rendering!");
    m_capabilities.supportsMutatableFOV = stereoViewConfiguration.fovMutable;

    XrGraphicsRequirementsVulkan2KHR graphicsRequirements = { XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR };
    checkXRResult(func_xrGetVulkanGraphicsRequirements2KHR(m_instance, m_systemId, &graphicsRequirements), "Couldn't get Vulkan2 requirements for the given VR headset!");
    // adapterLuid / minFeatureLevel are D3D12-specific fields that the
    // Vulkan2 graphics-requirements struct doesn't carry. On Linux the
    // composer picks the physical device via xrGetVulkanGraphicsDevice2KHR
    // (which gives us the VkPhysicalDevice directly), so the LUID is unused.
    m_capabilities.adapter = LUID{};
    m_capabilities.minFeatureLevel = D3D_FEATURE_LEVEL_12_0;

    // Print configuration used, mostly for debugging purposes
    Log::print<INFO>("Acquired system to be used:");
    Log::print<INFO>(" - System Name: {}", xrSystemProperties.systemName); // Oculus Quest2
    Log::print<INFO>(" - Runtime Name: {}", properties.runtimeName); // Oculus
    Log::print<INFO>(" - Runtime Version: {}.{}.{}", XR_VERSION_MAJOR(properties.runtimeVersion), XR_VERSION_MINOR(properties.runtimeVersion), XR_VERSION_PATCH(properties.runtimeVersion));
    Log::print<INFO>(" - Supports Mutable FOV: {}", m_capabilities.supportsMutatableFOV ? "Yes" : "No");
    Log::print<INFO>(" - Supports Orientation Tracking: {}", xrSystemProperties.trackingProperties.orientationTracking ? "Yes" : "No");
    Log::print<INFO>(" - Supports Positional Tracking: {}", xrSystemProperties.trackingProperties.positionTracking ? "Yes" : "No");
    Log::print<INFO>(" - Vulkan min API version: {}.{}.{}",
        XR_VERSION_MAJOR(graphicsRequirements.minApiVersionSupported),
        XR_VERSION_MINOR(graphicsRequirements.minApiVersionSupported),
        XR_VERSION_PATCH(graphicsRequirements.minApiVersionSupported));

    m_capabilities.isOculusLinkRuntime = std::string(properties.runtimeName) == "Oculus";
    Log::print<INFO>(" - Using Meta Quest Link OpenXR runtime: {}", m_capabilities.isOculusLinkRuntime ? "Yes" : "No");
    
    m_capabilities.isMetaSimulator = std::string(properties.runtimeName).find("Meta XR Simulator") != std::string::npos;
}

OpenXR::~OpenXR() {
    this->m_renderer.reset();

    for (XrSpace& handSpace : m_inGameHandSpaces) {
        if (handSpace != XR_NULL_HANDLE) {
            xrDestroySpace(handSpace);
            handSpace = XR_NULL_HANDLE;
        }
    }
    for (XrSpace& aimSpace : m_inGameAimSpaces) {
        if (aimSpace != XR_NULL_HANDLE) {
            xrDestroySpace(aimSpace);
            aimSpace = XR_NULL_HANDLE;
        }
    }
    for (XrSpace& handSpace : m_inMenuHandSpaces) {
        if (handSpace != XR_NULL_HANDLE) {
            xrDestroySpace(handSpace);
            handSpace = XR_NULL_HANDLE;
        }
    }
    for (XrSpace& aimSpace : m_inMenuAimSpaces) {
        if (aimSpace != XR_NULL_HANDLE) {
            xrDestroySpace(aimSpace);
            aimSpace = XR_NULL_HANDLE;
        }
    }

    if (m_headSpace != XR_NULL_HANDLE) {
        xrDestroySpace(m_headSpace);
    }

    if (m_stageSpace != XR_NULL_HANDLE) {
        xrDestroySpace(m_stageSpace);
    }

    if (m_session != XR_NULL_HANDLE) {
        xrDestroySession(m_session);
    }

    if (m_debugMessengerHandle != XR_NULL_HANDLE) {
        func_xrDestroyDebugUtilsMessengerEXT(m_debugMessengerHandle);
    }

    if (m_instance != XR_NULL_HANDLE) {
        xrDestroyInstance(m_instance);
    }
}

std::array<XrViewConfigurationView, 2> OpenXR::GetViewConfigurations() {
    uint32_t eyeViewsConfigurationCount = 0;
    checkXRResult(xrEnumerateViewConfigurationViews(m_instance, m_systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &eyeViewsConfigurationCount, nullptr), "Can't get number of individual views for stereo view available");
    checkAssert(eyeViewsConfigurationCount == 2, std::format("Expected 2 views for the stereo configuration but got {} which is unsupported!", eyeViewsConfigurationCount).c_str());

    std::array<XrViewConfigurationView, 2> xrViewConf = { XrViewConfigurationView{ XR_TYPE_VIEW_CONFIGURATION_VIEW }, XrViewConfigurationView{ XR_TYPE_VIEW_CONFIGURATION_VIEW } };
    checkXRResult(xrEnumerateViewConfigurationViews(m_instance, m_systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, eyeViewsConfigurationCount, &eyeViewsConfigurationCount, xrViewConf.data()), "Can't get individual views for stereo view available!");

    Log::print<INFO>("Swapchain configuration to be used:");
    Log::print<INFO>(" - [Left] Max View Resolution: w={}, h={} with {} samples", xrViewConf[0].maxImageRectWidth, xrViewConf[0].maxImageRectHeight, xrViewConf[0].maxSwapchainSampleCount);
    Log::print<INFO>(" - [Right] Max View Resolution: w={}, h={}  with {} samples", xrViewConf[1].maxImageRectWidth, xrViewConf[1].maxImageRectHeight, xrViewConf[1].maxSwapchainSampleCount);
    Log::print<INFO>(" - [Left] Recommended View Resolution: w={}, h={}  with {} samples", xrViewConf[0].recommendedImageRectWidth, xrViewConf[0].recommendedImageRectHeight, xrViewConf[0].recommendedSwapchainSampleCount);
    Log::print<INFO>(" - [Right] Recommended View Resolution: w={}, h={}  with {} samples", xrViewConf[1].recommendedImageRectWidth, xrViewConf[1].recommendedImageRectHeight, xrViewConf[0].recommendedSwapchainSampleCount);
    return xrViewConf;
}

void OpenXR::CreateSession(const XrGraphicsBindingVulkan2KHR& d3d12Binding) {
    Log::print<INFO>("Creating the OpenXR session...");

    XrSessionCreateInfo sessionCreateInfo = { XR_TYPE_SESSION_CREATE_INFO };
    sessionCreateInfo.systemId = m_systemId;
    sessionCreateInfo.next = &d3d12Binding;
    sessionCreateInfo.createFlags = 0;
    checkXRResult(xrCreateSession(m_instance, &sessionCreateInfo, &m_session), "Failed to create Vulkan-based OpenXR session!");

    Log::print<INFO>("Creating the OpenXR spaces...");
    XrReferenceSpaceCreateInfo stageSpaceCreateInfo = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    stageSpaceCreateInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
    stageSpaceCreateInfo.poseInReferenceSpace = s_xrIdentityPose;
    checkXRResult(xrCreateReferenceSpace(m_session, &stageSpaceCreateInfo, &m_stageSpace), "Failed to create reference space for stage!");

    XrReferenceSpaceCreateInfo headSpaceCreateInfo = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    headSpaceCreateInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    headSpaceCreateInfo.poseInReferenceSpace = s_xrIdentityPose;
    checkXRResult(xrCreateReferenceSpace(m_session, &headSpaceCreateInfo, &m_headSpace), "Failed to create reference space for head!");
}

void OpenXR::CreateActions() {
    Log::print<INFO>("Creating the OpenXR actions...");

    m_handPaths = { GetXRPath("/user/hand/left"), GetXRPath("/user/hand/right") };

    auto createAction = [this](const XrActionSet& actionSet, const char* id, const char* name, XrActionType actionType, XrAction& action) {
        XrActionCreateInfo actionInfo = { XR_TYPE_ACTION_CREATE_INFO };
        actionInfo.actionType = actionType;
        strncpy_s(actionInfo.actionName, id, XR_MAX_ACTION_NAME_SIZE-1);
        strncpy_s(actionInfo.localizedActionName, name, XR_MAX_LOCALIZED_ACTION_NAME_SIZE-1);
        actionInfo.countSubactionPaths = (uint32_t)m_handPaths.size();
        actionInfo.subactionPaths = m_handPaths.data();
        checkXRResult(xrCreateAction(actionSet, &actionInfo, &action), std::format("Failed to create action for {}", id).c_str());
    };

    {
        XrActionSetCreateInfo actionSetInfo = { XR_TYPE_ACTION_SET_CREATE_INFO };
        strcpy_s(actionSetInfo.actionSetName, "gameplay_fps");
        strcpy_s(actionSetInfo.localizedActionSetName, "Gameplay");
        actionSetInfo.priority = 0;
        checkXRResult(xrCreateActionSet(m_instance, &actionSetInfo, &m_gameplayActionSet), "Failed to create controller actions for gameplay_fps!");

        createAction(m_gameplayActionSet, "pose", "Grip Pose", XR_ACTION_TYPE_POSE_INPUT, m_inGameGripPoseAction);
        createAction(m_gameplayActionSet, "aim_pose", "Aim Pose", XR_ACTION_TYPE_POSE_INPUT, m_inGameAimPoseAction);
        createAction(m_gameplayActionSet, "move", "Move", XR_ACTION_TYPE_VECTOR2F_INPUT, m_moveAction);
        createAction(m_gameplayActionSet, "camera", "Camera Rotation", XR_ACTION_TYPE_VECTOR2F_INPUT, m_cameraAction);
        createAction(m_gameplayActionSet, "ingame_modmenu", "Mod Menu", XR_ACTION_TYPE_BOOLEAN_INPUT, m_inGame_modMenuAction);

        createAction(m_gameplayActionSet, "grab_interact", "Interact / Pick up objects from floor or weapon from body slots", XR_ACTION_TYPE_FLOAT_INPUT, m_grab_interactAction);
        createAction(m_gameplayActionSet, "jump", "Jump", XR_ACTION_TYPE_BOOLEAN_INPUT, m_jumpAction);
        createAction(m_gameplayActionSet, "run_interact_cancel", "Interact (Quick press) - Run/Cancel Interaction (Long press)", XR_ACTION_TYPE_BOOLEAN_INPUT, m_run_interactAction);
        createAction(m_gameplayActionSet, "userune_dpadmenu", "Use Rune (Quick press) - Dpad menu (Long press)", XR_ACTION_TYPE_BOOLEAN_INPUT, m_useRune_dpadMenu_Action);

        createAction(m_gameplayActionSet, "userighthanditem", "Use/Attack/Throw item held in right hand (Melee attacks/Draw bow/Throw object)", XR_ACTION_TYPE_BOOLEAN_INPUT, m_useRightItemAction);
        createAction(m_gameplayActionSet, "uselefthanditem", "Use item held in left hand (Rune/Shield Parry)", XR_ACTION_TYPE_BOOLEAN_INPUT, m_useLeftItemAction);

        createAction(m_gameplayActionSet, "crouch_scope", "Crouch (Quick press) - Open Scope (Long press)", XR_ACTION_TYPE_BOOLEAN_INPUT, m_crouch_scopeAction);
        createAction(m_gameplayActionSet, "ingame_inventory_map", "Open Inventory (Quick press) - Open Map (Long press)", XR_ACTION_TYPE_BOOLEAN_INPUT, m_inGame_inventory_mapAction);

        createAction(m_gameplayActionSet, "rumble", "Rumble", XR_ACTION_TYPE_VIBRATION_OUTPUT, m_rumbleAction);
    }

    {
        XrActionSetCreateInfo actionSetInfo = { XR_TYPE_ACTION_SET_CREATE_INFO };
        strcpy_s(actionSetInfo.actionSetName, "menu");
        strcpy_s(actionSetInfo.localizedActionSetName, "Menu Navigation");
        actionSetInfo.priority = 0;
        checkXRResult(xrCreateActionSet(m_instance, &actionSetInfo, &m_menuActionSet), "Failed to create controller bindings for the menu!");

        createAction(m_menuActionSet, "pose", "Grip Pose", XR_ACTION_TYPE_POSE_INPUT, m_inMenuGripPoseAction);
        createAction(m_menuActionSet, "aim_pose", "Aim Pose", XR_ACTION_TYPE_POSE_INPUT, m_inMenuAimPoseAction);

        createAction(m_menuActionSet, "scroll", "Scroll (Left Thumbstick)", XR_ACTION_TYPE_VECTOR2F_INPUT, m_scrollAction);
        createAction(m_menuActionSet, "navigate", "Navigate (Right Thumbstick)", XR_ACTION_TYPE_VECTOR2F_INPUT, m_navigateAction);
        createAction(m_menuActionSet, "select", "Select (A Button)", XR_ACTION_TYPE_BOOLEAN_INPUT, m_selectAction);
        createAction(m_menuActionSet, "cancel", "Back/Cancel (B Button)", XR_ACTION_TYPE_BOOLEAN_INPUT, m_backAction);
        createAction(m_menuActionSet, "sort", "Sort (Y Button)", XR_ACTION_TYPE_BOOLEAN_INPUT, m_sortAction);
        createAction(m_menuActionSet, "hold", "Hold (X Button)", XR_ACTION_TYPE_BOOLEAN_INPUT, m_holdAction);
        createAction(m_menuActionSet, "left_grip", "Switch To Left Tab (L Button)", XR_ACTION_TYPE_BOOLEAN_INPUT, m_leftGripAction);
        createAction(m_menuActionSet, "right_grip", "Switch To Right Tab (R Button)", XR_ACTION_TYPE_BOOLEAN_INPUT, m_rightGripAction);
        createAction(m_menuActionSet, "lefttrigger", "Left Trigger", XR_ACTION_TYPE_BOOLEAN_INPUT, m_leftTriggerAction);
        createAction(m_menuActionSet, "righttrigger", "Right Trigger", XR_ACTION_TYPE_BOOLEAN_INPUT, m_rightTriggerAction);

        createAction(m_menuActionSet, "inmenu_inventory_map", "Close Inventory (Wii U - Start Button) - Close Map (Wii U - Select Button)", XR_ACTION_TYPE_BOOLEAN_INPUT, m_inMenu_inventory_mapAction);
        createAction(m_menuActionSet, "inmenu_modmenu", "Mod Menu", XR_ACTION_TYPE_BOOLEAN_INPUT, m_inMenu_modMenuAction);
    }

    {
        std::array suggestedBindings = {
            // === gameplay suggestions ===
            XrActionSuggestedBinding{ .action = m_inGameGripPoseAction, .binding = GetXRPath("/user/hand/left/input/grip/pose") },
            XrActionSuggestedBinding{ .action = m_inGameGripPoseAction, .binding = GetXRPath("/user/hand/right/input/grip/pose") },
            XrActionSuggestedBinding{ .action = m_inGameAimPoseAction, .binding = GetXRPath("/user/hand/left/input/aim/pose") },
            XrActionSuggestedBinding{ .action = m_inGameAimPoseAction, .binding = GetXRPath("/user/hand/right/input/aim/pose") },
            XrActionSuggestedBinding{ .action = m_crouch_scopeAction, .binding = GetXRPath("/user/hand/left/input/menu/click") },

            XrActionSuggestedBinding{ .action = m_grab_interactAction, .binding = GetXRPath("/user/hand/left/input/select/click") },
            XrActionSuggestedBinding{ .action = m_grab_interactAction, .binding = GetXRPath("/user/hand/right/input/select/click") },

            // === menu suggestions ===
            //XrActionSuggestedBinding{ .action = m_inMenu_mapAction, .binding = GetXRPath("/user/hand/right/input/menu/click") },
            //XrActionSuggestedBinding{ .action = m_selectAction, .binding = GetXRPath("/user/hand/right/input/select/click") },
            XrActionSuggestedBinding{ .action = m_backAction, .binding = GetXRPath("/user/hand/right/input/menu/click") },
            XrActionSuggestedBinding{ .action = m_sortAction, .binding = GetXRPath("/user/hand/left/input/select/click") },
            XrActionSuggestedBinding{ .action = m_holdAction, .binding = GetXRPath("/user/hand/left/input/menu/click") }
        };
        XrInteractionProfileSuggestedBinding suggestedBindingsInfo = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
        suggestedBindingsInfo.interactionProfile = GetXRPath("/interaction_profiles/khr/simple_controller");
        suggestedBindingsInfo.countSuggestedBindings = (uint32_t)suggestedBindings.size();
        suggestedBindingsInfo.suggestedBindings = suggestedBindings.data();
        checkXRResult(xrSuggestInteractionProfileBindings(m_instance, &suggestedBindingsInfo), "Failed to suggest simple controller profile bindings!");
    }

    {
        std::array suggestedBindings = {
            // === gameplay suggestions ===
            XrActionSuggestedBinding{ .action = m_inGameGripPoseAction, .binding = GetXRPath("/user/hand/left/input/grip/pose") },
            XrActionSuggestedBinding{ .action = m_inGameGripPoseAction, .binding = GetXRPath("/user/hand/right/input/grip/pose") },
            XrActionSuggestedBinding{ .action = m_inGameAimPoseAction, .binding = GetXRPath("/user/hand/left/input/aim/pose") },
            XrActionSuggestedBinding{ .action = m_inGameAimPoseAction, .binding = GetXRPath("/user/hand/right/input/aim/pose") },
            XrActionSuggestedBinding{ .action = m_moveAction, .binding = GetXRPath("/user/hand/left/input/thumbstick") },
            XrActionSuggestedBinding{ .action = m_cameraAction, .binding = GetXRPath("/user/hand/right/input/thumbstick") },

            XrActionSuggestedBinding{ .action = m_grab_interactAction, .binding = GetXRPath("/user/hand/left/input/squeeze/value") },
            XrActionSuggestedBinding{ .action = m_grab_interactAction, .binding = GetXRPath("/user/hand/right/input/squeeze/value") },
            XrActionSuggestedBinding{ .action = m_jumpAction, .binding = GetXRPath("/user/hand/right/input/b/click") },
            XrActionSuggestedBinding{ .action = m_run_interactAction, .binding = GetXRPath("/user/hand/right/input/a/click") },
            XrActionSuggestedBinding{ .action = m_useRune_dpadMenu_Action, .binding = GetXRPath("/user/hand/left/input/y/click") },
            XrActionSuggestedBinding{ .action = m_inGame_modMenuAction, .binding = GetXRPath("/user/hand/left/input/x/click") },

            XrActionSuggestedBinding{ .action = m_useLeftItemAction, .binding = GetXRPath("/user/hand/left/input/trigger/value") },
            XrActionSuggestedBinding{ .action = m_useRightItemAction, .binding = GetXRPath("/user/hand/right/input/trigger/value") },

            XrActionSuggestedBinding{ .action = m_crouch_scopeAction, .binding = GetXRPath("/user/hand/left/input/thumbstick/click") },
            XrActionSuggestedBinding{ .action = m_inGame_inventory_mapAction, .binding = GetXRPath("/user/hand/right/input/thumbstick/click") },

            XrActionSuggestedBinding{ .action = m_rumbleAction, .binding = GetXRPath("/user/hand/left/output/haptic") },
            XrActionSuggestedBinding{ .action = m_rumbleAction, .binding = GetXRPath("/user/hand/right/output/haptic") },

            // === menu suggestions ===
            XrActionSuggestedBinding{ .action = m_inMenuGripPoseAction, .binding = GetXRPath("/user/hand/left/input/grip/pose") },
            XrActionSuggestedBinding{ .action = m_inMenuGripPoseAction, .binding = GetXRPath("/user/hand/right/input/grip/pose") },
            XrActionSuggestedBinding{ .action = m_inMenuAimPoseAction, .binding = GetXRPath("/user/hand/left/input/aim/pose") },
            XrActionSuggestedBinding{ .action = m_inMenuAimPoseAction, .binding = GetXRPath("/user/hand/right/input/aim/pose") },
            XrActionSuggestedBinding{ .action = m_scrollAction, .binding = GetXRPath("/user/hand/right/input/thumbstick") },
            XrActionSuggestedBinding{ .action = m_navigateAction, .binding = GetXRPath("/user/hand/left/input/thumbstick") },
            XrActionSuggestedBinding{ .action = m_selectAction, .binding = GetXRPath("/user/hand/right/input/a/click") },
            XrActionSuggestedBinding{ .action = m_backAction, .binding = GetXRPath("/user/hand/right/input/b/click") },
            XrActionSuggestedBinding{ .action = m_sortAction, .binding = GetXRPath("/user/hand/left/input/y/click") },
            XrActionSuggestedBinding{ .action = m_holdAction, .binding = GetXRPath("/user/hand/left/input/x/click") },
            XrActionSuggestedBinding{ .action = m_leftGripAction, .binding = GetXRPath("/user/hand/left/input/squeeze/value") },
            XrActionSuggestedBinding{ .action = m_rightGripAction, .binding = GetXRPath("/user/hand/right/input/squeeze/value") },
            XrActionSuggestedBinding{ .action = m_leftTriggerAction, .binding = GetXRPath("/user/hand/left/input/trigger/value") },
            XrActionSuggestedBinding{ .action = m_rightTriggerAction, .binding = GetXRPath("/user/hand/right/input/trigger/value") },
            XrActionSuggestedBinding{ .action = m_inMenu_inventory_mapAction, .binding = GetXRPath("/user/hand/right/input/thumbstick/click") },
            XrActionSuggestedBinding{ .action = m_inMenu_modMenuAction, .binding = GetXRPath("/user/hand/left/input/x/click") },
        };
        XrInteractionProfileSuggestedBinding suggestedBindingsInfo = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
        suggestedBindingsInfo.interactionProfile = GetXRPath("/interaction_profiles/oculus/touch_controller");
        suggestedBindingsInfo.countSuggestedBindings = (uint32_t)suggestedBindings.size();
        suggestedBindingsInfo.suggestedBindings = suggestedBindings.data();
        checkXRResult(xrSuggestInteractionProfileBindings(m_instance, &suggestedBindingsInfo), "Failed to suggest Oculus Touch Controller Profile bindings!");
    }

    {
        std::array suggestedBindings = {
            // === gameplay suggestions ===
            XrActionSuggestedBinding{ .action = m_inGameGripPoseAction, .binding = GetXRPath("/user/hand/left/input/grip/pose") },
            XrActionSuggestedBinding{ .action = m_inGameGripPoseAction, .binding = GetXRPath("/user/hand/right/input/grip/pose") },
            XrActionSuggestedBinding{ .action = m_inGameAimPoseAction, .binding = GetXRPath("/user/hand/left/input/aim/pose") },
            XrActionSuggestedBinding{ .action = m_inGameAimPoseAction, .binding = GetXRPath("/user/hand/right/input/aim/pose") },
            XrActionSuggestedBinding{ .action = m_moveAction, .binding = GetXRPath("/user/hand/left/input/thumbstick") },
            XrActionSuggestedBinding{ .action = m_cameraAction, .binding = GetXRPath("/user/hand/right/input/thumbstick") },

            XrActionSuggestedBinding{ .action = m_grab_interactAction, .binding = GetXRPath("/user/hand/left/input/squeeze/force") },
            XrActionSuggestedBinding{ .action = m_grab_interactAction, .binding = GetXRPath("/user/hand/right/input/squeeze/force") },
            XrActionSuggestedBinding{ .action = m_jumpAction, .binding = GetXRPath("/user/hand/right/input/b/click") },
            XrActionSuggestedBinding{ .action = m_run_interactAction, .binding = GetXRPath("/user/hand/right/input/a/click") },
            XrActionSuggestedBinding{ .action = m_useRune_dpadMenu_Action, .binding = GetXRPath("/user/hand/left/input/b/click") },
            XrActionSuggestedBinding{ .action = m_inGame_modMenuAction, .binding = GetXRPath("/user/hand/left/input/a/click") },

            XrActionSuggestedBinding{ .action = m_useLeftItemAction, .binding = GetXRPath("/user/hand/left/input/trigger/value") },
            XrActionSuggestedBinding{ .action = m_useRightItemAction, .binding = GetXRPath("/user/hand/right/input/trigger/value") },

            XrActionSuggestedBinding{ .action = m_crouch_scopeAction, .binding = GetXRPath("/user/hand/left/input/thumbstick/click") },
            XrActionSuggestedBinding{ .action = m_inGame_inventory_mapAction, .binding = GetXRPath("/user/hand/right/input/thumbstick/click") },

            XrActionSuggestedBinding{ .action = m_rumbleAction, .binding = GetXRPath("/user/hand/left/output/haptic") },
            XrActionSuggestedBinding{ .action = m_rumbleAction, .binding = GetXRPath("/user/hand/right/output/haptic") },

            // === menu suggestions ===
            XrActionSuggestedBinding{ .action = m_inMenuGripPoseAction, .binding = GetXRPath("/user/hand/left/input/grip/pose") },
            XrActionSuggestedBinding{ .action = m_inMenuGripPoseAction, .binding = GetXRPath("/user/hand/right/input/grip/pose") },
            XrActionSuggestedBinding{ .action = m_inMenuAimPoseAction, .binding = GetXRPath("/user/hand/left/input/aim/pose") },
            XrActionSuggestedBinding{ .action = m_inMenuAimPoseAction, .binding = GetXRPath("/user/hand/right/input/aim/pose") },
            XrActionSuggestedBinding{ .action = m_scrollAction, .binding = GetXRPath("/user/hand/right/input/thumbstick") },
            XrActionSuggestedBinding{ .action = m_navigateAction, .binding = GetXRPath("/user/hand/left/input/thumbstick") },
            XrActionSuggestedBinding{ .action = m_selectAction, .binding = GetXRPath("/user/hand/right/input/a/click") },
            XrActionSuggestedBinding{ .action = m_backAction, .binding = GetXRPath("/user/hand/right/input/b/click") },
            XrActionSuggestedBinding{ .action = m_sortAction, .binding = GetXRPath("/user/hand/left/input/b/click") },
            XrActionSuggestedBinding{ .action = m_holdAction, .binding = GetXRPath("/user/hand/left/input/a/click") },
            XrActionSuggestedBinding{ .action = m_leftGripAction, .binding = GetXRPath("/user/hand/left/input/squeeze/force") },
            XrActionSuggestedBinding{ .action = m_rightGripAction, .binding = GetXRPath("/user/hand/right/input/squeeze/force") },
            XrActionSuggestedBinding{ .action = m_leftTriggerAction, .binding = GetXRPath("/user/hand/left/input/trigger/value") },
            XrActionSuggestedBinding{ .action = m_rightTriggerAction, .binding = GetXRPath("/user/hand/right/input/trigger/value") },
            XrActionSuggestedBinding{ .action = m_inMenu_inventory_mapAction, .binding = GetXRPath("/user/hand/right/input/thumbstick/click") },
            XrActionSuggestedBinding{ .action = m_inMenu_modMenuAction, .binding = GetXRPath("/user/hand/left/input/a/click") },
        };
        XrInteractionProfileSuggestedBinding suggestedBindingsInfo = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
        suggestedBindingsInfo.interactionProfile = GetXRPath("/interaction_profiles/valve/index_controller");
        suggestedBindingsInfo.countSuggestedBindings = (uint32_t)suggestedBindings.size();
        suggestedBindingsInfo.suggestedBindings = suggestedBindings.data();
        checkXRResult(xrSuggestInteractionProfileBindings(m_instance, &suggestedBindingsInfo), "Failed to suggest Valve Index Controller Profile bindings!");
    }

    XrSessionActionSetsAttachInfo attachInfo = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
    std::array actionSets = { m_gameplayActionSet, m_menuActionSet };
    attachInfo.countActionSets = (uint32_t)actionSets.size();
    attachInfo.actionSets = actionSets.data();
    checkXRResult(xrAttachSessionActionSets(m_session, &attachInfo), "Failed to attach action sets to session!");

    for (EyeSide side : { EyeSide::LEFT, EyeSide::RIGHT }) {
        XrActionSpaceCreateInfo createInfo = { XR_TYPE_ACTION_SPACE_CREATE_INFO };
        createInfo.action = m_inGameGripPoseAction;
        createInfo.subactionPath = m_handPaths[side];
        createInfo.poseInActionSpace = s_xrIdentityPose;
        checkXRResult(xrCreateActionSpace(m_session, &createInfo, &m_inGameHandSpaces[side]), "Failed to create action space for hand pose!");

        createInfo.action = m_inGameAimPoseAction;
        checkXRResult(xrCreateActionSpace(m_session, &createInfo, &m_inGameAimSpaces[side]), "Failed to create action space for aim pose!");
    }

    for (EyeSide side : { EyeSide::LEFT, EyeSide::RIGHT }) {
        XrActionSpaceCreateInfo createInfo = { XR_TYPE_ACTION_SPACE_CREATE_INFO };
        createInfo.action = m_inMenuGripPoseAction;
        createInfo.subactionPath = m_handPaths[side];
        createInfo.poseInActionSpace = s_xrIdentityPose;
        checkXRResult(xrCreateActionSpace(m_session, &createInfo, &m_inMenuHandSpaces[side]), "Failed to create action space for hand pose!");

        createInfo.action = m_inMenuAimPoseAction;
        checkXRResult(xrCreateActionSpace(m_session, &createInfo, &m_inMenuAimSpaces[side]), "Failed to create action space for menu aim pose!");
    }

    // initialize rumble manager
    m_rumbleManager = std::make_unique<RumbleManager>(m_session, m_rumbleAction);
    m_rumbleManager.get()->initializeXrPathsAndStartTime(m_instance);
}

void CheckButtonState(bool buttonPressed, ButtonState& buttonState) {
    buttonState.resetFrameFlags();
    
    constexpr std::chrono::milliseconds longPressThreshold{ 250 };
    const bool down = buttonPressed;
    const auto now = std::chrono::steady_clock::now();
    
    // Rising edge - button just pressed
    if (down && !buttonState.wasDownLastFrame) {
        buttonState.pressStartTime = now;
    }
    
    // Pressed state - check for long press threshold
    if (down) {
        auto pressDuration = now - buttonState.pressStartTime;
        
        if (pressDuration >= longPressThreshold) {
            buttonState.lastEvent = ButtonState::Event::LongPress;
            buttonState.longFired = true;
            if (!buttonState.longFired_stillPressed) {
                buttonState.longFired_stillPressed = true;
                buttonState.longFired_actedUpon = true;
            }
        }
    }
    else {
        buttonState.longFired_stillPressed = false;
        buttonState.longFired_actedUpon = false;
    }
    
    // Falling edge - button just released
    if (!down && buttonState.wasDownLastFrame) {
        // Only register short press if long press didn't fire
        if (!buttonState.longFired) {
            buttonState.lastEvent = ButtonState::Event::ShortPress;
        }
        else
            buttonState.longFired = false; // reset long press fired flag
    }
    
    // Store current state for next frame
    buttonState.wasDownLastFrame = down;
}

std::optional<OpenXR::InputState> OpenXR::UpdateActions(XrTime predictedFrameTime, glm::fquat controllerRotation, bool inMenu) {
    BetterVRProfiler::Scope profile(BetterVRProfiler::Section::XRUpdateActions);

    XrActiveActionSet activeActionSet = { (inMenu ? m_menuActionSet : m_gameplayActionSet), XR_NULL_PATH };

    XrActionsSyncInfo syncInfo = { XR_TYPE_ACTIONS_SYNC_INFO };
    syncInfo.countActiveActionSets = 1;
    syncInfo.activeActionSets = &activeActionSet;
    checkXRResult(xrSyncActions(m_session, &syncInfo), "Failed to sync actions!");

    InputState newState = m_input.load();
    newState.shared.in_game = !inMenu;
    newState.shared.inputTime = predictedFrameTime;

    for (EyeSide side : { EyeSide::LEFT, EyeSide::RIGHT }) {
        auto locatePose = [&](XrAction action, XrSpace handSpace, XrActionStatePose& poseState, XrSpaceLocation& outLocation, XrSpaceVelocity* outVelocity, const char* errorContext) {
            XrActionStateGetInfo getPoseInfo = { XR_TYPE_ACTION_STATE_GET_INFO };
            getPoseInfo.action = action;
            getPoseInfo.subactionPath = m_handPaths[side];
            poseState = { XR_TYPE_ACTION_STATE_POSE };
            checkXRResult(xrGetActionStatePose(m_session, &getPoseInfo, &poseState), errorContext);

            outLocation = { XR_TYPE_SPACE_LOCATION };
            if (!poseState.isActive) {
                if (outVelocity != nullptr) {
                    *outVelocity = { XR_TYPE_SPACE_VELOCITY };
                }
                return;
            }

            XrSpaceLocation spaceLocation = { XR_TYPE_SPACE_LOCATION };
            XrSpaceVelocity spaceVelocity = { XR_TYPE_SPACE_VELOCITY };
            if (outVelocity != nullptr) {
                spaceLocation.next = &spaceVelocity;
                outVelocity->linearVelocity = { 0.0f, 0.0f, 0.0f };
                outVelocity->angularVelocity = { 0.0f, 0.0f, 0.0f };
            }

            checkXRResult(xrLocateSpace(handSpace, m_stageSpace, predictedFrameTime, &spaceLocation), "Failed to get location from controllers!");
            if ((spaceLocation.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0 && (spaceLocation.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0) {
                outLocation = spaceLocation;

                if (outVelocity != nullptr && (spaceLocation.locationFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) != 0 && (spaceLocation.locationFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT) != 0) {
                    // rotate angular velocity to world space when it's using a buggy runtime
                    auto mode = GetSettings().AngularVelocityFixer_GetMode();
                    bool isUsingQuestRuntime = m_capabilities.isOculusLinkRuntime;
                    if ((mode == AngularVelocityFixerMode::AUTO && isUsingQuestRuntime) || mode == AngularVelocityFixerMode::FORCED_ON) {
                        glm::vec3 angularVelocity = ToGLM(spaceVelocity.angularVelocity);
                        glm::fquat fix_angle = glm::fquat(0.924, -0.383, 0, 0);
                        angularVelocity = (ToGLM(spaceLocation.pose.orientation) * (fix_angle * angularVelocity)); // TODO: Contact other modders for similar issues with angular velocity being not on the grip rotation (quest 2) + Tune the angular velocity based on manually calculated on rotation positions
                        spaceVelocity.angularVelocity = { angularVelocity.x, angularVelocity.y, angularVelocity.z };
                    }

                    *outVelocity = spaceVelocity;
                }
            }
        };

        locatePose(
            newState.shared.in_game ? m_inGameGripPoseAction : m_inMenuGripPoseAction,
            newState.shared.in_game ? m_inGameHandSpaces[side] : m_inMenuHandSpaces[side],
            newState.shared.pose[side],
            newState.shared.poseLocation[side],
            &newState.shared.poseVelocity[side],
            "Failed to get pose of controller!"
        );

        locatePose(
            newState.shared.in_game ? m_inGameAimPoseAction : m_inMenuAimPoseAction,
            newState.shared.in_game ? m_inGameAimSpaces[side] : m_inMenuAimSpaces[side],
            newState.shared.aimPose[side],
            newState.shared.aimPoseLocation[side],
            nullptr,
            "Failed to get aim pose of controller!"
        );
    }
    // update shared actions
    XrActionStateGetInfo getInventoryMapInfo = { XR_TYPE_ACTION_STATE_GET_INFO };
    getInventoryMapInfo.action = newState.shared.in_game ? m_inGame_inventory_mapAction : m_inMenu_inventory_mapAction;
    getInventoryMapInfo.subactionPath = XR_NULL_PATH;
    auto& inventory_mapAction = newState.shared.inventory_map;
    inventory_mapAction = { XR_TYPE_ACTION_STATE_BOOLEAN };
    checkXRResult(xrGetActionStateBoolean(m_session, &getInventoryMapInfo, &inventory_mapAction), "Failed to get inventory_help action value!");

    auto& inventory_mapButtonState = newState.shared.inventory_mapState;
    if (inventory_mapAction.isActive == XR_TRUE) {
        auto buttonPressed = inventory_mapAction.currentState == XR_TRUE;
        CheckButtonState(buttonPressed, inventory_mapButtonState); 
    }

    XrActionStateGetInfo getModMenuInfo = { XR_TYPE_ACTION_STATE_GET_INFO };
    getModMenuInfo.action = newState.shared.in_game ? m_inGame_modMenuAction : m_inMenu_modMenuAction;
    getModMenuInfo.subactionPath = XR_NULL_PATH;
    auto& modMenuAction = newState.shared.modMenu;
    modMenuAction = { XR_TYPE_ACTION_STATE_BOOLEAN };
    checkXRResult(xrGetActionStateBoolean(m_session, &getModMenuInfo, &modMenuAction), "Failed to get mod menu action value!");

    auto& modMenuButtonState = newState.shared.modMenuState;
    if (modMenuAction.isActive == XR_TRUE) {
        auto buttonPressed = modMenuAction.currentState == XR_TRUE;
        CheckButtonState(buttonPressed, modMenuButtonState);
    }

    // update in-menu or in-game actions
    if (inMenu) {
        XrActionStateGetInfo getScrollInfo = { XR_TYPE_ACTION_STATE_GET_INFO };
        getScrollInfo.action = m_scrollAction;
        newState.inMenu.scroll = { XR_TYPE_ACTION_STATE_VECTOR2F };
        checkXRResult(xrGetActionStateVector2f(m_session, &getScrollInfo, &newState.inMenu.scroll), "Failed to get navigate action value!");

        XrActionStateGetInfo getNavigationInfo = { XR_TYPE_ACTION_STATE_GET_INFO };
        getNavigationInfo.action = m_navigateAction;
        newState.inMenu.navigate = { XR_TYPE_ACTION_STATE_VECTOR2F };
        checkXRResult(xrGetActionStateVector2f(m_session, &getNavigationInfo, &newState.inMenu.navigate), "Failed to get select action value!");

        XrActionStateGetInfo getSelectInfo = { XR_TYPE_ACTION_STATE_GET_INFO };
        getSelectInfo.action = m_selectAction;
        newState.inMenu.select = { XR_TYPE_ACTION_STATE_BOOLEAN };
        checkXRResult(xrGetActionStateBoolean(m_session, &getSelectInfo, &newState.inMenu.select), "Failed to get select action value!");

        XrActionStateGetInfo getBackInfo = { XR_TYPE_ACTION_STATE_GET_INFO };
        getBackInfo.action = m_backAction;
        newState.inMenu.back = { XR_TYPE_ACTION_STATE_BOOLEAN };
        checkXRResult(xrGetActionStateBoolean(m_session, &getBackInfo, &newState.inMenu.back), "Failed to get back action value!");

        XrActionStateGetInfo getSortInfo = { XR_TYPE_ACTION_STATE_GET_INFO };
        getSortInfo.action = m_sortAction;
        newState.inMenu.sort = { XR_TYPE_ACTION_STATE_BOOLEAN };
        checkXRResult(xrGetActionStateBoolean(m_session, &getSortInfo, &newState.inMenu.sort), "Failed to get sort action value!");

        XrActionStateGetInfo getHoldInfo = { XR_TYPE_ACTION_STATE_GET_INFO };
        getHoldInfo.action = m_holdAction;
        newState.inMenu.hold = { XR_TYPE_ACTION_STATE_BOOLEAN };
        checkXRResult(xrGetActionStateBoolean(m_session, &getHoldInfo, &newState.inMenu.hold), "Failed to get hold action value!");

        auto& holdButtonState = newState.inMenu.holdState;
        if (newState.inMenu.hold.isActive == XR_TRUE) {
            auto buttonPressed = newState.inMenu.hold.currentState == XR_TRUE;
            CheckButtonState(buttonPressed, holdButtonState);
        }

        XrActionStateGetInfo getLeftGripInfo = { XR_TYPE_ACTION_STATE_GET_INFO };
        getLeftGripInfo.action = m_leftGripAction;
        newState.inMenu.leftGrip = { XR_TYPE_ACTION_STATE_BOOLEAN };
        checkXRResult(xrGetActionStateBoolean(m_session, &getLeftGripInfo, &newState.inMenu.leftGrip), "Failed to get left grip action value!");

        if (newState.inMenu.leftGrip.currentState == XR_TRUE) {
            newState.shared.lastPickupSide = OpenXR::EyeSide::LEFT;
        }

        XrActionStateGetInfo getRightGripInfo = { XR_TYPE_ACTION_STATE_GET_INFO };
        getRightGripInfo.action = m_rightGripAction;
        newState.inMenu.rightGrip = { XR_TYPE_ACTION_STATE_BOOLEAN };
        checkXRResult(xrGetActionStateBoolean(m_session, &getRightGripInfo, &newState.inMenu.rightGrip), "Failed to get right grip action value!");

        if (newState.inMenu.rightGrip.currentState == XR_TRUE) {
            newState.shared.lastPickupSide = OpenXR::EyeSide::RIGHT;
        }

        XrActionStateGetInfo getLeftTriggerInfo = { XR_TYPE_ACTION_STATE_GET_INFO };
        getLeftTriggerInfo.action = m_leftTriggerAction;
        getLeftTriggerInfo.subactionPath = XR_NULL_PATH;
        newState.inMenu.leftTrigger = { XR_TYPE_ACTION_STATE_BOOLEAN };
        checkXRResult(xrGetActionStateBoolean(m_session, &getLeftTriggerInfo, &newState.inMenu.leftTrigger), "Failed to get left trigger action value!");

        XrActionStateGetInfo getRightTriggerInfo = { XR_TYPE_ACTION_STATE_GET_INFO };
        getRightTriggerInfo.action = m_rightTriggerAction;
        getRightTriggerInfo.subactionPath = XR_NULL_PATH;
        newState.inMenu.rightTrigger = { XR_TYPE_ACTION_STATE_BOOLEAN };
        checkXRResult(xrGetActionStateBoolean(m_session, &getRightTriggerInfo, &newState.inMenu.rightTrigger), "Failed to get right trigger action value!");
    }
    else {
        for (EyeSide side : { EyeSide::LEFT, EyeSide::RIGHT }) {
            XrActionStateGetInfo getGrabInfo = { XR_TYPE_ACTION_STATE_GET_INFO };
            getGrabInfo.action = m_grab_interactAction;
            getGrabInfo.subactionPath = m_handPaths[side];
            newState.inGame.grab[side] = { XR_TYPE_ACTION_STATE_FLOAT };
            checkXRResult(xrGetActionStateFloat(m_session, &getGrabInfo, &newState.inGame.grab[side]), "Failed to get grab action value!");

            auto& buttonState = newState.inGame.grabState[side];
            if (newState.inGame.grab[side].isActive == XR_TRUE) {
                auto buttonPressed = newState.inGame.grab[side].currentState > 0.75f;
                CheckButtonState(buttonPressed, buttonState);
            }
        }

        XrActionStateGetInfo getCrouchScopeInfo = { XR_TYPE_ACTION_STATE_GET_INFO };
        getCrouchScopeInfo.action = m_crouch_scopeAction;
        getCrouchScopeInfo.subactionPath = XR_NULL_PATH;
        newState.inGame.crouch_scope = { XR_TYPE_ACTION_STATE_BOOLEAN };
        checkXRResult(xrGetActionStateBoolean(m_session, &getCrouchScopeInfo, &newState.inGame.crouch_scope), "Failed to get crouch and map action value!");

        auto& crouch_scopeButtonState = newState.inGame.crouch_scopeState;
        if (newState.inGame.crouch_scope.isActive == XR_TRUE) {
            auto buttonPressed = newState.inGame.crouch_scope.currentState == XR_TRUE;
            CheckButtonState(buttonPressed, crouch_scopeButtonState);
        }

        XrActionStateGetInfo getMoveInfo = { XR_TYPE_ACTION_STATE_GET_INFO };
        getMoveInfo.action = m_moveAction;
        newState.inGame.move = { XR_TYPE_ACTION_STATE_VECTOR2F };
        checkXRResult(xrGetActionStateVector2f(m_session, &getMoveInfo, &newState.inGame.move), "Failed to get move action value!");

        XrActionStateGetInfo getCameraInfo = { XR_TYPE_ACTION_STATE_GET_INFO };
        getCameraInfo.action = m_cameraAction;
        newState.inGame.camera = { XR_TYPE_ACTION_STATE_VECTOR2F };
        checkXRResult(xrGetActionStateVector2f(m_session, &getCameraInfo, &newState.inGame.camera), "Failed to get camera action value!");

        XrActionStateGetInfo getJumpInfo = { XR_TYPE_ACTION_STATE_GET_INFO };
        getJumpInfo.action = m_jumpAction;
        getJumpInfo.subactionPath = XR_NULL_PATH;
        newState.inGame.jump_cancel = { XR_TYPE_ACTION_STATE_BOOLEAN };
        checkXRResult(xrGetActionStateBoolean(m_session, &getJumpInfo, &newState.inGame.jump_cancel), "Failed to get jump action value!");

        XrActionStateGetInfo getRunInfo = { XR_TYPE_ACTION_STATE_GET_INFO };
        getRunInfo.action = m_run_interactAction;
        getRunInfo.subactionPath = XR_NULL_PATH;
        newState.inGame.run_interact = { XR_TYPE_ACTION_STATE_BOOLEAN };
        checkXRResult(xrGetActionStateBoolean(m_session, &getRunInfo, &newState.inGame.run_interact), "Failed to get run action value!");

        auto& runButtonState = newState.inGame.runState;
        if (newState.inGame.run_interact.isActive == XR_TRUE) {
            auto buttonPressed = newState.inGame.run_interact.currentState == XR_TRUE;
            CheckButtonState(buttonPressed, runButtonState);
        }

        XrActionStateGetInfo getUseRuneInfo = { XR_TYPE_ACTION_STATE_GET_INFO };
        getUseRuneInfo.action = m_useRune_dpadMenu_Action;
        getUseRuneInfo.subactionPath = XR_NULL_PATH;
        newState.inGame.useRune_dpadMenu = { XR_TYPE_ACTION_STATE_BOOLEAN };
        checkXRResult(xrGetActionStateBoolean(m_session, &getUseRuneInfo, &newState.inGame.useRune_dpadMenu), "Failed to get use rune action value!");

        auto& useRuneButtonState = newState.inGame.useRune_runeMenuState;
        if (newState.inGame.useRune_dpadMenu.isActive == XR_TRUE) {
            auto buttonPressed = newState.inGame.useRune_dpadMenu.currentState == XR_TRUE;
            CheckButtonState(buttonPressed, useRuneButtonState);
        }

        XrActionStateGetInfo getUseRightItemInfo = { XR_TYPE_ACTION_STATE_GET_INFO };
        getUseRightItemInfo.action = m_useRightItemAction;
        getUseRightItemInfo.subactionPath = XR_NULL_PATH;
        newState.inGame.useRightItem = { XR_TYPE_ACTION_STATE_BOOLEAN };
        checkXRResult(xrGetActionStateBoolean(m_session, &getUseRightItemInfo, &newState.inGame.useRightItem), "Failed to get useRightItem action value!");

        XrActionStateGetInfo getUseLeftItemInfo = { XR_TYPE_ACTION_STATE_GET_INFO };
        getUseLeftItemInfo.action = m_useLeftItemAction;
        getUseLeftItemInfo.subactionPath = XR_NULL_PATH;
        newState.inGame.useLeftItem = { XR_TYPE_ACTION_STATE_BOOLEAN };
        checkXRResult(xrGetActionStateBoolean(m_session, &getUseLeftItemInfo, &newState.inGame.useLeftItem), "Failed to get useLeftItem action value!");
    }
    this->m_input.store(newState);
    return newState;
}


std::optional<XrSpaceLocation> OpenXR::UpdateSpaces(XrTime predictedDisplayTime) {
    BetterVRProfiler::Scope profile(BetterVRProfiler::Section::XRUpdateSpaces);

    XrSpaceLocation spaceLocation = { XR_TYPE_SPACE_LOCATION };
    if (XrResult result = xrLocateSpace(m_headSpace, m_stageSpace, predictedDisplayTime, &spaceLocation); XR_SUCCEEDED(result)) {
        if (result != XR_ERROR_TIME_INVALID) {
            checkXRResult(result, "Failed to get space location!");
        }
        checkXRResult(result, "Failed to get space location!");
    }
    if ((spaceLocation.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) == 0)
        return std::nullopt;

    return spaceLocation;
}

void OpenXR::ProcessEvents() {
    auto processSessionStateChangedEvent = [this](XrEventDataSessionStateChanged* stateChangedEvent) {
        switch (stateChangedEvent->state) {
            case XR_SESSION_STATE_IDLE:
                Log::print<VERBOSE>("OpenXR has indicated that the session is idle!");
                break;
            case XR_SESSION_STATE_READY: {
                Log::print<INFO>("[BVR-trace] XR state -> READY (creating RND_Renderer + xrBeginSession)");
                if (m_renderer) {
                    Log::print<WARNING>("OpenXR has indicated that the session is ready, but we already have a renderer!");
                }
                else {
                    m_renderer = std::make_unique<RND_Renderer>(m_session);
                }
                break;
            }
            case XR_SESSION_STATE_SYNCHRONIZED:
                Log::print<INFO>("[BVR-trace] XR state -> SYNCHRONIZED");
                break;
            case XR_SESSION_STATE_FOCUSED:
                Log::print<INFO>("[BVR-trace] XR state -> FOCUSED");
                break;
            case XR_SESSION_STATE_VISIBLE:
                Log::print<INFO>("[BVR-trace] XR state -> VISIBLE");
                break;
            case XR_SESSION_STATE_STOPPING:
                Log::print<VERBOSE>("OpenXR has indicated that the session should be ended!");
                break;
            case XR_SESSION_STATE_EXITING:
                Log::print<VERBOSE>("OpenXR has indicated that the session should be destroyed!");
                // an exception is thrown here instead of using exit() to allow Cemu to ideally gracefully shutdown
                //throw std::runtime_error("BetterVR mod has been requested to exit by OpenXR!");
                //this->m_renderer.reset();
                PostMessage(CemuHooks::m_cemuTopWindow, WM_CLOSE, 0, 0);
                break;
            case XR_SESSION_STATE_LOSS_PENDING:
                Log::print<VERBOSE>("OpenXR has indicated that the session is going to be lost!");
                // todo: implement being able to continuously check if xrGetSystem returns and then reinitialize the session
                break;
            default:
                Log::print<VERBOSE>("OpenXR has indicated that an unknown session state has occurred!");
                break;
        }
    };

    XrEventDataBuffer eventData = { XR_TYPE_EVENT_DATA_BUFFER };
    XrResult result = xrPollEvent(m_instance, &eventData);

    while (result == XR_SUCCESS) {
        switch (eventData.type) {
            case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
                processSessionStateChangedEvent((XrEventDataSessionStateChanged*)&eventData);
                break;
            case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
                Log::print<WARNING>("OpenXR has indicated that the instance is going to be lost!");
                break;
            case XR_TYPE_EVENT_DATA_EVENTS_LOST:
                Log::print<WARNING>("OpenXR has indicated that events are being lost!");
                break;
            case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED:
                Log::print<WARNING>("OpenXR has indicated that the interaction profile has changed!");
                break;
            case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
                Log::print<WARNING>("OpenXR has indicated that the reference space has changed!");
                break;
            default:
                Log::print<WARNING>("OpenXR has indicated that an unknown event with type {} has occurred!", std::to_underlying(eventData.type));
                break;
        }

        eventData = { XR_TYPE_EVENT_DATA_BUFFER };
        result = xrPollEvent(m_instance, &eventData);
    }
}
