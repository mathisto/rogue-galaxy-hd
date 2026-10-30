# HD movies

The game's 89 pre-rendered movies (the logos, the title loop, the opening,
the story and travel movies, the credits) can play from upscaled 1080p video
files instead of the disc's 512x336 MPEG-2. The files are built from your own
disc and are never part of the repository or a release.

## What the game does

Nothing changes in how the game plays a movie: it still reads the `.MVI` from
the disc, decodes it, plays its sound, paces it by its own clock, draws the
subtitles and the pause screen, and lets Start and Triangle skip it. Only
the picture drawn on screen is replaced, frame for frame, by the same-numbered
frame of the HD file. So sound, subtitles, pause, skip and the end of the
movie are exactly the game's, and a frame can be at most a frame (33 ms) from
where the game's own picture would be.

The picture is the movie's own shape: 16:9 for the 86 movies that are
512x336 on the disc (drawn between bars in the 4:3 frame, filling the window
with Widescreen on), 4:3 for `TITLE01`, `TITLE02` and `STAFF_A` (512x448).
The rectangle the game and `src/widescreen.cpp` compute for it is used as it
is, so Display > Aspect ratio, Widescreen and Borders treat an HD movie as
they treat the original.

## Building them

Run `Build-HD-Pack.bat` and choose **[2] movies** (or [3] for the textures too).
For each of the 89 movies it decrypts the `.MVI` from `disc\MOVIE`, upscales
every frame 2x with Real-ESRGAN (`realesr-animevideov3`), resamples to 1080p
and encodes HEVC Main10 (NVENC when the graphics card has it, else x265),
keeping the movie's sound; the files are written under `work\hd\movies\hd` and
linked into `textures\SCUS-97490\movies`. It needs a Vulkan graphics card,
FFmpeg (it asks before downloading one if none is on your PATH), and about
11 GB; it took about 1.5 hours for all 89 on an RTX 5070 Ti. It can be stopped
and run again (finished movies are skipped), `--gentle` keeps the PC usable,
and `--limit-movies N` makes the N shortest only, to try it. A movie where no
frame moves for 3 minutes (`--stall-timeout`; a hang was seen once, with
Real-ESRGAN idle while a pipe stalled) has its processes stopped and is tried
again, up to twice. Everything is in [HD-BUILD.md](HD-BUILD.md); the files are
yours alone and are not to be shared ([SHARING.md](SHARING.md)).

## Setting it up

Put the files in `textures\SCUS-97490\movies`, one folder a movie group, named
as on the disc:

```
textures\SCUS-97490\movies\00\TITLE01.mkv
textures\SCUS-97490\movies\00\TITLE02.mkv
textures\SCUS-97490\movies\01\01_01.mkv
textures\SCUS-97490\movies\07\07_05.mkv
textures\SCUS-97490\movies\99\STAFF_A.mkv
...
```

That is the layout `tools/hd/rg_movies_batch.py` writes
(`work\hd\movies\hd\<disc folder>\<name>.mkv`) and `Build-HD-Pack.bat` links
into place; or copy its `hd` folder's contents there by hand, or point `RG_HD_MOVIES` at the folder (a path, or `0` to switch HD movies off for
a run, `1` for the default folder). The game opens a file the moment its
movie starts, so files can be added while it runs.

What a file has to be: any video the game's FFmpeg (7.1, the runtime's own)
decodes, with as many frames as the disc's movie (the sound is not read from
it), 16:9 for a 512x336 movie, 4:3 for a 512x448 one. The batch's files are
HEVC Main10, 1920x1080 (1440x1080 for the 4:3 ones), about 7 GB for all 89.
The picture is resampled to a whole multiple of the movie's 512-wide
texture on the decoder's thread (4x for 1080p, 3x for the 4:3 ones) so the
renderer draws it as it draws a texture-pack picture.

F1 > Game > Convenience > **HD movies** (and the same row on the game's
Settings screen, "HD Movies", page 2) switches it; `hd_movies` under `[game]`
in `settings.ini`. It is on by default only when the folder holds files (the
option's help says how many of the 89 it found), so a fresh install without
files shows it off. The choice applies from the next movie; a movie in play
goes back to the game's own picture when it is switched off.

## Falling back

Any of these leaves the game's own picture, for that movie only (the next
one tries again), and logs a `[hdmovie]` line:

- there is no file for the movie, or it cannot be opened (a log line when it
  is there but not readable);
- the file's shape is neither 16:9 nor 4:3;
- the decoder has no picture ready for half a second (3 s for the first):
  the HD picture of an earlier frame stays meanwhile, then the movie goes
  back to the game's picture, mid-movie too;
- the file runs out more than two frames before the movie, or fails to
  decode.

The decoder runs six pictures ahead on its own thread. On the test PC
(Ryzen 9 7900X, RTX 5070 Ti) it keeps six ahead at 30 frames a second with
the resampling at 9 ms a 1080p picture on average (22 ms at most, 07_05) on
one thread, besides the FFmpeg decoder's four.

## For testing

`RG_HD_MOVIES_DEBUG=1`, `RG_HD_MOVIES_PLAY=NN/NAME`, `RG_HD_MOVIES_SHOT_AT`
and `RG_HD_MOVIES_DELAY_MS` are in [DEVELOPMENT.md](DEVELOPMENT.md); how it
was built and what was and was not checked is in
[SESSION-NOTES-hd-movies.md](SESSION-NOTES-hd-movies.md).
