# Usage

## Server Usage

The server handles motion-triggered recording and provides RTSP proxy streams for cameras.

### Starting the server

Simply run the server binary:

```bash
cd dist/server
./richserver
```

The server will:
- Start the HTTP REST API on port 8080 (default)
- Start the LIVE555 RTSP proxy on port 8554 (default)
- Listen for camera configurations from clients
- Begin motion detection and recording when cameras are added

### Server endpoints

- `GET /health` - Server health check
- `GET /cameras` - List configured cameras
- `POST /cameras` - Add a new camera
- `DELETE /cameras/{name}` - Remove a camera
- `GET /threads` - List active worker threads
- And more... (see server/main.cpp for full API)

---

## Client Usage

## Controls

- **Left-click** a stream tile: switch audio to that stream (if it contains audio)
- **Right-click** a tile: open context menu (reload, overlays, fullscreen, configuration, etc.)
- **Esc** / **Q**: quit

## Overlays

### Stream name overlay

- Appears on hover (and may also be forced on via context menu options)
- Auto-hides after a short idle period to avoid being distracting

### Audio controls overlay

- Only appears for the stream currently providing audio
- Provides **Mute** and **Volume** controls
- Auto-hides after a short idle period unless you are hovering/interacting with it

## Debug flags

- `--debug audio` enables verbose audio logging to stdout
- `--debug grid` enables grid debug information
- `--debug all` all debug info enabled

Example:

```bash
./richclient --debug audio
./richclient --debug audio rtsp://IP:PORT/STREAM-ID
./richclient --debug grid
./richclient --debug all
```
