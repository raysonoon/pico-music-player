# pico-music-player

Developer notes for a wireless music controller built with a **Raspberry Pi Pico W** for Spotify. The Pico talks to a small Python bridge server that drives Spotify playback, letting you control music (play, pause, next, previous, restart) and view the current track on a hardware remote.

> End users: see [`HOW_TO_SETUP.txt`](HOW_TO_SETUP.txt) for plug-and-play setup via `setup.bat`.

## Architecture
- **Pico W firmware** (`firmware/`) — reads buttons/gestures, renders OLED frames, and issues HTTP requests to the bridge.
- **Python bridge** (`bridge/main.py`) — FastAPI server that wraps the Spotify Web API. Polls playback state, renders text/art to 1-bit bitmaps the Pico displays, and handles OAuth via Spotipy.
- **Runtime network config** (`firmware/net_config.{c,h}`) — Wi-Fi / bridge settings are stored on the Pico in a flash sector (via `setup.bat`/`setup.py` over USB serial)

## Versions
- Python 3.12

## Dependencies
- **FastAPI** - web framework for the API that serves music/playback endpoints to the Pico
- **uvicorn** - ASGI server that runs and hosts the FastAPI application
- **spotipy** - Spotify Web API client used for OAuth, playback, and metadata
  - **requests** - HTTP library used for low-level API calls
- **python-dotenv** - loads environment variables (Spotify credentials) from a local `.env` file
- **pillow** - imaging library to render OLED bitmaps

## Getting started

### 1. Flash the firmware
- Open the `firmware/` folder in VS Code with the **Raspberry Pi Pico** extension and build + flash `pico-music-player`.
- Firmware references `pico_sdk_import.cmake`; CMake is configured by the Pico extension.

### 2. Run the bridge manually (development)
- Set `SPOTIFY_CLIENT_ID`, `SPOTIFY_CLIENT_SECRET`, and `SPOTIFY_REDIRECT_URI` in `bridge/.env` (see `bridge/.env.template`).
- From project root:
```bash
cd bridge
pip install -r requirements.txt
python -m uvicorn main:app --host 0.0.0.0 --port 5000 --reload
```
- Open `http://localhost:5000/login` in a browser to authenticate with Spotify.

> **Spotify login requirements:** the account must have **Premium** (playback control requires it), and its email must be added to the app in the Spotify Developer Dashboard.

### 3. Push network settings over USB (end-user flow)
- Non-technical users run `setup.bat` (or `python setup.py`). It warns about 2.4 GHz-only Wi-Fi, asks for SSID/password, auto-detects this computer's IP, and writes the config to the Pico over USB serial. The Pico must be in setup mode (no saved config, or button held during power-on).
- This also creates `bridge/.env` and opens the browser for Spotify login.

## Testing endpoints
FastAPI auto-generates interactive API docs. With the server running, open:
- `http://localhost:5000/docs` - interactive Swagger UI (try `/action/play`, `/action/pause`, `/action/next`, `/track`)
- `http://localhost:5000/redoc` - alternative ReDoc view

Note: `/action` commands require an active Spotify playback device (desktop app, phone, or web player).

## Firmware ↔ bridge contract
- Display bitmaps, OLED layout constants, and mode strings in `bridge/main.py` must match the firmware constants.
- Gestures post to `/action/{command}`; the Pico polls `/track` for the rendered frame.
- Album art and tempo come from ReccoBeats; BPM tap / find-song flows live in `bridge/main.py`.

## Reset w/o BOOTSEL
- 1200-baud serial reset
```powershell
$p = New-Object System.IO.Ports.SerialPort COM5,1200,None,8,one
$p.Open(); $p.Close()
```