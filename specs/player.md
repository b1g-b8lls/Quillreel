# Design spec: `quillreel play`

Status: implemented in v0.3.0 (`src/player.cpp` core, `src/play_sdl.cpp` SDL3 front end).
This spec is a design for our own tool. It does not describe what RADTools does.
Once `behavior-observed.md` §2 has notes on RADTools' player, compare the two and
record any differences here (especially the key bindings).

## Command

```
quillreel play [--audio-track N] [--start SECONDS] [--volume 0-100] [--loop] [--fullscreen]
               [--no-video] [--no-audio] [--verbose] FILE
```

- One file at a time. The window opens at the video's display size, shrunk to
  fit 90% of the screen, and can be resized. The picture is letterboxed.
- **Bink 2 files** (FFmpeg can't decode their video) open a small audio-only
  window with the file name, the reason, the position and the controls, and a
  note goes to stderr. A Bink 2 file without audio exits with code 4.
- If no audio device can be opened, the player warns and runs silently.
- Exit codes are the same as for the other commands; 5 = no window could be opened.

## Keys (our own choice; to compare with RADTools)

| Key | Action |
|-----|--------|
| Space, K | pause / resume |
| ← / → | seek 5 s (Shift: 30 s); seeking past the end finishes playback |
| Home | restart |
| ↑ / ↓ | volume ±10% |
| M | mute |
| A / Shift+A | next / previous audio track (resumes at the same position) |
| L | toggle looping |
| F, double-click | fullscreen |
| Esc | leave fullscreen, otherwise quit |
| Q | quit |

The status overlay (time, track, volume) shows for 2 s after a key press, the
whole time while paused, and always in audio-only mode. It uses SDL's built-in
debug font, so no font files ship with the program.

## Design

- **Worker thread** (`MediaSource`): demuxes, decodes, converts video to YUV420P
  and audio to interleaved float, and pushes them to a 6-frame video queue and
  an `AudioSink` (up to 1.5 s buffered). It has no UI code, so a Qt/GTK front
  end can reuse it.
- **Clock**: audio is the master clock (the sample being heard =
  written − buffered). The wall clock follows it whenever they drift more than
  40 ms apart, and takes over when there is no audio or the audio has run out.
  Late frames are dropped.
- **Seeking**: FFmpeg's Bink demuxer always seeks back to the start of the file
  [observed: `av_seek_frame` with and without `AVSEEK_FLAG_ANY`, user samples].
  So a seek rewinds, skips video packets without decoding up to the last indexed
  keyframe at or before the target, then decodes and drops frames and audio
  samples before the target. Audio-only seeks are fast. Video seeks in files with
  few keyframes have to decode from the start (the samples have 1–2 keyframes).
- **Bink audio timestamps**: only the first frame decoded from each packet has a
  timestamp (the first packet holds 0.76 s = 19 frames)
  [observed: ffprobe -show_frames on user samples]. Positions are continued
  from the sample count.
- Bink stores audio about 0.76 s ahead of the matching video
  [observed: packet order on user samples], which is why the audio buffer
  allows more than that.
- When the stream ends, the worker flushes the sink. SDL holds back a few samples
  while resampling (e.g. switching to a track at another sample rate) until it's
  told the stream has ended.

## Verified

`tests/test_play.py` runs headless with SDL's dummy drivers, injecting keys with the
hidden `--test-keys` option and reading statistics from the hidden `--report` option:
real-time pacing, every frame shown with none dropped, `--start` accuracy, pause,
seeking back and past the end, switching tracks across sample rates, looping,
errors. On real Bink 2 samples: audio-only playback from a start position ends
exactly at the end of the audio (±10 ms), plus track switching and seeking back.

A ThreadSanitizer build reports no races in Quillreel's code (its only report is
inside libdbus, used by SDL). ASan/UBSan builds are clean, with no leaks.

## Known gaps / open questions

- **Never watched or heard by a person yet**: tests use dummy drivers. Picture,
  colours and sound on real hardware need checking.
- **Bink video has never been played**: no Bink 1 sample, and FFmpeg has no Bink 2
  video decoder. The video path is only tested on FFV1 test clips.
- Audio is about one device buffer (~10–20 ms) early relative to the picture,
  because device latency isn't accounted for.
- 5.1/7.1 Bink audio would play in FFmpeg's default channel order. Check against
  SDL's expected order once a multichannel sample turns up.
- Alpha in Bink 1 (YUVA) is ignored: the picture shows over black.
- No frame stepping, speed control or playlist. What does RADTools offer?
