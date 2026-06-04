# BotW-BetterVR — Linux fork

> **This is a Linux port** of [Crementif/BotW-BetterVR](https://github.com/Crementif/BotW-BetterVR), a Vulkan-layer VR mod for [Cemu](http://cemu.info/) that plays The Legend of Zelda: Breath of the Wild in VR. Upstream is intentionally Windows-only (D3D12 + Win32 registry-based layer registration). This fork rewrites the renderer against Vulkan/OpenXR and ships a Linux-native Vulkan implicit layer.
>
> **The entire Linux port was written by [Claude](https://claude.com/claude-code) (Anthropic's Claude Code agent), over a multi-session experiment.** Architecture decisions, debugging, and code are documented in the session memory; the original C++ class structure is preserved 1:1 from upstream — only the rendering backend changes (`RND_D3D12` → `RND_VkComposer`, D3D12 → Vulkan, NT shared `HANDLE` → `VK_KHR_external_memory_fd`, etc.).
>
> Tested on Arch Linux + Hyprland (Wayland) + RTX 5070 Ti + Quest 2 streamed via [WiVRn](https://github.com/WiVRn/WiVRn).

## Status

**Working:**
- Full upstream architecture compiles and runs as a Vulkan implicit layer (`libVkLayer_BetterVR.so`, ~5.6 MB).
- Stereo 3D rendering at the headset's native per-eye resolution (Quest 2: 1832×2016/eye).
- Head-tracked first-person camera with async-timewarp pose pairing (smooth even at sub-headset framerates).
- 2D / HUD composition via `XrCompositionLayerQuad`, head-locked, alpha-keyed against BetterVR's magic clears.
- **Quest Touch controllers** (via upstream's `xrSyncActions` action sets) appear to drive BotW input — basic movement, look, and jump confirmed; full button/gesture coverage not exhaustively retested on Linux.
- Cemu-window mirror: captured 3D is blitted back into Cemu's swapchain at `vkQueuePresentKHR` so the desktop window shows what the headset sees — lets you iterate without putting the headset on (`BVR_CEMU_WINDOW=0` to disable).
- Magic-color framebuffer identification + per-eye ring-buffer slot capture (port of upstream `m_renderFrames[2]`).
- Cross-instance Vulkan stack: separate VkInstance/VkDevice for OpenXR side, shared GPU memory via `VK_KHR_external_memory_fd`.

> ⚠️ **Tested only very lightly** — the very beginning of the Great Plateau (post-Shrine-of-Resurrection escape, first few minutes of gameplay). Anything later, anything stress-inducing (combat, large open vistas, weather, shrines, menus beyond inventory), and any controller binding beyond basic movement/look/jump is unverified and may be broken.

**Needs work:**
- **Smoothness during gameplay.** FPS oscillates 30–80 in gameplay despite hardware comfortably underutilized (GPU ~70%, system load ~5/16). Bottleneck is per-frame sync: the cross-instance `SharedTexture::m_fenceCounter` protocol is fundamentally single-threaded; offloading composition to its own thread runs into `Double wait detected!` and `VK_ERROR_DEVICE_LOST`. A real fix needs a redesigned cross-instance sync protocol (separate counters per direction, or explicit values passed via `CommandContext`).
- **`DebugDrawPipeline::Render()`** is still a stub (returns early when `renderData.IsEmpty()`, which is the common case). Compiles, no debug visualization in VR.
- **Cemu-window HUD compositing.** The mirror blits 3D back to Cemu's swapchain via `vkCmdBlitImage` — can't chromakey-mask the magic-color HUD pixels. Needs a small shader pass to overlay the 2D capture with alpha.
- **ImGui in-VR menus** (upstream's `imgui_menus.cpp`) compile but the ImGui-Vulkan font texture path is flaky on Linux; gated behind `BVR_IMGUI=1`.

## Dependencies (Arch Linux)

```
sudo pacman -S --needed openxr vulkan-headers vulkan-icd-loader glm shaderc clang cmake ninja
```

Vendored, must be cloned manually into `dependencies/`:

```
git clone --depth 1 --branch v1.91.6 https://github.com/ocornut/imgui.git dependencies/imgui
git clone --depth 1 --branch v0.16  https://github.com/epezent/implot.git dependencies/implot
```

You also need a working **Linux build of Cemu** with two small modifications (so loaded Vulkan layers can `dlsym` Cemu's HLE exports): `DLLEXPORT` defined as `__attribute__((visibility("default")))` on `gameMeta_getTitleId` / `memory_getBase` / `osLib_registerHLEFunction`, and `-rdynamic` on the `CemuBin` link line. See `Cemu/src/CMakeLists.txt` in the parent project.

An OpenXR runtime — this fork is validated against **[WiVRn](https://github.com/WiVRn/WiVRn)** (flatpak: `io.github.wivrn.wivrn`). Make sure `~/.config/openxr/1/active_runtime.json` is symlinked to its `openxr_wivrn.json`. Other Linux-native runtimes (Monado) should work but aren't tested. **Proton's `wineopenxr.dll` does NOT work** — it expects SteamVR-specific registry values that no non-SteamVR Linux runtime populates (this fork's existence is largely because of that).

## Build

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=/usr/bin/clang++ -G Ninja
cmake --build build -j$(nproc)
# Produces build/lib/libVkLayer_BetterVR.so
```

Register the layer with the Vulkan loader by writing `~/.local/share/vulkan/implicit_layer.d/BetterVR_Layer.json`:

```json
{
    "file_format_version": "1.2.0",
    "layer": {
        "name": "VK_LAYER_BETTERVR_hook",
        "type": "GLOBAL",
        "library_path": "/absolute/path/to/build/lib/libVkLayer_BetterVR.so",
        "api_version": "1.3.250",
        "implementation_version": "1",
        "description": "BotW-BetterVR Linux Vulkan port",
        "functions": {
            "vkNegotiateLoaderLayerInterfaceVersion": "VRLayer_NegotiateLoaderLayerInterfaceVersion"
        },
        "enable_environment": { "ENABLE_BETTERVR_MOD": "1" },
        "disable_environment": { "DISABLE_BETTERVR_MOD": "1" }
    }
}
```

Sanity-check the layer loads cleanly with `VK_LOADER_DEBUG=layer,error ENABLE_BETTERVR_MOD=1 vkcube` — you should see `Insert instance layer "VK_LAYER_BETTERVR_hook"` and no rejection errors. If the loader silently skips the layer, you almost certainly have a symbol resolution issue — see `CLAUDE.md` for the canonical traps.

The graphic pack must be discoverable by Cemu. Symlink it into Cemu's graphic-pack directories:

```
ln -sf "$PWD/resources/BreathOfTheWild_BetterVR" ~/.local/share/Cemu/graphicPacks/BreathOfTheWild_BetterVR
ln -sf "$PWD/resources/BreathOfTheWild_BetterVR" /path/to/your/mlc01/graphicPacks/BreathOfTheWild_BetterVR
```

Make sure `<Entry filename="graphicPacks/BreathOfTheWild_BetterVR/rules.txt"/>` is in `~/.config/Cemu/settings.xml` (Cemu adds it automatically when you tick the pack in the Graphic Packs UI).

## Run

```
ENABLE_BETTERVR_MOD=1 VK_LOADER_LAYERS_ENABLE='VK_LAYER_BETTERVR_hook' /path/to/Cemu_release
```

Useful tuning env vars (defaults shown):

| Env var | Default | Effect |
|---|---|---|
| `BVR_CEMU_WINDOW` | 1 | Mirror captured 3D into Cemu's swapchain at present time |
| `BVR_SYMMETRIC_FOV` | 0 | Render per-eye with symmetric FOV (avoids cross-eye without true IPD camera offset) |
| `BVR_SWAP_EYES` | 0 | Swap left/right eye assignment |
| `BVR_FP_HEAD_HEIGHT` | 3.0 | First-person head anchor offset in BotW world units |
| `BVR_FP_HEAD_TRANSLATE` | 1 | Include headset translation in first-person camera |
| `BVR_HUD` | 0 | Enable in-eye HUD overlay (legacy path; HUD quad is the proper one) |
| `BVR_HUD_FROM_PRESENT` | 1 | Feed HUD quad from Cemu's present image (vs 2D capture) |
| `BVR_FORCE_HUE` | 0 | Show animated hue cycle instead of captured BotW image (proves OpenXR pipeline is alive) |
| `BVR_DISABLE_PROJ_HOOK` | 0 | Projection-rewrite hooks become strict no-ops |

Runtime log is written to `BetterVR_log.txt` in Cemu's CWD; settings in `BetterVR_settings.ini` alongside.

## Linux-specific Cemu settings

In `~/.config/Cemu/settings.xml`:
- Audio API must be `<api>3</api>` (Cubeb). The default `0` (DirectSound) segfaults Cemu on Linux during "Run title".
- `GX2DrawDoneSync=true` is **required** — `false` crashes within seconds (NULL deref in the layer's render path).
- FPS++ at 240FPS Limit is recommended (the rate-mismatch between BotW's framerate and the headset's compositor refresh is one of the dominant smoothness factors).

## Credit

Linux port was written by [Claude Code](https://claude.com/claude-code) (Claude Opus 4.7, 1M context window) across a multi-session experiment. The complete development history — every dead end, every architectural pivot, every "this didn't work and here's why" — is captured in conversation memory rather than git, because much of the value is in the rejected approaches.

Upstream BotW-BetterVR (Windows/D3D12) by [Crementif](https://github.com/Crementif) and contributors. All of the gameplay-side patches (`resources/BreathOfTheWild_BetterVR/*.asm`), the OpenXR session handling structure, the framebuffer-identification trick, the first-person camera math, the cross-API SharedTexture concept — all upstream. This port reimplements the rendering backend; it doesn't replace the idea.

---

## Upstream README follows

BetterVR is a VR mod/hook that adds a PC-VR mode for BotW using the Wii U emulator called Cemu.

It currently supports the following features:
* Fully stereo-rendered with 6DOF, with full roomscale support. No alternated eye rendering is used.
* Full hands and arms support. You can deck yourself out in all the fanciest clothes.
* Wield weapons, torches and bokoblin arms into combat.
* Gestures to equip and throw weapons.
* Use motion controls to interact with the world to solve puzzles or start fires.
* Large mod compatibility. BetterVR only modifies the code and no game data. Most other mods should be compatible.
* Optional third-person mode.

### Requirements

#### Supported VR headsets:

The app currently utilizes OpenXR, which is supported on all the major headsets (Valve Index, HTC Vive, Oculus Rift, Meta Quest,
Windows Mixed Reality etc.). However, controller bindings are currently only provided for Oculus Touch controllers.
While more integrated solutions are being found out, there's probably ways to setup OpenXR mappings through SteamVR or other applications.

#### Other Requirements:

* A gaming PC with a CPU that is good at single-threaded workloads (a recent Intel i5 or Ryzen 5 are recommended at least)! The GPU matters a bit, but the CPU is the bottleneck here.

* A legal copy of BotW for the Wii U.

* Windows OS. [It doesn't work under Linux (even with Wine/Proton) for now](https://github.com/Crementif/BotW-BetterVR/issues/18).

* A properly set up [Cemu](http://cemu.info/) emulator that's able to run at 60FPS or higher. See [this guide](https://cemu.cfw.guide/) for more info.
  * **Before reporting issues, make sure that you have a WORKING version of the game that can go in-game on your PC before you install this mod!**  

* A recent Cemu version. Only Cemu 2.6 is tested to work.

### Current Limitations & Known Issues

- There's a small chance that the screen stays black after exiting any menus, which requires restarting the game to continue.
- Bow Aiming is done via a crosshair on the VR headset. Bow support might be added at some point.
- Climbing ladders require jumping up the ladder to go up and you have to look at the ladder.
- You can get stuck behind ladders sometimes, especially when you stop moving at the very top of the ladder while climbing down. So keep moving at the start!
- Bombs, barrels etc. are thrown at a weird angle.
- It'll lack some comfort options for now, like a left-handed mode or snap turning. These will be added later.

### Mod Installation

> [!TIP]
> Meta/Oculus Link has terrible frame interpolation that will make game appear to run much worse while also making the grass and arms glitchy, even while using a cable.
> Its HIGHLY recommended to use [ALVR](https://github.com/alvr-org/ALVR) (free, both wired and wireless), [Virtual Desktop](https://www.meta.com/en-gb/experiences/virtual-desktop/2017050365004772/) (paid, wireless, most performant) or [Steam Link](https://www.meta.com/en-gb/experiences/steam-link/5841245619310585/) (free, wireless) instead for Meta Quest headsets.

1. Download the latest `BetterVR_Launcher.exe` release from the [Releases](https://github.com/Crementif/BotW-BetterVR/releases) page.

2. Move `BetterVR_Launcher.exe` into the same folder as `Cemu.exe`.

3. Modify the general Cemu settings first by launching the `Cemu.exe` and looking for the following things.
   - Cemu's window title says your Cemu is version 2.6 or newer.
   - Breath of the Wild is inside your game list, and lists update `V208` in the update column.
   - Go to `Debug` -> `Accurate Barriers (Vulkan)` and make sure it is disabled for better performance.
   - Go to `Options` -> `General Settings` -> `Graphics`. Make sure that the Renderer is set to Vulkan, that the correct GPU is selected, and VSync is off. Close the settings window.
   - Go to `Options` -> `Graphic Packs` and click the `Download Community Graphic Packs` button to make sure that your graphic packs are up to date.

4. Close Cemu, connect your VR headset to your PC, start Virtual Desktop, SteamVR and and make sure that it has its OpenXR runtime set properly:
    - For Virtual Desktop users: https://github.com/mbucchia/VirtualDesktop-OpenXR/wiki#download--installation
	- For SteamVR, ALVR and Meta Quest Link users: https://academy.vrex.no/knowledge-base/openxr/

5. Run BetterVR by using the `BetterVR_Launcher.exe`, then go to `Options` -> `Graphic packs` -> `The Legend of Zelda: Breath of the Wild` and make sure:
   - `BetterVR` is enabled.
   - `Mods` -> `FPS++` is enabled. The game will crash without it.

6. Besides those, there's some other recommended graphic pack settings to make your game run properly and look better in VR:
   - `Graphics`:  
	 - Set the VR Resolution Multiplier to change the resolution of the game when you're in VR. The mod is not very GPU intensive, so using a higher multiplier might not cost any performance.  
	 - Set the Anti-Aliasing to `Nvidia FXAA` or `None` (if you are using 2x resolution multiplier or higher).
   - `FPS++`: Set the FPS limit to at least 120 or 144. Your headset/runtime will still control the actual VR framerate.
   - `Enhancements`: Set anisotropic filtering to 16x, and optionally set a Clarity preset to make the colors less washed out.

7. [Optional] To avoid some heavy stutters when you first start the game, download the shader caches from https://chriztr.github.io/cemu_shader_and_pipeline_caches/ and follow the `How to install the caches` section on that page.

8. Double click the game in Cemu's game list while your VR headset is turned on and connected. If things went correctly, you should now be playing the game in VR!  

Use the in-game BetterVR menu to see the BetterVR settings and controls guide (hold the X or equivalent button on your left VR controller, or long press the Menu button on your Xbox controller).  
In the settings you can also enable an option to immediately start the game when you start the `BetterVR_Launcher.exe`. You can also add this executable to Virtual Desktop, SteamVR etc. as a non-official Steam game.


#### Troubleshooting

Here's some steps to help you debug issues when the game fails to launch:

1. Verify that you've got a working game copy. Use Cemu.exe and launch the game normally. If you can load the game normally, it should work.
2. Make sure that if you're using a laptop, both your integrated graphics card and your dedicated graphics card drivers are updated. Make sure that its also plugged into a wall outlet and that its NOT using a power saving setting.
3. Unfortunately, some older AMD GPUs might have issues. See contact options below to help us squash these.
4. Uninstall any Vulkan overlays or layers that might cause issues. Programs like Overwolf, RTSS and other things CAN cause issues, though certainly not guaranteed.
5. Go over all the installation steps one more time and make sure that you didn't diverge from any of the steps. If you're seeing a green screen, it means that the Vulkan layer isn't enabled. This is likely due to using OpenGL in Cemu's settings.
6. If Cemu shows a Vulkan error like -13, these issues are likely GPU driver related.

If none of these steps helped, check the BotW support channel on the [Flat2VR Discord server](https://discord.com/invite/flat2vr) (recommended) or make a GitHub issue. Me and other people will try to help you in our spare time.

### Controls

**You can now view the controls in-game now by opening the BetterVR menu by holding the X button on your left VR controller.**
With Valve Index Controllers you can use the A button.
For Xbox/Playstation etc. controllers, long press the Start button and use the mouse for changing settings.

You can find the controller image here (click to enlarge):  
<a href="https://raw.githubusercontent.com/Crementif/BotW-BetterVR/refs/heads/main/resources/controller_help.png">
<img src="https://raw.githubusercontent.com/Crementif/BotW-BetterVR/refs/heads/main/resources/controller_help.png" width="540">
</a>

---

### Technical Overview
#### Rendering an image to the VR headset
This mod ships with no game files, so you might ask how it works.

The game starts with the BetterVR Vulkan layer enabled. The Vulkan layer, which comes in the form of the .DLL file, is then able to intercept the Vulkan commands that Cemu submits so that we can get the final frame to render to the VR headset and draw the debugging tools.

A technical hurdle here was that due to OpenXR frameworks not being designed to be instantiated inside something that is intercepting Vulkan commands, this mod utilizes Vulkan <-> D3D12 interop to pipe the rendered output from Vulkan to a D3D12 application that's *just* used for rendering the captured image to the VR headset. That way the OpenXR framework is just interacting with root-level rendering handles, instead of what'd occur in the Vulkan hook.

Using an external DLL originally made a lot of sense when Cemu wasn't open-sourced (though it also makes it slightly less tied to a specific emulator or version of Cemu, and prevents a VR specific version of Cemu that'll quickly become outdated). In hindsight, it probably would've saved a lot of time spent trying to get the mod to work without using D3D12.

#### How to make it VR
However, while drawing the game's rendered output to the VR headset is one thing, getting a native game to render a 3D image is a whole other thing. For that, the mod has a bunch of PowerPC assembly patches (the Wii U has a PowerPC CPU) to modify the game's code. For example, an important patch is to make it so that the game renders two frames before updating all of the game's systems and objects that are on-screen. Then, among many other patches, you'll also find patches that change the camera or player model positions each frame, or trigger an attack.

Usually the assembly code will call into the C++ code if it wants to do complicated algebra to specify where the camera or Link's hands should be for example. And some assembly patches use a clearing instruction for the Wii U's GPU which, after being translated, will signal the Vulkan hook to send the almost-finished final game image to the D3D12 code where it can present it inside the VR headset.

Additionally, since combat is a large part of the original game, there's also a new swing and stab detection system that allows the player to cut trees and enemies down when they execute proper swings and stabbing motions. This prevents a situation where weapon hitboxes are abused to instantly stagger an enemy. There's plans for an even deeper integration, but as of today that's about it. This is fully optional since the mod still features an attack button, but the latter will offer a lot more immersion.

Understanding how the game works, finding and patching the exact parts inside the game's executable is by far the most difficult part and it took thousands of hours of reverse-engineering. Its without a doubt the most time consuming task of this VR mod, especially since this game uses a custom C++ engine of which is not much known about other then the good work of the (largely unfinished, but still very helpful) decompilation project.

If you want to know more about the technical details, feel free to ask in the BetterVR related channels in the [Flat2VR Discord server](https://discord.com/invite/flat2vr).
There's enough that was skipped over or left out in this explanation.


### Build Instructions (For Developers)

1. Install the latest Vulkan SDK from https://vulkan.lunarg.com/sdk/home#windows and make sure that VULKAN_SDK was added
   to your environment variables.

2. Install [vcpkg](https://github.com/microsoft/vcpkg) (make sure to run the bootstrap and install commands it mentions) and use the following command to install the required dependencies:
   `vcpkg install openxr-loader:x64-windows-static-md glm:x64-windows-static-md vulkan-headers:x64-windows-static-md imgui:x64-windows-static-md`

3. Change the CMakeUserPresets.json file to contain the directory where you've stored vcpkg. Its currently hardcoded.
   If you want to use [Meta XR Simulator](https://developers.meta.com/horizon/downloads/package/meta-xr-simulator-windows/) (which is quite helpful during debugging), you should change its path now too.
   **Meta XR Simulator doesn't work unless you edit the `[install folder]/config/sim_core_configuration.json` file from `    "disable_interop": false,` to `    "disable_interop": true,`.**

4. [Optional] Download and extract a new Cemu installation to the Cemu folder that's included.
   This step is technically not required, but it's the default install location and makes debugging much easier.

5. Use Visual Studio (Recommended) or Clion to open the CMake project. Make sure that it's compiling a x64 build.

6. For direct Visual Studio debugging, its recommended that instead of launching the BetterVR_Launcher, you set-up a launch.vs.json that launches Cemu.exe with the .dll loaded.
   Once the CMake project is open in VS (make sure its not ran as admin), go to `Debug`->`Debug and Launch Settings for BetterVR_Layer`.
   With that open, copy the configuration from [`launch.vs.json`](resources/launch.vs.json) and replace the placeholder paths.

7. If step 6 doesn't work, you can also just launch the BetterVR_Project by debugging the `BetterVR_Launcher` target (make sure to pick the Install option). You might have to manually attach or install a multi-process debugging extension.


### Credits
Crementif: Main Developer  
Acudofy: Sword & stab analysis system  
Holydh: Developed the input systems  
leoetlino: For the [BotW Decomp project](https://github.com/zeldaret/botw), which was very useful  
Exzap: Technical support and optimization help  
Mako Marci: Edited the trailer, made the logo and controller guide  
Tim, Mako Marci, Solarwolf07, Elliott Tate & Derra: Helped with testing, recording, feedback and support  

### Licenses

This project is licensed under the MIT license.
BetterVR also uses the following libraries:
 - [vkroots (MIT licensed)](https://github.com/Joshua-Ashton/vkroots/blob/main/LICENSES/MIT.txt)
 - [imgui (MIT licensed)](https://github.com/ocornut/imgui/blob/master/LICENSE.txt)
 - [ImPlot (MIT licensed)](https://github.com/epezent/implot/blob/master/LICENSE)
