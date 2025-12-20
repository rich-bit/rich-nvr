# Docker Usage

## Building the Image

From the project root:

```bash
docker build -t rich-nvr:latest .
```

## Running the Server

```bash
cd server
docker compose up -d
```

The server will:
- Listen on port 8080 for HTTP API
- Listen on port 8554 for RTSP proxy streams
- Store recordings in `./media/`
- Use `./cameras.json` for camera configuration (if present)

View logs:
```bash
docker compose logs -f
```

Stop:
```bash
docker compose down
```

## Running the Client

**Note**: The client requires X11 display access. This works automatically on WSL2 with WSLg.

```bash
cd client  
docker compose up -d
```

The client will:
- Connect to X11 display for GUI
- Store configuration in `./config/`
- Connect to audio via PulseAudio/PipeWire

For Linux hosts (not WSL), you may need to allow Docker to access X11:
```bash
xhost +local:docker
```

View logs:
```bash
docker compose logs -f
```

Stop:
```bash
docker compose down
```

## Running Both

```bash
# Start server
cd server && docker compose up -d && cd ..

# Start client  
cd client && docker compose up -d && cd ..
```

## Notes

- Client and server share the same Docker image (`rich-nvr:latest`)
- The image includes both binaries (`nvrclient` and `nvrserver`)
- Qt6Core is still required by VideoExporter (TODO: replace with std:: alternatives)
- ImGui is used for the client UI (bundled in third_party/)
