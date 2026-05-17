# Build a self-contained Neovere distribution on Windows.
#
# Output: dist\Neovere-1.0-windows-x64.zip  (a portable folder users can unzip
# anywhere and run by double-clicking Neovere.exe)
#
# Bundled (the end user needs NOTHING preinstalled):
#   * Qt 6 DLLs + plugins   (via windeployqt.exe)
#   * OpenCV DLLs
#   * ffmpeg.exe + ffprobe.exe
#   * CPython (python-build-standalone) - a fully self-contained interpreter
#
# Python *site-packages* are NOT bundled. On first launch Neovere creates
# %USERPROFILE%\neovere_venv from the bundled interpreter and pip-installs
# its dependencies (numpy, opencv-python, scipy, librosa, ...). This keeps the
# distribution under ~400 MB.
#
# Requirements (must already be installed on the build machine):
#   - Qt 6 (MinGW or MSVC), with windeployqt on PATH or at a known location below
#   - CMake (any 3.16+)
#   - A C++ compiler matching the Qt build (g++ for MinGW Qt, MSVC for MSVC Qt)
#   - OpenCV (DLLs locatable via $OPENCV_BIN below)
#   - ffmpeg.exe and ffprobe.exe on PATH (e.g. via `winget install Gyan.FFmpeg`
#     or a Chocolatey / Scoop install)
#
# Run from a developer command prompt that has the compiler / Qt on PATH:
#     powershell -ExecutionPolicy Bypass -File package_windows.ps1

$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

# ============================================================================
# Configuration  --  adjust these paths if your install layout differs
# ============================================================================
$APP_NAME        = 'Neovere'
$VERSION         = '1.0'
$BUILD_DIR       = "build-windows"
$DIST_DIR        = "dist"
$DEPLOY_DIR      = "$BUILD_DIR\deploy"
$ZIP_NAME        = "$APP_NAME-$VERSION-windows-x64.zip"

# python-build-standalone version pin (same as the mac script).
$PYTHON_VERSION  = '3.12.7'
$PBS_RELEASE     = '20241016'
$PBS_TARBALL     = "cpython-$PYTHON_VERSION+$PBS_RELEASE-x86_64-pc-windows-msvc-shared-install_only.tar.gz"
$PBS_URL         = "https://github.com/astral-sh/python-build-standalone/releases/download/$PBS_RELEASE/$PBS_TARBALL"
$PBS_CACHE_DIR   = ".pbs-cache"

# Common Qt install locations to probe for windeployqt.
$QT_CANDIDATES = @(
    'C:\Qt\6.8.0\mingw_64',
    'C:\Qt\6.8.0\msvc2022_64',
    'C:\Qt\6.7.0\mingw_64',
    'C:\Qt\6.7.0\msvc2022_64',
    'C:\Qt\6.6.0\mingw_64',
    'C:\Qt\6.6.0\msvc2022_64'
)

# Common OpenCV install locations to probe for opencv_world*.dll.
$OPENCV_CANDIDATES = @(
    'C:\opencv\build\x64\vc16\bin',
    'C:\opencv\build\x64\vc15\bin',
    'C:\opencv\build\x64\mingw\bin',
    'C:\Tools\opencv\build\x64\vc16\bin'
)

# ============================================================================
# Tool discovery
# ============================================================================
function Find-Tool($name, $candidates) {
    $cmd = (Get-Command $name -ErrorAction SilentlyContinue).Path
    if ($cmd) { return $cmd }
    foreach ($c in $candidates) {
        if (Test-Path $c) { return $c }
    }
    return $null
}

Write-Host '==> Locating build tools...'

# windeployqt
$WINDEPLOYQT = $null
foreach ($qt in $QT_CANDIDATES) {
    $cand = Join-Path $qt 'bin\windeployqt.exe'
    if (Test-Path $cand) { $WINDEPLOYQT = $cand; break }
}
if (-not $WINDEPLOYQT) {
    $WINDEPLOYQT = (Get-Command windeployqt -ErrorAction SilentlyContinue).Path
}
if (-not $WINDEPLOYQT) {
    throw "windeployqt.exe not found. Set its path or install Qt 6, then re-run."
}
$QT_BIN_DIR = Split-Path $WINDEPLOYQT
Write-Host "    windeployqt: $WINDEPLOYQT"

