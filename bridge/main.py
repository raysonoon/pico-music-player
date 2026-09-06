from fastapi import FastAPI, HTTPException
from spotipy import Spotify
from spotipy.oauth2 import SpotifyOAuth
import os
import threading
from contextlib import asynccontextmanager
from PIL import Image, ImageDraw, ImageFont
from dotenv import load_dotenv

load_dotenv()

POLL_INTERVAL_SECONDS = float(os.getenv("POLL_INTERVAL_SECONDS", "1"))

# In-memory mode state (set by gestures)
current_mode = "default"

# OLED text layout (must match the firmware constants)
FRAME_WIDTH = 128
TITLE_H = 13
ARTIST_H = 13
ALBUM_H = 12
TITLE_X_OFFSET = 13  # leaves room for the play/pause glyph

# Cached playback snapshot, refreshed by a background poller thread.
_snapshot = {
    "is_playing": False,
    "title": "Nothing Playing",
    "artist": "N/A",
    "album": "",
    "progress_ms": 0,
    "duration_ms": 0,
    "progress_percent": 0,
    "title_bmp": "00" * (FRAME_WIDTH * TITLE_H // 8),
    "artist_bmp": "00" * (FRAME_WIDTH * ARTIST_H // 8),
    "album_bmp": "00" * (FRAME_WIDTH * ALBUM_H // 8),
}
_snapshot_lock = threading.Lock()

# Spotify API Credentials
CLIENT_ID = os.getenv("SPOTIFY_CLIENT_ID")
CLIENT_SECRET = os.getenv("SPOTIFY_CLIENT_SECRET")
REDIRECT_URI = os.getenv("SPOTIFY_REDIRECT_URI")

# Permissions needed to control playback, read song info and save tracks
SCOPE = "user-modify-playback-state user-read-playback-state user-library-modify"

# Initialize Spotipy's SpotifyOAuth manager
# cache_path='.cache' saves refresh tokens locally so you only log in once
sp_oauth = SpotifyOAuth(
    client_id=CLIENT_ID,
    client_secret=CLIENT_SECRET,
    redirect_uri=REDIRECT_URI,
    scope=SCOPE,
    cache_path=".cache"
)


def _load_font(size):
    """Load a CJK-capable TTF, falling back to Pillow's default bitmap font."""
    candidates = [os.getenv("FONT_PATH")] + [
        r"C:\Windows\Fonts\msyh.ttc",
        r"C:\Windows\Fonts\simhei.ttf",
        r"C:\Windows\Fonts\simsun.ttc",
    ]
    for c in candidates:
        if c and os.path.exists(c):
            try:
                return ImageFont.truetype(c, size)
            except Exception:
                continue
    return ImageFont.load_default()


_font9 = _load_font(9)
_font8 = _load_font(8)


def _render_line(text, height, font, x_offset=0):
    """Render a single text line to a 1-bit bitmap, hex-encoded for the Pico."""
    img = Image.new("1", (FRAME_WIDTH, height), 0)
    d = ImageDraw.Draw(img)
    t = text or ""
    avail = FRAME_WIDTH - x_offset
    if d.textlength(t, font=font) > avail:
        ell = "..."
        while t and d.textlength(t + ell, font=font) > avail:
            t = t[:-1]
        t = t + ell
    d.text((x_offset, 0), t, font=font, fill=1)
    return img.tobytes().hex()


def _render_bitmaps(snap):
    """Attach the three rendered text lines (hex bitmaps) to a snapshot."""
    try:
        snap["title_bmp"] = _render_line(snap["title"], TITLE_H, _font9, TITLE_X_OFFSET)
        snap["artist_bmp"] = _render_line(snap["artist"], ARTIST_H, _font9, 0)
        snap["album_bmp"] = _render_line(snap["album"], ALBUM_H, _font8, 0)
    except Exception:
        snap["title_bmp"] = "00" * (FRAME_WIDTH * TITLE_H // 8)
        snap["artist_bmp"] = "00" * (FRAME_WIDTH * ARTIST_H // 8)
        snap["album_bmp"] = "00" * (FRAME_WIDTH * ALBUM_H // 8)


def _poll_playback(stop_event):
    """Background thread: poll Spotify playback state and cache the latest snapshot."""
    sp = Spotify(auth_manager=sp_oauth)
    while not stop_event.is_set():
        try:
            playback = sp.current_playback()
            if playback and playback.get("item"):
                item = playback["item"]
                artists = item.get("artists") or []
                artist = ", ".join(a["name"] for a in artists if a.get("name"))
                duration_ms = item.get("duration_ms", 0) or 0
                progress_ms = playback.get("progress_ms", 0) or 0
                progress_percent = round(progress_ms * 100 / duration_ms) if duration_ms else 0
                snap = {
                    "is_playing": bool(playback.get("is_playing")),
                    "title": item.get("name", "Unknown"),
                    "artist": artist or "N/A",
                    "album": (item.get("album") or {}).get("name", ""),
                    "progress_ms": progress_ms,
                    "duration_ms": duration_ms,
                    "progress_percent": progress_percent,
                }
            else:
                snap = {
                    "is_playing": False,
                    "title": "Nothing Playing",
                    "artist": "N/A",
                    "album": "",
                    "progress_ms": 0,
                    "duration_ms": 0,
                    "progress_percent": 0,
                }
            _render_bitmaps(snap)
            with _snapshot_lock:
                _snapshot.update(snap)
        except Exception:
            pass
        stop_event.wait(POLL_INTERVAL_SECONDS)


@asynccontextmanager
async def lifespan(app):
    stop_event = threading.Event()
    thread = threading.Thread(target=_poll_playback, args=(stop_event,), daemon=True)
    thread.start()
    yield
    stop_event.set()


app = FastAPI(title="Spotify Hardware Remote Bridge", lifespan=lifespan)


def get_spotify_client():
    """Helper to retrieve an authenticated Spotipy client, auto-refreshing tokens if expired."""
    token_info = sp_oauth.get_cached_token()
    if not token_info:
        raise HTTPException(
            status_code=401, 
            detail="User not authenticated. Please visit /login in your browser."
        )
    return Spotify(auth=token_info["access_token"])


def _safe_shuffle(sp, state):
    """Best-effort shuffle toggle; some playlists/contexts disallow shuffle."""
    try:
        sp.shuffle(state)
    except Exception:
        pass


@app.get("/")
def read_root():
    return {"status": "Spotify Bridge is Running"}


@app.get("/login")
def login():
    """Generates the Spotify OAuth login URL."""
    auth_url = sp_oauth.get_authorize_url()
    return {"message": "Open this URL in your browser to log in", "url": auth_url}


@app.get("/callback")
def callback(code: str):
    """Callback route that Spotify redirects to after login, saving tokens to disk."""
    token_info = sp_oauth.get_access_token(code)
    if token_info:
        return {"status": "Success", "message": "Authenticated successfully! You can close this tab."}
    raise HTTPException(status_code=400, detail="Failed to retrieve access token.")


@app.post("/action/{command}")
def handle_action(command: str):
    """
    Endpoint called by your Pico W or test tools.
    Commands supported: play, pause, next, previous, restart,
                        previous-or-restart, minimalist, party, favourite
    """
    global current_mode
    sp = get_spotify_client()
    result = ""
    
    try:
        if command == "play":
            sp.start_playback()
            result = "Play"
        elif command == "pause":
            sp.pause_playback()
            result = "Pause"
        elif command == "play-pause":
            # Toggle play/pause state dynamically
            playback = sp.current_playback()
            if playback and playback.get("is_playing"):
                sp.pause_playback()
                result = "Pause"
            else:
                sp.start_playback()
                result = "Play"
        elif command == "next":
            sp.next_track()
            result = "Next"
        elif command == "previous":
            sp.previous_track()
            result = "Previous"
        elif command == "restart":
            # Restart current track (seek to 0ms)
            sp.seek_track(position_ms=0)
            result = "Restart"
        elif command == "previous-or-restart":
            # Restart current track if it's been playing >= 4s, otherwise skip to previous
            playback = sp.current_playback()
            if playback and playback.get("progress_ms", 0) >= 10000:
                sp.seek_track(position_ms=0)
                result = "Restart"
            else:
                sp.previous_track()
                result = "Previous"
        elif command == "minimalist":
            # Toggle minimalist mode (single mode at a time)
            if current_mode == "minimalist":
                current_mode = "default"
                result = "Minimalist off"
            else:
                if current_mode == "party":
                    _safe_shuffle(sp, False)  # party's side effect off
                current_mode = "minimalist"
                result = "Minimalist on"
        elif command == "party":
            # Toggle party mode and shuffle playback (single mode at a time)
            if current_mode == "party":
                current_mode = "default"
                _safe_shuffle(sp, False)
                result = "Party off"
            else:
                current_mode = "party"
                _safe_shuffle(sp, True)
                result = "Party on"
        elif command == "favourite":
            # Save the currently playing track to Liked Songs
            track = sp.current_user_playing_track()
            if track and track.get("item"):
                sp.current_user_saved_tracks_add(tracks=[track["item"]["id"]])
                result = "Favourite"
            else:
                raise HTTPException(status_code=400, detail="Nothing currently playing.")
        else:
            raise HTTPException(status_code=400, detail="Invalid action command.")
            
        return {"status": "success", "executed_command": command, "result": result}
    except Exception as e:
        raise HTTPException(status_code=500, detail=str(e))


@app.get("/track")
def get_current_track():
    """Returns the cached playback snapshot refreshed by the background poller."""
    with _snapshot_lock:
        return dict(_snapshot)


@app.get("/mode")
def get_current_mode():
    """Returns the current bridge mode (e.g. 'default', 'minimalist', 'party')."""
    return {"mode": current_mode}