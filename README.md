# rich-nvr
This is a do-it-yourself (DIY), under-construction, network video recorder suitable for home surveillance.
Built with fantastic open-source software: **GStreamer**, **LIVE555**, **OpenCV**, **Qt 6**, **cpp-httplib** and **nlohmann/json**.

### Client idea illustration
```text
┌────────────────────────────────────────────────────────────────────────────────────────┐
│ RichNVR Client Dashboard                                  [Layout: 2x2] [⚙]            
│ Streams: rtsp://richnvr:8554/cam-a | cam-b | cam-c | cam-d                             │
├────────────────────────────────────────────────────────────────────────────────────────┤
│┌─────────────────────────────────────────┐  ┌─────────────────────────────────────────┐│
││ CAM A — Lobby                           │  │ CAM B — Garage                          ││
││                                         │  │                                         ││
││ Live Video [▒▒▒▒▒▒▒▒▒▒]                 │  │ Live Video [▒▒▒▒▒▒▒▒▒▒]                 ││
││ FPS: 25   Bitrate: 3.1 Mbps             │  │ FPS: 24   Bitrate: 2.6 Mbps             ││
││ Motion: [██▁▁▁]  Status: LIVE TCP           Motion: [▁▁▁▁▁]  Status: LIVE TCP     
│└─────────────────────────────────────────┘  └─────────────────────────────────────────┘│
├────────────────────────────────────────────────────────────────────────────────────────┤
│┌─────────────────────────────────────────┐  ┌─────────────────────────────────────────┐│
││ CAM C — Driveway                        │  │ CAM D — Backyard                        ││
││                                         │  │                                         ││
││ Live Video [▒▒▒▒▒▒▒▒▒▒]                 │  │ Live Video [▒▒▒▒▒▒▒▒▒▒]                 ││
││ FPS: 25   Bitrate: 3.4 Mbps             │  │ FPS: 25   Bitrate: 2.9 Mbps             ││
││ Motion: [████▁]  Status: LIVE TCP            Motion: [▁▁▁▁▁]  Status: LIVE TCP      
│└─────────────────────────────────────────┘  └─────────────────────────────────────────┘│
├────────────────────────────────────────────────────────────────────────────────────────┤
│ [Play/Pause] [Mute] [Snapshot] [Record] [Full Screen]   CPU: 23%  GPU: 41%             │
│ Timeline: |■■■──────|   Last event: CAM C motion                                       │
└────────────────────────────────────────────────────────────────────────────────────────┘
```

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

