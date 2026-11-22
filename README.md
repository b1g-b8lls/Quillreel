# Quillreel

A tool for Linux and Windows for Bink video files (.bik / .bk2): inspect, convert
and play them. Built on FFmpeg and SDL3. Not affiliated with RAD Game Tools or Epic Games.
See [docs/LEGAL.md](docs/LEGAL.md).

## Status

| Command | State |
|---------|-------|
| `inspect` | done: header info, text or JSON |
| `convert` | done: mp4/mov/mkv/webm, raw .y4m, PNG sequence, wav/flac/m4a/ogg/opus/mp3/mka |
| `play` | done: window with sound, seeking, audio track switching, looping |
| `bink2` | done: enables Bink 2 video once you provide RAD's decoder (see below) |
| GUI (`quillreel-gui`) | done: file browser, info panel, built-in player, convert dialog, Bink 2 DLL setup (Qt 6) |

### Bink 2 video: not supported until you provide the decoder

**Bink 2 video (.bk2) does not work after installing Quillreel.** Quillreel includes
no Bink 2 video decoder (and FFmpeg has none). Until you provide one, .bk2 files can
be inspected and their audio played and converted, but their video can't be shown or
converted.

To enable Bink 2 video, provide RAD's own decoder, `bink2w64.dll`, from a game that
uses Bink 2 (tested: Fallout 4, GTA V Enhanced):

In the GUI: Help > Bink 2 Video > Install DLL…. Or from the command line:

```sh
quillreel bink2 install "/path/to/Fallout 4/bink2w64.dll"   # enables .bk2 video
quillreel bink2 status                                        # enabled or not
quillreel bink2 remove                                        # disables it again
```

After that, `play` and `convert` handle .bk2 video exactly like RAD's decoder does.
Quillreel loads the DLL directly; Wine is not needed. Details: [docs/bink2.md](docs/bink2.md).

## Install

Ready-made packages are on the [Releases page](https://github.com/b1g-b8lls/Quillreel/releases);
you can also build them with `make package` (see packaging/README.md).

- **Debian 13**: `sudo apt install ./quillreel_<version>-1.deb13_amd64.deb`
- **Ubuntu 26.04**: `sudo apt install ./quillreel_<version>-1.ubuntu26.04_amd64.deb`
- **Other distros**: `chmod +x Quillreel-<version>-x86_64.AppImage` and run it. It
  opens the GUI; `./Quillreel-….AppImage convert …` (or `inspect`, `play`,
  `bink2`) runs the CLI.
- **Windows 10 (1903 or later) and 11, 64-bit**: run
  `Quillreel-<version>-windows-x86_64-setup.exe`. It adds Quillreel to the Start
  menu and can open .bik/.bk2 files from Explorer. The command-line tool is
  `quillreel.exe` in the install folder (`C:\Program Files\Quillreel`). The
  Bink 2 DLL you install is kept in `%LOCALAPPDATA%\Quillreel`.

The AppImage and the Windows version update themselves from new releases (Help >
Check for Updates; on first start Quillreel asks whether to check automatically,
at most once a day). Downloads are checked against the SHA-256 checksums GitHub
publishes. The `.deb` packages only say a new version exists; update them like
any other package.

The GUI follows the desktop's look: Plasma's theme on KDE, the GTK theme on
GNOME and other GTK desktops, and dark mode. None of the packages include Bink 2
video support; add RAD's `bink2w64.dll` yourself (see above).

## Build

Needs g++ with C++23, make, pkg-config, SDL3 and the FFmpeg development libraries
(libavformat, libavcodec, libavfilter, libavutil, libswscale, libswresample).
The GUI also needs Qt 6 (Widgets, OpenGL Widgets, Network); without it, `make`
builds the CLI only. On Arch: `pacman -S ffmpeg sdl3 qt6-base`.

```sh
make
./build/quillreel inspect movie.bk2
./build/quillreel inspect --json *.bk2
./build/quillreel convert movie.bik movie.mp4
./build/quillreel convert --no-video --all-audio movie.bk2 audio.mka
./build/quillreel convert movie.bik frames/%05d.png
./build/quillreel convert --quality small --height 720 movie.bk2 small.mp4
./build/quillreel play movie.bk2
./build/quillreel play --audio-track 2 --start 60 movie.bk2
./build/quillreel-gui ~/Videos/bink        # the GUI, with a folder of Bink files
sudo make install            # installs quillreel and quillreel-gui (PREFIX=/usr/local)
```

Run `quillreel --help` for every option and the player's keys.

The Windows build is cross-compiled from Linux with mingw-w64 against MSYS2's
libraries: `make package-windows` (needs podman; see packaging/README.md).

## Test

```sh
make test                                   # CLI, convert and headless playback tests
QUILLREEL_SAMPLES=/videos/a:/videos/b make test  # also tests against real Bink files
QUILLREEL_BINK2_DLL=/path/to/bink2w64.dll QUILLREEL_SAMPLES=... make test  # plus Bink 2 video
```

The tests ignore any installed `bink2w64.dll`; pass one with `QUILLREEL_BINK2_DLL`.

Playback tests run in real time with SDL's dummy drivers and Qt's offscreen platform
(no window, no sound) and take about two minutes.

## Layout

- `src/`: `inspect.*`, `convert.*` and `player.*` (library code a future GUI can reuse), `play_sdl.*` (SDL3 player window), `output_format.*` (extension → encoders), `report.*` (inspect output), `main.cpp` (CLI)
- `src/bink2_dll.*`, `src/pe_image.*`, `src/win32_shim.*`: running a user-supplied `bink2w64.dll` (docs/bink2.md)
- `src/gui/`: the Qt GUI (`main_window`, `player_widget`, `video_view`, `qt_audio_sink`), see specs/gui.md
- `data/`: desktop entry and icon (SVG sources in `data/icons/src/`; `data/icons/render.sh` makes the PNG sizes)
- `specs/`: behavior specs (observed) and design specs
- `tests/`: test runner, ffprobe comparison, convert and playback tests
- `docs/LEGAL.md`: legal notes and repository rules

License: GPL-3.0-or-later.
