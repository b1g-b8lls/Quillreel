# Release packages

```sh
make package          # all of the below, into dist/
make package-debian13 # .deb for Debian 13 + the AppImage
make package-ubuntu26.04
```

Needs `podman` (rootless is fine). Each target builds a container image from
`packaging/Containerfile` with `BASE` set to the distro, then runs
`packaging/build.sh` inside it. The repository is mounted read-only and the
packages are written to `dist/`. The first build downloads the images and
packages (a few minutes); later builds take about a minute.

| File | For | Notes |
|------|-----|-------|
| `quillreel_<v>-1.deb13_amd64.deb` | Debian 13 | uses Debian's FFmpeg 7.1, Qt 6.8, SDL3 |
| `quillreel_<v>-1.ubuntu26.04_amd64.deb` | Ubuntu 26.04 LTS | FFmpeg 8, Qt 6.10, SDL 3.4 |
| `Quillreel-<v>-x86_64.AppImage` | any desktop distro with glibc 2.41+ | bundles Qt, FFmpeg, SDL3, x264 |
| `Quillreel-<v>-x86_64.AppImage-sources.md` | (with the AppImage) | the Debian source packages of the bundled libraries, for the GPL/LGPL source offer (docs/LEGAL.md) |

Each `.deb` is linked against its distro's own libraries, so it only installs
there; `dpkg-shlibdeps` works out the dependencies. Each holds both programs,
the desktop entry, icons and docs, and recommends the Qt GTK3 and desktop portal
theme plugins. `MAINTAINER="Name <email>"` sets the package's Maintainer field
(default: `b1g-b8lls <b1gb8lls@tutamail.com>`).

## AppImage

- Run with no arguments it starts the GUI. With a command (`inspect`, `convert`,
  `play`, `bink2`, `--help`, `--version`) it runs the CLI. See `packaging/AppRun`.
- Desktop look: like Qt does for native installs, it uses the GTK3 theme on
  GNOME, Cinnamon, MATE, XFCE, Budgie, Pantheon, Unity and LXDE, and the desktop
  portal theme on KDE and LXQt (the system's own Qt plugins can't be loaded into
  the bundled Qt). Set `QT_QPA_PLATFORMTHEME` to override. When the desktop asks
  for dark mode but the theme only has light colours, Quillreel uses a dark
  palette (src/gui/main_gui.cpp).
- Libraries every desktop has are taken from the system, as is usual for
  AppImages, because they must match its graphics drivers and display server:
  OpenGL/EGL, X11/XCB, Wayland, fontconfig/freetype/harfbuzz, ALSA, PipeWire.
  So it is meant for desktops; on a headless server use the `.deb`.
- Built with linuxdeploy and linuxdeploy-plugin-qt (downloaded into the image;
  `build.sh` prints their checksums). Plugins added beyond the defaults: the
  GTK3 and portal themes, Wayland EGL (needed for the video view on Wayland), and
  offscreen (headless tests).
- Without FUSE, run it with `--appimage-extract-and-run` or set
  `APPIMAGE_EXTRACT_AND_RUN=1`.

## Checked (0.6.0)

- Debian 13 and Ubuntu 26.04 `.deb`s, installed in clean containers:
  - install, `--version`, Bink 1 to MP4, Bink 2 video (with the DLL) to WebM;
  - GUI file list and playback of all 262 frames;
  - clean removal.
- AppImage on clean Debian 13, Fedora 45, Ubuntu 26.04 and Arch (with only the
  standard desktop libraries added): the same checks.
- AppImage on a KDE Plasma desktop (Wayland): video renders, and the portal theme
  follows dark mode. As GNOME: the GTK3 theme, also dark.
- Not included: Bink 2's `bink2w64.dll`. Users add their own (Help > Bink 2 Video).
