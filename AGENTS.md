# AGENTS.md

## Architecture
- Two independent codebases that meet only over LAN HTTP:
  - `bridge/` — Python 3.12 FastAPI server (port 5000) that drives Spotify. It is the server; the Pico is its client.
  - `firmware/` — C firmware for a Raspberry Pi Pico W (Pico SDK 2.3.0). It is an HTTP *client* only.
- No shared code between them.

## Gitignored, recreated manually (a fresh clone will not run)
- `bridge/.env` — Spotify client id/secret. Tracked template: `bridge/.env.template`.
- `bridge/.cache` — Spotify OAuth token, written after completing `/login` + `/callback`.
- `firmware/network_config.h` — WiFi SSID/password + `BRIDGE_IP`/`BRIDGE_PORT`. Must be created before the firmware compiles.
- `firmware/.vscode/` — Pico VS Code build config (also local-only).
- Never commit these. `TODO.md`, `build/`, `__pycache__/` are also gitignored.

## Bridge (Python)
- Run from `bridge/`: `python -m uvicorn main:app --host 0.0.0.0 --port 5000 --reload`
- Auth: open `/login` in a browser, finish OAuth, `/callback` writes `.cache`. `/action/*` requires an active Spotify playback device.
- `/track` returns a *cached* snapshot refreshed by a background poller thread (interval = `POLL_INTERVAL_SECONDS`, default 1s), started via FastAPI `lifespan`. Don't add per-request Spotify calls expecting fresher data.
- No tests/CI. Verify manually via `/docs` (Swagger) or curl.

## Firmware (C, Pico W)
- Built only through the VS Code "Raspberry Pi Pico" extension (`firmware/.vscode/extensions.json`); no command-line build is documented. SDK/toolchain live under `~/.pico-sdk/`. Output in `firmware/build/` (gitignored).
- lwIP raw API (`NO_SYS=1`, no BSD sockets), configured in `firmware/lwipopts.h`.
- The HTTP client in `pico-music-player.c` buffers only ~64 bytes (`http_state_t.response`): enough for an HTTP status line, NOT a JSON body. Reading `/track` JSON needs a larger buffer + a parser.
- Button on GPIO15 (active-low); tap/long-press gesture logic is in `main()`'s `while` loop.
- Display: 1.3" 128×64 monochrome OLED (SH1106). Drivers in `firmware/lib/OLED`, drawing API `lib/GUI/GUI_Paint.h`, fonts `lib/Fonts` (Font8/12/16/20/24).
- `firmware/test/` is standalone throwaway code, not built by the main `CMakeLists.txt`.
- Pico W joins 2.4 GHz networks only.

## Gotchas
- `BRIDGE_IP` in `network_config.h` is a hardcoded LAN IP; update it when the host/network changes.
- The two codebases have separate build/test flows; editing one never validates the other.
