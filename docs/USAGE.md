# Client usage

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

Example:

```bash
./richclient --debug audio
```
