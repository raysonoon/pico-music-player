from fastapi import FastAPI, HTTPException
from spotipy import Spotify
from spotipy.oauth2 import SpotifyOAuth
import os
import random
import io
import threading
from contextlib import asynccontextmanager
from PIL import Image, ImageDraw, ImageFont, ImageFilter
import requests
from dotenv import load_dotenv

load_dotenv()

POLL_INTERVAL_SECONDS = float(os.getenv("POLL_INTERVAL_SECONDS", "1"))

# In-memory mode state (set by gestures)
current_mode = "default"
_party_prev = None  # resume snapshot (context/track/shuffle) captured before party mode
_mini_prev = None

# Party mode: album art + tempo sync
ART_SIZE = 40
DEFAULT_BPM = 120.0
RECCOBEATS_BASE = "https://api.reccobeats.com"
_tempo_cache = {}  # track_id -> tempo (BPM float)
_art_cache = {}    # image_url -> hex art_bmp string

# Find-a-song: per-band seed tracks (Spotify track IDs) and feature steering.
# Seed the recommendation on the matching band's genre + the currently playing
# track; a higher featureWeight lets the tap-derived tempo/features dominate.
BAND_SEEDS = {
    # 0-80 BPM: ambient, lofi, acoustic, jazz
    "ambient": [
        "1vgSaC0BPlL6LEm4Xsx59J",  # Brian Eno - An Ending (Ascent)
        "7o2AeQZzfCERsRmOM86EcB",  # Aphex Twin - Xtal
    ],
    # 80-115 BPM: indie rock, r&b, city pop, slow pop
    "indie": [
        "3lq6i23fC6j1v1AR0GeNg8",  # Mariya Takeuchi - Plastic Love
        "3rq5w4bQGigXOfdN30ATJt",  # Arctic Monkeys - Do I Wanna Know?
    ],
    # 115-140 BPM: midwest emo, funk, alt rock, upbeat pop
    "altrock": [
        "6kZqCqD1r08sJAQ1TjuEpM",  # American Football - Never Meant
        "69kOkLUCkxIZYexIgSG8rq",  # Daft Punk - Get Lucky
    ],
    # >140 BPM: edm, j-rock, punk, metal
    "metal": [
        "2DlHlPMa4M17kufBvI2lEN",  # System Of A Down - Chop Suey!
        "5cW3DEWYYv0tXZMwcUZNRg",  # Metallica - Master Of Puppets
    ],
}
FEATURE_WEIGHT = 2.0   # undocumented scale; tunable, verified against live API
SPEECHINESS_CAP = 0.35  # reject candidates with more spoken word than this

# Party mode: curated upbeat playlists, picked at random when party mode starts.
PARTY_PLAYLISTS = {
    "Today's Top Hits": "37i9dQZF1DXcBWIGoYBM5M",
    "Just Hits": "37i9dQZF1DXcRXFNfZr7Tp",
    "Party Hits 2010s": "37i9dQZF1DWWylYLMvjuRG",
}

# BPM bands -> label + audio-feature targets (seed = band genre + current track).
BANDS = [
    (80,   "Ambient", dict(energy=0.25, acousticness=0.75, danceability=0.30, instrumentalness=0.55, valence=0.40)),
    (115,  "Indie",   dict(energy=0.50, acousticness=0.40, danceability=0.60, instrumentalness=0.10, valence=0.60)),
    (140,  "Alt Rock", dict(energy=0.75, acousticness=0.15, danceability=0.65, instrumentalness=0.05, valence=0.60)),
    (1e9,  "Metal",   dict(energy=0.90, acousticness=0.05, danceability=0.55, instrumentalness=0.15, valence=0.50)),
]

# OLED text layout (must match the firmware constants)
FRAME_WIDTH = 128
OLED_H_SHIFT = 8
TITLE_H = 13
ARTIST_H = 13
ALBUM_H = 12
TITLE_X_OFFSET = 13  # leaves room for the play/pause glyph

# Minimalist mode layout (must match the firmware constants)
MINI_TITLE_H = 21
MINI_ARTIST_H = 15
MINI_ALBUM_H = 15
MINI_GLYPH_COL = 15  # leaves room for the status glyph

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
    "mode": "default",
    "remaining_ms": 0,
    "tempo": 0.0,
    "art_bmp": "",
}
_snapshot_lock = threading.Lock()

