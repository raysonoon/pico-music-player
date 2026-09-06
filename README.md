# pico-music-player

A wireless music controller built with a **Raspberry Pi Pico W** for Spotify. The Pico talks to a small Python bridge server that drives Spotify playback, letting you control your music (play, pause, next, previous, restart) and view the current track on a hardware remote.

## Versions
- Python 3.12

## Dependencies
- **FastAPI** - web framework for building the API that serves music/playback endpoints to the Pico
- **uvicorn** - async server gateway interface (ASGI) server that runs and hosts the FastAPI application
- **spotipy** - Spotify Web API client used to fetch track metadata, playlists, and stream URLs
  - **requests** - HTTP library used for low-level API calls and downloading audio data
- **python-dotenv** - loads environment variables (Spotify credentials) from a local `.env` file
= **pillow** - imaging library to add image processing capabilities

## Getting started
- Set `SPOTIFY_CLIENT_ID`, `SPOTIFY_CLIENT_SECRET`, and `SPOTIFY_REDIRECT_URI` in a `.env` file in the `bridge/` directory
- From project root:
```bash
cd bridge
pip install -r requirements.txt
python -m uvicorn main:app --host 0.0.0.0 --port 5000 --reload
```
- Open `http://localhost:5000/login` in a browser to authenticate with Spotify

## Testing endpoints
FastAPI automatically generates interactive API docs. With the server running, open:

- `http://localhost:5000/docs` - interactive Swagger UI to test all endpoints (try `/action/play`, `/action/pause`, `/action/next`, `/track`)
- `http://localhost:5000/redoc` - alternative ReDoc view

Note: `/action` commands require an active Spotify playback device (desktop app, phone, or web player).