# Changelog

## v1.2.0 — clean stereo with DLSS/DLAA (2026-10-24)
- **Stereo ghosting / halos fixed** with DLSS and DLAA: each eye now keeps its own DLSS history, and the motion vectors are
  rebuilt per eye (same eye, two frames back) by a compute shader before every DLSS call. Self-calibrating against the
  game's own motion vectors; if it cannot calibrate, or the game does not use DLSS, the image is left exactly as the game renders it.
  On by default (`dlss_fix=1`), **Num1** toggles.
- **Sharpening and colour:** AMD CAS sharpening (**Num9**: off/25/50/75/100%) and contrast/saturation/brightness (`vr.ini`).
  At neutral values no extra pass runs.
- Checked on the latest game update (all hooks found by signature).
- Recommended AA is now DLAA where the GPU holds 72 fps.

## v1.1.0 — first public release (2026-10-18)
- True stereo VR for Enshrouded via OpenXR (Vulkan): head-tracked third-person camera, alternate-eye rendering with pose-locked eye pairs.
- Full headset FOV injected at the game's FOV source; projection layer fitted to each eye's (asymmetric) lens FOV — Quest 3 support.
- Automatic OpenXR runtime selection (active → Meta → Virtual Desktop → others), explicit `xr_runtime` override.
- Square render frame via `force_window` (Windowed mode), ~33% sharper per eye at the same pixel cost.
- Head-locked HUD panel (55°/70°), menus on a virtual screen, virtual cursor that reaches the whole oversized window.
- Settings app with live apply, presets, key rebinding, Display & Headset tab.
- Clean shutdown of the VR session on game exit.
- Tested: Meta Quest 3 (Air Link), Pico Neo 3 (Virtual Desktop).
