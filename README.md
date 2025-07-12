# rich-nvr
This is a do-it-yourself (DIY), under-construction network video recorder suitable for home surveillance.
Built with fantastic open-source software: **GStreamer**, **LIVE555**, **OpenCV**, **Qt 6**, **cpp-httplib**, and **nlohmann/json**.


### Using Live555 to create a proxied stream for clients to preview:
```text
### Using LIVE555 to create a proxied stream for clients to preview
```text
(LAN/VLAN)
Camera A ─┐
Camera B ─┼──▶ LIVE555 Proxy @ rtsp://richnvr:8554 ──▶ Clients (VLC, ffplay, NVR)
Camera C ─┘
             proxyStream       → cam-a
             proxyStream-1     → cam-b
             proxyStream-2     → cam-c
```

### Motion-triggered recording:
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

