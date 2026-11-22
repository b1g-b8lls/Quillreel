# Release packages

```sh
make package          # all of the below, into dist/
make package-debian13 # .deb for Debian 13 + the AppImage
make package-ubuntu26.04
make package-windows  # Windows installer (cross-compiled)
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
| `Quillreel-<v>-windows-x86_64-setup.exe` | Windows 10 1903+ / 11, x64 | bundles Qt 6, FFmpeg, SDL3 from MSYS2 |
| `Quillreel-<v>-windows-x86_64-setup.exe-sources.md` | (with the installer) | the MSYS2 packages of the bundled DLLs, for the GPL/LGPL source offer |

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

## Releases and the updater

Quillreel finds updates through GitHub's latest-release API and picks files by
name, so a release must be tagged `vX.Y.Z` and keep these asset names exactly:
`Quillreel-<v>-x86_64.AppImage` and `Quillreel-<v>-windows-x86_64-setup.exe`.
GitHub computes the SHA-256 the updater checks; nothing extra is needed. Mark
test builds as pre-releases: the API skips them.

## Windows installer

- Built by `packaging/Containerfile.windows` and `packaging/build-windows.sh`:
  Arch Linux's mingw-w64 GCC (which targets UCRT, like MSYS2's UCRT64
  environment) compiles against MSYS2's prebuilt UCRT64 FFmpeg, SDL3 and Qt 6.
  pacman installs those into `/opt/msys` in the image and checks their
  signatures against MSYS2's keyring. Qt's `moc` and `rcc` are the Linux ones
  from Arch's Qt, which must be the same Qt version as MSYS2's. The installer is
  made with NSIS (`makensis`, taken from Debian).
- The Makefile cross-compiles when `CXX` is a mingw-w64 compiler: `.exe` names,
  no `win32_shim.cpp` (Windows loads `bink2w64.dll` itself), `-mwindows` for the
  GUI, and `data/windows/quillreel.rc` for the icon, version information and
  manifest. The manifest sets UTF-8 as the programs' code page, so file names
  with any characters work (this is why Windows 10 1903 is the minimum).
- `build-windows.sh` copies every DLL the programs and Qt plugins import from
  MSYS2 (found by walking their imports) and the Qt plugins they use (Windows
  platform, offscreen, Windows 11 style, icon format, Schannel TLS). The
  installer's file list is generated from that, so the uninstaller removes
  exactly what was installed; upgrading first runs the old uninstaller.
- Installs for all users under `C:\Program Files\Quillreel` (needs admin). Options:
  Start menu shortcut (on), desktop shortcut (off), .bik/.bk2 file association
  (on; Quillreel is added to "Open with" and becomes the default only if no other
  program is). Uninstalling leaves the user's `bink2w64.dll` and settings.
- `/S /UPDATE` is how Quillreel's updater runs the installer: silent, with the
  components recorded at first install (`HKLM\Software\Quillreel\Components`),
  waiting for Quillreel to close and starting it again afterwards through
  Explorer (not elevated). See specs/gui.md, "Updates".
- `make package-windows WINDOWS_STAGE=1` stops before the installer and leaves
  the programs with their DLLs in `dist/Quillreel-<v>-windows`, e.g. to run the
  tests under Wine.
- Size: about 70 MB to download, 245 MB installed. MSYS2's FFmpeg is built with
  every codec library it packages; a smaller FFmpeg built for Quillreel could
  cut that a lot.

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
