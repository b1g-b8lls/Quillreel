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
- **Bink 2 files** play their video through a user-supplied bink2w64.dll
  (docs/bink2.md). Without it they open a small audio-only window with the file
  name, the reason, the position and the controls, and a note goes to stderr.
  A Bink 2 file without audio then exits with code 4.
- If no audio device can be opened, the player warns and runs silently.
- Exit codes are the same as for the other commands; 5 = no window could be opened.

## Keys (YouTube's, plus our own)

| Key | Action |
|-----|--------|
| Space, K | pause / resume |
| J / L | back / forward 10 s |
| ← / → | back / forward 5 s (Shift: 30 s); seeking past the end finishes playback |
| , / . | previous / next frame (pauses first) |
| 0–9 | jump to 0–90 % of the file |
| Home / End | start / end (End finishes playback) |
| ↑ / ↓ | volume ±10% |
| M | mute |
| A / Shift+A | next / previous audio track (resumes at the same position) |
| R | toggle looping ("repeat"; it used to be L, which is now YouTube's 10 s forward) |
| F, double-click | fullscreen |
| Esc | leave fullscreen, otherwise quit |
| Q | quit |

The status overlay (time, track, volume) shows for 2 s after a key press, the
whole time while paused, and always in audio-only mode. It uses SDL's built-in
debug font, so no font files ship with the program.

## Frame stepping

Bink files usually have a single keyframe, so seeking back means decoding from
the start (about 11 s to reach the end of a 2-minute 1080p Bink 2 file; RAD's
`BinkGoto` is no faster). Stepping back can't be a seek, so `MediaSource` keeps:

- **Frame history**: every frame handed to the front end, and during a seek the
  frames decoded before the target, up to 120 frames or 192 MB (about 2 s of
  1080p). `step_back()` returns the previous frame and puts the current one back
  at the front of the queue, so stepping forward again replays it. These are
  extra references to the same picture buffers, not copies.
- **Audio history**: the last 10 s of samples sent to the sink, including the
  up to 3 s a seek decodes before its target. After each step, `align_audio(t)`
  makes the sink start at the frame shown: it clears the sink and queues the
  history from t again, or tells the worker to drop decoded audio up to t if t
  is ahead of it. Resuming is then instantly in sync. A stretch older than the
  audio history plays as silence; it never seeks, as a seek would empty the
  frame history.

Stepping back past the frame history seeks to just after the previous frame's
start (0.25 of a frame), which decodes again but fills the history with the
frames before it. Keys pressed while a seek runs are ignored. Before a step the
front end shows any frame already due, so the frame on screen is always the last
one handed out (the history's newest).

A seek keeps the frame on screen at its target (the last starting at or before
it), so seeking while paused shows the new position at once.

## Design

- **Worker thread** (`MediaSource`): demuxes, decodes, converts video to YUV420P
  and audio to interleaved float, and pushes them to a 6-frame video queue and
  an `AudioSink` (up to 1.5 s buffered). It has no UI code, so a Qt/GTK front
  end can reuse it.
- **Clock**: audio is the master clock (the sample being heard =
  written − buffered). That position moves in device-sized steps (tens of ms),
  so the wall clock eases towards it (~250 ms time constant) and jumps only for
  errors over 150 ms. Jumping every time skipped frames. The wall clock takes
  over when there is no audio or the audio has run out. At start-up and after a
  seek, the clock waits for the first frame (or for the audio, when there is no
  video). Late frames are dropped.
- **Seeking**: FFmpeg's Bink demuxer always seeks back to the start of the file
  [observed: `av_seek_frame` with and without `AVSEEK_FLAG_ANY`, user samples],
  and every sample has a single keyframe. A forward seek past everything read
  so far, with no keyframe in between, just keeps decoding from where the
  worker is (about 0.15 s for 5 s at 720p). Any other seek rewinds, skips video packets without decoding up to the last indexed
  keyframe at or before the target, then decodes and drops frames and audio
  samples before the target. Audio-only seeks are fast. Backward video seeks in
  single-keyframe files decode from the start: Bink 1 at 720p decodes about 800
  frames/s, so a seek to 110 s into OblivionIntro takes about 4 s.
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
frame steps (exact frame, audio aligned, resuming in sync, stepping back past the
history decoding once), a paused seek showing the frame at the target, J/L, End and 0–9,
errors, and forward seeks not rewinding (hidden `rewinds` counter). On real Bink 1
samples: every frame shown in real time, video with audio ending exactly, and the
forward/backward seek behaviour. On real Bink 2 samples: audio-only playback from a start position ends
exactly at the end of the audio (±10 ms), plus track switching and seeking back.

A ThreadSanitizer build reports no races in Quillreel's code (its only report is
inside libdbus, used by SDL). ASan/UBSan builds are clean, with no leaks.

## Known gaps / open questions

- Bink 2 audio playback, track switching and seeking were checked by a person on
  real speakers. Bink 1 video playback was checked by a person on a real screen.
- Bink 2 video needs the user's bink2w64.dll. Checked headless (every frame shown in
  real time, seeking back, late `--start`); still to be checked on a real screen.
- Audio is about one device buffer (~10–20 ms) early relative to the picture,
  because device latency isn't accounted for.
- 5.1/7.1 Bink audio would play in FFmpeg's default channel order. Check against
  SDL's expected order once a multichannel sample turns up.
- Alpha in Bink 1 (YUVA) is ignored: the picture shows over black.
- No frame stepping, speed control or playlist. What does RADTools offer?
