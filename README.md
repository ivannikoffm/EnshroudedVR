# EnshroudedVR — play Enshrouded in real VR

**Version 1.1.0** · free, open source (MIT) · by **ivannikoffm**

EnshroudedVR is an unofficial mod that runs Enshrouded on a PC VR headset with **true stereoscopic 3D**, a
**head-tracked third-person camera** and the **full field of view of your headset**. You play with **keyboard and mouse**,
much like a UEVR "flat-to-VR" mod, with a natively rendered image instead of a 3D screen in front of you.

- Real stereo: each eye gets its own camera position. It is not a depth-map effect or a fake 3D screen.
- Head tracking turns the view around your character. The mouse still steers the game camera as usual.
- Uses the full headset FOV, with the image fitted to each eye's lens. Asymmetric optics such as Quest 3 are handled correctly.
- Optional 6DoF: lean in with your head.
- A crisp head-locked **HUD panel** (55° for combat, 70° for exploring). Menus, inventory and map appear on a flat virtual screen with a working cursor.
- An in-headset text message for every hotkey, a settings app with live apply, and rebindable keys.
- No game files are changed. The mod works in memory only, and you remove it by deleting one file.

> Not affiliated with or endorsed by Keen Games. Use at your own risk. The mod only changes what is rendered on your PC and
> gives no gameplay advantage. Tested in single-player; multiplayer sessions have not been tested yet.

---

