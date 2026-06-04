#pragma once

#include "hooking/rumble.h"

class RND_VkComposer;
class RND_Renderer;

// ---- Atomic-wrapper compat ----
// `std::atomic<T>` on Linux libstdc++ rejects struct types whose members have
// in-class default initializers (the implicit default ctor is "non-trivial").
// Windows MSVC is more permissive. To keep the same call-site syntax
// (`.load()`, `.store()`, `= value`), wrap with a mutex-backed shim on Linux.
#if BETTERVR_HAS_WIN32
template <typename T>
using AtomicCompat = std::atomic<T>;
#else
template <typename T>
class AtomicCompat {
public:
    AtomicCompat() = default;
    AtomicCompat(T desired) : m_value(desired) {}
    AtomicCompat(const AtomicCompat&) = delete;
    AtomicCompat& operator=(const AtomicCompat&) = delete;

    T load(std::memory_order = std::memory_order_seq_cst) const {
        std::lock_guard<std::mutex> g(m_mutex);
        return m_value;
    }
    void store(T desired, std::memory_order = std::memory_order_seq_cst) {
        std::lock_guard<std::mutex> g(m_mutex);
        m_value = desired;
    }
    T exchange(T desired, std::memory_order = std::memory_order_seq_cst) {
        std::lock_guard<std::mutex> g(m_mutex);
        T old = m_value;
        m_value = desired;
        return old;
    }
    void operator=(T desired) { store(desired); }
    operator T() const { return load(); }
private:
    mutable std::mutex m_mutex;
    T m_value{};
};
#endif

class OpenXR {
    friend class RND_Renderer;
    friend class RND_VkComposer;

public:
    OpenXR();
    ~OpenXR();

    enum EyeSide : uint8_t {
        LEFT = 0,
        RIGHT = 1
    };

    struct Capabilities {
        // adapter: was an LUID on Windows so D3D12 could enumerate by it. On
        // Linux we use VkPhysicalDevice via xrGetVulkanGraphicsDevice2KHR — the
        // field is preserved as a stub so unchanged code compiles.
        LUID adapter;
        D3D_FEATURE_LEVEL minFeatureLevel;
        bool supportsOrientational;
        bool supportsPositional;
        bool supportsMutatableFOV;
        bool isOculusLinkRuntime;
        bool isMetaSimulator;
    } m_capabilities = {};

    struct InputState {
        struct ButtonState {
            enum class Event { None, ShortPress, LongPress };
            bool wasDownLastFrame = false;
            bool longFired = false;
            bool waitingForSecond = false;
            bool longFired_actedUpon = false;
            bool longFired_stillPressed = false;
            std::chrono::steady_clock::time_point pressStartTime;
            std::chrono::steady_clock::time_point lastReleaseTime;
            Event lastEvent = Event::None;
            void resetFrameFlags() { lastEvent = Event::None; }
            void resetButtonState() { wasDownLastFrame = false; longFired = false; waitingForSecond = false; }
        };

        struct Shared {
            bool in_game = true;
            XrTime inputTime;
            std::optional<EyeSide> lastPickupSide = std::nullopt;
            std::array<XrActionStatePose, 2> pose;
            std::array<XrSpaceLocation, 2> poseLocation;
            std::array<XrSpaceVelocity, 2> poseVelocity;
            std::array<XrActionStatePose, 2> aimPose;
            std::array<XrSpaceLocation, 2> aimPoseLocation;
            std::array<XrSpaceLocation, 2> hmdRelativePoseLocation;
            XrActionStateBoolean inventory_map;
            ButtonState inventory_mapState;
            XrActionStateBoolean modMenu;
            ButtonState modMenuState;
        } shared;

        struct InGame {
            XrActionStateBoolean crouch_scope;
            ButtonState crouch_scopeState;
            XrActionStateVector2f move;
            XrActionStateVector2f camera;
            std::array<XrActionStateFloat, 2> grab;
            XrActionStateBoolean jump_cancel;
            XrActionStateBoolean run_interact;
            ButtonState runState;
            XrActionStateBoolean useRune_dpadMenu;
            ButtonState useRune_runeMenuState;
            XrActionStateBoolean useLeftItem;
            XrActionStateBoolean useRightItem;
            std::array<bool, 2> drop_weapon;
            std::array<ButtonState, 2> grabState;
        } inGame;
        struct InMenu {
            XrActionStateVector2f scroll;
            XrActionStateVector2f navigate;
            XrActionStateBoolean select;
            XrActionStateBoolean back;
            XrActionStateBoolean sort;
            XrActionStateBoolean hold;
            ButtonState holdState;
            XrActionStateBoolean leftGrip;
            XrActionStateBoolean rightGrip;
            XrActionStateBoolean leftTrigger;
            XrActionStateBoolean rightTrigger;
        } inMenu;
    };
    std::atomic<InputState> m_input = InputState{};
    std::atomic<glm::fquat> m_inputCameraRotation = glm::identity<glm::fquat>();

