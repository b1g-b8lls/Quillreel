# Design spec: `quillreel-gui`

Status: first milestone (file browser and player) in v0.4.0, in `src/gui/`.
This is our own design; `behavior-observed.md` has no notes on RADTools' window
yet. Convert and Bink 2 setup are not in the GUI yet; see "Next".

## Build

A separate program, `quillreel-gui`, built by `make` when Qt 6 (Widgets,
Multimedia, OpenGLWidgets) is installed. The CLI never needs Qt. Both programs
share the core code (inspect, the playback core `MediaSource`, convert, and the
Bink 2 DLL loader). `make install` also installs `data/quillreel.desktop`.

```
quillreel-gui [FILE|FOLDER...]
```

Files and folders on the command line, or dropped on the window, are added to
the list. Folders are searched recursively for `.bik`/`.bk2`.

## Window

- **File list** (left): Name, Type (Bink 1 / Bink 2; "Bink 2 (audio only)" when
  no bink2w64.dll is provided), Length, Video (size and fps), Tracks (number of
  audio tracks), Size. Sortable by any column. Files are read in the background
  with the same code as `quillreel inspect`. Unreadable files show "error", with
  the reason as a tooltip. Double-click or Enter plays a file. Up/Down move
  through the list.
- **Player** (right): the video, letterboxed to its display aspect. For audio-only
  playback it shows the file name and the reason instead, e.g. Bink 2 video not
  enabled. The transport bar has play/pause, restart, a position slider (click to
  jump, drag to seek on release), time, an audio track menu (named from the
  inspect data, e.g. "Track 1 (48000 Hz stereo)"), mute, volume, loop and
  fullscreen.
- **Info panel** (bottom): `quillreel inspect` text for the selected files.
- **Menus**: File (Open Files, Open Folder, Clear List, Quit), Playback, View
  (Fullscreen, show/hide the list and info panel), Help (Bink 2 Video status,
  About).
- Window size and splitter positions are remembered (QSettings,
  `~/.config/Quillreel/Quillreel.conf`), and so is the last folder opened.

## Keys

The same as `quillreel play` (specs/player.md), as menu shortcuts that work
anywhere in the window. The exception is volume: Up/Down move through the file
list, so volume is Ctrl+Up/Down, or plain Up/Down while the player has focus.
F or F11 (or a double-click on the video) toggles fullscreen, which shows only
the video and the transport bar. Esc leaves fullscreen. Q is not bound; use
Ctrl+Q.

## Playback

The same core as the CLI player: `MediaSource` on a worker thread, the shared
`SyncClock` (src/sync_clock.h; audio is the master clock and the wall clock
eases towards it), and the same frame dropping and end-of-file rule. A 4 ms Qt
timer drives it. At the end the player pauses on the last frame; Play starts
again from the beginning. Loop restarts instead.

- **Sound**: Qt Multimedia `QAudioSink` in pull mode, about 60 ms device buffer,
  float output (16-bit if the device refuses float). The buffered amount counts
  what the device was given within the last buffer length, so the clock and the
  end-of-file check still work when the device holds the last samples.
- **Video**: `QOpenGLWidget`. The Y, U and V planes are uploaded as textures and
  converted in a shader. The matrix is chosen like the SDL player: BT.601 limited
  range unless the frame says BT.709 and/or full range.
  [checked: SMPTE bars played in the GUI on a real display; every bar within ±2 of
  FFmpeg's own RGB conversion]

## Tests

`tests/test_gui.py` runs headless (Qt offscreen platform, own settings folder,
volume 0) using hidden options: `--test-play FILE`, `--test-keys
1.0:Right,2.0:Space` (triggers the menu action with that shortcut),
`--test-quit-after S`, `--test-dump-list`, `--report`, `--test-silent`,
`--test-screenshot S:PATH` and `--allow-non-bink`. It covers real-time
playback, pause, seeking back, track switching, looping, the file list (real
samples and a broken file), Bink 1 playback, and Bink 2 with and without the DLL.
Offscreen Qt has no OpenGL, so the picture itself is checked only by hand or with
`--test-screenshot` on a real display.

## Next

- Convert dialog: format, tracks, output folder, progress, batch over the
  selected files.
- Bink 2 page: install or remove the DLL from the GUI (today: Help > Bink 2 Video
  shows the status and the command).
- Open questions: should the list show more (bit rate, keyframes)? Thumbnails?
  Remember the file list between runs?
