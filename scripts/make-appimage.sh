#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Fakturo — invoicing for Slovak and Czech sole traders
# Copyright (C) 2026 Peter Mercell
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.

#
# make-appimage.sh — build Fakturo-<version>-<arch>.AppImage from an existing
# Linux build. The counterpart of make-dmg.sh, and it exists for the same
# reason: the binary CMake produces links against a Qt that lives in one
# developer's home directory, so it runs on exactly one machine.
#
#   ./scripts/make-appimage.sh                   # uses ./build-linux
#   ./scripts/make-appimage.sh path/to/build
#   ./scripts/make-appimage.sh path/to/fakturo   # what the CMake target passes
#
# Four steps, each with teeth:
#
#   1. **cmake --install into an AppDir.** An AppImage is a squashfs image of a
#      /usr tree plus an AppRun. The install() rule in CMakeLists already puts
#      the binary in bin/, so DESTDIR does the rest — nothing has to know the
#      file list twice.
#
#   2. **A .desktop file and a 256x256 PNG.** Not decoration: linuxdeploy reads
#      the desktop file to learn what to run, and refuses to build without an
#      icon whose basename matches the Icon= key. Fakturo ships an .icns and an
#      .ico and no PNG, so one is rendered here from the .ico.
#
#   3. **linuxdeploy + its qt plugin** copy libQt6*.so, the platform plugins,
#      the image formats and the TLS backends into the AppDir and rewrite every
#      RUNPATH to $ORIGIN. Skipping this produces an AppImage that starts only
#      where ~/Qt6/6.5.3/gcc_64 exists, which is to say on the build machine.
#      The plugin is pointed at the same Qt CMake found, via QMAKE — Rocky can
#      easily carry a system qt6-qtbase beside the online-installer Qt, and
#      deploying one Qt's plugins next to the other one's libraries produces a
#      binary that aborts on "Cannot mix incompatible Qt library".
#
#   4. **An ldd sweep** over the finished AppDir, because linuxdeploy reports
#      what it could not resolve and then carries on. A tree that will not start
#      anywhere else looks exactly like one that will — on this machine, where
#      the developer's Qt is still on disk. This is the check that tells them
#      apart.
#
# Two things about Rocky 9 in particular:
#
#   * An AppImage mounts itself with libfuse.so.2, and Rocky 9 ships only
#     fuse3. linuxdeploy and appimagetool are themselves AppImages, hence
#     APPIMAGE_EXTRACT_AND_RUN=1 below. The AppImage this script produces has
#     the same problem on the same machine: run `sudo dnf install fuse-libs`
#     once, or start it with `./Fakturo-*.AppImage --appimage-extract-and-run`.
#
#   * The result carries Qt, not glibc. Built on Rocky 9 it needs glibc 2.34 or
#     newer on the target — Rocky/RHEL/Alma 9, Ubuntu 22.04+, Fedora 35+,
#     Debian 12+. It will NOT start on CentOS 7 or Ubuntu 20.04. Building for
#     those means building on those.

set -Eeuo pipefail

# Without this, a command substitution that fails under `set -o pipefail` ends
# the script with no output whatsoever — the last line printed is whatever step
# was announced before it, and nothing says why. That happened here once
# already (ldconfig is in /usr/sbin, which a conda shell does not always carry)
# and cost more time than the bug was worth.
trap 'echo "make-appimage.sh: failed at line $LINENO (exit $?)" >&2' ERR

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# Either a build directory or the executable itself. The CMake target passes
# the executable, because $<TARGET_FILE:fakturo> is the only spelling that is
# right under every generator.
ARG="${1:-$ROOT/build-linux}"
if [[ -f "$ARG" ]]; then
    BIN="$(cd "$(dirname "$ARG")" && pwd)/$(basename "$ARG")"
    BUILD="$(dirname "$BIN")"
else
    BUILD="$(cd "$ARG" 2>/dev/null && pwd || echo "$ARG")"
    BIN="$BUILD/fakturo"
fi

if [[ ! -x "$BIN" ]]; then
    echo "No executable at $BIN" >&2
    echo "Build first:  cmake --build ${BUILD##*/} -j" >&2
    exit 1
