# DLSS 5 (neural rendering) — experiment

Status (2026-11-03): runs as a showcase, not playable or stable.

DLSS 5 is NVIDIA's neural rendering feature (`nvngx_dlssnr.dll`, NGX feature 18). This page records
what we tried in the Vulkan port and the setup that works.

## Direct from our DLSS bridge on Vulkan: does not work

`tlg_dlss.dll` can create feature 18 next to DLAA when `TLG_DLSSNR=1` is set (see
`gpu/dlss_bridge/tlg_dlss_bridge.cpp`, `NrCreate`). With `nvngx_dlssnr.dll` 310.8.0.0 the creation fails
with `0xBAD0000B` (unable to initialize feature), and the feature-requirements query returns
`0xBAD00012` (not implemented). That runtime is D3D12-only. The bridge then falls back to normal DLAA.

## Through ReShade on D3D12: works with a patch

Setup, all in the folder next to `tlg-probe.exe`:

- ReShade with Vulkan support, the **DLSS 5 Feed** add-on (`dlss5-feed.addon64`, tested 1.17.0) and the
  **RenoDX DLSS 5** add-on (`renodx-dlss5.addon64`, tested 6.5.3, file version 0.2026.917.1432). The
  feed copies the finished Vulkan frame plus depth and motion to a private D3D12 device, runs DLAA
  there, and the RenoDX add-on hooks that DLAA call to run DLSS 5. Many thanks to the authors of both
  add-ons; DLSS 5 here is their work.
- **Patch needed:** the RenoDX add-on installs a "queue completion tracker" by detouring vtable slot 10
  (D3D12 `ExecuteCommandLists`) of the native object behind ReShade's command queue, without checking
  the graphics API. In a Vulkan game that object is a `VkQueue`, so it detours an internal function of
  the NVIDIA Vulkan driver (`nvoglv64.dll`). The driver then calls the detour with unrelated arguments
  and the game crashes on the first DLSS 5 frame (null read at `renodx-dlss5.addon64+0x25261`).
  `tools/patch_renodx_dlss5.py` makes the tracker installer return "not installed" (3 bytes); the
  add-on then defers resource retirement to shutdown, which it already supports.
- `dlss5-feed.cfg`: `mode=2`, `async_home=1`, `vk_present_sync=1`.
- `ReShade.ini`, `[RenoDX.DLSS5]`: `NRAutoMask=0`, `NRIntensity=1`, `NRResolutionScale=1`.
- Game: `TLG_DLAA=0` (the feed runs its own DLAA; ours would run twice).

Result on an RTX 5090 at 3840x1600: about 76 fps unlocked (85 fps with the feed alone, no DLSS 5).

## Known problems

- Any rebuild of the feature (resizing the window, a ReShade effect reload) crashes inside the add-on's
  feature creation; the feed catches it but DLSS 5 stays off until the game restarts.
- The feed grabs the final frame, so menus and subtitles are processed too. The fix is to hand the
  frame over from our renderer before the UI pass, as in games with built-in DLSS.
- `NRResolutionScale=0.75` is slower than 1 (the add-on switches to two passes plus scaling).
  `NRAutoMask=1` holds DLSS 5 back on large parts of the image. The feed's `work_resolution` does not
  lower the DLSS 5 resolution.
- NVIDIA's screen recording stops after about 2 seconds; OBS display capture works.