# cmake
$CMAKE = (Get-Command cmake -ErrorAction SilentlyContinue).Path
if (-not $CMAKE) {
    foreach ($p in @('C:\Program Files\CMake\bin\cmake.exe', 'C:\Qt\Tools\CMake_64\bin\cmake.exe')) {
        if (Test-Path $p) { $CMAKE = $p; break }
    }
}
if (-not $CMAKE) { throw 'cmake.exe not found.' }
Write-Host "    cmake: $CMAKE"

# ffmpeg / ffprobe
$FFMPEG  = (Get-Command ffmpeg  -ErrorAction SilentlyContinue).Path
$FFPROBE = (Get-Command ffprobe -ErrorAction SilentlyContinue).Path
if (-not $FFMPEG)  { throw "ffmpeg.exe not on PATH. Install via 'winget install Gyan.FFmpeg' or 'choco install ffmpeg'." }
if (-not $FFPROBE) { throw "ffprobe.exe not on PATH (ships alongside ffmpeg in the same package)." }
Write-Host "    ffmpeg:  $FFMPEG"
Write-Host "    ffprobe: $FFPROBE"

# OpenCV bin dir
$OPENCV_BIN = $null
foreach ($c in $OPENCV_CANDIDATES) {
    if (Test-Path $c) {
        if ((Get-ChildItem $c -Filter 'opencv_world*.dll' -ErrorAction SilentlyContinue)) {
            $OPENCV_BIN = $c; break
        }
    }
}
if (-not $OPENCV_BIN) { throw 'OpenCV bin dir not found. Edit $OPENCV_CANDIDATES at the top of this script.' }
Write-Host "    opencv:  $OPENCV_BIN"

# ============================================================================
# 1) Build (Release)
# ============================================================================
Write-Host ''
Write-Host '==> Configuring + building Release...'
if (Test-Path $BUILD_DIR) { Remove-Item -Recurse -Force $BUILD_DIR }
New-Item -ItemType Directory -Force $BUILD_DIR | Out-Null

# Detect generator: prefer the one Qt was built with. Mingw / MSVC.
$generator = 'Ninja'  # falls back to default if Ninja not present
if (-not (Get-Command ninja -ErrorAction SilentlyContinue)) {
    if ($WINDEPLOYQT -like '*mingw*') { $generator = 'MinGW Makefiles' }
    else { $generator = 'Visual Studio 17 2022' }
}
Write-Host "    generator: $generator"

& $CMAKE -S . -B $BUILD_DIR -DCMAKE_BUILD_TYPE=Release -G $generator
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }

& $CMAKE --build $BUILD_DIR --config Release -j
if ($LASTEXITCODE -ne 0) { throw "cmake build failed" }

# Find the produced .exe (CMake may place it at $BUILD_DIR\Neovere.exe or
# $BUILD_DIR\Release\Neovere.exe depending on generator).
$produced = @(
    "$BUILD_DIR\$APP_NAME.exe",
    "$BUILD_DIR\Release\$APP_NAME.exe",
    "$BUILD_DIR\SimpleQtApp.exe",
    "$BUILD_DIR\Release\SimpleQtApp.exe"
) | Where-Object { Test-Path $_ } | Select-Object -First 1

if (-not $produced) { throw "Could not find built .exe under $BUILD_DIR" }
Write-Host "    built: $produced"

# ============================================================================
# 2) Stage the deploy directory
# ============================================================================
Write-Host ''
Write-Host '==> Staging deploy dir...'
if (Test-Path $DEPLOY_DIR) { Remove-Item -Recurse -Force $DEPLOY_DIR }
New-Item -ItemType Directory -Force $DEPLOY_DIR | Out-Null

# Copy the main exe in (rename if the cmake target was still SimpleQtApp.exe)
Copy-Item $produced "$DEPLOY_DIR\$APP_NAME.exe"

