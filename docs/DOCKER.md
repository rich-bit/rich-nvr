# Docker Usage

The project provides a Docker Compose configuration for running the NVR system in containers.

## Building the Image

From the project root:

```bash
docker build -t rich-nvr:latest .
# Or using docker-compose:
docker-compose build
```

## First Time Setup

Create the persistent data directories:

```bash
cd /path/to/rich-nvr

# Create persistent data directories
mkdir -p docker-dist/server/config docker-dist/server/media docker-dist/client

# Build the Docker images
docker-compose build
```

## Running the Services

**Run only the server (headless/background recording):**
```bash
docker-compose up -d nvrserver
```

**Run only the client (GUI viewer):**
```bash
docker-compose up nvrclient
```

**Note:** The client requires X11/WSLg for GUI display and runs in foreground mode (no `-d` flag) so you can see the window.

**Run both services together:**
```bash
docker-compose up -d
```

## Managing Services

**View logs:**
```bash
docker-compose logs -f nvrserver
docker-compose logs -f nvrclient
```

**Stop all services:**
```bash
docker-compose down
```

**Restart a specific service:**
```bash
docker-compose restart nvrserver
docker-compose restart nvrclient
```

**Rebuild after code changes:**
```bash
docker-compose build
docker-compose up -d
```

## Persistent Data Locations

All Docker persistent data is stored in `docker-dist/` (git-ignored):

- **Server config**: `docker-dist/server/config/cameras.json`
- **Recordings**: `docker-dist/server/media/`
- **Client config**: `docker-dist/client/client_config.json`

Configuration files are automatically created on first run.

## Ports

- **8080**: HTTP API (server)
- **8554**: RTSP proxy (server)

## X11 Display Access

**WSL2 with WSLg**: Works automatically, no configuration needed.

**Linux hosts**: You may need to allow Docker to access X11:
```bash
xhost +local:docker
```

## Notes

- Client and server share the same Docker image (`rich-nvr:latest`)
- The image includes both binaries (`nvrclient` and `nvrserver`)
- Qt6Core is still required by VideoExporter (TODO: replace with std:: alternatives)
- ImGui is used for the client UI (bundled in third_party/)
