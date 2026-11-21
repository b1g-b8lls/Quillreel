# Design spec: `quillreel convert`

Status: implemented in v0.2.0 (`src/convert.cpp`, `src/output_format.cpp`).
This spec is a design for our own tool. It does not describe what RADTools does.
Once `behavior-observed.md` §3 has notes on RADTools' conversion, compare the two
and record any differences here.

## Command

```
quillreel convert [--no-video] [--no-audio] [--audio-track N | --all-audio] [-y] [--verbose] INPUT OUTPUT
```

One input per run. OUTPUT's extension picks the container and encoders:

| Ext | Container | Video | Audio | Several audio tracks |
|-----|-----------|-------|-------|----------------------|
| mp4, m4v, mov | mp4/mov | libx264, CRF 18 | aac | yes |
| mkv | matroska | libx264, CRF 18 | flac | yes |
| webm | webm | libvpx-vp9 CRF 30 (fallback libsvtav1) | libopus (fallback libvorbis) | yes |
| png | image2 | png, one file per frame; path needs `%d`/`%05d` | none | none |
| wav / flac / m4a / ogg / opus / mp3 | audio only | none | pcm_s16le / flac / aac / libvorbis / libopus / libmp3lame | no |
| mka | matroska | none | flac | yes |

- The pixel format is the encoder's closest match to the source, keeping alpha
  where the encoder supports it. Frame size, aspect ratio and frame rate are kept.
- Audio keeps the source sample rate and channel layout unless the encoder can't
  take them (Opus gets resampled to 48 kHz). Lossy audio runs at 80 kb/s per channel.
- Audio tracks are numbered as `inspect` lists them (`#0`, `#1`, ...). The
  default is track 0. When others are skipped, a note on stderr says so.
- Video formats with `--no-video` (e.g. `.mp4`) give audio-only files. Audio-only
  formats drop the video without needing `--no-video`.
- Damaged packets are skipped and counted, and the count is printed as a warning.
- A failed conversion deletes its partial output file. An existing output is
  never replaced without `-y`.
- Progress (`converting: 0:01:02 / 0:02:37 (40%)`) goes to stderr, only on a terminal.
- `--allow-non-bink` accepts any input FFmpeg can read. It exists for the test suite.

Exit codes: 0 ok, 1 usage, 2 cannot open input, 3 not Bink, 4 stream/format not
supported by this FFmpeg build (e.g. Bink 2 video), 5 output exists or writing failed.

## Pipeline

demux (libavformat) → decode (libavcodec) → libavfilter (`format=` for video;
`aresample,aformat=` for audio, with the sink's frame size set to the encoder's
frame size) → encode → interleaved mux. The filter graph is built from the
first decoded frame, so it matches what the decoder really outputs. Video
timestamps are rescaled to 1/fps and forced to increase. Audio timestamps
are counted in samples.

## Verified

`tests/test_convert.py`:
- Video path on a generated FFV1/MKV clip (48 frames at 24000/1001, mono and
  stereo tracks): frame count, frame rate, codecs, PSNR > 35 dB by frame
  index (actual ~47 dB), webm, png sequence, track selection, error cases.
  The mp4 output decodes identically to `ffmpeg -c:v libx264 -crf 18`.
- Audio from real Bink 2 samples: `--no-video` WAV and `--all-audio` MKA are
  bit-exact with FFmpeg's own decode, for every track and in order. `--audio-track N`
  picks the right track (tested on a sample whose tracks differ).

## Known gaps / open questions

- **Bink video has never gone through this pipeline.** FFmpeg can't decode
  Bink 2 video, and there is no Bink 1 sample. The video path is only tested on
  non-Bink input. First job once a `.bik` file turns up.
- No quality options yet (CRF, bitrate, scaling, codec choice). What does RADTools offer?
- Decoded Bink audio runs slightly longer than the video (10.960 s against 10.928 s
  on GameIntro_V3_B.bk2). FFmpeg's own decoder does the same. Should we trim to
  the video length?
- Audio track metadata (language) isn't set because the input carries none. Do the
  Bink track ids map to languages by convention?
- Batch conversion (many inputs, output naming) is not designed yet. See RADTools.