fi

ARCH="$(uname -m)"

# From the build, not from CMakeLists: the version on the file can then only
# ever be the version that was actually compiled into it.
# awk 'exit', not '| head -1': head closes the pipe after one line, awk takes
# SIGPIPE, pipefail turns that into a failed substitution and `set -e` ends the
# script silently. The same shape appears three times in this file and none of
# them may use head.
VERSION="$(awk -F= '/^CMAKE_PROJECT_VERSION:/ {print $2; exit}' "$BUILD/CMakeCache.txt" 2>/dev/null || true)"
VERSION="${VERSION:-0.0.0}"

# ------------------------------------------------------------------------ Qt
# The Qt CMake actually found, taken from the cache rather than from PATH or
# LD_LIBRARY_PATH. Reading it here means this script cannot deploy a different
# Qt from the one the binary was linked against — the Linux version of the
# split-Homebrew trap that make-dmg.sh has a paragraph about.
QT_CMAKE_DIR="$(awk -F= '/^Qt6Core_DIR:/ {print $2; exit}' "$BUILD/CMakeCache.txt" 2>/dev/null || true)"
[[ -n "$QT_CMAKE_DIR" ]] || QT_CMAKE_DIR="$(awk -F= '/^Qt6_DIR:/ {print $2; exit}' "$BUILD/CMakeCache.txt" 2>/dev/null || true)"
QT_PREFIX=""
if [[ -n "$QT_CMAKE_DIR" ]]; then
    # .../lib/cmake/Qt6Core  ->  ...
    QT_PREFIX="$(cd "$QT_CMAKE_DIR/../../.." 2>/dev/null && pwd || true)"
fi

QMAKE="${QMAKE:-}"
if [[ ! -x "$QMAKE" && -n "$QT_PREFIX" ]]; then
    for c in "$QT_PREFIX/bin/qmake6" "$QT_PREFIX/bin/qmake"; do
        [[ -x "$c" ]] && QMAKE="$c" && break
    done
fi
if [[ ! -x "$QMAKE" ]]; then
    QMAKE="$(command -v qmake6 || command -v qmake || true)"
    [[ -n "$QMAKE" ]] && echo "  warning: falling back to $QMAKE — it may be a different Qt" >&2
fi
if [[ ! -x "${QMAKE:-}" ]]; then
    echo "No qmake found. Point this script at the Qt the app was built with:" >&2
    echo "  QMAKE=\$HOME/Qt6/6.5.3/gcc_64/bin/qmake6 $0 $*" >&2
    exit 1
fi
export QMAKE

# linuxdeploy resolves the binary's dependencies with the ordinary loader, so
# the Qt it must copy has to be findable the ordinary way.
[[ -n "$QT_PREFIX" ]] && export LD_LIBRARY_PATH="$QT_PREFIX/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

echo "Fakturo $VERSION  ($ARCH)"
echo "  Qt from ${QT_PREFIX:-$(dirname "$(dirname "$QMAKE")")}"

# -------------------------------------------------------------------- AppDir
APPDIR="$BUILD/AppDir"
rm -rf "$APPDIR"
echo "  staging the AppDir"
DESTDIR="$APPDIR" cmake --install "$BUILD" --prefix /usr >/dev/null

if [[ ! -x "$APPDIR/usr/bin/fakturo" ]]; then
    echo "cmake --install put no fakturo in $APPDIR/usr/bin" >&2
    exit 1
fi

# The desktop entry. linuxdeploy reads Exec= to find the binary and Icon= to
# find the icon, and both are matched by basename — a mismatch here is an error
# in a build log three steps later.
install -Dm644 "$ROOT/resources/fakturo.desktop" \
    "$APPDIR/usr/share/applications/fakturo.desktop"

# The icon. resources/ ships Fakturo.icns and Fakturo.ico because macOS and
# Windows want those; freedesktop wants a PNG at a named pixel size, so one is
# rendered from the .ico rather than checked in as a fourth copy of the same
# drawing.
ICON="$APPDIR/usr/share/icons/hicolor/256x256/apps/fakturo.png"
mkdir -p "$(dirname "$ICON")"
if python3 -c 'import PIL' 2>/dev/null; then
    python3 - "$ROOT/resources/Fakturo.ico" "$ICON" <<'PY'
