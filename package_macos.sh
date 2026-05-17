#!/bin/bash
# Build a fully self-contained Neovere.app on macOS and package it into a DMG.
#
# Bundled inside the .app (the end user needs NOTHING preinstalled):
#   • Qt frameworks       (via macdeployqt)
#   • OpenCV dylibs       (+ all transitive /opt/homebrew /usr/local deps)
#   • ffmpeg binary
#   • CPython (python-build-standalone) — a relocatable, self-contained interpreter
#
# Python *site-packages* are NOT bundled — on first launch Neovere creates
# ~/neovere_venv from the bundled interpreter and pip-installs its requirements
# (numpy, opencv-python, scipy, librosa, soundfile, openai, pyqt5, psutil, pillow).
# This keeps the .app under ~400 MB and the DMG under ~150 MB.

set -euo pipefail

cd "$(dirname "$0")"
PROJECT_ROOT="$(pwd)"

BUILD_DIR="$PROJECT_ROOT/build-release"
DIST_DIR="$PROJECT_ROOT/dist"
APP_NAME="Neovere"
APP_BUNDLE="$BUILD_DIR/$APP_NAME.app"
DMG_NAME="Neovere-1.0.dmg"

# python-build-standalone — pinned for reproducibility
PYTHON_VERSION="3.12.7"
PBS_RELEASE="20241016"
PBS_CACHE_DIR="$PROJECT_ROOT/.pbs-cache"

# --------------------------------------------------------------
# Tool discovery
# --------------------------------------------------------------
MACDEPLOYQT="$(brew --prefix qt 2>/dev/null)/bin/macdeployqt"
[ -x "$MACDEPLOYQT" ] || MACDEPLOYQT="$(command -v macdeployqt || true)"
if [ ! -x "${MACDEPLOYQT:-}" ]; then
    echo "ERROR: macdeployqt not found. Install Qt via 'brew install qt'."
    exit 1
fi

CMAKE_BIN="$(command -v cmake || true)"
if [ -z "$CMAKE_BIN" ]; then
    for candidate in \
        "/Applications/CLion.app/Contents/bin/cmake/mac/aarch64/bin/cmake" \
        "/Applications/CLion.app/Contents/bin/cmake/mac/x86_64/bin/cmake" \
        "/opt/homebrew/bin/cmake" \
        "/usr/local/bin/cmake"
    do
        [ -x "$candidate" ] && { CMAKE_BIN="$candidate"; break; }
    done
fi
[ -z "$CMAKE_BIN" ] && { echo "ERROR: cmake not found."; exit 1; }
echo "Using cmake: $CMAKE_BIN"

FFMPEG_BIN="$(command -v ffmpeg || true)"
[ -z "$FFMPEG_BIN" ] && { echo "ERROR: ffmpeg not found. brew install ffmpeg."; exit 1; }

# Homebrew's ffmpeg formula ships a separate ffprobe binary, which the renderer
# uses for audio-stream detection. Bundle it alongside ffmpeg or the worker hits
# FileNotFoundError when probing video files.
FFPROBE_BIN="$(command -v ffprobe || true)"
[ -z "$FFPROBE_BIN" ] && { echo "ERROR: ffprobe not found. brew install ffmpeg."; exit 1; }

OPENCV_LIB_DIR="$(brew --prefix opencv 2>/dev/null)/lib"
[ -d "$OPENCV_LIB_DIR" ] || { echo "ERROR: OpenCV lib dir not found. brew install opencv."; exit 1; }

# Detect host architecture for python-build-standalone
case "$(uname -m)" in
    arm64)  PBS_ARCH="aarch64-apple-darwin" ;;
    x86_64) PBS_ARCH="x86_64-apple-darwin"  ;;
    *) echo "ERROR: unsupported arch $(uname -m)"; exit 1 ;;
esac
PBS_TARBALL_NAME="cpython-${PYTHON_VERSION}+${PBS_RELEASE}-${PBS_ARCH}-install_only.tar.gz"
PBS_URL="https://github.com/astral-sh/python-build-standalone/releases/download/${PBS_RELEASE}/${PBS_TARBALL_NAME}"

