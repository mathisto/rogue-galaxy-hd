# Session notes: HD movies (2026-10-28)

Branch `feature/hd-movies` (from `feature/polish-2`), local commits only,
nothing pushed. User docs: [HD-MOVIES.md](HD-MOVIES.md). Research and the
upscale pipeline: `research\16-hd-cutscenes.md` (the design of section 8 is
what was built, with the corrections below). Screenshots:
`research\assets\hd-movies-ingame\` (the author's notes, outside the repository).
Test build `build\game-dev\rg-hdmovies.exe`; the player's
`build\game\src\roguegalaxy.exe` was running throughout and was not relinked
(its objects in `build\game` are now this branch's: `cmake --build build\game`
relinks it, once the game is closed).

## What was built

- `src/movie_hd.{h,cpp}`: the player. Three hooks and a decoder thread.
  - **MoviePlayerOpen (0x1D61A0), not MovieOpen (0x1D5CF0).** The research
    named MovieOpen; the game's callers (`MoviePlayModal`, the title
    overlay's `TitleMoviePlay`) call MoviePlayerOpen, which `j`umps into
    MovieOpen without going through the function table, so a hook on
    MovieOpen is never entered. The source name is the string at
    `params + 0x80` (a1): `00\title01.mvi` (no `MOVIE`, lower case), mapped
    to `<folder>\00\TITLE01.mkv`.
  - **Frame number = pictures the game has taken.** `MovieGetFrameCount`'s
    word (0x4B3218) stays 0 here. The runtime's `sceMpegGetPicture` leaves
    the mpeg object's word at +8 zero when it served a picture, which the game
    then uploads to the movie texture before calling again, so each entry of
    the game's call (hooked at 0x111148, from `VideoDecDecodeBs` 0x1D29E0)
    finds the previous call's outcome and counts it. (`return_hook`'s
    `callThen` cannot be used: the call waits for the next vblank and the
    deferred `after` never ran; see "return_hook" below.) MovieDrawFrame
    (0x1D5870) hands the draw the HD picture numbered `served - 1`.
  - Shutdown (0x1D56C0) stops the decoder and clears the picture.
- Decoder: the FFmpeg the runtime already links (avformat/avcodec/swscale,
  7.1 LGPL shared build): HEVC decoded with four threads, `sws_scale` (bicubic,
  BT.709 limited) straight to RGBA at `512 x scale` by `336 or 448 x scale`
  (scale 4 for 1080p, 3 for 1440x1080), alpha set to the GS's 0x80, six
  pictures ahead on the thread. Nothing was added to the build.
- Backend (`thirdparty/dq8-gfx`): `sdlgpu_video.h` (`GsVideoOverride`, a
  published immutable frame) and `GsTextureCache::acquireVideo`: a draw whose
  TEX0 is the frame's TBP and 512 wide samples it instead of the texture built
  from GS memory, through the texture-pack path (`textureScale` = the integer
  scale, so UVs, clamps, linear filter and sprite clamping are the guest's).
  Three GPU textures in turn, each rewritten after `beforeRewrite` lands the
  queued draws that use it. The game still uploads its own 672 or 896
  macroblocks a picture (wasted, harmless).
- **TBP depends on the height**: 336-line movies draw from 0x2E00, the
  448-line ones (TITLE01/02, STAFF_A) from 0x2A00 (a first version matched
  only 0x2E00 and the 4:3 movies stayed the game's own while the log said HD;
  `draws on HD pictures` in the end-of-movie line is the check).
- Menu: Game > Convenience > "HD movies" (`hd_movies`, on by default only
  when the folder holds files, help text says how many of 89), the game's own
  Settings screen's switches page (a tenth row, "HD Movies"); `RG_HD_MOVIES`.

## Verified (hidden window, `RG_INPUT_FILE`, scratch settings and a copy of the memory card)

- Boot: TITLE01 (4:3, 540 frames) and TITLE02 (the title loop under the
  menu) play from the files; the log line at the end says 542 draws on HD
  pictures, 533 uploads, 0 held. Sharper than the game's picture at the same
  frame (`01-`, `02-`).
- Opening: New Game, the recap, "Press X", then 01_01 (16:9 between bars in a
  4:3 window, `07-`). Pause (START) shows "PAUSE" over the frozen HD picture
  (`08-`); 12 pause/resume cycles in a row all worked; Triangle skips; the
  game goes on to the street scene (`10-`).
- Mid-game movie: `RG_HD_MOVIES_PLAY=07/07_05` plays that movie in place of the
  opening, through the same `EvCmdPlayMovie` -> `MoviePlayModal` path the story
  movies use (traced), 475 pictures, 476 draws on HD pictures, 1 held (the
  end), resampling 12.9 ms average, 22 ms at most (`03-`, `04-`). Also
  99/STAFF_A (4:3 with Widescreen, 1600x900).
- Same frame, HD against the game's: screenshots at the same picture number
  with and without HD, matched against the decoded frames of both: within one
  frame (HD 119 / SD 120, 240 / 241, 303 / 303, 360 / 360; 07_05 101 / 102,
  239 / 240, 402 / 402; TITLE01 240 / 239, 362 / 361). The sign changes, so it
  is the game's async GS queue, not an offset.
- Geometry: the pixel extents of the picture are identical with and without
  HD at 1280x960 and, with Widescreen, at 1600x900 (07_05: rows 0-837, cols
  0-1599; STAFF_A: rows 41-725, cols 194-1402).
- Fallback: a missing file (every movie without one), a truncated file
  (`the HD file ended before the movie`, at once), a garbage file (`cannot
  open`), a decoder made slow with `RG_HD_MOVIES_DELAY_MS=70` (`decoding fell
  behind` after 0.5 s): each leaves the game's picture and the movie
  continues; the next movie tries HD again.
- Menu: the F1 Game page shows the option on with files found and off with
  `hd_movies = 0` saved; the game's Settings screen shows it and changing it
  there writes `hd_movies = 0` into settings.ini.

## Not verified

- **Sound.** The runs were muted (`RG_MUTE=1`). The game's own PCM and its
  clock are untouched, and the picture follows the game's picture counter, but
  nothing was listened to. Measured: within one frame of the game's own
  picture.
- A movie reached by the story rather than by substitution: the save in the
  repository's card is at Juraika in chapter 2; no movie was within reach
  (the warp movies need the ship). GAMEOVER, the travel movies and the
  credits were not played. 07_05 and STAFF_A went through the opening's
  slot (`RG_HD_MOVIES_PLAY`).
- 32:9 and other window shapes beyond 4:3 and 16:9; fullscreen; resolution
  scales other than 2x (the scale is applied by the render scale's own
  linear filter for a non-matching size, as for a texture pack).
- Frame pacing under a heavily loaded PC; the fallback is tested with an
  artificial delay only.
- A hang seen once: in one of the first runs, the pause screen's second START
  never arrived (the main thread sat in a semaphore wait for 15+ s, stall
  watchdog report). The same sequence ran fine in later runs (six pause and resume
  pairs in a row, and two single pauses) with HD and in the baseline; the baseline also prints the watchdog line once
  while paused. Not reproduced, so not explained; it happened before the
  4:3 fix and with extra screenshot requests in flight.

## return_hook note

`rg::hooks::callThen` records the thread id when it defers (`deferUntilReturn`)
after the original threw `EeDispatcherTransfer`; for a call that blocks
(`waitVSync`), the current thread at that moment is the scheduler's next one,
so the entry never matches at the resume (`due` stays false and `after` never
runs). Seen with `sceMpegGetPicture`; not changed here (the existing users do
not block).

## Test method

`build\game-dev\rg-hdmovies.exe` from the repository folder, `RG_SETTINGS_FILE`
and `RG_MC0_DIR` in the session scratchpad, `DQ8_GFX_HIDDEN_WINDOW=2`,
`RG_MUTE=1`, `RG_TEXTURE_REPLACEMENTS=0`, `RG_HD_MOVIES=work\hd\movies\hd`,
`RG_INPUT_FILE` appended to step by step (`UP`, `CROSS`, `START:20`, `shot`);
`RG_HD_MOVIES_SHOT_AT` for screenshots at a picture number. Frame
identification: each screenshot cropped to the picture and compared (MSE at
160 px) with the decoded frames of the HD file and of the lossless
intermediate.

## Cost

Resampling 9 ms (4:3, 1440x1080 -> 1536x1344) to 13 ms (07_05) a picture on
the decoder's thread, on top of FFmpeg's four decode threads; each picture is
11 to 15 MB uploaded once. Memory: six pictures ahead (about 70 to 90 MB) and
the three GPU textures. A GPU-side scale (upload the 1080p picture, blit it
to the 4x texture) would cut the CPU and the upload.
