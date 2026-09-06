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
# make-dmg.sh — build a Fakturo.dmg from an existing build.
#
#   ./scripts/make-dmg.sh                     # uses ./build-brew
#   ./scripts/make-dmg.sh path/to/build
#   ./scripts/make-dmg.sh path/to/fakturo.app # what the `dmg` CMake target passes
#
# Three steps, and each one is here for a reason that has teeth:
#
#   1. **macdeployqt** copies the Qt frameworks into the bundle and rewrites
#      the install names to point inside it. Without this the .app runs only
#      on a machine that has the same Homebrew Qt at the same path — which is
#      to say only on the machine that built it, and only until Homebrew
#      upgrades Qt underneath it. The failure is not subtle: the app refuses
#      to launch with a dyld error naming a library nobody else has.
#
#   2. **Re-signing.** On Apple Silicon every binary must carry a signature to
#      run at all; the linker gives it an ad-hoc one. macdeployqt then rewrites
#      the load commands, which invalidates that signature — so it has to be
#      applied again *after* the deploy step, never before.
#
#   3. **hdiutil** packs the bundle next to a symlink to /Applications, so the
#      window that opens is the drag-to-install one people recognise.
#
# The result is unsigned by any developer identity and not notarized. On this
# Mac it will open. On anyone else's, Gatekeeper will refuse it until they
# right-click → Open, or strip the quarantine flag:
#
#   xattr -dr com.apple.quarantine /Applications/Fakturo.app
#
# Making that unnecessary needs a paid Apple Developer account.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# Either a build directory or the bundle itself. CMake's `dmg` target passes
# the bundle, because under the Xcode generator the .app sits in a per-config
# subdirectory and "$BUILD/fakturo.app" would not be there.
ARG="${1:-$ROOT/build-brew}"
if [[ "$ARG" == *.app || "$ARG" == *.app/ ]]; then
    APP="${ARG%/}"
    BUILD="$(cd "$(dirname "$APP")" && pwd)"
else
    BUILD="$ARG"
    APP="$BUILD/fakturo.app"
fi

if [[ ! -d "$APP" ]]; then
    echo "No bundle at $APP" >&2
    echo "Build first:  cmake --build ${BUILD##*/} -j" >&2
    exit 1
fi

# From the bundle rather than from CMakeLists, so the name on the file can only
# ever be the version that is actually inside it.
VERSION="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' \
           "$APP/Contents/Info.plist" 2>/dev/null || echo 0.0.0)"

# The CMake `dmg` target exports MACDEPLOYQT, pointing at the deploy tool that
# belongs to the Qt the app was actually linked against. That matters on a Mac
# carrying more than one Homebrew Qt — a `qtbase` prefix beside `qt`, say:
# taking macdeployqt from `brew --prefix qt` then deploys one Qt into a bundle
# built against another, and it cannot resolve a single @rpath. Every
# "Cannot resolve rpath" line is that mismatch. The fallbacks below are for
# running this script by hand.
if [[ -n "${MACDEPLOYQT:-}" && ! -x "${MACDEPLOYQT:-}" ]]; then
    # CMake named the tool belonging to the Qt it found and it is not there —
    # a split Homebrew Qt, where qmake lives in qtbase and macdeployqt ships
    # with qttools. Falling back is better than stopping, but the fallback may
    # be a different Qt, so say so rather than let the rpath errors say it.
    echo "  macdeployqt is not at $MACDEPLOYQT" >&2
    echo "  (the Qt this app was linked against does not ship it)" >&2
    echo "  falling back to brew --prefix qt — if that is a different Qt," >&2
    echo "  rebuild against it:  rm -rf ${BUILD##*/} && cmake -B ${BUILD##*/} \\" >&2
    echo "      -DCMAKE_PREFIX_PATH=\"\$(brew --prefix qt)\" -DCMAKE_BUILD_TYPE=Release" >&2
    MACDEPLOYQT=""
fi
if [[ ! -x "${MACDEPLOYQT:-}" ]]; then
    MACDEPLOYQT="$(brew --prefix qt 2>/dev/null)/bin/macdeployqt"
fi
if [[ ! -x "$MACDEPLOYQT" ]]; then
    MACDEPLOYQT="$(command -v macdeployqt || true)"
fi
if [[ ! -x "${MACDEPLOYQT:-}" ]]; then
    echo "macdeployqt not found. Install Qt via Homebrew:  brew install qt" >&2
    exit 1