# --------------------------------------------------------------
# 1) Build (Release)
# --------------------------------------------------------------
echo "==> Configuring + building Release..."
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"
"$CMAKE_BIN" -S "$PROJECT_ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
"$CMAKE_BIN" --build "$BUILD_DIR" -j

# The CMake target is `SimpleQtApp` with OUTPUT_NAME "Neovere". Depending on
# the CMake version the bundle directory ends up named either
# `SimpleQtApp.app` (newer) or `Neovere.app` (older).  Normalise to Neovere.app.
PRODUCED_APP="$(find "$BUILD_DIR" -maxdepth 1 -name '*.app' -type d | head -1)"
if [ -z "$PRODUCED_APP" ]; then
    echo "ERROR: cmake build did not produce any .app bundle under $BUILD_DIR"
    echo "       (got the following at top level:)"
    ls "$BUILD_DIR" | head -20
    exit 1
fi
if [ "$PRODUCED_APP" != "$APP_BUNDLE" ]; then
    echo "    Renaming $(basename "$PRODUCED_APP") → $(basename "$APP_BUNDLE")"
    mv "$PRODUCED_APP" "$APP_BUNDLE"
fi

# --------------------------------------------------------------
# 2) Qt frameworks via macdeployqt (its rpath complaints are non-fatal —
#    our recursive walker handles unresolved deps below)
# --------------------------------------------------------------
echo "==> Running macdeployqt..."
"$MACDEPLOYQT" "$APP_BUNDLE" -always-overwrite -verbose=1 || true

FRAMEWORKS_DIR="$APP_BUNDLE/Contents/Frameworks"
MACOS_DIR="$APP_BUNDLE/Contents/MacOS"
EXE="$MACOS_DIR/$APP_NAME"
mkdir -p "$FRAMEWORKS_DIR"

# --------------------------------------------------------------
# 3) ffmpeg + ffprobe
# --------------------------------------------------------------
echo "==> Bundling ffmpeg + ffprobe..."
cp "$FFMPEG_BIN" "$MACOS_DIR/ffmpeg"
chmod +x "$MACOS_DIR/ffmpeg"
cp "$FFPROBE_BIN" "$MACOS_DIR/ffprobe"
chmod +x "$MACOS_DIR/ffprobe"

# --------------------------------------------------------------
# 4) OpenCV dylibs
# --------------------------------------------------------------
echo "==> Bundling OpenCV dylibs..."
for src in "$OPENCV_LIB_DIR"/libopencv_*.dylib; do
    [ -L "$src" ] && continue
    cp -p "$src" "$FRAMEWORKS_DIR/"
done
for link in "$OPENCV_LIB_DIR"/libopencv_*.dylib; do
    if [ -L "$link" ]; then
        target="$(readlink "$link")"
        name="$(basename "$link")"
        [ -e "$FRAMEWORKS_DIR/$name" ] || (cd "$FRAMEWORKS_DIR" && ln -sf "$target" "$name")
    fi
done

# --------------------------------------------------------------
# 5) Recursive dependency walker:
#    For every Mach-O in Frameworks/ (and the main exe + bundled ffmpeg),
#    copy any /opt/homebrew or /usr/local dep into Frameworks/, then rewrite
#    every reference to @rpath/<basename>. Iterate until the set is closed.
# --------------------------------------------------------------
echo "==> Walking dependencies..."

