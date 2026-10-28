#pragma once

#include <string>

class PS2Runtime;

// HD movies: the game's movies played from upscaled video files.
//
// The game keeps playing the original movie (MVI read, libmpeg picture, its
// PCM sound, the clock that paces them, subtitles, Start to skip, the end).
// Only the picture MovieDrawFrame (0x1D5870) draws is swapped: when a movie
// opens (MovieOpen, 0x1D5CF0) its path, cdrom0:\MOVIE\NN\NAME.MVI, names a
// file in the HD movie folder, NN\NAME.mkv (the layout tools/rg_movies_batch.py
// writes); a host thread decodes it ahead (FFmpeg, as the runtime already
// uses for the MPEG), and each MovieDrawFrame hands the draw the HD picture
// of the frame the game is showing (its frame counter, MovieGetFrameCount
// 0x1D52D0), resampled to a whole multiple of the game's 512-wide movie
// texture so the backend's texture-pack path draws it in place of the
// texture at the movie's TBP (GsVideoOverride, dq8-gfx).
//
// The picture is the movie's own, 16:9 in 512x336 between bars (4:3 for the
// 512x448 ones), so the rectangle the game and widescreen.cpp already
// compute is the right one for it. Anything wrong (no file, no decoder,
// decoding falls behind, a frame count that disagrees) leaves the game's
// own picture, mid-movie too.
//
// Folder: RG_HD_MOVIES=dir, else textures/SCUS-97490/movies. RG_HD_MOVIES=0
// turns the feature off, =1 means the default folder. The menu's "HD movies"
// (game option hd_movies) switches it, on by default when the folder holds
// files.
namespace rg::hdmovie
{

// After loadELF().
void install(PS2Runtime &runtime);

void setEnabled(bool on);
bool enabled();

// The movie folder, and how many .mkv files it holds (scanned once, then on
// request); 0 when there is none.
std::string folder();
int fileCount();
// The movies the game has (the disc's MOVIE tree).
constexpr int kMovieCount = 89;

} // namespace rg::hdmovie
