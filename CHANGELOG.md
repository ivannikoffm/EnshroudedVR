# Changelog

## v1.1.0 — first public release (2026-10-18)
- True stereo VR for Enshrouded via OpenXR (Vulkan): head-tracked third-person camera, alternate-eye rendering with pose-locked eye pairs.
- Full headset FOV injected at the game's FOV source; projection layer fitted to each eye's (asymmetric) lens FOV — Quest 3 support.
- Automatic OpenXR runtime selection (active → Meta → Virtual Desktop → others), explicit `xr_runtime` override.
- Square render frame via `force_window` (Windowed mode), ~33% sharper per eye at the same pixel cost.
- Head-locked HUD panel (55°/70°), menus on a virtual screen, virtual cursor that reaches the whole oversized window.
- Settings app with live apply, presets, key rebinding, Display & Headset tab.
- Clean shutdown of the VR session on game exit.
- Tested: Meta Quest 3 (Air Link), Pico Neo 3 (Virtual Desktop).
