# Usage

## Server Usage

The server handles motion-triggered recording and provides RTSP proxy streams for cameras.

### Starting the server

Simply run the server binary:

```bash
cd dist/server
./nvrserver
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
- `--debug motion-frame` enables motion frame processing debug output
- `--debug perf` enables performance/decode timing debug output (includes frame rate limiting info)
- `--debug all` all debug info enabled

Example:

```bash
./nvrclient --debug audio
./nvrclient --debug audio rtsp://IP:PORT/STREAM-ID
./nvrclient --debug grid
./nvrclient --debug motion-frame
./nvrclient --debug perf
./nvrclient --debug all
```

## Frame Rate Limiting

The client includes automatic frame rate limiting to prevent videos from playing too fast when RTSP servers send frames faster than real-time.

**How it works:**
- The client automatically detects the stream's native frame rate (24fps, 25fps, 30fps, etc.) from FFmpeg metadata
- Frames are paced to match the detected rate, preventing playback from being too fast
- **Enabled by default** for new cameras

**When to use:**
- ✅ Publishing files with FFmpeg **without** `-re` flag (frames sent as fast as possible)
- ✅ Testing with looped video files
- ✅ Streams that send frames faster than real-time

**When to disable:**
- ❌ Streams already rate-limited by the source (e.g., real IP cameras)
- ❌ When using FFmpeg with `-re` flag (real-time rate limiting)
- ❌ Via-server cameras (automatically disabled)

**To configure:**
- Check/uncheck "Limit frame rate to stream's native FPS" in Add Camera tab
- Frame rate limiting is automatically disabled for via-server cameras

**Example - Testing with BigBuckBunny:**

```bash
# OPTION 1: Use -re flag (FFmpeg controls timing)
# Frame rate limiting can be disabled - FFmpeg already limits to 24fps
ffmpeg -re -stream_loop -1 -i BigBuckBunny.mp4 \
  -c copy \
  -f rtsp \
  -rtsp_transport tcp \
  rtsp://localhost:8553/bunny

# OPTION 2: Without -re (FFmpeg sends as fast as possible)
# Frame rate limiting REQUIRED - client limits to detected 24fps
ffmpeg -stream_loop -1 -i BigBuckBunny.mp4 \
  -c copy \
  -f rtsp \
  -rtsp_transport tcp \
  rtsp://localhost:8553/bunny

# Use --debug perf to see frame rate detection and pacing
./nvrclient --debug perf
# Output: [Perf] [Stream 0] Frame rate limiting: ENABLED | Target FPS: 24.000000
#         [Perf] [Stream 0] Frame pacing sleep: 36ms
```

## RTSP Stream Configuration

Each camera can have custom RTSP settings to optimize for different network conditions and stream types.

**Access via:**
- **Add Camera tab**: "More stream settings" button (for direct connections only)
- **Right-click stream**: "More stream settings" menu item (for existing streams)

**Quick Presets:**
- **Low Latency (UDP)**: Minimal buffering, UDP transport - best for local network, lowest latency
- **Low Latency (TCP)**: Minimal buffering, TCP transport - reliable low latency
- **Stable (TCP)**: More buffering, TCP transport - best for unstable connections
- **Reset to Defaults**: Back to recommended settings

**Available Settings:**
- **Transport Protocol**: TCP (reliable) vs UDP (lower latency, can drop packets)
- **Timeout**: Connection/read timeout in seconds
- **Max Delay**: Maximum demuxing delay (lower = less latency, higher = more buffering)
- **Buffer Size**: Network receive buffer (increase for unstable connections)
- **Disable Internal Buffering**: Reduces latency by disabling FFmpeg's buffer
- **Low Latency Mode**: Skip B-frames for lower decode latency
- **Hardware Acceleration**: Use GPU for decoding (CUDA, D3D11VA, VAAPI, etc.)
- **Probe/Analyze settings**: How much to analyze stream on connect

**Settings persist** to `client_config.json` and reload on startup.
