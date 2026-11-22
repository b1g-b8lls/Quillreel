#!/bin/bash
# Runs inside the packaging container (packaging/Containerfile):
# builds Quillreel from /src and writes the .deb and AppImage to /out.
set -euo pipefail

VERSION=$(sed -n 's/.*kVersion = "\(.*\)".*/\1/p' /src/src/main.cpp)
DEB_REVISION=${DEB_REVISION:-1}
# Distro suffix for the .deb version, e.g. 0.6.0-1~deb13 / 0.6.0-1~ubuntu26.04.
OS_ID=$(. /etc/os-release && echo "$ID")
OS_VERSION=$(. /etc/os-release && echo "$VERSION_ID")
case "$OS_ID" in
    debian) DISTRO=deb$OS_VERSION ;;
    *) DISTRO=$OS_ID$OS_VERSION ;;
esac
DEB_VERSION=$VERSION-$DEB_REVISION~$DISTRO
BUILD_APPIMAGE=${BUILD_APPIMAGE:-1}
MAINTAINER=${MAINTAINER:-Quillreel developers <quillreel@localhost>}
ARCH=$(dpkg --print-architecture)
WORK=/tmp/quillreel-package
rm -rf "$WORK"
cp -r /src "$WORK"
cd "$WORK"
rm -rf build

echo "== building Quillreel $VERSION"
make -j"$(nproc)" CXXFLAGS="-O2" >/dev/null

# ---------------------------------------------------------------- .deb
echo "== .deb"
PKG=$WORK/deb/quillreel
make install DESTDIR="$PKG" PREFIX=/usr >/dev/null
strip --strip-unneeded "$PKG/usr/bin/quillreel" "$PKG/usr/bin/quillreel-gui"
install -Dm644 README.md "$PKG/usr/share/doc/quillreel/README.md"
install -Dm644 docs/bink2.md "$PKG/usr/share/doc/quillreel/bink2.md"
install -Dm644 docs/LEGAL.md "$PKG/usr/share/doc/quillreel/LEGAL.md"
cat > "$PKG/usr/share/doc/quillreel/copyright" <<COPYRIGHT
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: Quillreel

Files: *
License: GPL-3.0-or-later
 On Debian systems, the full text of the GNU General Public License version 3
 can be found in /usr/share/common-licenses/GPL-3.
COPYRIGHT

# Library dependencies, worked out from the binaries.
mkdir -p "$WORK/deb/debian"
printf 'Source: quillreel\n\nPackage: quillreel\nArchitecture: any\n' > "$WORK/deb/debian/control"
DEPENDS=$(cd "$WORK/deb" && dpkg-shlibdeps -O "$PKG/usr/bin/quillreel" "$PKG/usr/bin/quillreel-gui" 2>/dev/null \
          | sed -n 's/^shlibs:Depends=//p')

mkdir -p "$PKG/DEBIAN"
cat > "$PKG/DEBIAN/control" <<CONTROL
Package: quillreel
Version: $DEB_VERSION
Architecture: $ARCH
Maintainer: $MAINTAINER
Installed-Size: $(du -sk "$PKG/usr" | cut -f1)
Depends: $DEPENDS
Recommends: qt6-gtk-platformtheme, qt6-xdgdesktopportal-platformtheme
Section: video
Priority: optional
Description: inspect, convert and play Bink video files
 Quillreel reads Bink (.bik) and Bink 2 (.bk2) video files: it shows their
 details, converts them to MP4, MKV, WebM, PNG frames or audio files, and
 plays them with seeking and audio track selection.
 .
 It provides a command-line tool (quillreel) and a Qt desktop application
 (quillreel-gui).
 .
 Bink 2 video is not supported out of the box: it needs RAD's bink2w64.dll,
 which you provide from a game that uses Bink 2 (quillreel bink2 install, or
 Help > Bink 2 Video in the GUI). Without it, .bk2 files are inspected and
 their audio played and converted.