import sys
from PIL import Image
src, dst = sys.argv[1], sys.argv[2]
im = Image.open(src)
# Pillow's ICO reader hands back the largest frame; ask for 256 explicitly so
# a future .ico without one is resized rather than silently written small.
try:
    im.size = (256, 256)
    im.load()
except Exception:
    pass
im.convert("RGBA").resize((256, 256), Image.LANCZOS).save(dst, "PNG")
PY
elif command -v magick >/dev/null; then
    magick "$ROOT/resources/Fakturo.ico[0]" -resize 256x256 "$ICON"
elif command -v convert >/dev/null; then
    convert "$ROOT/resources/Fakturo.ico[0]" -resize 256x256 "$ICON"
else
    echo "Cannot make the PNG icon: no Pillow and no ImageMagick." >&2
    echo "  pip install Pillow    (or)    sudo dnf install ImageMagick" >&2
    exit 1
fi

# OpenSSL, by hand. Qt's TLS backend dlopen()s libssl at runtime, so it appears
# in no dependency list and linuxdeploy has nothing to follow. Leave it out and
# every HTTPS call — the registry lookups, SAPI — fails on any machine whose
# OpenSSL is not this exact soname, with a "TLS initialization failed" and no
# other clue.
mkdir -p "$APPDIR/usr/lib"

# ldconfig is in /usr/sbin. A login shell has that on PATH; a conda shell, a
# cron job or a CI runner may not — so it is looked for by absolute path too,
# and the library directories are searched directly if it is missing entirely.
find_lib() {
    local so="$1" p="" ld d
    for ld in "$(command -v ldconfig || true)" /usr/sbin/ldconfig /sbin/ldconfig; do
        [[ -x "$ld" ]] || continue
        p="$("$ld" -p 2>/dev/null | awk -v s="$so" '$1==s {print $NF; exit}' || true)"
        [[ -n "$p" ]] && break
    done
    if [[ -z "$p" ]]; then
        for d in /usr/lib64 /lib64 /usr/lib/x86_64-linux-gnu /usr/lib; do
            [[ -e "$d/$so" ]] && p="$d/$so" && break
        done
    fi
    printf '%s' "$p"
}

for so in libssl.so.3 libcrypto.so.3; do
    p="$(find_lib "$so")"
    if [[ -n "$p" && -e "$p" ]]; then
        cp -Lf "$p" "$APPDIR/usr/lib/$so"
    else
        echo "  warning: no $so on this machine — HTTPS will fail on any target" >&2
        echo "           whose OpenSSL differs.  sudo dnf install openssl-libs" >&2
    fi
done

# ---------------------------------------------------------------- linuxdeploy
# Cached, because these are 100 MB of downloads and this script gets run often.
TOOLS="${FAKTURO_APPIMAGE_TOOLS:-$HOME/.cache/fakturo-appimage}"
mkdir -p "$TOOLS"
fetch() {
    local url="$1" dest="$TOOLS/$2"
    if [[ ! -x "$dest" ]]; then
        echo "  fetching $2"
        curl -fL --retry 3 -o "$dest" "$url" || {
            echo "Could not download $2. With no network, put it in $TOOLS by hand:" >&2
            echo "  $url" >&2
            exit 1
        }
        chmod +x "$dest"
    fi
}
BASE=https://github.com/linuxdeploy
fetch "$BASE/linuxdeploy/releases/download/continuous/linuxdeploy-$ARCH.AppImage" \
      "linuxdeploy-$ARCH.AppImage"
fetch "$BASE/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-$ARCH.AppImage" \
      "linuxdeploy-plugin-qt-$ARCH.AppImage"

# linuxdeploy finds its plugins by looking for linuxdeploy-plugin-* on PATH.
export PATH="$TOOLS:$PATH"

# Rocky 9 has fuse3 and no libfuse.so.2, so an AppImage cannot mount itself —
# and linuxdeploy, its qt plugin and appimagetool are all AppImages. This makes
# all three unpack to a temporary directory and run from there instead. It is
# inherited by the child processes, which is the point.
export APPIMAGE_EXTRACT_AND_RUN=1

