# Quillreel

A native Linux command-line tool for Bink video files (.bik / .bk2), built on
FFmpeg. It is a clean-room project, not affiliated with RAD Game Tools or Epic
Games. See [docs/CLEANROOM.md](docs/CLEANROOM.md).

## Status

| Command | State |
|---------|-------|
| `inspect` | done: header info, text or JSON |
| `convert` | done: mp4/mov/mkv/webm, PNG sequence, wav/flac/m4a/ogg/opus/mp3/mka |
| `play` | planned |
| GUI | later, wrapping the same code |

FFmpeg decodes Bink 1 video and Bink audio, but **it has no Bink 2 video decoder**.
For .bk2 files, `inspect` shows everything and `convert --no-video` extracts the
audio, but the video can't be converted yet.

## Build

Needs g++ with C++23, make, pkg-config and the FFmpeg development libraries
(libavformat, libavcodec, libavfilter, libavutil).

```sh
make
./build/quillreel inspect movie.bk2
./build/quillreel inspect --json *.bk2
./build/quillreel convert movie.bik movie.mp4
./build/quillreel convert --no-video --all-audio movie.bk2 audio.mka
./build/quillreel convert movie.bik frames/%05d.png
```

Run `quillreel --help` for every option.

## Test

```sh
make test                                   # CLI and error-path tests
QUILLREEL_SAMPLES=/path/to/videos make test # also compares every file against ffprobe
```

## Layout

- `src/`: `inspect.*` and `convert.*` (library code a future GUI can reuse), `output_format.*` (extension → encoders), `report.*` (inspect output), `main.cpp` (CLI)
- `specs/`: behavior specs (observed) and design specs
- `tests/`: test runner and ffprobe comparison
- `docs/CLEANROOM.md`: project ground rules

License: GPL-3.0-or-later.
