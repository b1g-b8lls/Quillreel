# Legal notes and repository rules

Quillreel is a native Linux tool for working with Bink video files (.bik, .bk2):
inspect, play and convert them. It is not affiliated with or endorsed by RAD
Game Tools or Epic Games. "Bink" and "RAD Tools" are their trademarks and are
used here only to name the file format Quillreel reads.

## Repository rules

1. No RAD code, binaries (including `bink2w64.dll`), assets, icons, UI strings
   or SDK headers go into this repository. `.gitignore` blocks `*.dll`.
2. Sample media files are never committed (`.gitignore` blocks `*.bik`, `*.bk2`
   and `*.binka`). Use only files you have the right to use. Tests take a
   sample directory from `QUILLREEL_SAMPLES`.
3. Bink 1 video and Bink audio are decoded by FFmpeg. Bink 2 video is decoded
   only by RAD's own `bink2w64.dll`, which each user supplies from a game they
   own (see `docs/bink2.md`); Quillreel never ships, downloads or installs it.

## License

GPL-3.0-or-later (see `LICENSE`). The FFmpeg build that Arch Linux ships is
configured with `--enable-gpl --enable-version3`, so a binary linked against it
is covered by GPLv3 anyway.

## Binary packages

The `.deb` packages contain only Quillreel and depend on the distribution's own
FFmpeg, Qt and SDL. The AppImage bundles third-party libraries: Qt (LGPL-3.0),
FFmpeg (built with GPL options), x264 (GPL-2.0+), SDL3 (zlib) and their
dependencies. Whoever distributes the AppImage must make the corresponding source
code of the GPL/LGPL parts available, e.g. by linking the Debian 13 source
packages of the exact versions bundled (`apt-get source` in the build container)
next to the release. `packaging/build.sh` builds them from Debian 13's archive.

## Not legal advice

The points below still need to be confirmed. None of this is legal advice.

- Interoperability reverse engineering is treated differently from country to
  country (EU Software Directive art. 6, US DMCA §1201(f), and so on).
  `docs/bink2.md` records what was learned from RAD's DLL to load and drive it.
- Running a user-supplied `bink2w64.dll`: check RAD's runtime licence terms as
  shipped with the game the DLL comes from. Loading a proprietary DLL at runtime
  from a GPL program is generally treated more leniently when users supply it
  themselves, but check before publishing.
- Encoding *to* Bink is out of scope.
