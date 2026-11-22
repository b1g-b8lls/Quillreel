#!/bin/bash
# Runs inside the Windows packaging container (packaging/Containerfile.windows):
# cross-compiles Quillreel from /src and writes the installer to /out.
# With STAGE_ONLY=1 it stops after putting the programs and their DLLs in
# /out/Quillreel-<version>-windows (for testing, e.g. under Wine).
set -euo pipefail

VERSION=$(sed -n 's/.*kVersion = "\(.*\)".*/\1/p' /src/src/main.cpp)
HOST=x86_64-w64-mingw32
MSYS_DB=(--root /opt/msys --dbpath /opt/msys/var/lib/pacman --config /etc/pacman-msys2.conf)
QT_PLUGINS=$MSYS_ROOT/share/qt6/plugins
WORK=/tmp/quillreel-windows
rm -rf "$WORK"
cp -r /src "$WORK"
cd "$WORK"
rm -rf build

echo "== building Quillreel $VERSION for Windows"
export PKG_CONFIG_LIBDIR=$MSYS_ROOT/lib/pkgconfig PKG_CONFIG_SYSROOT_DIR=/opt/msys
make -j"$(nproc)" CXX=$HOST-g++ CXXFLAGS="-O2" >/dev/null

# ---------------------------------------------------------------- staging
STAGE=$WORK/stage
mkdir -p "$STAGE"
cp build/quillreel.exe build/quillreel-gui.exe "$STAGE/"
$HOST-strip --strip-unneeded "$STAGE"/*.exe
# Qt plugins, in the folders Qt looks for next to the executable: the Windows
# platform (and offscreen, for headless use), the Windows 11 style, and the
# multimedia backend (audio output; it uses the same FFmpeg DLLs).
for p in platforms/qwindows.dll platforms/qoffscreen.dll styles/qmodernwindowsstyle.dll \
         multimedia/ffmpegmediaplugin.dll imageformats/qico.dll; do
    install -Dm644 "$QT_PLUGINS/$p" "$STAGE/$p"
done

# Every DLL the executables and plugins import that comes from MSYS2, found
# recursively. DLLs not in MSYS2's bin folder are part of Windows.
declare -A seen
deploy() {  # FILE
    local dll lower
    while read -r dll; do
        lower=${dll,,}
        [[ -n ${seen[$lower]:-} ]] && continue
        seen[$lower]=1
        local src
        src=$(find "$MSYS_ROOT/bin" -maxdepth 1 -iname "$dll" -print -quit)
        [[ -n $src ]] || continue
        cp "$src" "$STAGE/"
        deploy "$src"
    done < <($HOST-objdump -p "$1" | sed -n 's/^\s*DLL Name: //p')
}
while read -r f; do deploy "$f"; done < <(find "$STAGE" -maxdepth 1 -name '*.exe'; find "$STAGE" -mindepth 2 -name '*.dll')
$HOST-strip --strip-unneeded "$STAGE"/*.dll "$STAGE"/*/*.dll 2>/dev/null || true

cp README.md "$STAGE/README.md"
cp LICENSE "$STAGE/LICENSE.txt"
mkdir -p "$STAGE/docs"
cp docs/bink2.md docs/LEGAL.md "$STAGE/docs/"
echo "   $(find "$STAGE" -name '*.dll' | wc -l) DLLs, $(du -sh "$STAGE" | cut -f1)"

if [ "${STAGE_ONLY:-0}" = 1 ]; then
    rm -rf "/out/Quillreel-$VERSION-windows"
    cp -r "$STAGE" "/out/Quillreel-$VERSION-windows"
    echo "   /out/Quillreel-$VERSION-windows"
    exit 0
fi

# ---------------------------------------------------------------- sources
# The MSYS2 packages every bundled DLL comes from, with links to their source
# archives: the corresponding source the GPL/LGPL parts (FFmpeg, x264, x265,
# Qt, ...) must be offered with. Publish it next to the installer (docs/LEGAL.md).
INSTALLER=/out/Quillreel-$VERSION-windows-x86_64-setup.exe
SOURCES=$INSTALLER-sources.md
{
    echo "# Sources of the libraries bundled in $(basename "$INSTALLER")"
    echo
    echo "Cross-compiled with mingw-w64 GCC $($HOST-gcc -dumpversion) against these MSYS2 (UCRT64) packages."
    echo "Quillreel's own source is this repository at the matching tag."
    echo
    echo "| MSYS2 package | Version | Source |"
    echo "|---|---|---|"
    find "$STAGE" -name '*.dll' -printf '%f\n' | while read -r dll; do
        f=$(find "$MSYS_ROOT" -iname "$dll" -path '*/bin/*' -print -quit)
        [[ -n $f ]] || f=$(find "$QT_PLUGINS" -iname "$dll" -print -quit)
        pacman "${MSYS_DB[@]}" -Qqo "$f"
    done | sort -u | while read -r pkg; do
        desc=$(echo /opt/msys/var/lib/pacman/local/"$pkg"-[0-9]*/desc)
        ver=$(sed -n '/^%VERSION%$/{n;p}' "$desc")
        base=$(sed -n '/^%BASE%$/{n;p}' "$desc")
        echo "| $pkg | $ver | [$base-$ver.src.tar.zst](https://repo.msys2.org/mingw/sources/$base-$ver.src.tar.zst) |"
    done
} > "$SOURCES"

# ---------------------------------------------------------------- installer
echo "== installer"
# The file list, so the uninstaller removes exactly what was installed.
{
    (cd "$STAGE" && find . -type d ! -name . | sort) | while read -r d; do
        d=${d#./}; echo "CreateDirectory \"\$INSTDIR\\${d//\//\\}\""
    done
    (cd "$STAGE" && find . -type f | sort) | while read -r f; do
        f=${f#./}; echo "File \"/oname=${f//\//\\}\" \"$STAGE/$f\""
    done
} > "$WORK/files-install.nsh"
{
    (cd "$STAGE" && find . -type f | sort) | while read -r f; do
        f=${f#./}; echo "Delete \"\$INSTDIR\\${f//\//\\}\""
    done
    (cd "$STAGE" && find . -type d ! -name . | sort -r) | while read -r d; do
        d=${d#./}; echo "RMDir \"\$INSTDIR\\${d//\//\\}\""
    done
} > "$WORK/files-uninstall.nsh"
SIZE_KB=$(du -sk "$STAGE" | cut -f1)
makensis -V2 -DVERSION="$VERSION" -DSIZE_KB="$SIZE_KB" -DFILES_DIR="$WORK" -DSRC="$WORK" \
    -DOUTFILE="$INSTALLER" packaging/windows/quillreel.nsi
echo "   $INSTALLER"
echo "   $SOURCES"
