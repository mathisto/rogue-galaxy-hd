# Upscaling: DLSS, FSR and what fits this renderer

Written 2026-10-27, with the FSR 1 options added to F1 > Display.

## How the picture is made

The GS backend (`thirdparty/dq8-gfx`, SDL3 GPU on Vulkan here) draws every
frame natively at the internal resolution (1x to 8x the PS2's 512x448).
That is real supersampling: polygons, edges and alpha tests are rasterized
at the higher resolution, not stretched afterwards. The presenter then fits
that picture into the window with a blit (Sharp, Smooth or Nearest), or the
FSR or Supersample passes below. Any upscaler would sit at that last step.

## DLSS: not practical

- DLSS (2/3/4) is NVIDIA's proprietary NGX SDK. It needs the SDK's headers
  and a redistributable DLL whose licence is not GPL-compatible, so it cannot
  go into this GPL-3.0 repository, and nothing was downloaded or added.
- It integrates with D3D11, D3D12 or Vulkan by handing it native resource
  handles. SDL3 GPU hides those: there is no supported way to get the
  `VkImage`/`VkCommandBuffer` behind an `SDL_GPUTexture` or to record
  foreign commands into SDL's command buffers. The backend would have to
  leave SDL GPU, or SDL would need an interop API it does not have.
- Most of all, DLSS is a temporal upscaler: each frame it needs per-pixel
  motion vectors, depth and a sub-pixel camera jitter. A PS2 game supplies
  none of these. The GS has no notion of motion; depth exists per draw but
  vertices are transformed by the game's own VU1 microcode, so motion vectors
  would mean tracking every vertex across frames, and jittering the
  projection would mean patching each of the game's camera paths. The GS's
  2D layers (HUD, menus, text, movies, screen-space effects) would smear.
- And it buys little here: the backend can already render at up to 8x.
  Upscalers exist to avoid the cost of native resolution; this game is cheap
  to draw at native 4K-class resolutions on any GPU that could run DLSS.

## FSR 2/3, XeSS, TAA-style upscalers: same wall

FSR 2 and 3 (MIT, open) and XeSS are also temporal: the same motion vectors,
depth and jitter are required. Open licence, same impossibility, for the same
reasons as DLSS.

## FSR 1: what was added

FSR 1 is spatial: one frame in, one frame out, no motion vectors. It has two
passes, both now in the presenter (`shaders/present_easu.frag`,
`shaders/present_rcas.frag`, plain GLSL written after AMD's `ffx_fsr1.h`):

- **Scaling filter: FSR** (EASU). An edge-directed, Lanczos-like 12-tap
  upscale that keeps edges smooth without the blur of bilinear. Used only
  when the window is larger than the internal resolution; otherwise it falls
  back to bilinear (EASU is not a downscaler).
- **Sharpening: Off, Low, Medium, High** (RCAS). Contrast-adaptive
  sharpening that limits itself per pixel so it cannot overshoot its
  neighbours (no halos), after whichever filter. Off by default. Works with
  any internal resolution, so it also helps at 2x and above, where the
  window usually shows a downscaled picture.

Recommendation: raise the internal resolution as far as the GPU allows
(real detail), use FSR only when it cannot reach the window's size, and add
Low or Medium sharpening to taste.

Both passes are SPIR-V full-screen passes recorded into the present's
command buffer (`SdlGpuDevice::drawEasu`, `drawRcas`), through one texture
of the picture's size on screen. A device without SPIR-V (Metal) falls back
to the blit with one `[gfx] FSR is off` line. Settings file:
`[display] filter = fsr`, `sharpening = 0..3`; `RG_UI_SCRIPT`: `filter fsr`,
`sharpening N`.

Tested: the title screen at 1x into a 1536x1152 window (hidden window,
`DQ8_GFX_HIDDEN_WINDOW=2`, screenshots from the composed frame): the
picture lands where it did, FSR removes the stair steps of the 3x
enlargement, sharpening crisps edges without visible halos. Not yet looked
at: a 3D field scene, widescreen, the Blurred and Image borders with FSR on,
and the cost on a slow GPU (two extra full-window passes).

## Supersample: a proper downscale (2026-10-28)

At 4x to 8x internal resolution the picture is larger than the window, and
Smooth's bilinear blit reads only the 2x2 texels nearest each pixel's
centre: most of what was rendered is skipped, so thin lines and texture
detail shimmer. **Scaling filter: Supersample** resizes it with a Lanczos-2
kernel stretched over each window pixel's whole footprint, in two passes
(across, then down; `shaders/present_downscale.frag`,
`SdlGpuDevice::drawDownscale`), into the same texture FSR uses, so
Sharpening still applies after it. When the window is the larger it falls
back to bilinear. Settings file `filter = supersample`, `RG_UI_SCRIPT`
`filter supersample`. Not the default: next to Smooth the difference is real
but small in still pictures (railings, signs and the menus' thin rules come
out cleaner, with less stair-stepping; `research\assets\polish\04-downscale-*`),
and it costs two passes reading up to 2 x 16 texels a pixel at 8x.

## ReShade and RenderDoc

Both launchers keep other programs' Vulkan layers out of the game
(`VK_LOADER_LAYERS_DISABLE=~implicit~`), because overlays hooked into every
Vulkan process (OBS, Steam, GOG, Epic) sat on its presents. To let them in:

1. In Windows, set the user environment variable `RG_ALLOW_VK_LAYERS=1`
   (Settings > System > About > Advanced system settings > Environment
   Variables), then start `Rogue Galaxy.exe` as usual. Remove it to go back.
   `debug\RogueGalaxy.bat` honours it too, and a `VK_LOADER_LAYERS_DISABLE`
   you set yourself is no longer overwritten.
2. **ReShade**: install it for `build\game\src\roguegalaxy.exe` with the
   Vulkan API. Colour shaders work (LUTs, CAS/LumaSharpen, SMAA, FXAA, film
   grain, colour grading, CRT); shaders that need depth (SSAO, MXAO, depth of
   field, fog by depth) do not: this renderer never hands the game's depth to
   the swapchain. All the other implicit layers come in with it too (the
   Steam and Epic overlays included).
3. **RenderDoc**: launch `build\game\src\roguegalaxy.exe` from RenderDoc
   with the working directory set to the repository folder and
   `disc\SCUS_974.90` as its argument. With the variable set the command
   buffers carry labels: `GS draws: target <base> <width>x<height>` before
   each of the GS's render passes, `Display: ...` for the composed frame, a
   `Present` group holding the sides, the picture and the named
   FSR/downscale/RCAS passes. `DQ8_GFX_DEBUG_LABELS=1` turns the labels on
   alone.

Checked (hidden runs, `VK_LOADER_DEBUG=layer`): with the variable the loader
inserts the implicit layers (here NVIDIA's `VK_LAYER_NV_present` and
`_optimus`) and the game reaches its title screen; without it they are
"forced disabled". ReShade and RenderDoc themselves were not installed or
tried (nothing is downloaded by this project).

## Licence of the FSR 1 derivation

The shaders follow the algorithm of FidelityFX Super Resolution 1.0, whose
MIT notice is (abridged here; the full text is in AMD's `ffx_fsr1.h`):

> Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved.
>
> Permission is hereby granted, free of charge, to any person obtaining a
> copy of this software and associated documentation files (the
> "Software"), to deal in the Software without restriction, including
> without limitation the rights to use, copy, modify, merge, publish,
> distribute, sublicense, and/or sell copies of the Software, and to permit
> persons to whom the Software is furnished to do so, subject to the
> following conditions: The above copyright notice and this permission
> notice shall be included in all copies or substantial portions of the
> Software. THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.

MIT is compatible with the project's GPL-3.0.
