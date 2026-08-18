# Neovere

A desktop tool for generative and audio-reactive video. Qt 6 C++ application
with an embedded Python interpreter — you write Python in the built-in editor
and Neovere renders the frames.

> Early work in progress. Expect rough edges.

## Concepts

- **Fields** — vector-based masks stored as geometry (polygons / contours)
  rather than bitmaps, rasterized on demand at whatever resolution the
  renderer needs. Carries reversible render-time effects: blur, feather, invert.
- **Filters** — an effect masked through a Field.
- **Classes** — helpers for frame data, e.g. `FrameAudio` exposing per-frame
  volume, frequency bins, and magnitudes.

See [`documentation/`](documentation) for the generated API reference.

## Features

- Python editor with syntax highlighting, tabs, and search
- Frame-accurate scrubbing and preview playback with a render cache
- Audio analysis helpers for driving effects from a soundtrack
- Adjustable preview vs. render quality (`dx` / `dt` scaling)
- Optional AI assist in the editor (OpenAI API)

Projects are saved as `.nv` files.

## Building

Requirements: CMake 3.5+, a C++14 compiler, Qt 6 (Widgets, Multimedia,
MultimediaWidgets), Python 3 with development headers, and OpenCV.

```bash
cmake -S . -B build && cmake --build build
```

CMake looks for Qt and OpenCV under Homebrew prefixes on macOS and
`C:/Qt` / `C:/opencv` on Windows — adjust `CMAKE_PREFIX_PATH` in
`CMakeLists.txt` if yours live elsewhere.

Python runtime dependencies (`setup_env.sh` creates a venv with these):

```
pillow  opencv-python  scipy  librosa  soundfile  openai  numpy  psutil
```

## Packaging

- macOS — `./package_macos.sh` (bundles Qt via `macdeployqt`, ad-hoc signed)
- Windows — `./package_windows.ps1` (bundles Qt via `windeployqt`), then
  `installer_windows.iss` for an Inno Setup installer

Builds are not code-signed with a CA certificate, so expect an unidentified
developer warning on first launch.

## Configuration

Settings live in `settings.txt` next to the executable, written by the in-app
Settings dialog. This file holds your OpenAI API key and is gitignored — don't
commit it, and don't paste keys directly into project files or source.
