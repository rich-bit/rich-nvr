# Network Video Recorder (rich-nvr)
This is a network video recorder suitable for home surveillance or for recording with many cameras optionally triggered via motion.
Built with open-source software, including **FFmpeg**, **SDL2**, **Dear ImGui**, **GStreamer**, **LIVE555**, **OpenCV**, **cpp-httplib** and **nlohmann/json**.

## Documentation

- Building: [docs/BUILDING.md](docs/BUILDING.md)
- Client usage: [docs/USAGE.md](docs/USAGE.md)

## Features

- **RTSP grid client** (SDL2 + ImGui)  
![Adding streams](showcase/add_streams.gif)

- **Setup motion-detect triggered recording with motion regions**  
![Motion detection setup](showcase/setup_motion.gif)

- **Running threads listed in info tab**  
![Info tab](showcase/info_tab.png)

- Switch audio between streams (left-click)
- Audio overlay: volume + mute (auto-hides)
- Stream name overlay (auto-hides unless pinned)
- MinGW-w64 cross-build for Windows via `mingw3264/`

### Server, using Live555 to create a proxied stream for clients to preview:
```text
(LAN/VLAN)
Camera A ─┐
Camera B ─┼──▶ LIVE555 Proxy @ rtsp://richnvr:8554 ──▶ Clients (VLC, ffplay, NVR)
Camera C ─┘
             proxyStream       → cam-a
             proxyStream-1     → cam-b
             proxyStream-2     → cam-c
```

### Server, motion-triggered recording:
```text
           ┌──────────────┐     pulls RTSP      ┌──────────────┐
rtsp://camA│  Camera A    │====================>│              │
           └──────────────┘                     │              │
           ┌──────────────┐     pulls RTSP      │  RichServer  │
rtsp://camB│  Camera B    │====================>│ (GStreamer   │
           └──────────────┘                     │ + OpenCV/Py) │
           ┌──────────────┐     pulls RTSP      │              │
rtsp://camC│  Camera C    │====================>│              │
           └──────────────┘                     └─────┬────────┘
                                                      │
                                                      │ (per camera)
                                              ┌───────▼────────────────────────┐
                                              │ Decode frames → Motion detect  │
                                              │  (BG subtract / frame diff)    │
                                              └───────┬────────────────────────┘
                                                      │   motion = TRUE / FALSE
                                     ┌────────────────┴────────────────┐
                                     │                                 │
                                     ▼                                 ▼
                            MOTION = TRUE                        MOTION = FALSE
                     ─────────────────────────            ─────────────────────────
                     • Open record gate                   • Keep analyzing frames
                     • Write .mkv file                    • No files written
                       named by date/time                   (idle)
                       e.g. recordings/<cam>/2025-09-10_12-34-56_<cam>.mkv

```