## Contents
1. [Tested hardware](#1-tested-hardware)
2. [Requirements](#2-requirements)
3. [Installation](#3-installation)
4. [Recommended setup (important for image quality)](#4-recommended-setup)
5. [Headset-specific setup: Quest / Pico / SteamVR](#5-headset-specific-setup)
6. [Controls](#6-controls)
7. [Settings app](#7-settings-app-enshroudedvr-configexe)
8. [vr.ini reference — every setting explained](#8-vrini-reference)
9. [Comfort guide: depth, world scale, HUD](#9-comfort-guide)
10. [Troubleshooting](#10-troubleshooting)
11. [Known limitations](#11-known-limitations)
12. [Game updates](#12-game-updates)
13. [Uninstall](#13-uninstall)
14. [How it works](#14-how-it-works)
15. [Building from source](#15-building-from-source)
16. [Credits & license](#16-credits--license)

---

## 1. Tested hardware

| Headset | Connection / OpenXR runtime | Status | Notes |
|---|---|---|---|
| **Meta Quest 3** | Meta Horizon Link, **Air Link** (Wi-Fi) — Meta OpenXR runtime | ✅ Fully tested | 72 Hz, Link bitrate 150 Mbps, square frame 2160×2160 |
| **Pico Neo 3** | **Virtual Desktop** (VDXR runtime) | ✅ Fully tested | 72 Hz, square frame 2160×2160 |
| Quest 2 / Quest Pro / Quest 3S | Meta Link / Air Link / Virtual Desktop | ⚪ Expected to work | Same runtime path as Quest 3, not tested yet |
| Pico 4 / Pico 4 Ultra | Virtual Desktop / PICO Connect | ⚪ Expected to work | PICO Connect must be selected explicitly (see §8 `xr_runtime`) |
| Valve Index, HTC Vive, WMR, others | SteamVR OpenXR | 🟡 Untested | Runtime support is implemented; reports welcome |

**Test PC:** RTX 5090 32 GB, Intel i9-14900KF, 64 GB RAM, Windows 11 24H2, monitor 1920×1080.
Result: steady **72 fps** at 2160×2160, graphics preset Ultra with DLSS Quality.

**Game build tested:** Enshrouded 1.0 (Steam build 25988036, October 2026). After a game update the mod first tries to
re-find its hooks automatically (see §12).

Tried another headset? Please report it, with `enshrouded_vr\vrlog.txt` attached.

---

## 2. Requirements

- **Enshrouded** on Steam, version 1.0 or newer. Windows 10 or 11, 64-bit.
- A PC VR headset and an **OpenXR runtime**: Meta Horizon Link (Quest), Virtual Desktop, SteamVR, or PICO Connect.
- **A strong GPU.** The game renders the left and right eye on alternating frames, so it needs **at least as many fps as the
  headset's refresh rate** (72 fps for 72 Hz). Choose 72 Hz on your headset. DLSS Quality helps a lot.
  Roughly: RTX 3080 / 4070 Ti class at the 1620×1620 frame, RTX 4080 / 5080+ at 2160×2160.
- An NVIDIA GPU is the tested configuration. AMD/Intel should work (it is plain Vulkan), but are untested.
- Keyboard and mouse. A numpad is handy; every key can be rebound.

---

## 3. Installation

1. Download `EnshroudedVR_v1.1.0.zip`.
2. Open the game folder: Steam → right-click Enshrouded → *Manage* → *Browse local files*.
   Usually it is `...\steamapps\common\Enshrouded\`, the folder that contains `enshrouded.exe`.
3. Copy **`dinput8.dll`** and the **`enshrouded_vr`** folder into that folder. The result should be:
   ```
   Enshrouded\
   ├─ enshrouded.exe
   ├─ dinput8.dll            ← the mod
   └─ enshrouded_vr\
      ├─ EnshroudedVR-Config.exe   ← settings app
      ├─ openxr_loader.dll
      └─ vr.ini                    ← settings (created/edited by the app)
   ```
4. In the game, set **Settings → Graphics → Display Mode = Windowed** (strongly recommended, see §4).
5. Start your VR link first (Quest Link / Air Link / Virtual Desktop / SteamVR), then start Enshrouded from Steam
   **on the PC** (on Quest, start it from the desktop or from the Link "Desktop" view).
6. Once you are in the world, look straight ahead and press **Home** to recenter.

The first time, you can open `enshrouded_vr\EnshroudedVR-Config.exe` and choose a preset. The app can stay open while you
play.

**Compatible with:** Enshrouded Mod Loader (EML, `dbghelp.dll`) and most other mods.
**Conflicts with:** other mods that also use `dinput8.dll`, and other VR injectors (vorpX, UEVR-style injectors).

---

## 4. Recommended setup

These settings make the biggest difference to sharpness and smoothness.

### 4.1 Game graphics settings (in-game: Settings → Graphics)

| Setting | Recommended | Why |
|---|---|---|
| **Display Mode** | **Windowed** | Lets the mod render a **square 2160×2160 frame** (`force_window`). Headsets see an almost square field of view, so a 16:9 frame wastes more than half of its pixels. The square frame gives about **+33% sharpness for the same GPU cost**. |
| Anti-aliasing / upscaler | **DLSS Quality** (or DLAA if you have headroom) | Keeps ≥72 fps. A missed frame is visible as a stutter in VR. |
| Automatic render scale | **Off** | Otherwise the game quietly lowers resolution (blur). |
| Render scale | 100% | |
| V-Sync | Off | The headset paces the game. |
| Preset | as high as your GPU can hold at 72 fps | |

Notes:
- In Windowed mode the game window becomes **larger than your monitor** and centered: on the monitor you only see the middle of
  the image. That is expected, because the headset shows the whole frame.
- In Fullscreen mode the mod leaves the window alone. VR still works, but you get the game's 16:9 resolution and a softer image.
- Since game version 1.0 the game stores its graphics settings in
  `%USERPROFILE%\Saved Games\keen games\Enshrouded\local_settings.json`, **not** in `enshrouded_local.json` next to the exe
  (that file is no longer read). Change settings in the game menu or in that file, with the game closed.

### 4.2 Headset refresh rate
Use **72 Hz** unless your PC holds 80/90 fps or more in the game. In-game fps must be equal to or higher than the headset rate.

---

## 5. Headset-specific setup

### Meta Quest 2 / 3 / 3S / Pro — Meta Horizon Link (cable or Air Link)
- The mod finds the **Meta OpenXR runtime** automatically, even if another runtime (Virtual Desktop, SteamVR) is set
  as the system default (`xr_runtime=auto`).
- Meta Horizon app → *Devices* → your headset → *Graphics preferences*: **72 Hz**.
- **Air Link bitrate:** open `C:\Program Files\Oculus\Support\oculus-diagnostics\OculusDebugTool.exe` and set
  **Encode Bitrate (Mbps)**:
  - Air Link on Wi-Fi 5/6: **150** gave the best balance in testing. 200 is sharper, but caused stream glitches on Wi-Fi 5.
  - USB 3 cable (Link): 300–500.
  - The default (~100 Mbps, dynamic) is noticeably blurry, especially the HUD text.
- **Asynchronous Spacewarp:** tested with *Disabled* (Debug Tool). ASW synthesises frames and can add artifacts with alternating-eye rendering.
- Quest 3 has angled (asymmetric) lenses. The mod fits the image to each eye automatically (`layer_eyefov=1`). Never turn
  that off on a Quest: if you do, the world will not fuse into one image.

### Virtual Desktop (Quest, Pico, other standalone headsets)
- In the VD Streamer app → *OpenXR runtime*: **VDXR** (default). The mod detects it automatically.
- Frame rate 72 Hz. Quality *High/Ultra* or *Godlike* to taste. Codec H.265 or AV1.
- Do not run other OpenXR test tools against VD right before starting the game. If the headset stays black or grey, restart
  the VD Streamer.

### Pico headsets via PICO Connect
- The PICO runtime reports a headset even when none is connected, so `auto` will **not** pick it unless it is your
  system's active OpenXR runtime. Select it explicitly: settings app → *Display & Headset* → *OpenXR runtime* → *PICO Connect*
  (or `xr_runtime=pico`).

### SteamVR (Index, Vive, WMR, others)
- Select SteamVR explicitly (`xr_runtime=steamvr`) or make it the active OpenXR runtime in SteamVR → Settings → OpenXR.
- `auto` does not probe SteamVR, because probing would launch SteamVR every time you start the game.

---

## 6. Controls

The game is played as usual with keyboard and mouse. Mouse = game camera, head = look around.
Every mod hotkey shows a short message inside the headset and beeps. Hotkeys work only while the game window is focused.
All keys can be rebound in the settings app (*Keys* tab) or in `vr.ini` → `[keys]`.

| Default key | Action |
|---|---|
| **Home** | Recenter (look straight ahead and press) |
| **]** / **[** (also **Num+** / **Num−**) | Stereo strength (IPD) +10% / −10% |
| **Num0** | Stereo strength reset to 100% |
| **Num4** / **Num6** | Camera closer / farther (third-person distance, −/+10%) |
| **Num3** | Camera distance reset |
| **NumDel** (Num .) | HUD: in the game image → panel 55° (combat) → panel 70° (exploring) |
| **Num\*** | Headset full field of view on/off (off = the game's own FOV) |
| **Num/** | FOV mode FILL ↔ FIT |
| **Num5** | Head look on/off (off = a fixed stereo screen, mouse only) |
| **Num8** | 6DoF head position on/off (lean in / out) |
| **Insert** | Head tracking on/off |
| **ScrollLock** | Stereo → swapped eyes → mono |
| *(unbound)* | Advanced: pose lag, eye pair lock, yaw/pitch sign, roll mode, pair rate |

The mod has no keys with **Ctrl**, because Ctrl is *sneak* in the game. The game's own **Z** (first-person view, used when
building) works normally.

---

## 7. Settings app (`EnshroudedVR-Config.exe`)

Located in `enshrouded_vr\`. Every change is written to `vr.ini`, and the running game applies it **within a second**.
Hotkey changes made in the game show up in the app too.

- **Comfort:** stereo strength (IPD), camera distance, HUD mode and distance, headset FOV on/off, FILL/FIT, head look,
  6DoF, HUD from one eye, stereo/swap/mono.
- **Display & Headset:** game frame size (square 2160 / 1620 / 2560, 4:3, off), fit the image to each eye's lens, OpenXR runtime
  selection, rendered FOV %.
- **Keys:** rebind every action (double-click → press the key). *Unbind*, *Default keys*.
- **Help:** a short version of this README.
- Bottom: presets (*Defaults*, *Deep stereo*, *Combat*, *Exploring*), *Open log*, *Open folder*, and **Disable VR**, which lets you play flat
  without uninstalling. It creates `enshrouded_vr\xr_off.txt`.

---

## 8. vr.ini reference

`enshrouded_vr\vr.ini`, section `[vr]`. Most values apply **live**. Values marked ⟳ apply on the next game start.
Values marked *advanced* are tuned for this game: leave them unless you are troubleshooting.

| Key | Default | Values | Meaning |
|---|---|---|---|
| `xr_runtime` ⟳ | `auto` | `auto`, `meta`, `vd`, `steamvr`, `pico`, `active`, or a full path to a runtime `.json` | Which OpenXR runtime to use. `auto` = the system's active runtime, then Meta, then Virtual Desktop, then others, picking the first one that has a headset connected. PICO and SteamVR are used only if active or chosen explicitly. Env override: `EVR_XR_RUNTIME`. |
| `force_window` ⟳ | `2160x2160` | `WxH` (640–4096) or `0` | Game frame size forced by the mod. Only works when the game is in **Windowed** mode. Square suits all tested headsets. `1620x1620` for weaker GPUs, `2496x1872` (4:3) if you prefer wider menus, `0` = off. |
| `layer_eyefov` | `1` | `1` / `0` | Fit the image to each eye's lens FOV. **Required for Quest 3** and other angled-lens headsets. Harmless on others. |
| `stereo` | `1` | `1` stereo, `-1` swapped eyes, `0` mono | Stereo mode (ScrollLock). |
| `ipd_pct` | `100` | 10–400 | Stereo strength: eye separation as % of your real IPD. Higher = stronger 3D but a smaller-looking world. See §9. |
| `cam_dist_pct` | `100` | 20–150 | Third-person camera distance as % of the game's own distance (VR image only). Closer gives more depth at normal world scale. |
| `head_look` | `1` | `1` / `0` | Head rotation turns the view. `0` = fixed stereo screen, mouse only. |
| `head_pos` | `0` | `1` / `0` | 6DoF: head position moves the eyes (lean in). |
| `head_tracking` | `1` | `1` / `0` | Master switch for head tracking (Insert). |
| `fov_override` | `1` | `1` / `0` | Render the headset's full FOV instead of the game's FOV slider (Num\*). |
| `fov_axis` | `0` | `0` FILL / `1` FIT | FILL = the frame covers the headset's full height (recommended with the square frame). FIT = full width (bands top/bottom). |
| `fov_pct` | `100` | 60–100 | Rendered FOV as % of the headset FOV. Lower = sharper centre and slightly faster, black edges. |
| `hud_mode` | `1` | `0` in image, `1` panel 55°, `2` panel 70° | HUD display (NumDel). The panel is head-locked at `hud_dist_cm`. |
| `hud_dist_cm` | `150` | 50–500 | HUD panel distance. |
| `hud_sync` | `1` | `1` / `0` | Update the HUD from one eye only. Stops doubled world prompts ("E — Pick up"). |
| `pair_lock` | `1` | *advanced* | Show only complete left+right pairs rendered with the same head pose. Prevents double images when you turn your head. |
| `pair_rate` | `0` | *advanced* | Alternative pacing for games running at ≥2× the headset rate. Keep 0. |
| `pose_lag` | `2` | *advanced*, 0–3 | Frames between the head pose and the image it is applied to. |
| `head_mode` | `1` | *advanced* | Third-person head rotation applied to the final view (not to the camera orbit). |
| `yaw_sign`, `pitch_sign` | `1` | *advanced* | Axis directions. |
| `roll_sign` | `0` | *advanced* | `0` = horizon stays level, and the headset compensates for head roll (most comfortable). |
| `fov_mode`, `fov_off`, `mat_proj`, `pose_log` | — | *advanced/diagnostic* | Internal. Do not change. |

Section `[keys]`: `action=Key`, using names like `Home`, `Insert`, `Num0`–`Num9`, `Num+`, `Num-`, `Num*`, `Num/`, `NumDel`,
`F1`–`F12`, `A`–`Z`, `0`–`9`, `[`, `]`, `PageUp`, `ScrollLock`… `None` = unbound.

---

## 9. Comfort guide

**Depth vs. world scale.** Raising stereo strength (IPD) above 100% makes 3D stronger, but the world looks smaller, like a
miniature diorama. That is how stereo vision works: 2× eye distance looks exactly like a 2× smaller world.
To get **depth at normal scale**, use these instead:
1. **Move the camera closer** (Num4, `cam_dist_pct` 50–80%): the character and nearby objects get real depth.
2. **Enable 6DoF** (Num8) for parallax when you move your head.
3. Keep the **full headset FOV** on (Num\*, FILL).

Many players end up around **IPD 100–140%** with the camera at **50–100%**. Experiment: everything applies live.

**HUD.** The 55° panel is good for combat because health and the hotbar stay in view. 70° is good for exploring and reading.
Switch with NumDel. *In the game image* puts the HUD into the 3D world (not recommended: it stretches).

**Menus, inventory and map.** When the mouse cursor is visible, the mod shows a flat virtual screen and pauses head look.
When the cursor hides, the full VR view is back. With the square frame the game lays out menus as a square, which is expected.
In menus the cursor covers the whole game window even though the window is bigger than your monitor (it moves a little faster vertically).

**Motion comfort.** The horizon always stays level (`roll_sign=0`). If turning the camera with the mouse is uncomfortable,
lower the mouse sensitivity in the game, or turn off head look (Num5) for a fixed stereo screen.

---

## 10. Troubleshooting

Always look at `enshrouded_vr\vrlog.txt` first (the settings app has *Open log*). Attach it to any bug report.
`vrlog_prev.txt` is the previous session.

| Symptom | Cause / fix |
|---|---|
| Game starts flat, nothing in the headset | The VR link was not running, or the wrong runtime was picked. Start Link/VD/SteamVR first. Check `[xr] using runtime [...]` in the log. Choose the runtime explicitly (§8 `xr_runtime`). |
| Log: `XR_ERROR_FORM_FACTOR_UNAVAILABLE` | That runtime has no headset connected (for example, VD is the active runtime but you use Link). `auto` tries the next one. Otherwise choose explicitly. |
| Quest: the world shows **two images that never fuse** | `layer_eyefov` is 0. Set it to 1. |
| Black or grey headset, log shows `xrCreateSwapchain … RUNTIME_FAILURE` | The runtime is in a bad state. Restart Virtual Desktop Streamer / Link, then the game. |
| Image is soft | Use Windowed + square frame (§4.1), turn off automatic render scale, raise the Quest Link bitrate (§5), make sure the game holds 72 fps. |
| Stutter or judder | The game fps is below the headset rate: use DLSS Quality or lower settings, set the headset to 72 Hz. On Air Link, lower the bitrate (Wi-Fi glitches). |
| Square frame not applied (log: `force_window … Fullscreen mode -> ignored`) | Set the game's Display Mode to **Windowed**. |
| Objects double when you turn your **head** | Make sure `pair_lock=1`. A higher IPD makes it more visible. |
| Fast-moving objects show slight ghosting | A limitation of alternating-eye rendering (§11). |
| Depth looks wrong or "inside out" | ScrollLock, cycle back to normal stereo (swapped eyes may be active). |
| World prompts ("E — Pick up") look doubled | `hud_sync=1`. |
| Can't reach the edges of menus with the cursor | Make sure you run v1.1.0+ (the virtual menu cursor). |
| After closing the game the headset shows a frozen picture | Fixed in v1.1.0: the mod closes the VR session on exit. Check `[xr] shutdown` in the log. |
| A hotkey does nothing | The game window must be focused. Check `[key]` lines in the log and bindings in the *Keys* tab. |
| Need to play flat temporarily | Settings app → *Disable VR*, or create an empty `enshrouded_vr\xr_off.txt`. |

---

## 11. Known limitations

- **Alternate-eye rendering (AER).** The game renders the left and right eye on alternating frames. Each eye therefore updates
  at half the game fps (36 Hz at 72 fps), and fast-moving objects can show slight ghosting. The mod pairs frames carefully so
  that static scenes and head turns are clean.
- **No motion controllers.** Keyboard and mouse (or the game's gamepad support) only.
- **The HUD** is drawn once (from one eye) on a head-locked panel. It is not stereoscopic.
- **Square menus** with the square frame (the game lays out its UI to the frame shape). Use `force_window=2496x1872` (4:3) or
  `0` if you prefer.
- **The monitor** shows only the centre of the oversized game window in Windowed mode.
- Losing the headset connection in the middle of a session can make the game fall back to flat mode or close. Reconnect and
  restart the game.
- Tested with NVIDIA GPUs only.

---

## 12. Game updates

The mod finds the game's camera code with byte-pattern signatures. It has already survived several game patches
unchanged. If an update breaks a hook, the log shows a missing `[hook] … INSTALLED` line, and the mod falls back to flat
or partial VR. The game itself keeps working. Update the mod when a new release is published.

---

## 13. Uninstall

Delete **`dinput8.dll`** from the game folder. Optionally also delete the `enshrouded_vr` folder.
If you changed the game to Windowed, switch it back to Fullscreen in the game's graphics settings.
The mod does not modify any game file.

---

## 14. How it works

For the curious and for other modders.

- **Loading:** `dinput8.dll` is a proxy DLL. The game loads it, and it forwards DirectInput to the real system DLL. It also
  intercepts `GetProcAddress` for the game's Vulkan loader. No game file is changed and nothing is written to disk except
  `vr.ini` and the logs.
- **Rendering:** Enshrouded (Keen Games' Holistic engine) is Vulkan-only. The mod creates an **OpenXR** session on the
  game's own Vulkan device (`XR_KHR_vulkan_enable`) and copies every presented frame into an XR swapchain.
- **Camera:** the mod hooks the game's render-camera builder. In third person, head rotation is applied to the final view
  matrix (not to the camera orbit), and each eye is offset along the view's right axis.
  Frames alternate left/right (AER) and are submitted to the headset only as **complete pairs rendered with one head pose**.
- **FOV:** the mod replaces the game's FOV at its source, so projection, DLSS/DLAA and motion vectors all agree. The
  projection layer is cut to each eye's own (possibly asymmetric) FOV.
- **HUD:** the game's last render pass (UI only) is redirected into a transparent texture and shown as a head-locked quad.
- **Square frame:** the mod subclasses the game window and keeps its client area at the configured size. It centres the window on the monitor
  and maps the menu cursor to the whole window.

---

## 15. Building from source

Repository layout: `src/proxy/` (the DLL), `src/config/` (settings app), `src/third_party/` (Vulkan and OpenXR headers).

- **Mod DLL:** Visual Studio 2022 (MSVC, C++17, x64). Run `src/proxy/build_public.bat` to get
  `src/proxy/build_public/dinput8.dll` (the release configuration, `/DEVR_PUBLIC`). `build.bat` builds the developer
  configuration with diagnostic hotkeys.
  Headers: Vulkan headers (`third_party/vulkan`) and OpenXR SDK headers (`third_party/openxr/include`).
- **OpenXR loader:** `openxr_loader.dll` from the Khronos OpenXR SDK (NuGet package `OpenXR.Loader`, 1.1.x, x64).
- **Settings app:** `src/config/build.bat`, which uses the .NET Framework 4.8 `csc.exe` that ships with Windows (no SDK needed).
  Self-test: `EnshroudedVR-Config.exe --ini test.ini --selftest`.

Pull requests and issue reports are welcome. Please attach `vrlog.txt` and your headset, runtime, GPU and game build.

---

## 16. Credits & license

- Mod, research and code: **ivannikoffm**.
- Thanks to the Flat2VR community and to the UEVR / VR modding playbook authors, whose work inspired the approach.
- OpenXR loader © The Khronos Group Inc., Apache-2.0 OR MIT. See `THIRD_PARTY_NOTICES.txt`.
- Enshrouded © Keen Games. This is a fan project, not affiliated with Keen Games.

EnshroudedVR is released under the **MIT License**. See `LICENSE.txt`.
