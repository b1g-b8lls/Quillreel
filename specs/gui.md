# Design spec: `quillreel-gui`

Status: file browser and player in v0.4.0; convert dialog and Bink 2 setup in
v0.5.0. Code in `src/gui/`. This is our own design; `behavior-observed.md` has no
notes on RADTools' window yet.

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
- **Menus**: File (Open Files, Open Folder, Convert (Ctrl+E), Clear List, Quit),
  Playback, View (Fullscreen, show/hide the list and info panel), Help (Bink 2
  Video, About). Right-clicking the list gives Play, Convert, Open Containing
  Folder and Remove from List.
- **Look**: the desktop's Qt theme (Plasma/Breeze on KDE, GTK3 on GNOME and
  other GTK desktops). If the desktop asks for dark but the theme only has light
  colours, a dark palette is used. A colour scheme change while running is
  followed, including Breeze's menu bar colour, which plain Qt apps otherwise
  keep from startup (the style is re-created on the change).
  [checked: Breeze ↔ Breeze Dark switched live, colour scheme and global theme]
- Window size and splitter positions are remembered (QSettings,
  `~/.config/Quillreel/Quillreel.conf`), and so is the last folder opened.

## Convert dialog

File > Convert converts the selected files, or every listed file if none are
selected, with the same code as `quillreel convert`. They are converted one after
another on a worker thread.

- **Format**: every format the CLI supports, described ("MP4 (H.264 video, AAC
  audio)"). Formats whose encoder this FFmpeg build lacks are shown but disabled.
- **Save to**: a folder, created if needed. Output names are the input name with
  the new extension, made unique within the batch ("Intro (2).mp4"). A PNG
  sequence gets a folder per file (`Intro/00001.png`, …).
- **Video**: include or leave out. **Audio**: first track, all tracks (formats
  that hold several), a track number, or none. **Replace existing files**: off
  by default, so existing outputs are skipped.
- **Quality** (the CLI's quality options, specs/converter.md):
  - Video quality: Best (the default), High, Medium, Small file, or a custom CRF.
  - Encoding speed: Normal, Fast or Slow.
  - Size: Original, 1080p, 720p, 480p, 360p, or a custom height.
  - Audio bitrate: the default, or 64–320 kb/s.
  Rows the chosen format can't use are greyed out, and their values are not
  applied. The custom CRF goes up to 51 for H.264 and 63 for WebM.
- A plan is made per file before starting, and anything adjusted or skipped is
  noted:
  - Bink 2 without the DLL converts its audio only; for a video-only format
    (PNG, Y4M) it is skipped.
  - "All tracks" in a one-track format gives track 0.
  - A track number the file doesn't have, or an existing output, means the file
    is skipped.
  The dialog warns about these before starting, too.
- Progress bars for the current file and the batch, a log line per file
  (✓ converted with what was written, – skipped/cancelled with why, ✗ failed
  with the error), then a summary. Open Folder opens the output folder.
- **Cancel** stops after the current packet. The file being written is deleted,
  and so are the frames a PNG sequence has written so far. Closing the dialog while it runs
  asks first. Settings are remembered.

## Bink 2 Video dialog

Help > Bink 2 Video (also the "Enable Bink 2 Video…" button the player shows for
a .bk2 file without the DLL). It shows whether Bink 2 video is enabled, the
DLL's path and version, and whether `QUILLREEL_BINK2_DLL` overrides the
installed copy.

- **Install DLL… / Replace DLL…** picks a file and runs the same check and copy
  as `quillreel bink2 install`. Errors (32-bit DLL, not a Bink DLL, unknown
  layout) are shown as they are.
- **Remove** asks first, then deletes the installed copy.
- After either one, every listed file is read again (Type switches between
  "Bink 2" and "Bink 2 (audio only)"), and a playing .bk2 is reopened at the same
  position, with or without video.

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
`--test-screenshot S:PATH`, `--test-convert EXT:DIR[,options]`,
`--test-convert-cancel-after S`, `--test-bink2 install:PATH|remove`,
`--test-dialog convert|bink2` and `--allow-non-bink`. It covers:
- real-time playback, pause, seeking back, track switching and looping;
- the file list (real samples and a broken file);
- Bink 1 playback, and Bink 2 with and without the DLL;
- conversions through the dialog: formats, track choice, skipping and
  replacing existing files, missing tracks, cancelling, and Bink 2 with and
  without the DLL;
- installing and removing the DLL, with the list refreshing.
Offscreen Qt has no OpenGL, so the picture itself is checked only by hand or with
`--test-screenshot` on a real display.

## Next

- Open questions: should the list show more (bit rate, keyframes)? Thumbnails?
  Remember the file list between runs?
