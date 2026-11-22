# Bink 2 video: RAD's decoder, supplied by the user

FFmpeg has no Bink 2 video decoder, and Quillreel does not contain one either.
Instead, Quillreel runs **RAD's own decoder, `bink2w64.dll`**, which the user
copies from a game that ships it. Quillreel never includes, downloads or
installs that file. Without it, .bk2 files still inspect fully and play and
convert their audio. This part of the project is outside the clean room (see
CLEANROOM.md).

```sh
quillreel bink2 install "/path/to/Fallout 4/bink2w64.dll"   # checks it, copies it
quillreel bink2 status                                        # which DLL, does it load
quillreel bink2 remove
QUILLREEL_BINK2_DLL=/path/to/bink2w64.dll quillreel play movie.bk2   # use without installing
```

The installed copy lives at `$XDG_DATA_HOME/quillreel/bink2w64.dll` (default
`~/.local/share/quillreel/`). Only the 64-bit DLL works; a 32-bit
`bink2w32.dll` can't be loaded into a 64-bit process.

## Tested DLLs

| Source | FileVersion | Result |
|--------|-------------|--------|
| Fallout 4 (original and GOTY) | 1.992e | bit-exact |
| Fallout 4 Anniversary Edition, an older Fallout 4 backup | 1.992e (other builds) | same output as the above |
| GTA V Enhanced | 2020.07 | bit-exact; newer frame-buffer layout (below) |

"Bit-exact" means `quillreel convert x.bk2 x.y4m` matched, frame for frame and
byte for byte, what the same DLL produces in a small Windows program run under
Wine (a test harness, not part of the repo). Checked on all frames of Intro.bk2
(3944) and MainMenuLoop.bk2 (4028).

## How it works

- **`src/pe_image.*`** is a minimal PE32+ loader. It maps the DLL, applies base
  relocations, binds imports and delay-load imports, and sets page protections.
  Then it runs `DllMain`. The DLL has no TLS directory. Its only access to the
  Windows thread block is MSVC's `__chkstk` reading the stack limit
  (`mov r11, gs:[0x10]`). The loader replaces that one instruction with
  `xor r11d, r11d`, found by byte pattern so it works on other builds too. With
  a limit of 0 the probe loop is skipped, which is safe on Linux.
- **`src/win32_shim.*`** provides the Windows functions the DLL imports, called
  with the Microsoft x64 convention (`__attribute__((ms_abi))`):
  - heap, files, critical sections, threads (on pthreads), semaphores, mutexes,
    events, fiber-local storage and timers
  - the C runtime's start-up queries: code pages, environment, startup info
  Window, GDI, sound and COM imports are stubs that return 0. Bink only uses
  them for its own playback window and audio output, which Quillreel never asks
  for. Set `QUILLREEL_BINK2_TRACE=1` to log files opened, threads created and
  any call to a stubbed function.
- **`src/bink2_dll.*`** opens the file with `BinkOpen(path, BINKNOFRAMEBUFFERS)`
  and registers its own frame buffers. For each video packet from FFmpeg's Bink
  demuxer (pts = frame number) it calls `BinkDoFrame`, copies the planes into an
  AVFrame, then calls `BinkNextFrame`. A non-sequential request uses
  `BinkGoto`. To the player and converter it looks like an FFmpeg decoder
  (send packet / receive frame). Audio is still decoded by FFmpeg.

### Frame-buffer struct

`BinkGetFrameBuffersInfo` / `BinkRegisterFrameBuffers` exchange a struct whose
layout differs between SDK versions. It was found by probing both DLLs and
reading the 2020.07 build's `BinkGetFrameBuffersInfo`. Both versions share a
24-byte header: `u32 sets, y_width, y_height, c_width, c_height, current_set`.
The frame sets follow, with planes Y, cR, cB, A:

| Version | Plane | Size | Planes per set |
|---------|-------|------|----------------|
| 1.992e | `{i32 allocate; void* buffer; u32 pitch}` | 24 bytes | 4 |
| 2020.07 | `{void* buffer; i32 allocate; u32 pitch}` | 16 bytes | 5 |

Quillreel tells them apart by where the "allocate" flags of Y, cR and cB land
(32-bit words 6/12/18 against 8/12/16), and refuses unknown layouts.
`current_set` names the set holding the frame just decoded.

## Limits and open questions

- Alpha (YUVA) .bk2 files are untested; no sample has alpha.
- Other Bink 2 SDK versions may use another struct layout. They fail with a
  clear message rather than decoding garbage.
- Files with a single keyframe (all samples so far) decode from the start on a
  backward seek. That runs at about 180–330 frames/s for 1080p on the test machine.
- The decoder runs inside Quillreel's process, so a crash in the DLL takes
  Quillreel down with it.
