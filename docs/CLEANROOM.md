# Clean-room rules

Quillreel is a native Linux tool for working with Bink video files (.bik, .bk2):
inspect, play and convert them. It is an independent clean-room design. It is
not affiliated with, endorsed by, or derived from RAD Game Tools / Epic Games
software. "Bink" and "RAD Tools" are their trademarks and are used here only to
name the file format we read.

## Roles

| Role | Who | May look at | Produces |
|------|-----|-------------|----------|
| **Spec side** | the project owner | the original tool from the outside: its UI, command-line options, input/output files and public documentation | plain-language behavior specs in `specs/` |
| **Implementation side** | Claude Code and any code contributor | `specs/`, public documentation, FFmpeg's public API and docs | code in `src/`, tests in `tests/` |

The implementation side never sees disassembly, decompiler output, RAD binaries,
RAD SDK headers or RAD source code. The spec side does not paste any of those
into a spec either. Specs describe *what the program does*, not *how its code does it*.

## Rules

1. No RAD code, assets, icons, UI strings or SDK headers go into this repository.
2. Every spec records its source for each fact: "observed by running X with Y",
   "FFmpeg docs", "ffprobe output on user-supplied sample", and so on.
3. Unknown details are written down as open questions. Nobody guesses from
   memory of the original program's code.
4. Sample media files are never committed (`.gitignore` blocks `*.bik`, `*.bk2`
   and `*.binka`). Use only files you have the right to use. Tests take a
   sample directory from `QUILLREEL_SAMPLES`.
5. Decoding is done by FFmpeg's libavcodec/libavformat, an independent
   implementation. Quillreel does not contain its own Bink decoder.

## Bink 2 video: outside the clean room

Decided by the project owner on 2026-11-21. Bink 2 video is decoded by RAD's own
`bink2w64.dll`, which each user supplies from a game they own. Quillreel never
ships, downloads or installs it. Making Quillreel load and drive that DLL meant
running it as a black box and reading parts of its disassembly: the frame-buffer
struct layout, the `__chkstk` stack probe and the Windows functions it imports.
That work is documented in `docs/bink2.md`. The repository still contains no RAD
code, headers or binaries (rule 1). The rest of the project (inspect, Bink 1,
audio, and the RADTools-observed behaviour) keeps the roles above.

## License

GPL-3.0-or-later (see `LICENSE`). The FFmpeg build that Arch Linux ships is
configured with `--enable-gpl --enable-version3`, so a binary linked against it
is covered by GPLv3 anyway.

## Not legal advice

The points below still need to be confirmed. None of this is legal advice.

- Read the RADTools EULA before doing any black-box observation, and note
  anything it says about reverse engineering or benchmarking.
- Interoperability reverse engineering is treated differently from country to
  country (EU Software Directive art. 6, US DMCA §1201(f), and so on).
- Encoding *to* Bink is out of scope until the legal position is clear.
- Running a user-supplied `bink2w64.dll` (see above): check RAD's runtime licence
  terms as shipped with the game the DLL comes from.
