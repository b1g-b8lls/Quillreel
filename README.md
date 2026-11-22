# Quillreel

A native Linux tool for Bink video files (.bik / .bk2): inspect, convert and play
them. Built on FFmpeg and SDL3. Not affiliated with RAD Game Tools or Epic Games.
See [docs/CLEANROOM.md](docs/CLEANROOM.md).

## Status

| Command | State |
|---------|-------|
| `inspect` | done: header info, text or JSON |
| `convert` | done: mp4/mov/mkv/webm, raw .y4m, PNG sequence, wav/flac/m4a/ogg/opus/mp3/mka |
| `play` | done: window with sound, seeking, audio track switching, looping |
| `bink2` | done: add RAD's Bink 2 decoder from a game (see below) |
| GUI | later, wrapping the same code |

FFmpeg decodes Bink 1 video and Bink audio, but it has no Bink 2 video decoder, and
**Quillreel doesn't ship one**. Bink 2 video is decoded by RAD's own `bink2w64.dll`,
which you copy from a game that uses Bink 2 (Fallout 4 and GTA V Enhanced are tested):

```sh
quillreel bink2 install "/path/to/Fallout 4/bink2w64.dll"
```

Quillreel loads the DLL directly; Wine is not needed. Without it, .bk2 files still
inspect fully, and `play`/`convert` handle their audio only. Details:
[docs/bink2.md](docs/bink2.md).

## Build

Needs g++ with C++23, make, pkg-config, SDL3 and the FFmpeg development libraries
(libavformat, libavcodec, libavfilter, libavutil, libswscale, libswresample).
On Arch: `pacman -S ffmpeg sdl3`.

```sh
make
./build/quillreel inspect movie.bk2
./build/quillreel inspect --json *.bk2
./build/quillreel convert movie.bik movie.mp4
./build/quillreel convert --no-video --all-audio movie.bk2 audio.mka
./build/quillreel convert movie.bik frames/%05d.png
./build/quillreel play movie.bk2
./build/quillreel play --audio-track 2 --start 60 movie.bk2
sudo make install            # installs only the quillreel binary (PREFIX=/usr/local)
```

Run `quillreel --help` for every option and the player's keys.

## Test

```sh
make test                                   # CLI, convert and headless playback tests
QUILLREEL_SAMPLES=/videos/a:/videos/b make test  # also tests against real Bink files
QUILLREEL_BINK2_DLL=/path/to/bink2w64.dll QUILLREEL_SAMPLES=... make test  # plus Bink 2 video
```

The tests ignore any installed `bink2w64.dll`; pass one with `QUILLREEL_BINK2_DLL`.

Playback tests run in real time with SDL's dummy drivers (no window, no sound) and
take about a minute.

## Layout

- `src/`: `inspect.*`, `convert.*` and `player.*` (library code a future GUI can reuse), `play_sdl.*` (SDL3 player window), `output_format.*` (extension → encoders), `report.*` (inspect output), `main.cpp` (CLI)
- `src/bink2_dll.*`, `src/pe_image.*`, `src/win32_shim.*`: running a user-supplied `bink2w64.dll` (docs/bink2.md)
- `specs/`: behavior specs (observed) and design specs
- `tests/`: test runner, ffprobe comparison, convert and playback tests
- `docs/CLEANROOM.md`: project ground rules

License: GPL-3.0-or-later.
