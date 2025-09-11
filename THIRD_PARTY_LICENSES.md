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

### Qt 6
- **License:** LGPL-3.0 (with GPL/commercial alternatives)
- **Source:** https://www.qt.io/
- **Notes:** Rich-NVR is built against the LGPL version of Qt 6.  
  Under LGPL terms, users may relink against a modified Qt version.

### OpenCV
- **License:** BSD 3-Clause
- **Source:** https://opencv.org/
- **Notes:** Permissive license, compatible with GPL-3.0.

---

## Supporting Libraries

### nlohmann/json
- **License:** MIT
- **Source:** https://github.com/nlohmann/json
- **Notes:** Header-only JSON parser/serializer.

### cpp-httplib
- **License:** MIT
- **Source:** https://github.com/yhirose/cpp-httplib
- **Notes:** Header-only HTTP/HTTPS server & client library.

---

## Licensing Compatibility

- **GPL-3.0** (this project’s license) is fully compatible with LGPL, MIT, and BSD-licensed libraries.
- All permissive libraries (MIT/BSD) are combined under GPL-3.0 without conflict.
- LGPL libraries (Qt, GStreamer, LIVE555) are linked in compliance with LGPL; the combined work is distributed under GPL-3.0.

---

## Obligations for Distributors

If you redistribute Rich-NVR:
1. Provide access to the **complete corresponding source code** of Rich-NVR (per GPL-3.0).
2. Make available notices/licenses for all third-party libraries (this file).
3. For LGPL libraries (Qt, GStreamer, LIVE555):
   - You must allow users to **relink against modified versions** of those libraries (e.g. by using dynamic linking or providing object files).

---
