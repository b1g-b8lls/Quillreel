# Quillreel

A native Linux tool for Bink video files (.bik / .bk2): inspect, convert and play
them. Built on FFmpeg and SDL3. It is a clean-room project, not affiliated with RAD Game Tools or Epic
Games. See [docs/CLEANROOM.md](docs/CLEANROOM.md).

## Status

| Command | State |
|---------|-------|
| `inspect` | done: header info, text or JSON |
| `convert` | done: mp4/mov/mkv/webm, PNG sequence, wav/flac/m4a/ogg/opus/mp3/mka |
| `play` | done: window with sound, seeking, audio track switching, looping |
| GUI | later, wrapping the same code |

FFmpeg decodes Bink 1 video and Bink audio, but **it has no Bink 2 video decoder**.
For .bk2 files, `inspect` shows everything, `convert --no-video` extracts the audio
and `play` plays the audio only. The video can't be decoded yet.

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
```

Run `quillreel --help` for every option and the player's keys.

## Test

```sh
make test                                   # CLI, convert and headless playback tests
QUILLREEL_SAMPLES=/path/to/videos make test # also tests against real Bink files
```

Playback tests run in real time with SDL's dummy drivers (no window, no sound) and
take about a minute.

## Layout

- `src/`: `inspect.*`, `convert.*` and `player.*` (library code a future GUI can reuse), `play_sdl.*` (SDL3 player window), `output_format.*` (extension → encoders), `report.*` (inspect output), `main.cpp` (CLI)
- `specs/`: behavior specs (observed) and design specs
- `tests/`: test runner, ffprobe comparison, convert and playback tests
- `docs/CLEANROOM.md`: project ground rules

License: GPL-3.0-or-later.