CONTROL
DEB=/out/quillreel_${DEB_VERSION}_${ARCH}.deb
dpkg-deb --root-owner-group --build "$PKG" "$DEB" >/dev/null
echo "   $DEB"

# ---------------------------------------------------------------- AppImage
[ "$BUILD_APPIMAGE" = 1 ] || exit 0
echo "== AppImage"
APPDIR=$WORK/AppDir
make install DESTDIR="$APPDIR" PREFIX=/usr >/dev/null
cd "$WORK"
export APPIMAGE_EXTRACT_AND_RUN=1 QMAKE=/usr/bin/qmake6 NO_STRIP=0
export EXTRA_PLATFORM_PLUGINS="libqwayland-generic.so;libqoffscreen.so"  # Wayland as well as X11; offscreen for headless tests
export LDAI_OUTPUT="/out/Quillreel-${VERSION}-$(uname -m).AppImage"
# Desktop integration: Qt's GTK3 theme (GTK desktops) and the desktop portal
# theme (KDE and others). AppRun picks one by desktop, like Qt does natively.
QT_PLUGINS=$(qmake6 -query QT_INSTALL_PLUGINS)
mkdir -p "$APPDIR/usr/plugins/platformthemes"
cp "$QT_PLUGINS/platformthemes/libqgtk3.so" "$QT_PLUGINS/platformthemes/libqxdgdesktopportal.so" \
   "$APPDIR/usr/plugins/platformthemes/"
# OpenGL on Wayland (the video view) needs the EGL client buffer integration,
# which linuxdeploy-plugin-qt doesn't deploy.
cp -r "$QT_PLUGINS/wayland-graphics-integration-client" "$APPDIR/usr/plugins/"
linuxdeploy --appdir "$APPDIR" \
    --deploy-deps-only "$APPDIR/usr/plugins/platformthemes" \
    --deploy-deps-only "$APPDIR/usr/plugins/wayland-graphics-integration-client" \
    --executable "$APPDIR/usr/bin/quillreel" \
    --executable "$APPDIR/usr/bin/quillreel-gui" \
    --desktop-file "$APPDIR/usr/share/applications/quillreel.desktop" \
    --icon-file "$APPDIR/usr/share/icons/hicolor/256x256/apps/quillreel.png" \
    --custom-apprun packaging/AppRun \
    --plugin qt --output appimage > "$WORK/linuxdeploy.log" 2>&1 || { tail -40 "$WORK/linuxdeploy.log"; exit 1; }
echo "   $LDAI_OUTPUT"
echo "== linuxdeploy used:"
cat /usr/local/share/linuxdeploy.sha256

# The Debian source packages of every library bundled in the AppImage, with
# links to their source on snapshot.debian.org: the corresponding source the
# GPL/LGPL parts (FFmpeg, x264, x265, Qt, ...) must be offered with. Publish it
# next to the AppImage (docs/LEGAL.md).
SOURCES="$LDAI_OUTPUT-sources.md"
{
    echo "# Sources of the libraries bundled in $(basename "$LDAI_OUTPUT")"
    echo
    echo "Built on $(. /etc/os-release && echo "$PRETTY_NAME") from these Debian source packages."
    echo "Quillreel's own source is this repository at the matching tag."
    echo
    echo "| Source package | Version |"
    echo "|---|---|"
    find "$APPDIR" -name '*.so*' -printf '%f\n' | sort -u | while read -r lib; do
        dpkg -S "*/$lib" 2>/dev/null | head -1 | cut -d: -f1
    done | sort -u | xargs dpkg-query -W -f='${source:Package} ${source:Version}\n' | sort -u |
    while read -r pkg ver; do
        url_ver=$(printf '%s' "$ver" | sed 's/:/%3A/g; s/+/%2B/g; s/~/%7E/g')
        echo "| $pkg | [$ver](https://snapshot.debian.org/package/$pkg/$url_ver/) |"
    done
} > "$SOURCES"
echo "   $SOURCES"
