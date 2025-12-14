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
cmake --build build --target richclient -j
```

Artifacts are typically staged under `dist/` by the project’s CMake.

## Cross-build Windows from Linux (MinGW-w64)

This repo includes a dedicated cross-build CMake project in `mingw3264/` that builds `richclient.exe` using MinGW-w64 and can **stage** the `.exe` + required `.dll`s into `dist/client/`.

### Prerequisites
- `x86_64-w64-mingw32-g++` (64-bit Windows target)
- A Windows-target SDL2 build (headers + import libs + `SDL2.dll`)
- A Windows-target FFmpeg build (headers + import libs + runtime DLLs)

### Configure / build / stage (mingw64)

```bash
cd mingw3264
cmake --preset mingw64-release
cmake --build build/mingw64-release -j
cmake --build build/mingw64-release --target stage
```

### Output

The `stage` target installs into `dist/client/` (as configured by `RICH_DIST_DIR`) and copies:
- `richclient.exe`
- `SDL2.dll`
- FFmpeg runtime `.dll`s
- MinGW runtime `.dll`s (best-effort)

### mingw32

A 32-bit build is supported **only if** you also have 32-bit Windows SDL2 + FFmpeg builds.

```bash
cd mingw3264
cmake --preset mingw32-release
cmake --build build/mingw32-release -j
cmake --build build/mingw32-release --target stage
```

## Note: CMake vs Ninja

- **CMake** configures/generates build files.
- **Ninja** (or Make) executes the compile/link steps.

Presets may use Ninja because it’s fast and reliable for incremental builds.