    struct GameState {
        uint32_t previous_button_hold;
        bool in_game = false;
        bool was_in_game = false;
        bool map_open = false;
        bool dpad_menu_open_requested = false;
        bool was_dpad_menu_open = false;
        EquipType last_dpad_menu_open = EquipType::None;
        bool prevent_inputs = false;
        std::chrono::steady_clock::time_point prevent_inputs_time;
        bool prevent_grab_inputs = false;
        std::chrono::steady_clock::time_point prevent_grab_time;
        bool right_hand_was_over_left_shoulder_slot = false;
        bool right_hand_was_over_right_shoulder_slot = false;
        bool right_hand_was_over_left_waist_slot = false;
        bool left_hand_was_over_left_shoulder_slot = false;
        bool left_hand_was_over_right_shoulder_slot = false;
        bool left_hand_was_over_left_waist_slot = false;
        EquipType right_hand_current_equip_type = EquipType::None;
        EquipType left_hand_current_equip_type = EquipType::None;
        EquipType right_hand_previous_frame_equip_type = EquipType::None;
        EquipType left_hand_previous_frame_equip_type = EquipType::None;
        EquipType last_equip_type_held = EquipType::None;
        bool dpad_menu_selection_already_equipped = false;
        bool rune_need_reequip = false;
        float rune_reequip_timer = 0.0f;
        int right_hand_equip_type_change_requested_over_frames = 0;
        int left_hand_equip_type_change_requested_over_frames = 0;
        bool has_something_in_left_hand = false;
        bool has_something_in_right_hand = false;
        bool is_throwable_object_held = false;
        bool is_locking_on_target = false;
        bool is_shield_guarding = false;
        bool is_riding_mount = false;
        bool is_climbing = false;
        bool is_paragliding = false;
        float previous_left_hand_velocity = 0.0f;
        glm::fvec3 stored_left_hand_position = glm::fvec3(0.0f, 0.0f, 0.0f);
        bool left_hand_position_stored = false;
        glm::fvec3 stored_right_hand_position = glm::fvec3(0.0f, 0.0f, 0.0f);
        bool right_hand_position_stored = false;
        int magnesis_forward_frames_interval = 0;
        bool trigger_pressed_over_body_slot = false;
    };
    AtomicCompat<GameState> m_gameState{};
    std::atomic_bool m_isMenuOpen = false;
    std::atomic_uint8_t m_currMenuTab = 0;
    std::atomic_bool m_forceTabChange = false;

    struct RumbleParameters {
        bool leftHand = false;
        double duration = 0;
        float frequency = 0.0f;
        float amplitude = 0.0f;
    } rumbleParameters;
    AtomicCompat<RumbleParameters> m_rumbleParameters{};

    // Linux: Vulkan graphics binding. On Windows the same method takes a D3D12
    // binding — keep the signature divergence #ifdef'd if cross-compile ever
    // matters. For now this header is Linux-only.
    void CreateSession(const XrGraphicsBindingVulkan2KHR& vkBinding);
    void CreateActions();
    std::array<XrViewConfigurationView, 2> GetViewConfigurations();
    std::optional<XrSpaceLocation> UpdateSpaces(XrTime predictedDisplayTime);
    std::optional<InputState> UpdateActions(XrTime predictedFrameTime, glm::fquat controllerRotation, bool inMenu);

    void ProcessEvents();

    XrSession GetSession() const { return m_session; }
    RND_Renderer* GetRenderer() const { return m_renderer.get(); }
    RumbleManager* GetRumbleManager() const { return m_rumbleManager.get(); }

private:
    XrPath GetXRPath(const char* str) const {
        XrPath path;
        checkXRResult(xrStringToPath(m_instance, str, &path), std::format("Failed to get path for {}", str).c_str());
        return path;
    }