is_external_path() {
    case "$1" in /opt/homebrew/*|/usr/local/*) return 0 ;; *) return 1 ;; esac
}

scanned_marker="$BUILD_DIR/.scanned"
: > "$scanned_marker"

TO_SCAN=()
for f in "$EXE" "$MACOS_DIR/ffmpeg" "$MACOS_DIR/ffprobe"; do
    [ -f "$f" ] && TO_SCAN+=("$f")
done
while IFS= read -r f; do
    TO_SCAN+=("$f")
done < <(find "$FRAMEWORKS_DIR" -maxdepth 1 -type f -name '*.dylib' ! -type l)
while IFS= read -r f; do
    TO_SCAN+=("$f")
done < <(find "$FRAMEWORKS_DIR" -type f -path '*/Versions/*' ! -name '*.prl' 2>/dev/null)

iteration=0
while [ ${#TO_SCAN[@]} -gt 0 ]; do
    iteration=$((iteration + 1))
    [ $iteration -gt 12 ] && { echo "    (dep walk exceeded 12 iterations; bailing)"; break; }

    NEXT_SCAN=()
    for bin in "${TO_SCAN[@]}"; do
        [ -L "$bin" ] && continue
        [ -f "$bin" ] || continue
        grep -qxF "$bin" "$scanned_marker" 2>/dev/null && continue
        echo "$bin" >> "$scanned_marker"

        # Fix own id if it's absolute
        own_id="$(otool -D "$bin" 2>/dev/null | tail -n +2 || true)"
        if [ -n "$own_id" ] && [[ "$own_id" != @rpath/* && "$own_id" != @executable_path/* && "$own_id" != @loader_path/* ]]; then
            install_name_tool -id "@rpath/$(basename "$bin")" "$bin" 2>/dev/null || true
        fi

        # Walk dependency references
        otool -L "$bin" 2>/dev/null | awk 'NR>1 {print $1}' | while read -r dep; do
            [ -z "$dep" ] && continue
            depname="$(basename "$dep")"
            [ "$depname" = "$(basename "$bin")" ] && continue

            if is_external_path "$dep"; then
                if [ ! -e "$FRAMEWORKS_DIR/$depname" ] && [ -f "$dep" ]; then
                    cp -p "$dep" "$FRAMEWORKS_DIR/$depname"
                    echo "    + bundled $depname"
                fi
                install_name_tool -change "$dep" "@rpath/$depname" "$bin" 2>/dev/null || true
            elif [[ "$dep" == @rpath/* ]]; then
                if [ ! -e "$FRAMEWORKS_DIR/$depname" ]; then
                    for prefix in /opt/homebrew/lib /opt/homebrew/opt/*/lib /usr/local/lib /usr/local/opt/*/lib; do
                        if [ -f "$prefix/$depname" ]; then
                            cp -p "$prefix/$depname" "$FRAMEWORKS_DIR/$depname"
                            echo "    + bundled $depname (from $prefix)"
                            break
                        fi
                    done
                fi
            fi
        done
    done

    # Pick up new arrivals (.dylib or extension-less Mach-O like Python)
    while IFS= read -r f; do
        if ! grep -qxF "$f" "$scanned_marker" 2>/dev/null; then
            file -b "$f" 2>/dev/null | grep -q 'Mach-O' && NEXT_SCAN+=("$f")
        fi
    done < <(find "$FRAMEWORKS_DIR" -maxdepth 1 -type f)
    TO_SCAN=("${NEXT_SCAN[@]+"${NEXT_SCAN[@]}"}")
done

# Ensure rpath includes Frameworks on the binaries that go through dyld first
ensure_rpath() {
    local bin="$1" rp="$2"
    if ! otool -l "$bin" 2>/dev/null | grep -A2 LC_RPATH | grep -q "path $rp "; then
        install_name_tool -add_rpath "$rp" "$bin" 2>/dev/null || true
    fi
}
ensure_rpath "$EXE" "@executable_path/../Frameworks"
ensure_rpath "$MACOS_DIR/ffmpeg" "@executable_path/../Frameworks"
ensure_rpath "$MACOS_DIR/ffprobe" "@executable_path/../Frameworks"

# --------------------------------------------------------------
# 6) Bundle python-build-standalone — a fully self-contained,
#    relocatable CPython that does NOT depend on Xcode CLT / Homebrew.
#
#    Layout in the bundle:
#      Contents/Frameworks/python/bin/python3
#      Contents/Frameworks/python/lib/python3.12/...
#
#    Tarball is cached under .pbs-cache/ so re-runs are fast.
# --------------------------------------------------------------
echo "==> Bundling Python ${PYTHON_VERSION} (${PBS_ARCH})..."
mkdir -p "$PBS_CACHE_DIR"
PBS_TARBALL="$PBS_CACHE_DIR/$PBS_TARBALL_NAME"
if [ ! -s "$PBS_TARBALL" ]; then
    echo "    Downloading $PBS_URL"
    curl -fL --retry 3 -o "$PBS_TARBALL.part" "$PBS_URL"
    mv "$PBS_TARBALL.part" "$PBS_TARBALL"
fi

# Place the python tree under Contents/Resources/ — NOT Contents/Frameworks/.
# codesign treats Resources/ as opaque data, but Frameworks/ as a directory
# it actively scans for nested CFBundles. The python install's directory
# layout (bin/ lib/ ...) trips that scanner up, so Resources/ is the right
# home for it.
RESOURCES_DIR="$APP_BUNDLE/Contents/Resources"
mkdir -p "$RESOURCES_DIR"
PYTHON_DIR="$RESOURCES_DIR/python"
rm -rf "$PYTHON_DIR"
mkdir -p "$PYTHON_DIR"
# python-build-standalone tarballs unpack to a top-level "python/" directory,
# so --strip-components=1 lands the contents directly under Frameworks/python/.
tar -xzf "$PBS_TARBALL" -C "$PYTHON_DIR" --strip-components=1
chmod +x "$PYTHON_DIR/bin/python3"* 2>/dev/null || true

# Strip non-essential files from the bundled Python:
#   - shell-script wrappers (2to3, idle, pip, pydoc, python3-config) — they're
#     not Mach-O, so codesign refuses to seal a .app that contains them.
#     Neovere only needs `python3` itself for `python3 -m venv`.
#   - the stdlib test suite (~80 MB and never used at runtime)
#   - the IDLE GUI and turtledemo (Tk/Tcl dependents we don't use)
#   - man pages
# Keep only the actual Mach-O python interpreter binaries; delete every other
# entry (shell-script wrappers like `python3.12-config`, `pip`, `pydoc3.12`,
# symlinks to them, etc.). We test the file type via `file` so we catch
# wrappers regardless of name.
for entry in "$PYTHON_DIR/bin/"*; do
    [ -e "$entry" ] || continue
    # Keep Mach-O executables; delete everything else (shell scripts, configs, broken symlinks)
    if file -b "$entry" 2>/dev/null | grep -q 'Mach-O'; then
        continue
    fi
    rm -rf "$entry"
done
# Re-create the python3 → python3.12 symlink in case we removed it above
if [ ! -e "$PYTHON_DIR/bin/python3" ] && [ -e "$PYTHON_DIR/bin/python3.12" ]; then
    (cd "$PYTHON_DIR/bin" && ln -sf python3.12 python3)
fi
rm -rf "$PYTHON_DIR/lib/python3.12/test"             2>/dev/null || true
rm -rf "$PYTHON_DIR/lib/python3.12/idlelib"          2>/dev/null || true
rm -rf "$PYTHON_DIR/lib/python3.12/turtledemo"       2>/dev/null || true
rm -rf "$PYTHON_DIR/lib/python3.12/tkinter"          2>/dev/null || true
rm -rf "$PYTHON_DIR/share"                           2>/dev/null || true
# include/ is C headers we don't need (we're not embedding libpython into the C++
# binary at runtime, just spawning python3 as a subprocess). It also makes
# codesign mis-detect python/ as a framework-like bundle.
rm -rf "$PYTHON_DIR/include"                         2>/dev/null || true
# Tcl/Tk packages — Neovere doesn't use tkinter, and these directories have
# structures (e.g. thread2.8.7/) that codesign mis-identifies as malformed bundles.
rm -rf "$PYTHON_DIR/lib/tcl"*                        2>/dev/null || true
rm -rf "$PYTHON_DIR/lib/tk"*                         2>/dev/null || true
rm -rf "$PYTHON_DIR/lib/itcl"*                       2>/dev/null || true
rm -rf "$PYTHON_DIR/lib/thread"*                     2>/dev/null || true
rm -rf "$PYTHON_DIR/lib/pkgconfig"                   2>/dev/null || true

# --------------------------------------------------------------
# 7) Ad-hoc codesign every Mach-O binary in the bundle.
#
#   install_name_tool invalidates any existing signatures, and recent macOS
#   refuses to load unsigned dylibs even with a quarantine bit set.
#
#   We avoid `codesign --deep` because python-build-standalone contains
#   directories like `lib/thread2.8.7/` that codesign misinterprets as
#   bundles. Instead we sign every Mach-O leaf individually (bottom-up),
#   then seal the .app at the end WITHOUT --deep.
# --------------------------------------------------------------
echo "==> Ad-hoc codesigning..."

sign_mach_o() {
    # Sign a single file ad-hoc. Returns 0 even on failure (best-effort).
    codesign --force --sign - --timestamp=none "$1" 2>/dev/null || true
}

# Step 1: walk every regular file in the bundle and ad-hoc sign anything
# that's Mach-O. This covers .dylib, .so, python interpreter binaries,
# Qt framework Mach-O binaries inside Versions/A/, helper executables, etc.
SIGNED_COUNT=0
while IFS= read -r f; do
    [ -L "$f" ] && continue
    if file -b "$f" 2>/dev/null | grep -q 'Mach-O'; then
        sign_mach_o "$f"
        SIGNED_COUNT=$((SIGNED_COUNT + 1))
    fi
done < <(find "$APP_BUNDLE" -type f)
echo "    signed $SIGNED_COUNT Mach-O files"

# Step 2: re-seal each Qt framework. We just modified the Mach-O binary
# inside it, so its _CodeSignature/CodeResources is now stale.  Without
# --deep this only re-signs the framework's own seal; the inner binary
# keeps the ad-hoc signature we just gave it.
FRAMEWORK_COUNT=0
while IFS= read -r -d '' fw; do
    codesign --force --sign - --timestamp=none "$fw" 2>/dev/null || true
    FRAMEWORK_COUNT=$((FRAMEWORK_COUNT + 1))
done < <(find "$FRAMEWORKS_DIR" -maxdepth 1 -type d -name '*.framework' -print0)
echo "    re-sealed $FRAMEWORK_COUNT frameworks"

# Step 3: seal the .app itself (no --deep — we already covered every Mach-O).
# Using --deep would crash on python-build-standalone's lib/thread2.8.7/ dir.
codesign --force --sign - --timestamp=none "$APP_BUNDLE" 2>/dev/null || true

# Verify. Wrap the diagnostic in `|| true` so pipefail doesn't kill the script
# when the verifier exits nonzero — we want the DMG step to still run.
if codesign --verify --strict "$APP_BUNDLE" 2>/dev/null; then
    echo "    ✓ codesign verify passed"
else
    echo "    ! codesign verify reports issues:"
    { codesign --verify --verbose=2 "$APP_BUNDLE" 2>&1 || true; } | sed 's/^/      /' | tail -5
fi

# --------------------------------------------------------------
# 8) DMG
# --------------------------------------------------------------
echo "==> Creating DMG..."
mkdir -p "$DIST_DIR"
rm -f "$DIST_DIR/$DMG_NAME"

DMG_STAGING="$BUILD_DIR/dmg-staging"
rm -rf "$DMG_STAGING"
mkdir -p "$DMG_STAGING"
cp -R "$APP_BUNDLE" "$DMG_STAGING/"
ln -s /Applications "$DMG_STAGING/Applications"

hdiutil create -volname "Neovere" \
    -srcfolder "$DMG_STAGING" \
    -ov -format UDZO \
    "$DIST_DIR/$DMG_NAME"

echo ""
echo "==> Done."
echo "App bundle: $APP_BUNDLE"
echo "DMG:        $DIST_DIR/$DMG_NAME"
echo ""
du -sh "$APP_BUNDLE" "$DIST_DIR/$DMG_NAME" 2>/dev/null || true
