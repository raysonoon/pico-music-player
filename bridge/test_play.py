"""Quick standalone test: play a random Spotify playlist via PUT /me/player/play.

Uses the bridge's OAuth (.env + .cache). Verifies playback actually starts.

Usage (from bridge/):
    python test_play.py                 # play a random one of your saved playlists
"""
import os
import random
import sys
import time

from dotenv import load_dotenv
from spotipy import Spotify
from spotipy.oauth2 import SpotifyOAuth

HERE = os.path.dirname(os.path.abspath(__file__))
load_dotenv(os.path.join(HERE, ".env"))

oa = SpotifyOAuth(
    client_id=os.getenv("SPOTIFY_CLIENT_ID"),
    client_secret=os.getenv("SPOTIFY_CLIENT_SECRET"),
    redirect_uri=os.getenv("SPOTIFY_REDIRECT_URI"),
    scope="user-read-playback-state user-modify-playback-state playlist-read-private playlist-read-collaborative",
    cache_path=os.path.join(HERE, ".cache"),
)
sp = Spotify(auth_manager=oa, requests_timeout=10)


def pick_playlist():
    pls = sp.current_user_playlists(limit=50).get("items") or []
    if not pls:
        return None
    return random.choice(pls)


def main():
    pl = pick_playlist()
    if not pl:
        print("No playlist found.")
        sys.exit(1)
    print("playlist:", pl["name"], pl["uri"])

    devs = sp.devices().get("devices") or []
    print("devices:", [(d.get("id"), d.get("name"), d.get("type"),
                        d.get("is_active"), d.get("is_restricted")) for d in devs])
    active = [d for d in devs if d.get("is_active")]
    dev = active[0] if active else (devs[0] if devs else None)
    print("target:", dev.get("name") if dev else None)

    if not dev:
        print("No active device - open Spotify.")
        sys.exit(1)

    try:
        sp.start_playback(device_id=dev["id"], context_uri=pl["uri"])
        print("PUT /me/player/play (context_uri) accepted.")
    except Exception as e:
        print("start_playback FAILED:", type(e).__name__, e)
        sys.exit(1)

    for i in range(5):
        time.sleep(1.0)
        now = sp.current_playback() or {}
        item = now.get("item") or {}
        ctx = (now.get("context") or {}).get("uri")
        print("check %d: is_playing=%s name=%r uri=%s context=%s"
              % (i + 1, now.get("is_playing"), item.get("name"), item.get("uri"), ctx))


if __name__ == "__main__":
    main()