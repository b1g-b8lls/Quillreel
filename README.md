# Quillreel

A native Linux command-line tool for Bink video files (.bik / .bk2), built on
FFmpeg. It is a clean-room project, not affiliated with RAD Game Tools or Epic
Games. See [docs/CLEANROOM.md](docs/CLEANROOM.md).

## Status

| Command | State |
|---------|-------|
| `inspect` | done: header info, text or JSON |
| `play` | planned |
| `convert` | planned |
| GUI | later, wrapping the same code |

FFmpeg decodes Bink 1 video and Bink audio, but **it has no Bink 2 video decoder**.
For .bk2 files, `inspect` shows everything, while playback and conversion would
only get audio.

## Build

Needs g++ with C++23, make, pkg-config and the FFmpeg development libraries
(libavformat, libavcodec, libavutil).

```sh
make
./build/quillreel inspect movie.bk2
./build/quillreel inspect --json *.bk2
```

## Test

```sh
make test                                   # CLI and error-path tests
QUILLREEL_SAMPLES=/path/to/videos make test # also compares every file against ffprobe
```

## Layout

- `src/`: `inspect.*` (FFmpeg probing, reusable by a future GUI), `report.*` (output), `main.cpp` (CLI)
- `specs/`: behavior specs (observed) and design specs
- `tests/`: test runner and ffprobe comparison
- `docs/CLEANROOM.md`: project ground rules

License: GPL-3.0-or-later.