# Spotify API Credentials
CLIENT_ID = os.getenv("SPOTIFY_CLIENT_ID")
CLIENT_SECRET = os.getenv("SPOTIFY_CLIENT_SECRET")
REDIRECT_URI = os.getenv("SPOTIFY_REDIRECT_URI")

# Permissions needed to control playback, read song info and save tracks
SCOPE = "user-modify-playback-state user-read-playback-state user-library-modify playlist-read-private"

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
_font14 = _load_font(14)
_font10 = _load_font(10)


def _render_line(text, height, font, x_offset=0):
    """Render a single text line to a 1-bit bitmap, hex-encoded for the Pico."""
    img = Image.new("1", (FRAME_WIDTH, height), 0)
    d = ImageDraw.Draw(img)
    t = text or ""
    avail = FRAME_WIDTH - OLED_H_SHIFT - x_offset
    if d.textlength(t, font=font) > avail:
        ell = "..."
        while t and d.textlength(t + ell, font=font) > avail:
            t = t[:-1]
        t = t + ell
    d.text((x_offset, 0), t, font=font, fill=1, anchor="la")
    return img.tobytes().hex()


def _render_bitmaps(snap):
    """Attach the three rendered text lines (hex bitmaps) to a snapshot."""
    try:
        mode = snap.get("mode", "default")
        if mode == "minimalist":
            snap["title_bmp"] = _render_line(snap["title"], MINI_TITLE_H, _font14, MINI_GLYPH_COL)
            snap["artist_bmp"] = _render_line(snap["artist"], MINI_ARTIST_H, _font10, 0)
            snap["album_bmp"] = _render_line(snap["album"], MINI_ALBUM_H, _font10, 0)
        else:
            snap["title_bmp"] = _render_line(snap["title"], TITLE_H, _font9, TITLE_X_OFFSET)
            snap["artist_bmp"] = _render_line(snap["artist"], ARTIST_H, _font9, 0)
            snap["album_bmp"] = _render_line(snap["album"], ALBUM_H, _font9, 0)
    except Exception:
        snap["title_bmp"] = "00" * (FRAME_WIDTH * TITLE_H // 8)
        snap["artist_bmp"] = "00" * (FRAME_WIDTH * ARTIST_H // 8)
        snap["album_bmp"] = "00" * (FRAME_WIDTH * ALBUM_H // 8)


def _poll_playback(stop_event):
    """Background thread: poll Spotify playback state and cache the latest snapshot."""
    sp = Spotify(auth_manager=sp_oauth, requests_timeout=10)
    while not stop_event.is_set():
        try:
            playback = sp.current_playback()
            track_id = None
            art_url = ""
            if playback and playback.get("item"):
                item = playback["item"]
                track_id = item.get("id")
                images = ((item.get("album") or {}).get("images")) or []
                art_url = images[0]["url"] if images else ""
                artists = item.get("artists") or []
                artist = ", ".join(a["name"] for a in artists if a.get("name"))
                duration_ms = item.get("duration_ms", 0) or 0
                progress_ms = playback.get("progress_ms", 0) or 0
                progress_percent = round(progress_ms * 100 / duration_ms) if duration_ms else 0
                remaining_ms = max(duration_ms - progress_ms, 0) if duration_ms else 0
                snap = {
                    "is_playing": bool(playback.get("is_playing")),
                    "title": item.get("name", "Unknown"),
                    "artist": artist or "N/A",
                    "album": (item.get("album") or {}).get("name", ""),
                    "progress_ms": progress_ms,
                    "duration_ms": duration_ms,
                    "progress_percent": progress_percent,
                    "remaining_ms": remaining_ms,
                    "mode": current_mode,
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
                    "remaining_ms": 0,
                    "mode": current_mode,
                }
            snap["tempo"] = _get_tempo(track_id) if track_id else 0.0
            snap["art_url"] = art_url
            snap["art_bmp"] = _render_album_art(art_url) if art_url else ""
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
    return Spotify(auth=token_info["access_token"], requests_timeout=10)


def _safe_shuffle(sp, state):
    """Best-effort shuffle toggle; some playlists/contexts disallow shuffle."""
    try:
        sp.shuffle(state)
    except Exception:
        pass


def _capture_resume(sp):
    """Snapshot the current playback for later resume: context + track + shuffle."""
    try:
        playback = sp.current_playback()
    except Exception:
        return None
    if not playback or not playback.get("item"):
        return None
    ctx = playback.get("context") or {}
    ctx_uri = ctx.get("uri") if ctx.get("type") in ("playlist", "album", "artist") else None
    item = playback["item"]
    uri = item.get("uri") or (f"spotify:track:{item['id']}" if item.get("id") else None)
    return {
        "context_uri": ctx_uri,
        "track_uri": uri,
        "shuffle": bool(playback.get("shuffle_state")),
    }


def _restore_resume(sp, snap):
    """Restore the previous context at the same track (from 0ms) + its shuffle."""
    if not snap:
        return
    if snap.get("context_uri"):
        try:
            sp.start_playback(context_uri=snap["context_uri"], offset={"uri": snap["track_uri"]})
        except Exception:
            pass
    _safe_shuffle(sp, snap.get("shuffle", False))


def _shuffle_only(snap):
    """Keep only the shuffle state, dropping any context we didn't actually leave."""
    return {"context_uri": None, "track_uri": None,
            "shuffle": (snap or {}).get("shuffle", False)}


# Keywords used to find a focus playlist for minimalist mode.
MINI_KEYWORDS = ("study", "work", "focus", "lock in", "grind")


def _find_focus_playlist(sp):
    """Return a random (name, id) for a library playlist whose name contains a
    MINI_KEYWORD, else None."""
    try:
        matches = []
        offset = 0
        while offset < 200:  # sanity cap
            page = sp.current_user_playlists(limit=50, offset=offset)
            items = page.get("items") or []
            for pl in items:
                name = (pl.get("name") or "").lower()
                if any(k in name for k in MINI_KEYWORDS):
                    matches.append((pl.get("name"), pl.get("id")))
            if not items or not page.get("next"):
                break
            offset += len(items)
        return random.choice(matches) if matches else None
    except Exception:
        return None


def _get_tempo(track_id):
    """Fetch tempo (BPM) for a Spotify track via ReccoBeats. Returns 0.0 on failure."""
    if track_id in _tempo_cache:
        return _tempo_cache[track_id]
    tempo = 0.0
    try:
        r = requests.get(f"{RECCOBEATS_BASE}/v1/track", params={"ids": track_id}, timeout=5)
        r.raise_for_status()
        data = r.json()
        tracks = data.get("content") if isinstance(data, dict) else data
        if isinstance(tracks, list) and tracks:
            rec_id = tracks[0].get("id")
            if rec_id:
                af = requests.get(f"{RECCOBEATS_BASE}/v1/track/{rec_id}/audio-features", timeout=5)
                af.raise_for_status()
                tempo = float((af.json().get("tempo") or 0.0))
    except Exception:
        tempo = 0.0
    _tempo_cache[track_id] = tempo
    return tempo


def _band_for_bpm(bpm):
    """Return (label, feature_targets) for a tap tempo."""
    for limit, label, features in BANDS:
        if bpm < limit:
            return label, features
    return BANDS[-1][1], BANDS[-1][2]


def _spotify_id_from_href(href):
    """Extract a Spotify track id from a reccobeats href, else None.

    Handles both 'https://open.spotify.com/track/{id}' and
    'https://api.spotify.com/v1/tracks/{id}' URL forms.
    """
    if not href:
        return None
    parts = [p for p in href.rstrip("/").split("/") if p]
    for seg in ("track", "tracks"):
        if seg in parts:
            i = parts.index(seg)
            if i + 1 < len(parts):
                return parts[i + 1]
    return parts[-1] if parts else None


def _find_song(sp, bpm):
    """Recommend and play a track matching the tapped tempo.

    Seeds the reccobeats recommendation on the band's genre tracks plus the
    currently playing track, steers with audio-feature targets, hard-filters
    out spoken-word (speechiness) candidates, then plays the first match.
    Returns the band label on success.
    """
    # Require active playback: the recommendation is queued after the current
    # track and skipped to, so something must be playing.
    playback = sp.current_playback()
    item = (playback or {}).get("item") or {}
    if not item:
        raise HTTPException(status_code=400, detail="Nothing playing - open Spotify")
    current_id = item.get("id") if item.get("type") == "track" else None

    label, features = _band_for_bpm(bpm)
    print(f"[find-song] bpm={bpm} band={label}", flush=True)
    seeds = list(BAND_SEEDS.get(label.lower().replace(" ", ""), []))
    if current_id:
        seeds.append(current_id)
    print(f"[find-song] seeds={seeds}", flush=True)
    if not seeds:
        raise HTTPException(status_code=400, detail="No seed track")

    params = {
        "seeds": ",".join(seeds),
        "size": 20,
        "tempo": bpm,
        "speechiness": 0.30,
        "featureWeight": FEATURE_WEIGHT,
    }
    params.update(features)
    r = requests.get(f"{RECCOBEATS_BASE}/v1/track/recommendation", params=params, timeout=4)
    r.raise_for_status()
    tracks = (r.json().get("content") or []) if isinstance(r.json(), dict) else []
    print(f"[find-song] recommendation returned {len(tracks)} tracks", flush=True)
    if not tracks:
        raise HTTPException(status_code=400, detail="No match")

    # Hard-filter speechiness in one batch call using reccobeats UUIDs.
    ids = [t.get("id") for t in tracks if t.get("id")]
    speechy = {}
    try:
        af = requests.get(f"{RECCOBEATS_BASE}/v1/audio-features",
                          params={"ids": ",".join(ids)}, timeout=4)
        af.raise_for_status()
        for feat in (af.json().get("content") or []):
            if feat.get("id") in ids:
                speechy[feat["id"]] = float(feat.get("speechiness") or 0.0)
    except Exception:
        pass  # if the batch lookup fails, skip the hard filter

    queued = None
    for t in tracks:
        tid = t.get("id")
        if tid in speechy and speechy[tid] > SPEECHINESS_CAP:
            print(f"[find-song] skip (speechy) id={tid} title={t.get('trackTitle')}", flush=True)
            continue
        sid = _spotify_id_from_href(t.get("href"))
        if not sid:
            print(f"[find-song] no id in href id={tid} title={t.get('trackTitle')}", flush=True)
            continue
        uri = f"spotify:track:{sid}"
        print(f"[find-song] queue id={tid} title={t.get('trackTitle')} uri={uri} "
              f"speechiness={speechy.get(tid)}", flush=True)
        try:
            sp.add_to_queue(uri)
            queued = uri
            break
        except Exception as e:
            print(f"[find-song] add_to_queue failed {uri}: {e}", flush=True)
            continue
    if not queued:
        raise HTTPException(status_code=400, detail="Could not queue any recommendation")

    try:
        sp.next_track()
        print(f"[find-song] skipped to queued track", flush=True)
    except Exception as e:
        print(f"[find-song] next_track failed: {e}", flush=True)
    return label


def _render_album_art(url):
    """Download album art and render to a 40x40 1-bit bitmap, hex-encoded."""
    if url in _art_cache:
        return _art_cache[url]
    art = ""
    try:
        resp = requests.get(url, timeout=5)
        resp.raise_for_status()
        img = Image.open(io.BytesIO(resp.content)).convert("L")
        img = img.resize((ART_SIZE, ART_SIZE), Image.Resampling.LANCZOS)
        # Soften before binarizing to reduce harsh dither speckle.
        img = img.filter(ImageFilter.GaussianBlur(radius=1))
        img = img.convert("1", dither=Image.Dither.NONE)
        art = img.tobytes().hex()
    except Exception:
        art = ""
    _art_cache[url] = art
    return art


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


# Personality strings for playback feedback, keyed by the active mode.
# Minimalist stays plain on purpose; default and party get the fun wording.
_PLAY_TEXT = {
    "default": "bringin' vibes",
    "party": "jumpin' back in!",
    "minimalist": "Play",
}
_PAUSE_TEXT = {
    "default": "waiting for u...",
    "party": "who killed it?",
    "minimalist": "Pause",
}
_NEXT_TEXT = {
    "default": "changin' it up!",
    "party": "rock n rollin!",
    "minimalist": "Next",
}
_PREV_TEXT = {
    "default": "windin' back",
    "party": "got ur back!",
    "minimalist": "Previous",
}
_RESTART_TEXT = {
    "default": "from the top",
    "party": "roll it back!",
    "minimalist": "Restart",
}


def _mode_text(table, plain):
    return table.get(current_mode, plain)


@app.post("/action/{command}")
def handle_action(command: str, bpm: int = 0):
    """
    Endpoint called by your Pico W or test tools.
    Commands supported: play, pause, next, previous, restart,
                        previous-or-restart, minimalist, party, favourite,
                        find-song (expects bpm query param)
    """
    global current_mode
    global _party_prev
    global _mini_prev
    sp = get_spotify_client()
    result = ""

    try:
        if command == "find-song":
            if not bpm:
                raise HTTPException(status_code=400, detail="bpm required")
            result = _find_song(sp, bpm)
        elif command == "play":
            sp.start_playback()
            result = _mode_text(_PLAY_TEXT, "Play")
        elif command == "pause":
            sp.pause_playback()
            result = _mode_text(_PAUSE_TEXT, "Pause")
        elif command == "play-pause":
            # Toggle play/pause state dynamically
            playback = sp.current_playback()
            if playback and playback.get("is_playing"):
                sp.pause_playback()
                result = _mode_text(_PAUSE_TEXT, "Pause")
            else:
                sp.start_playback()
                result = _mode_text(_PLAY_TEXT, "Play")
        elif command == "next":
            sp.next_track()
            result = _mode_text(_NEXT_TEXT, "Next")
        elif command == "previous":
            sp.previous_track()
            result = _mode_text(_PREV_TEXT, "Previous")
        elif command == "restart":
            # Restart current track (seek to 0ms)
            sp.seek_track(position_ms=0)
            result = _mode_text(_RESTART_TEXT, "Restart")
        elif command == "previous-or-restart":
            # Restart current track if it's been playing >= 4s, otherwise skip to previous
            playback = sp.current_playback()
            if playback and playback.get("progress_ms", 0) >= 10000:
                sp.seek_track(position_ms=0)
                result = _mode_text(_RESTART_TEXT, "Restart")
            else:
                sp.previous_track()
                result = _mode_text(_PREV_TEXT, "Previous")
        elif command == "minimalist":
            # Toggle minimalist mode (single mode at a time). On enter, play a
            # random study/work/focus playlist with shuffle off; fall back to the
            # current playlist if none found or starting one fails. On exit,
            # revert to the pre-minimalist context. Entering from party folds in
            # leaving party (its pre-context becomes the resume point).
            if current_mode == "minimalist":
                current_mode = "default"
                _restore_resume(sp, _mini_prev)
                _mini_prev = None
                result = "takin' it easy!"
            else:
                if current_mode == "party":
                    prev = _party_prev
                    _party_prev = None
                else:
                    prev = _capture_resume(sp)
                current_mode = "minimalist"
                match = _find_focus_playlist(sp)
                if match:
                    name, pid = match
                    try:
                        sp.start_playback(context_uri=f"spotify:playlist:{pid}")
                        _safe_shuffle(sp, True)
                        _mini_prev = prev
                        result = f"time to lock in|{name}"
                    except Exception:
                        _safe_shuffle(sp, True)
                        _mini_prev = _shuffle_only(prev)
                        result = "time to lock in"
                else:
                    _safe_shuffle(sp, True)
                    _mini_prev = _shuffle_only(prev)
                    result = "time to lock in"
        elif command == "party":
            # Toggle party mode (single mode at a time). On enter, play and
            # shuffle a random upbeat playlist; fall back to shuffling the
            # current playlist if starting a curated one fails. On exit, revert
            # to the pre-party context (same playlist/track, from the start).
            if current_mode == "party":
                current_mode = "default"
                _restore_resume(sp, _party_prev)
                _party_prev = None
                result = "that was fire!"
            else:
                prev = _capture_resume(sp)
                current_mode = "party"
                name, pid = random.choice(list(PARTY_PLAYLISTS.items()))
                try:
                    sp.start_playback(context_uri=f"spotify:playlist:{pid}")
                    _safe_shuffle(sp, True)
                    _party_prev = prev
                    result = f"let's partyyy!|{name}"
                except Exception:
                    _safe_shuffle(sp, True)
                    _party_prev = _shuffle_only(prev)
                    result = "let's partyyy!"
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


@app.get("/art")
def get_art():
    """Returns the album art bitmap (hex) for the current track."""
    with _snapshot_lock:
        return {"art_bmp": _snapshot.get("art_bmp", "")}


@app.get("/mode")
def get_current_mode():
    """Returns the current bridge mode (e.g. 'default', 'minimalist', 'party')."""
    return {"mode": current_mode}


@app.get("/debug/devices")
def debug_devices():
    """Returns the current Spotify devices for debugging."""
    sp = get_spotify_client()
    return {"devices": sp.devices().get("devices") or []}