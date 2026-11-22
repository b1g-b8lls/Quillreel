# Design spec: `quillreel convert`

Status: implemented in v0.2.0 (`src/convert.cpp`, `src/output_format.cpp`).
This spec is a design for our own tool. It does not describe what RADTools does.
Once `behavior-observed.md` §3 has notes on RADTools' conversion, compare the two
and record any differences here.

## Command

```
quillreel convert [--no-video] [--no-audio] [--audio-track N | --all-audio] [-y] [--verbose]
                  [--quality high|medium|small | --crf N] [--speed fast|normal|slow]
                  [--width N] [--height N] [--audio-bitrate KBPS] INPUT OUTPUT
```

One input per run. OUTPUT's extension picks the container and encoders:

| Ext | Container | Video | Audio | Several audio tracks |
|-----|-----------|-------|-------|----------------------|
| mp4, m4v, mov | mp4/mov | libx264, CRF 18 | aac | yes |
| mkv | matroska | libx264, CRF 18 | flac | yes |
| webm | webm | libvpx-vp9 CRF 30 (fallback libsvtav1) | libopus (fallback libvorbis) | yes |
| y4m | yuv4mpegpipe | rawvideo (uncompressed, exactly as decoded) | none | none |
| png | image2 | png, one file per frame; path needs `%d`/`%05d` | none | none |
| wav / flac / m4a / ogg / opus / mp3 | audio only | none | pcm_s16le / flac / aac / libvorbis / libopus / libmp3lame | no |
| mka | matroska | none | flac | yes |

- The frame rate comes from the demuxer's `avg_frame_rate`. (`av_guess_frame_rate`
  needs `avformat_find_stream_info`, which Bink files skip, so it fell back to
  25 fps until the first Bink 1 sample caught it.)
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

## Quality options

Our own design (RADTools' options are unknown). The defaults are the settings
above (near-lossless, for archiving and editing), so a plain conversion doesn't
change.

| Option | Effect | Formats |
|--------|--------|---------|
| `--quality high\|medium\|small` | CRF level per encoder: H.264 15/23/28, VP9 24/36/42, AV1 24/38/45 (defaults 18, 30, 32) | mp4, m4v, mov, mkv, webm |
| `--crf N` | exact CRF instead: H.264 0-51, VP9 0-63, AV1 1-63 | same |
| `--speed fast\|normal\|slow` | x264 preset veryfast/medium/slow; VP9 cpu-used 6/4/2; AV1 preset 10/8/5 | same |
| `--width N`, `--height N` | resize (Lanczos). One side alone keeps the display aspect ratio; both are rounded to even numbers; scaled output has square pixels | every video format, also png and y4m |
| `--audio-bitrate K` | kb/s per audio track (default 80 per channel) | lossy audio: mp4, m4v, mov, webm, m4a, ogg, opus, mp3 |

- An option the output format can't use is a usage error (exit 1), e.g. `--crf`
  with .wav, `--audio-bitrate` with .flac, or `--height` with an audio-only
  format. So is `--crf` outside the encoder's range, `--quality` together with
  `--crf`, and video options with `--no-video`.
- [verified on a generated clip] `--quality small` < default < `high` in file
  size, `--height 120` on 320×240 gives 160×120, `--width 100` gives 100×76,
  and `--audio-bitrate 48` gives AAC at about 48 kb/s.

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
- Video from real Bink 1 samples: frame rate and frame count are kept, and PNG
  frames are pixel-exact with FFmpeg's own decode.
- Audio from real Bink 2 samples: `--no-video` WAV and `--all-audio` MKA are
  bit-exact with FFmpeg's own decode, for every track and in order. `--audio-track N`
  picks the right track (tested on a sample whose tracks differ).

## Known gaps / open questions

- Bink 2 video converts only with a user-supplied bink2w64.dll (docs/bink2.md);
  without it the refusal suggests `--no-video`. Output is bit-exact with RAD's
  decoder (checked with `.y4m`).
- No codec choice within a format (e.g. AV1 in .webm when VP9 is available), and
  no bitrate-targeted video mode. What does RADTools offer?
- Decoded Bink audio runs slightly longer than the video (10.960 s against 10.928 s
  on GameIntro_V3_B.bk2). FFmpeg's own decoder does the same. Should we trim to
  the video length?
- Audio track metadata (language) isn't set because the input carries none. Do the
  Bink track ids map to languages by convention?
- Batch conversion (many inputs, output naming) is not designed yet. See RADTools.