fi
QT_PREFIX="$(cd "$(dirname "$MACDEPLOYQT")/.." && pwd)"

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

echo "Fakturo $VERSION"
echo "  staging the bundle"
# A copy, so a half-finished deploy cannot leave the build tree's app broken.
cp -R "$APP" "$STAGE/Fakturo.app"

echo "  bundling Qt from $QT_PREFIX"
# Not fatal, deliberately. macdeployqt prints an ERROR for every plugin
# belonging to a Qt module this app does not use, and its own signing pass
# fails because it rewrites load commands after signing — which is the very
# reason this script signs again below. Aborting here on a non-zero status
# throws away the one thing that actually answers the question: the otool
# sweep further down. So record the status, say so, and carry on.
deploy_status=0
"$MACDEPLOYQT" "$STAGE/Fakturo.app" -always-overwrite || deploy_status=$?
if [[ $deploy_status -ne 0 ]]; then
    echo "  macdeployqt exited $deploy_status — continuing to the checks below"
fi

# Plugins Fakturo has never used, and whose frameworks Homebrew does not even
# install — macdeployqt walks every plugin in the Qt tree and tries to deploy
# them all, which is where the QtVirtualKeyboard errors come from.
rm -rf "$STAGE/Fakturo.app/Contents/PlugIns/virtualkeyboard" \
       "$STAGE/Fakturo.app/Contents/PlugIns/platforminputcontexts" 2>/dev/null || true

# ------------------------------------------------------------- repointing
# What macdeployqt leaves behind. Homebrew's Qt is split across prefixes —
# qtbase, qtdeclarative, and separate formulae for brotli and webp — and
# macdeployqt copies a framework in without always rewriting its install name
# or the references to it, and gives up on libraries it cannot find. The
# bundle then loads on this Mac, where /opt/homebrew exists, and nowhere else.
#
# Three passes: bring in what is referenced but missing, give everything in
# Frameworks an @rpath install name, then repoint every absolute Homebrew
# reference at it. All of it BEFORE signing, because install_name_tool
# invalidates a signature exactly the way macdeployqt does.
APP_STAGE="$STAGE/Fakturo.app"
FW="$APP_STAGE/Contents/Frameworks"
mkdir -p "$FW"
# install_name_tool needs to write to every one of these. It is a copy in a
# temporary directory, so nothing of yours is being made writable.
chmod -R u+w "$APP_STAGE"
HOMEBREW="$(brew --prefix 2>/dev/null || echo /opt/homebrew)"

is_macho() { [[ "$(file -b "$1" 2>/dev/null)" == *Mach-O* ]]; }

# The name a file inside Frameworks should answer to: its path relative to
# Frameworks, which is exactly the shape of a framework install name.
rpath_name() { printf '@rpath/%s' "${1#$FW/}"; }

# Where a dependency would live if it were bundled.
bundled_path() {
    case "$1" in
        */lib/*.framework/Versions/*/*) printf '%s/%s' "$FW" "${1##*/lib/}" ;;
        *)                              printf '%s/%s' "$FW" "${1##*/}" ;;
    esac
}