# Named here rather than left to linuxdeploy, which would derive the name from
# the desktop entry and leave the version off it.
# Both spellings: the current plugin wants LDAI_OUTPUT and warns about OUTPUT,
# older builds know only OUTPUT.
export OUTPUT="Fakturo-$VERSION-$ARCH.AppImage"
export LDAI_OUTPUT="$OUTPUT"
export LINUXDEPLOY_OUTPUT_VERSION="$VERSION"

echo "  bundling Qt and packing the image"
cd "$BUILD"
rm -f "$OUTPUT"
"$TOOLS/linuxdeploy-$ARCH.AppImage" \
    --appdir "$APPDIR" \
    --executable "$APPDIR/usr/bin/fakturo" \
    --desktop-file "$APPDIR/usr/share/applications/fakturo.desktop" \
    --icon-file "$ICON" \
    --plugin qt \
    --output appimage

OUT="$BUILD/$OUTPUT"
[[ -f "$OUT" ]] || { echo "linuxdeploy produced no $OUTPUT" >&2; exit 1; }

# ----------------------------------------------------------------- the sweep
# Is it actually self-contained? linuxdeploy prints what it could not resolve
# and carries on, and on this machine the developer's Qt is still on disk, so
# an image that starts nowhere else starts here. Anything still resolving into
# the Qt prefix or into a home directory, and anything not found at all, is the
# difference.
echo "  checking the AppDir is self-contained"
checked=0
leaks=0
while IFS= read -r f; do
    [[ "$(file -b "$f" 2>/dev/null)" == *ELF* ]] || continue
    # env -u LD_LIBRARY_PATH, and it is the whole point of this check. This
    # script puts the developer's Qt on LD_LIBRARY_PATH so linuxdeploy can
    # resolve it, and the loader searches LD_LIBRARY_PATH BEFORE DT_RUNPATH —
    # so an ldd run in this script's own environment resolves every bundled Qt
    # library back to ~/Qt6 and reports a perfectly self-contained AppDir as 31
    # leaks. Clearing it asks the question that actually matters: what does
    # this file load when nothing on the machine is pointing the way?
    out="$(env -u LD_LIBRARY_PATH ldd "$f" 2>/dev/null || true)"
    refs="$(printf '%s\n' "$out" | grep -E "not found" || true)"
    if [[ -n "$QT_PREFIX" ]]; then
        more="$(printf '%s\n' "$out" | grep -F "$QT_PREFIX" || true)"
        [[ -n "$more" ]] && refs="$refs${refs:+$'\n'}$more"
    fi
    if [[ -n "$refs" ]]; then
        [[ $leaks -eq 0 ]] && echo
        echo "  ${f#"$APPDIR"/}"
        echo "$refs" | sed 's/^/      /'
        leaks=$((leaks + 1))
    fi
    checked=$((checked + 1))
done < <(find "$APPDIR" -type f)

if [[ $leaks -eq 0 ]]; then
    # Say how many were examined. "No output" and "nothing was looked at" read
    # identically, and one of them is a lie.
    echo "  $checked ELF files, none pointing outside the AppDir"
else
    echo
    echo "  $leaks of $checked file(s) still point outside the AppDir."
    echo "  The image will start on THIS machine and may start nowhere else."
    echo "  A missing libxcb-cursor.so.0 is the usual one on Rocky 9:"
    echo "    sudo dnf install xcb-util-cursor libxkbcommon-x11"
    echo "  then re-run this script so the library gets bundled."
    echo
fi

GLIBC="$(ldd --version 2>/dev/null | awk 'NR==1 {print $NF; exit}' || true)"
GLIBC="${GLIBC:-?}"

echo
echo "Done: $OUT"
ls -lh "$OUT" | awk '{print "  " $5}'
echo "  needs glibc $GLIBC or newer on the target machine"
echo "  needs libfuse.so.2 to self-mount:  sudo dnf install fuse-libs"
echo "  or run it as:  $OUTPUT --appimage-extract-and-run"
