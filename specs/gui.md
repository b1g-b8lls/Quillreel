# Design spec: `quillreel-gui`

Status: file browser and player in v0.4.0; convert dialog and Bink 2 setup in
v0.5.0. Code in `src/gui/`. This is our own design; `behavior-observed.md` has no
notes on RADTools' window yet.

## Build

A separate program, `quillreel-gui`, built by `make` when Qt 6 (Widgets,
OpenGLWidgets, Network) is installed. It also uses SDL3, for sound only. The CLI never needs Qt. Both programs
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
- **Info panel** (bottom): a summary of the selected files in plain words (name
  and folder, type, length, picture, whether the video can be shown, sound
  tracks, size; `src/gui/file_details.*`). View > Technical Details (remembered)
  shows the `quillreel inspect` text instead.
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
- **Lossless MKV**: a format entry of its own (MKV with FFV1 and FLAC fixed).
- **Codecs**: video and audio codec menus with the choices the format holds
  (specs/converter.md); hidden for formats without a choice and for Lossless MKV.
  Codecs this FFmpeg build lacks are disabled. Remembered.
- **Part**: "Only from … to …" (seconds or m:ss), applied to every file.
- **Video**: include or leave out. **Audio**: first track, all tracks (formats
  that hold several), a track number, or none. **Replace existing files**: off
  by default, so existing outputs are skipped.
- **Quality** (the CLI's quality options, specs/converter.md):
  - Video quality: Best (the default), High, Medium, Small file, or a custom CRF.
  - Encoding speed: Normal, Fast or Slow.
  - Size: Original, 1080p, 720p, 480p, 360p, or a custom height.
  - Audio bitrate: the default, or 64–320 kb/s.
  Rows the chosen format and codec can't use are greyed out, and their values
  are not applied. The custom CRF goes up to 51 for H.264/H.265 and 63 for
  VP9/AV1.
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

## Open in HandBrake

File > Open in HandBrake… (and the list's context menu). HandBrake can't read
Bink, so the convert dialog opens preset to Lossless MKV with all tracks, saving
to `Videos/Quillreel for HandBrake` (several files: a dated subfolder, so
HandBrake's batch scan sees just them). When the batch is done:

- Linux, `ghb` on PATH: `ghb -d FILE` (one file) or `ghb -d FOLDER`.
- Linux, the `fr.handbrake.ghb` Flatpak: `flatpak run --file-forwarding
  fr.handbrake.ghb -d @@ FILE @@`, which grants the sandbox that file. Forwarding
  can't pass a folder, so with several files it gets the first and the folder
  opens.
- Windows (`HandBrake.exe` on PATH or in Program Files): HandBrake's GUI
  documents no way to be given a file, so Quillreel starts it, opens the folder,
  and says to drag the files in.
- Not found: a message pointing to handbrake.fr. Quillreel never bundles HandBrake.

Tested with a stand-in `ghb` (`--test-handbrake`); not yet with a real HandBrake.
Credit for the idea: AudiFormer.

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

## Updates

`src/gui/updater.*`. Quillreel updates itself from its GitHub releases
(`https://api.github.com/repos/b1g-b8lls/Quillreel/releases/latest`, which
skips drafts and pre-releases). How depends on how it was installed:

| Build | Detected by | Update |
|-------|-------------|--------|
| AppImage | `$APPIMAGE` is set | downloads `Quillreel-<v>-x86_64.AppImage` next to the running one (hidden `.part` file), then renames it over the real file (symlinks resolved), keeping its permissions plus the executable bits. The running copy keeps its old file. Offers to restart. |
| Windows installer | `uninstall.exe` next to the program | downloads `Quillreel-<v>-windows-x86_64-setup.exe` to the temp folder and starts it with `/S /UPDATE` (UAC asks), then closes. The installer waits up to 30 s for `quillreel-gui.exe` to be free, installs with the components chosen at first install (kept in `HKLM\Software\Quillreel\Components`), and starts Quillreel again through Explorer so it doesn't run elevated. |
| everything else (.deb, source build, unpacked Windows folder) | | only reports the new version, with a button to open the release page. apt owns the .deb's files. |

- **Checking**: Help > Check for Updates… reports every outcome. On first start
  (not with test options or the offscreen platform, where it would block) Quillreel
  asks once whether to check automatically; Help > Check for Updates
  Automatically changes it. Automatic checks run 2 s after start, at most every
  20 hours, and stay silent unless there is a newer version that wasn't skipped
  ("Skip This Version"). Settings: `updates/automatic`, `updates/last_check`,
  `updates/skip`.
- **Verification**: a file is only offered if its download URL starts with
  `https://github.com/b1g-b8lls/Quillreel/releases/download/` and the API gives
  its SHA-256 (`digest`). The download must have exactly that size and hash, or it
  is deleted and nothing changes. This catches corrupt and tampered downloads,
  not a compromised GitHub account (releases are not signed).
- **Release names**: the updater finds files by name, so releases must keep the
  tag `vX.Y.Z` and the asset names above.
- The request sends `User-Agent: Quillreel/<version>` and nothing else about the
  user. HTTPS uses Qt's TLS plugins: OpenSSL in the AppImage and the distro
  packages, Schannel on Windows.

## Keys

The same as `quillreel play` (specs/player.md), as menu shortcuts that work
anywhere in the window (0–9 work as shortcuts without menu entries, and the
Playback menu lists the rest with their keys). The exception is volume: Up/Down move through the file
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

- **Sound**: the same SDL3 audio stream as `quillreel play` (`src/sdl_audio_sink.*`).
  It used Qt Multimedia's `QAudioSink` until 0.7.0, which found no audio
  device under Wine (and most likely on some Windows systems): files then played
  silently and audio-only ones (Bink 2 without the DLL) not at all.
- **Transport icons**: drawn in the palette's colours (`src/gui/glyphs.*`) and
  redrawn when the palette changes, because the styles' built-in media icons are
  fixed dark pixmaps, unreadable on dark themes. Mute and Loop are checkable
  buttons drawn in the highlight colour when on; Mute's tooltip says "Unmute"
  while muted. `--test-color-scheme dark|light` forces either look for screenshots.
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
`--test-dialog convert|bink2`, `--test-update check|install` (with
`QUILLREEL_UPDATE_API` pointing at a local fake API; downloads must then come
from that server) and `--allow-non-bink`. It covers:
- real-time playback, pause, seeking back, track switching and looping;
- the file list (real samples and a broken file);
- Bink 1 playback, and Bink 2 with and without the DLL;
- conversions through the dialog: formats, track choice, skipping and
  replacing existing files, missing tracks, cancelling, and Bink 2 with and
  without the DLL;
- installing and removing the DLL, with the list refreshing;
- updates: finding a newer release, not offering a download that is from
  elsewhere, lacks a checksum or doesn't fit this build, rejecting bad tags and
  a missing release, replacing the AppImage, and refusing a download whose
  checksum doesn't match.
Offscreen Qt has no OpenGL, so the picture itself is checked only by hand or with
`--test-screenshot` on a real display.

## Next

- Open questions: should the list show more (bit rate, keyframes)? Thumbnails?
  Remember the file list between runs?