echo "  repointing what macdeployqt left outside the bundle"
copied=0
# Twice: a library copied in on the first round brings dependencies of its own.
for _round in 1 2; do
    while IFS= read -r bin; do
        is_macho "$bin" || continue
        while IFS= read -r dep; do
            src=""
            want=""
            case "$dep" in
                "$HOMEBREW"/*)
                    want="$(bundled_path "$dep")"
                    src="$dep"
                    ;;
                @rpath/*.dylib)
                    # macdeployqt could not resolve it — libwebp and friends
                    # live in their own Homebrew prefix, not in Qt's. Homebrew
                    # links them all into its own lib directory.
                    want="$FW/${dep#@rpath/}"
                    src="$HOMEBREW/lib/${dep#@rpath/}"
                    ;;
                *) continue ;;
            esac
            if [[ -e "$want" || ! -e "$src" ]]; then continue; fi
            mkdir -p "$(dirname "$want")"
            cp -f "$src" "$want"
            chmod u+w "$want"
            copied=$((copied + 1))
        done < <(otool -L "$bin" | tail -n +2 | awk '{print $1}')
    done < <(find "$APP_STAGE" -type f)
done
if [[ $copied -gt 0 ]]; then
    echo "  copied in $copied librar$([[ $copied -eq 1 ]] && echo y || echo ies) macdeployqt had missed"
fi

# Install names first, so the pass below never sees a Homebrew path that is
# really some library's own identity.
while IFS= read -r bin; do
    is_macho "$bin" || continue
    install_name_tool -id "$(rpath_name "$bin")" "$bin" 2>/dev/null || true
done < <(find "$FW" -type f)

while IFS= read -r bin; do
    is_macho "$bin" || continue
    while IFS= read -r dep; do
        case "$dep" in "$HOMEBREW"/*) ;; *) continue ;; esac
        want="$(bundled_path "$dep")"
        [[ -e "$want" ]] || continue
        install_name_tool -change "$dep" "$(rpath_name "$want")" "$bin" 2>/dev/null || true
    done < <(otool -L "$bin" | tail -n +2 | awk '{print $1}')

    # And the rpaths that make @rpath resolvable from where each binary sits.
    # A duplicate is refused with an error nobody needs to read.
    install_name_tool -add_rpath "@executable_path/../Frameworks" "$bin" 2>/dev/null || true
    case "$bin" in
        "$FW"/*)
            install_name_tool -add_rpath "@loader_path" "$bin" 2>/dev/null || true
            install_name_tool -add_rpath "@loader_path/../../.." "$bin" 2>/dev/null || true
            ;;
        "$APP_STAGE"/Contents/PlugIns/*)
            install_name_tool -add_rpath "@loader_path/../../Frameworks" "$bin" 2>/dev/null || true
            ;;
    esac
done < <(find "$APP_STAGE" -type f)

echo "  signing ad-hoc"
# After macdeployqt, never before: it rewrites the load commands and that
# invalidates whatever signature was there.
codesign --force --deep --sign - "$STAGE/Fakturo.app"
codesign --verify --deep "$STAGE/Fakturo.app" \
    || echo "  (signature did not verify — the app may still run locally)"

# Is the bundle actually self-contained? macdeployqt reports what it could not
# resolve and then carries on, so a bundle that will not launch anywhere else
# looks exactly like one that will — on this machine, because the Homebrew
# paths it kept are still there. This is the check that tells the difference.
echo "  checking the bundle is self-contained"
checked=0
leaks=0
while IFS= read -r bin; do
    # Every Mach-O file, not only the ones with the execute bit. Homebrew's
    # dylibs are commonly mode 444, and filtering on -perm -u+x walked straight
    # past them — which would report a clean bundle while every Qt framework in
    # it still pointed at /opt/homebrew. A check that cannot fail is worse than
    # no check, because it is believed.
    [[ "$(file -b "$bin" 2>/dev/null)" == *Mach-O* ]] || continue
    refs="$(otool -L "$bin" 2>/dev/null | tail -n +2 | grep -E '/opt/homebrew|/usr/local/(opt|Cellar)' || true)"
    if [[ -n "$refs" ]]; then
        [[ $leaks -eq 0 ]] && echo
        echo "  ${bin#$STAGE/Fakturo.app/}"
        echo "$refs" | sed 's/^/      /'
        leaks=$((leaks + 1))
    fi
    checked=$((checked + 1))
done < <(find "$STAGE/Fakturo.app" -type f)

if [[ $leaks -eq 0 ]]; then
    # Say how many were examined. "No output" and "nothing was looked at" read
    # identically, and one of them is a lie.
    echo "  $checked Mach-O files, none pointing outside the bundle"
fi

if [[ $leaks -gt 0 ]]; then
    echo
    echo "  $leaks of $checked file(s) still point outside the bundle."
    echo "  The disk image will open on THIS Mac, where those paths exist, and"
    echo "  may fail to launch on any other. Fix before giving it to anyone:"
    echo "    brew install qt        # the full formula, not qtbase alone"
    echo "    rm -rf ${BUILD##*/} && cmake -B ${BUILD##*/} -DCMAKE_PREFIX_PATH=\$(brew --prefix qt) ..."
    echo
fi

# The drag-to-install target. A symlink, not a copy: copying /Applications
# would be a very large mistake.
ln -s /Applications "$STAGE/Applications"

OUT="$BUILD/Fakturo-$VERSION.dmg"
rm -f "$OUT"
echo "  packing the disk image"
hdiutil create \
    -volname "Fakturo $VERSION" \
    -srcfolder "$STAGE" \
    -fs HFS+ \
    -format UDZO \
    -ov -quiet \
    "$OUT"

echo
echo "Done: $OUT"
ls -lh "$OUT" | awk '{print "  " $5}'
