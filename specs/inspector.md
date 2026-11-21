# Design spec: `quillreel inspect`

Status: implemented in v0.1.0 (`src/inspect.cpp`, `src/report.cpp`).
This spec is a design for our own tool. It does not describe what RADTools does.
Once `behavior-observed.md` has notes on RADTools' file-info view, compare the
two and record any differences here.

## Command

```
quillreel inspect [--json] [--verbose] FILE...
```

- One text block per file, separated by blank lines. Errors go to stderr.
- `--json` prints one JSON array to stdout with one object per file. A file that
  failed gets an object with `path`, `error` and `exit_code` instead.
- `--verbose` lets FFmpeg warnings through. By default only errors are shown.
- Exit codes: 0 ok, 1 usage error, 2 cannot open/recognise the file, 3 a media
  file that is not Bink. With several files, the highest code wins.

## Fields and where they come from

All values come from FFmpeg's Bink demuxer after `avformat_open_input` alone.
No packets are read, so a 400 MB file takes about 20 ms.

| Field | Source |
|-------|--------|
| fourcc | video stream `codec_tag`, little-endian, e.g. `KB2g` [ffprobe on user samples] |
| generation | fourcc starting `BIK` → Bink 1, `KB2` → Bink 2 [FFmpeg bink demuxer doc/behavior; Bink 1 not yet tested with a real file] |
| revision | 4th character of the fourcc |
| width, height, frame_rate | video `codecpar`, `avg_frame_rate` |
| frame_count | video stream `duration` rescaled to frames (the demuxer stores the header frame count there) [matches ffprobe `duration_ts`] |
| keyframe_count | demuxer index entries with `AVINDEX_KEYFRAME` [matches ffprobe packet `K` flags on 2 samples] |
| decoder | `avcodec_find_decoder`. `null` means this FFmpeg build cannot decode the video |
| duration | frame_count × fps⁻¹ (`fmt->duration` is unset without `find_stream_info`) |
| bit_rate | file_size × 8 / duration |
| audio[] | one entry per audio stream: codec, sample rate, channel count, demuxer track id |

## Verified

`tests/compare_ffprobe.py` checks every field above except keyframes against
ffprobe. 14/14 user-supplied Bink 2 (KB2g) samples match.

## Known gaps / open questions

- **No Bink 1 test file yet.** Every local sample is Bink 2 revision `g`.
- **FFmpeg has no Bink 2 video decoder.** Bink 2 video shows `codec_name=unknown`.
  The demuxer and the Bink audio decoders still work. This blocks video for
  `play`/`convert` on .bk2 files.
- What do the audio track ids mean (language? channel role?). AGILITY.bk2 numbers
  them 1–5, GameIntro_V3_B.bk2 numbers them 0–5.
- Header fields FFmpeg doesn't expose (largest frame size, header flags such as
  alpha or grayscale) are not shown. Do we need them? See what RADTools displays.
