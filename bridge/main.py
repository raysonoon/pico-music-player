from fastapi import FastAPI, HTTPException
from spotipy import Spotify
from spotipy.oauth2 import SpotifyOAuth
import os
from dotenv import load_dotenv

load_dotenv()

app = FastAPI(title="Spotify Hardware Remote Bridge")

# In-memory mode state (set by gestures)
current_mode = "default"

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

def get_spotify_client():
    """Helper to retrieve an authenticated Spotipy client, auto-refreshing tokens if expired."""
    token_info = sp_oauth.get_cached_token()
    if not token_info:
        raise HTTPException(
            status_code=401, 
            detail="User not authenticated. Please visit /login in your browser."
        )
    return Spotify(auth=token_info["access_token"])


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
    
    try:
        if command == "play":
            sp.start_playback()
        elif command == "pause":
            sp.pause_playback()
        elif command == "play-pause":
            # Toggle play/pause state dynamically
            playback = sp.current_playback()
            if playback and playback.get("is_playing"):
                sp.pause_playback()
            else:
                sp.start_playback()
        elif command == "next":
            sp.next_track()
        elif command == "previous":
            sp.previous_track()
        elif command == "restart":
            # Restart current track (seek to 0ms)
            sp.seek_track(position_ms=0)
        elif command == "previous-or-restart":
            # Restart current track if it's been playing >= 3s, otherwise skip to previous
            playback = sp.current_playback()
            if playback and playback.get("progress_ms", 0) >= 3000:
                sp.seek_track(position_ms=0)
            else:
                sp.previous_track()
        elif command == "minimalist":
            # Toggle minimalist mode
            current_mode = "default" if current_mode == "minimalist" else "minimalist"
        elif command == "party":
            # Enable party mode and shuffle playback
            current_mode = "party"
            sp.shuffle(True)
        elif command == "favourite":
            # Save the currently playing track to Liked Songs
            track = sp.current_user_playing_track()
            if track and track.get("item"):
                sp.current_user_saved_tracks_add(tracks=[track["item"]["id"]])
            else:
                raise HTTPException(status_code=400, detail="Nothing currently playing.")
        else:
            raise HTTPException(status_code=400, detail="Invalid action command.")
            
        return {"status": "success", "executed_command": command}
    except Exception as e:
        raise HTTPException(status_code=500, detail=str(e))


@app.get("/track")
def get_current_track():
    """Returns currently playing track title and artist for your OLED display."""
    sp = get_spotify_client()
    try:
        track = sp.current_user_playing_track()
        if track and track.get("item"):
            item = track["item"]
            title = item["name"]
            artist = item["artists"][0]["name"]
            return {
                "is_playing": track["is_playing"],
                "title": title,
                "artist": artist
            }
        return {"is_playing": False, "title": "Nothing Playing", "artist": "N/A"}
    except Exception as e:
        raise HTTPException(status_code=500, detail=str(e))


@app.get("/mode")
def get_current_mode():
    """Returns the current bridge mode (e.g. 'default', 'minimalist', 'party')."""
    return {"mode": current_mode}