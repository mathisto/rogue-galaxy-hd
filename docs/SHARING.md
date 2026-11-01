# What may and may not be shared

This project is a way to build the game *on your own PC from your own disc*.
It is published without any of the game. The rule is simple: **if a file
contains the game's pictures, sound, movies, text or code, or was made by
running those through a program, it stays on your PC.** That is also what keeps
a project like this on GitHub: a game company's takedown looks for the game's
assets, not for a recompiler.

Not affiliated with, authorised by or endorsed by Level-5, Sony Computer
Entertainment, Sony Interactive Entertainment or any other rights holder.
"Rogue Galaxy" and the game's art, music and movies belong to them. You must
own the game and make the disc image yourself.

## May be shared (and is what the repository holds)

| What | Why it is fine |
|---|---|
| Source code, scripts, patches, CMake files | Our own work (GPL-3.0) or freely licensed (see [`THIRD-PARTY-NOTICES.md`](../THIRD-PARTY-NOTICES.md)) |
| Documentation | Our own words |
| `tools/setup/` (the setup program, `Install.bat`) and `tools/hd/` (the HD pack tools, `Build-HD-Pack.bat`) | Code only; it reads *your* disc and writes on *your* PC |
| `tools/hd/data/stone-floor.txt`, `skip-hashes.txt` | Lists of 16-character hashes naming pictures; no pixels, no way to rebuild one |
| `config/SCUS_974.90/*.csv`, `overlays.toml`, `hashes.json` | Function names and addresses (from RGDecomp, CC0) and file sizes and hashes of the disc (Redump): facts about the game, not its data |
| `src/button_art_table.inc` | Boxes (x, y, size) saying where PlayStation button pictures sit in the game's pictures; no pixels |
| Our own art: the launcher icon, the Xbox-style button prompts (drawn in code) | Made from nothing; the font is Roboto (Apache-2.0) |
| Bug reports, logs (`build/last-run.log`) | They name files and addresses, not their contents. Look them over for your user name and paths first |
| `rg-install.json`, `work/setup/logs/` (what the setup finished, and its logs) | Hashes of nothing from the disc: names of steps, times, versions. The state names the disc image's file name, not its path; look the logs over for your user name and paths like any log |
| Settings you made (the menu's `settings.ini`) | Yours to share; it holds no game data (it does hold paths on your PC) |
| Screenshots and videos of the game that you take | Everyone does it, and a few frames are not the game, but they *are* the game's art: your call. The repository's own screenshots are listed in the findings below |

## May not be shared

| What | Why not |
|---|---|
| The disc image, `disc/` (the extracted disc, `SCUS_974.90`, the `.DAT` archives, the movies) | It is the game |
| **Texture packs**: `textures/SCUS-97490/replacements`, `pack-manifest.csv`, any folder of the HD pictures | Every picture in it is one of the game's pictures, scaled up. An AI upscale does not make it yours: it is a derivative of the art. It is the file most likely to draw a takedown, and the one people ask for |
| **HD movies**: `textures/SCUS-97490/movies`, `work/hd/movies` | Each frame is a frame of the game's movie, and the sound is the game's |
| Everything the builder makes: `work/hd/` (the extracted archives, the decoded pictures `tim2/`, `im3/`, `all/`, the upscales `upscaled/`, `keys.csv`) | Pictures and tables of the game's data. `keys.csv` is only hashes, but it maps them to the game's file names and positions; keep it with the rest |
| Learned pictures (`RG_TEXTURE_LEARN` output, `replacements/learned`) | Pictures the game showed |
| `work/setup/` (a disc image the setup converted from a CHD or BIN/CUE, before it is deleted) | It is the disc |
| Memory cards and saves (`disc/mc0`, `mc1`) | Yours, and they carry the game's save format and your name; not data to publish |
| The game's text pages, fonts, dumps of memory, audio or GS captures | The game's data (`debug\` launchers write some of them) |
| Built executables with the disc inside them | The build contains recompiled game code, which is the game's code in another form: share the *source* and let people build their own (`Install.bat`) |
| The BIOS | Not needed and never to be shared |

The repository's `.gitignore` already leaves out `/disc/`, `*.iso`, `/mc0/`,
`/mc1/`, `/build/`, `/toolchain/`, `/textures/`, `/work/`, `/borders/*` and
`*.log`, so a plain `git add -A` does not take them. `Rogue Galaxy.exe` (the
launcher) is ignored too.

## The release zip

`python tools/make_release.py --zip` makes `RogueGalaxy-Setup-<version>-<commit>-windows.zip`: the setup window, the build
tools and the sources, and nothing from any disc (docs/INSTALLER.md, "The release"). It may be shared, because it holds only
what the rows above allow plus pinned third-party tools under their own licences (THIRD-PARTY-NOTICES.md). Before you upload one:

1. `tools/make_release.py` ran `tools/check_content.py` over the folder and it printed `no hit`. Run it with
   `--disc` pointing at your unpacked disc (read only) so it also looks for runs of the disc's bytes: the builder does this on its own when a `disc/` folder is beside it.
2. Unzip it in a scratch folder and run `Setup-Console.cmd --estimate` there: the plan must list no download for the game
   (only Real-ESRGAN, FFmpeg and chdman, and only when the choice needs them).
3. Say in the release text that the program is **not code-signed**. Windows SmartScreen shows "Windows protected your PC /
   Unknown publisher" for a program it has not seen often: that is not a finding about the file. Tell people to download
   only from the release page of the repository, to compare the SHA-256 the page lists, and then use "More info", "Run
   anyway". Signing needs a certificate (a yearly cost) and still shows the warning until the program has a reputation;
   it is the author's decision whether to buy one.
4. Name the commit (the zip's name and `REVISION` do) and attach the source: `tools/release/source_zip.py` makes the source zip
   the GPL asks for, and the release bundles the pinned upstream sources of its GPL parts (MinGit) by link in THIRD-PARTY-NOTICES.md.

## Sharing HD packs between friends

Do not. Ask them to run the setup (**Everything**, or `Build-HD-Pack.bat`): it takes a few hours of waiting
and nothing else, and the result is bit for bit the one you have (the models and
the steps are fixed). That is the whole reason the pipeline is a one-click
builder and not a download.

If you improve something that is *code* (a better class for the upscale, a
bug in a key), a pull request is welcome. If you hand-pick pictures (as
`stone-floor.txt` does), share the **hash list**, not the pictures.

## Before you publish a fork

Run through [`THIRD-PARTY-NOTICES.md`](../THIRD-PARTY-NOTICES.md) and these:

1. `git status` shows nothing from `disc/`, `textures/`, `work/`, `toolchain/`.
2. `git ls-files | rg -i "\.(iso|dat|hd6|mvi|mkv|tm2|tim2|pac|chr|img)$"` finds nothing.
3. No personal paths (`C:\Users\<you>`), user names or e-mail addresses in the
   files (`rg -i "C:.Users|@gmail"`; also `git log --format=%ae` for the e-mail in the commits), and the project URLs in
   `thirdparty/dq8-gfx/src/ui/ui_overlay.cpp` and `ui_pages.cpp` point to your fork.
4. The README says what it should: no game data included, not affiliated.
5. History: files you removed are still in older commits. If a game file was ever
   committed, rewrite the history before publishing; this project's history has none
   (checked when this page was written; run the check again for your own fork).