# ============================================================================
# 3) Qt DLLs + plugins via windeployqt
# ============================================================================
Write-Host ''
Write-Host '==> Running windeployqt...'
& $WINDEPLOYQT --release --no-translations --no-system-d3d-compiler `
    --no-opengl-sw "$DEPLOY_DIR\$APP_NAME.exe"
if ($LASTEXITCODE -ne 0) { throw "windeployqt failed" }

# ============================================================================
# 4) ffmpeg + ffprobe
# ============================================================================
Write-Host ''
Write-Host '==> Bundling ffmpeg + ffprobe...'
Copy-Item $FFMPEG  "$DEPLOY_DIR\ffmpeg.exe"
Copy-Item $FFPROBE "$DEPLOY_DIR\ffprobe.exe"

# ============================================================================
# 5) OpenCV DLLs
# ============================================================================
Write-Host ''
Write-Host '==> Bundling OpenCV DLLs...'
Get-ChildItem $OPENCV_BIN -Filter 'opencv_world*.dll' | ForEach-Object {
    Copy-Item $_.FullName $DEPLOY_DIR
    Write-Host "    + $($_.Name)"
}
# Some Homebrew/conda OpenCV builds also ship libopencv_video*.dll, etc.
Get-ChildItem $OPENCV_BIN -Filter 'libopencv_*.dll' -ErrorAction SilentlyContinue | ForEach-Object {
    Copy-Item $_.FullName $DEPLOY_DIR
    Write-Host "    + $($_.Name)"
}

# ============================================================================
# 6) MinGW runtime DLLs (only needed when Qt was built with MinGW)
# ============================================================================
if ($WINDEPLOYQT -like '*mingw*') {
    Write-Host ''
    Write-Host '==> Bundling MinGW runtime DLLs...'
    foreach ($dll in @('libgcc_s_seh-1.dll','libstdc++-6.dll','libwinpthread-1.dll')) {
        $src = Join-Path $QT_BIN_DIR $dll
        if (Test-Path $src) {
            Copy-Item $src $DEPLOY_DIR
            Write-Host "    + $dll"
        }
    }
}

# ============================================================================
# 7) python-build-standalone (CPython interpreter)
# ============================================================================
Write-Host ''
Write-Host "==> Bundling Python $PYTHON_VERSION..."
if (-not (Test-Path $PBS_CACHE_DIR)) { New-Item -ItemType Directory -Force $PBS_CACHE_DIR | Out-Null }
$tarball = Join-Path $PBS_CACHE_DIR $PBS_TARBALL
if (-not (Test-Path $tarball) -or (Get-Item $tarball).Length -lt 1MB) {
    Write-Host "    Downloading $PBS_URL"
    Invoke-WebRequest -Uri $PBS_URL -OutFile $tarball
}

# python-build-standalone for Windows unpacks to a top-level python/ dir
# containing python.exe, python312.dll, Lib/, DLLs/, etc.
$PYTHON_TARGET = "$DEPLOY_DIR\python"
if (Test-Path $PYTHON_TARGET) { Remove-Item -Recurse -Force $PYTHON_TARGET }
tar -xzf $tarball -C $DEPLOY_DIR
if ($LASTEXITCODE -ne 0) { throw "tar extract failed (Windows 10+ ships tar; install otherwise)" }

# Prune the same non-essential trees we strip on macOS:
$prunePaths = @(
    "$PYTHON_TARGET\Lib\test",
    "$PYTHON_TARGET\Lib\idlelib",
    "$PYTHON_TARGET\Lib\turtledemo",
    "$PYTHON_TARGET\Lib\tkinter",
    "$PYTHON_TARGET\tcl",
    "$PYTHON_TARGET\Doc",
    "$PYTHON_TARGET\share"
)
foreach ($p in $prunePaths) {
    if (Test-Path $p) { Remove-Item -Recurse -Force $p -ErrorAction SilentlyContinue }
}

# ============================================================================
# 8) Zip the deploy directory
# ============================================================================
Write-Host ''
Write-Host '==> Creating zip distribution...'
if (-not (Test-Path $DIST_DIR)) { New-Item -ItemType Directory -Force $DIST_DIR | Out-Null }
$zipPath = Join-Path $DIST_DIR $ZIP_NAME
if (Test-Path $zipPath) { Remove-Item $zipPath }

# Compress-Archive nests the source folder; we want the deploy contents at the
# zip root under a single Neovere/ folder, so stage a parent dir first.
$stage = "$BUILD_DIR\zip-stage\$APP_NAME"
if (Test-Path "$BUILD_DIR\zip-stage") { Remove-Item -Recurse -Force "$BUILD_DIR\zip-stage" }
New-Item -ItemType Directory -Force $stage | Out-Null
Copy-Item -Recurse "$DEPLOY_DIR\*" $stage
Compress-Archive -Path $stage -DestinationPath $zipPath

Write-Host ''
Write-Host '==> Done.'
Write-Host "Portable folder: $DEPLOY_DIR"
Write-Host "Zip:             $zipPath"
$size = (Get-Item $zipPath).Length / 1MB
Write-Host ("Zip size:        {0:N1} MB" -f $size)
Write-Host ''
Write-Host 'To distribute: ship the zip. End users unzip it and double-click Neovere.exe.'
Write-Host 'First launch will create %USERPROFILE%\neovere_venv and pip-install dependencies.'
