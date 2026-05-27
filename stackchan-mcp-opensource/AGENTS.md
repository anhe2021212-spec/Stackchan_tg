# Stack-chan MCP Project Guide

This repository contains firmware and host-side tooling for a push-based
Stack-chan voice avatar running on M5Stack CoreS3.

## Scope

- `firmware/` — Arduino/PlatformIO firmware for M5Stack CoreS3
- `mcp-server/` — Python MCP server that lets Claude control Stack-chan over HTTP
- `faces/` and `firmware/data/` — PNG face assets used by the device

## Firmware HTTP API (port 80)

- `POST /play` — queue a WAV URL for playback
- `POST /move` — move head servos (x/y/speed)
- `POST /home` / `POST /nod` / `POST /shake` — preset gestures
- `POST /face` / `GET /face` — set/get face expression
- `GET /snapshot` — capture 320x240 JPEG from camera

Key source files:

- `firmware/src/http_server.cpp`
- `firmware/src/mic_service.cpp` (PTT recording + upload)
- `firmware/src/playback_service.cpp`
- `firmware/src/face_service.cpp`
- `firmware/src/servo_service.cpp`
- `firmware/src/wifi_manager.cpp`

## Build & Upload

```sh
cd firmware
pio run              # build
pio run -t upload    # flash to device
pio device monitor   # serial monitor
pio run -t uploadfs  # upload face assets to SPIFFS
```

## MCP Server

`mcp-server/server.py` provides MCP tools for speaking, moving the head,
changing faces, and taking snapshots.

Environment variables (see `.env.example`):

- `STACKCHAN_IP` / `STACKCHAN_PORT` — device address
- `MAC_IP` / `AUDIO_SERVE_PORT` — host audio server
- `TTS_ENGINE` — `edge-tts` (free), `minimax`, or `fish-audio`

## Quality Checks

```sh
make lint   # ruff + pio check
make test   # pytest + pio build
```

## Development Style

- Small, focused changes matching existing Arduino C++ and Python style
- Keep firmware responsive: async where possible, no blocking in main loop
- Preserve PSRAM-aware allocation for audio, face assets, and camera buffers
- When changing HTTP contracts, update both firmware and MCP server
