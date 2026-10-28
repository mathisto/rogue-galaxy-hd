# Upscaling: DLSS, FSR and what fits this renderer

Written 2026-10-27, with the FSR 1 options added to F1 > Display.

## How the picture is made

The GS backend (`thirdparty/dq8-gfx`, SDL3 GPU on Vulkan here) draws every
frame natively at the internal resolution (1x to 8x the PS2's 512x448).
That is real supersampling: polygons, edges and alpha tests are rasterized
at the higher resolution, not stretched afterwards. The presenter then fits
that picture into the window with a blit (Sharp, Smooth or Nearest). Any
upscaler would sit at that last step.

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
