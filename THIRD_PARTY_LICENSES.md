# Third-Party Licenses

This document lists the main third-party libraries used in **Rich-NVR** and their respective licenses.  
Rich-NVR itself is licensed under **GPL-3.0**.  
Each dependency remains under its original license.

---

## Core Dependencies

### LIVE555 Streaming Media
- **License:** LGPL (GNU Lesser General Public License)
- **Source:** http://www.live555.com/liveMedia/
- **Notes:** Linked as a library. Redistribution requires compliance with LGPL (allowing relinking with modified versions).

### GStreamer
- **License:** LGPL-2.1+
- **Source:** https://gstreamer.freedesktop.org/
- **Notes:** Rich-NVR uses core GStreamer libraries and plugins. Some optional plugins may carry different licenses (e.g. GPL). Users are responsible for ensuring plugin license compliance.

### OpenCV
- **License:** BSD 3-Clause
- **Source:** https://opencv.org/
- **Notes:** Permissive license, compatible with GPL-3.0.

### Qt 6 (QtCore)
- **License:** LGPL-3.0 **or** GPL-3.0 **or** Commercial (depends on how Qt is obtained/licensed)
- **Source:** https://www.qt.io/
- **Notes:** Rich-NVR uses Qt (Qt6::Core) in the server/export pipeline. Qt is typically provided by the system or a separate Qt distribution (not vendored in this repo).
   If redistributing Qt binaries, you must comply with the license terms of the specific Qt build you ship.

### FFmpeg (libavformat / libavcodec / libavutil / libswscale / libswresample)
- **License:** LGPL-2.1+ **or** GPL-2.0+ (depends on how FFmpeg is configured/built)
- **Source:** https://ffmpeg.org/
- **Notes:** Rich-NVR includes and links against FFmpeg libraries for decoding and media processing.
   FFmpeg is typically provided by the system or a separate FFmpeg distribution (not vendored in this repo).
   If redistributing FFmpeg binaries, you must also comply with the license terms of the specific FFmpeg build you ship.

---

## Supporting Libraries

### Dear ImGui
- **License:** MIT
- **Source:** https://github.com/ocornut/imgui
- **Notes:** ImGui is vendored in this repository under `third_party/imgui/`. See `third_party/imgui/LICENSE.txt`.

### SDL2
- **License:** zlib
- **Source:** https://www.libsdl.org/
- **Notes:** The client uses SDL2 for windowing, rendering, and audio. When cross-building for Windows, `SDL2.dll` is staged into `dist/client/`.

### nlohmann/json
- **License:** MIT
- **Source:** https://github.com/nlohmann/json
- **Notes:** Header-only JSON parser/serializer.

### cpp-httplib
- **License:** MIT
- **Source:** https://github.com/yhirose/cpp-httplib
- **Notes:** Header-only HTTP/HTTPS server & client library.

---

## Windows Distribution Components

### MinGW-w64 / GCC runtime libraries
- **License:** GPL with runtime exception and/or LGPL (varies by component)
- **Source:** https://www.mingw-w64.org/ , https://gcc.gnu.org/
- **Notes:** Windows builds staged to `dist/client/` may include MinGW/GCC runtime DLLs such as:
   - `libstdc++-6.dll`
   - `libgcc_s_seh-1.dll`
   - `libwinpthread-1.dll`
   These DLLs are not vendored in this repository. If you redistribute them, you must comply with the license terms applicable to the specific binaries you ship.

---

## Licensing Compatibility

- **GPL-3.0** (this project’s license) is fully compatible with LGPL, MIT, BSD, and zlib-licensed libraries.
- All permissive libraries (MIT/BSD) are combined under GPL-3.0 without conflict.
- LGPL libraries (GStreamer, LIVE555, Qt if using an LGPL-licensed Qt build) are linked in compliance with LGPL; the combined work is distributed under GPL-3.0.

---

## Obligations for Distributors

If you redistribute Rich-NVR:
1. Provide access to the **complete corresponding source code** of Rich-NVR (per GPL-3.0).
2. Make available notices/licenses for all third-party libraries (this file).
3. For LGPL libraries (GStreamer, LIVE555, Qt if using an LGPL-licensed Qt build):
   - You must allow users to **relink against modified versions** of those libraries (e.g. by using dynamic linking or providing object files).

---
