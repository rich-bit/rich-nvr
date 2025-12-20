# Building

## Prerequisites

### Common
- C++17 compiler
- CMake >= 3.20

### Linux native build (typical)
- SDL2 development headers/libs
- FFmpeg development headers/libs (`libavformat`, `libavcodec`, `libavutil`, `libswscale`, `libswresample`)

Package names vary by distro.

## Linux build

From the repo root:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Build just the client target:

```bash
cmake --build build --target nvrclient -j
```

Artifacts are typically staged under `dist/` by the project’s CMake.

## Cross-build Windows from Linux (MinGW-w64)

This repo includes a dedicated cross-build CMake project in `mingw/` that builds `nvrclient.exe` using MinGW-w64 and can **stage** the `.exe` + required `.dll`s into `dist/client/`.

### Prerequisites
- `x86_64-w64-mingw32-g++` (64-bit Windows target)
- A Windows-target SDL2 build (headers + import libs + `SDL2.dll`)
- A Windows-target FFmpeg build (headers + import libs + runtime DLLs)

### Set environment variables

Export paths to your Windows dependencies:

```bash
export SDL2_ROOT=/path/to/SDL2-2.28.5/x86_64-w64-mingw32
export FFMPEG_ROOT=/path/to/ffmpeg-8.0.1-full_build-shared
```

### Configure / build / stage (mingw64)

```bash
cd mingw
cmake --preset mingw64-release
cmake --build build/mingw64-release -j
cmake --build build/mingw64-release --target stage
```

### Output

The `stage` target installs into `dist/client/` (as configured by `RICH_DIST_DIR`) and copies:
- `nvrclient.exe`
- `SDL2.dll`
- FFmpeg runtime `.dll`s
- MinGW runtime `.dll`s (best-effort)

### mingw32

A 32-bit build is supported **only if** you also have 32-bit Windows SDL2 + FFmpeg builds.

```bash
cd mingw
cmake --preset mingw32-release
cmake --build build/mingw32-release -j
cmake --build build/mingw32-release --target stage
```

## Note: CMake vs Ninja

- **CMake** configures/generates build files.
- **Ninja** (or Make) executes the compile/link steps.

Presets may use Ninja because it’s fast and reliable for incremental builds.