    XrInstance m_instance = XR_NULL_HANDLE;
    XrSystemId m_systemId = XR_NULL_SYSTEM_ID;
    XrSession m_session = XR_NULL_HANDLE;
    XrSpace m_stageSpace = XR_NULL_HANDLE;
    XrSpace m_headSpace = XR_NULL_HANDLE;
    std::array<XrSpace, 2> m_inGameHandSpaces = { XR_NULL_HANDLE, XR_NULL_HANDLE };
    std::array<XrSpace, 2> m_inGameAimSpaces = { XR_NULL_HANDLE, XR_NULL_HANDLE };
    std::array<XrSpace, 2> m_inMenuHandSpaces = { XR_NULL_HANDLE, XR_NULL_HANDLE };
    std::array<XrSpace, 2> m_inMenuAimSpaces = { XR_NULL_HANDLE, XR_NULL_HANDLE };
    std::array<XrPath, 2> m_handPaths = { XR_NULL_PATH, XR_NULL_PATH };

    XrAction m_inGameGripPoseAction = XR_NULL_HANDLE;
    XrAction m_inGameAimPoseAction = XR_NULL_HANDLE;
    XrAction m_inMenuGripPoseAction = XR_NULL_HANDLE;
    XrAction m_inMenuAimPoseAction = XR_NULL_HANDLE;

    XrActionSet m_gameplayActionSet = XR_NULL_HANDLE;
    XrAction m_moveAction = XR_NULL_HANDLE;
    XrAction m_cameraAction = XR_NULL_HANDLE;
    XrAction m_grab_interactAction = XR_NULL_HANDLE;
    XrAction m_jumpAction = XR_NULL_HANDLE;
    XrAction m_run_interactAction = XR_NULL_HANDLE;
    XrAction m_useRune_dpadMenu_Action = XR_NULL_HANDLE;
    XrAction m_inGame_modMenuAction = XR_NULL_HANDLE;
    XrAction m_useLeftItemAction = XR_NULL_HANDLE;
    XrAction m_useRightItemAction = XR_NULL_HANDLE;
    XrAction m_crouch_scopeAction = XR_NULL_HANDLE;
    XrAction m_inGame_inventory_mapAction = XR_NULL_HANDLE;
    XrAction m_rumbleAction = XR_NULL_HANDLE;

    XrActionSet m_menuActionSet = XR_NULL_HANDLE;
    XrAction m_scrollAction = XR_NULL_HANDLE;
    XrAction m_navigateAction = XR_NULL_HANDLE;
    XrAction m_selectAction = XR_NULL_HANDLE;
    XrAction m_backAction = XR_NULL_HANDLE;
    XrAction m_sortAction = XR_NULL_HANDLE;
    XrAction m_holdAction = XR_NULL_HANDLE;
    XrAction m_leftGripAction = XR_NULL_HANDLE;
    XrAction m_rightGripAction = XR_NULL_HANDLE;
    XrAction m_leftTriggerAction = XR_NULL_HANDLE;
    XrAction m_rightTriggerAction = XR_NULL_HANDLE;
    XrAction m_inMenu_modMenuAction = XR_NULL_HANDLE;
    XrAction m_inMenu_inventory_mapAction = XR_NULL_HANDLE;

    std::unique_ptr<RND_Renderer> m_renderer;
    std::unique_ptr<RumbleManager> m_rumbleManager;

    constexpr static XrPosef s_xrIdentityPose = { .orientation = { .x = 0, .y = 0, .z = 0, .w = 1 }, .position = { .x = 0, .y = 0, .z = 0 } };

    XrDebugUtilsMessengerEXT m_debugMessengerHandle = XR_NULL_HANDLE;

    // Vulkan2 graphics requirement query function (loaded from XrInstance).
    PFN_xrGetVulkanGraphicsRequirements2KHR func_xrGetVulkanGraphicsRequirements2KHR = nullptr;
    PFN_xrCreateDebugUtilsMessengerEXT func_xrCreateDebugUtilsMessengerEXT = nullptr;
    PFN_xrDestroyDebugUtilsMessengerEXT func_xrDestroyDebugUtilsMessengerEXT = nullptr;
};
using ButtonState = OpenXR::InputState::ButtonState;
using EyeSide = OpenXR::EyeSide;

template <>
struct std::formatter<EyeSide> : std::formatter<string> {
    auto format(const EyeSide side, std::format_context& ctx) const {
        return std::format_to(ctx.out(), "{}", side == EyeSide::LEFT ? "LEFT" : "RIGHT");
    }
};
