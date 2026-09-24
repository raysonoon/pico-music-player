#!/usr/bin/env python3
"""One-time setup for the Pico Music Player.

Configures the Pico's Wi-Fi + program address over USB serial, waits for the Pico to join the network, then starts
the Python program and drives the Spotify login.

Run from setup.bat
"""

import getpass
import socket
import subprocess
import sys
import time
import webbrowser
from pathlib import Path

ROOT = Path(__file__).resolve().parent
BRIDGE_DIR = ROOT / "bridge"

DEFAULT_PORT = 5000
PICO_VID = 0x2E8A          # Raspberry Pi USB vendor ID
SETUP_BAUD = 115200


def banner():
    print("=" * 70)
    print("  Pico Music Player - setup & program launcher")
    print("=" * 70)
    print()
    print("  *** IMPORTANT: the Pico W only supports 2.4 GHz Wi-Fi. ***")
    print("  It CANNOT connect to 5 GHz networks.")
    print("  If your router broadcasts both bands, pick the 2.4 GHz")
    print('  network name (it often ends in "-2.4G" or "2.4GHz").')
    print("=" * 70)
    print()


def ask(question, default=None):
    suffix = f" [{default}]" if default else ""
    raw = input(f"{question}{suffix}: ").strip()
    return raw if raw else (default or "")


def detect_lan_ip():
    """Best-effort guess of this computer's LAN IP."""
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            s.connect(("8.8.8.8", 80))
            return s.getsockname()[0]
        finally:
            s.close()
    except Exception:
        return None


def ensure_pyserial():
    try:
        import serial  # noqa: F401
        return
    except ImportError:
        print("Installing 'pyserial' ...")
        subprocess.check_call([sys.executable, "-m", "pip", "install", "pyserial"])


def wait_for_lines(s, targets, timeout_s=5.0):
    """Read serial lines until one starts with/equals a target. Returns it."""
    s.timeout = 0.5
    deadline = time.time() + timeout_s
    buf = ""
    while time.time() < deadline:
        try:
            chunk = s.read(256)
        except Exception:
            return None
        if not chunk:
            continue
        buf += chunk.decode("utf-8", "replace")
        lines = buf.split("\n")
        buf = lines.pop()
        for ln in lines:
            ln = ln.strip()
            for t in targets:
                if ln == t or ln.startswith(t):
                    return ln
    return None


def send_config(device, ssid, password, ip, port):
    """Full handshake against one port. Returns True when config is saved."""
    import serial
    try:
        s = serial.Serial(device, SETUP_BAUD, timeout=2)
    except serial.SerialException:
        return False
    try:
        s.reset_input_buffer()
        s.write(b"CFG\n")
        if wait_for_lines(s, ("READY",), 5.0) is None:
            return False
        s.write(f"{ssid}\t{password}\t{ip}\t{port}\n".encode())
        resp = wait_for_lines(s, ("OK", "ERR"), 10.0)
        return resp is not None and resp.startswith("OK")
    finally:
        s.close()


def find_and_configure(ssid, password, ip, port, timeout_s=60):
    import serial.tools.list_ports as list_ports
    print()
    print("Plug the Pico in via USB. If it already has a config saved,")
    print("hold the button while it powers on (or press reset) to enter")
    print("setup mode. The screen should show 'craving connection...'")
    print("Scanning for the Pico...")
    deadline = time.time() + timeout_s
    last_dev = None
    fail_streak = 0
    last_status = time.time()
    while time.time() < deadline:
        found_pico = False
        for p in list_ports.comports():
            if p.vid != PICO_VID and "2E8A" not in (p.hwid or "").upper():
                continue
            found_pico = True
            if p.device != last_dev:
                print(f"  {p.device} - sending Wi-Fi config...")
                last_dev = p.device
                fail_streak = 0
            if send_config(p.device, ssid, password, ip, port):
                print(f"  Config saved to {p.device}.")
                return True
            fail_streak += 1
            if fail_streak >= 3:
                # The Pico is plugged in but never answers the setup handshake.
                print()
                print(f"  Found the Pico on {p.device}, but it is not in setup mode")
                print("  (it did not answer the setup handshake).")
                print("  To reconfigure: hold the button while powering it on,")
                print("  then run setup again.")
                print("  If it is already configured and on your Wi-Fi, choose")
                print("  option [2] to just run the bridge.")
                return False
        if not found_pico and time.time() - last_status >= 5:
            remaining = int(max(deadline - time.time(), 0))
            print(f"  No Pico detected yet - keep it plugged in via USB. ({remaining}s left)")
            last_status = time.time()
        time.sleep(0.5)
    return False


def wait_for_wifi_result(timeout_s=45):
    """Re-open the Pico's port and wait for WIFI-OK / WIFI-FAIL."""
    import serial
    import serial.tools.list_ports as list_ports
    print("Pico connecting to your Wi-Fi...")
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        for p in list_ports.comports():
            if p.vid != PICO_VID and "2E8A" not in (p.hwid or "").upper():
                continue
            try:
                s = serial.Serial(p.device, SETUP_BAUD, timeout=1)
            except serial.SerialException:
                continue
            try:
                s.timeout = 0.5
                end = time.time() + 5
                buf = ""
                while time.time() < end and time.time() < deadline:
                    try:
                        chunk = s.read(256)
                    except Exception:
                        break
                    if not chunk:
                        continue
                    buf += chunk.decode("utf-8", "replace")
                    if "WIFI-OK" in buf:
                        return True
                    if "WIFI-FAIL" in buf:
                        return False
            finally:
                s.close()
        time.sleep(0.3)
    return None


def ensure_env_file():
    env_path = BRIDGE_DIR / ".env"
    if env_path.exists():
        print(f"  Using existing {env_path}")
        return
    print()
    print("No bridge/.env found. You need a Spotify Developer app to continue.")
    print("Create one at https://developer.spotify.com/dashboard")
    print("  - App type: Web API")
    print(f"  - Redirect URI: http://127.0.0.1:{DEFAULT_PORT}/callback")
    print()
    cid = ask("Spotify Client ID")
    sec = getpass.getpass("Spotify Client Secret: ")
    redirect = ask("Redirect URI", f"http://127.0.0.1:{DEFAULT_PORT}/callback")
    env_path.write_text(
        'SPOTIFY_CLIENT_ID="%s"\n'
        'SPOTIFY_CLIENT_SECRET="%s"\n'
        'SPOTIFY_REDIRECT_URI="%s"\n'
        'POLL_INTERVAL_SECONDS=1\n' % (cid, sec, redirect),
        encoding="utf-8",
    )
    print(f"  Wrote {env_path}")


def ensure_bridge_deps():
    missing = []
    for mod in ("fastapi", "uvicorn", "spotipy", "PIL", "dotenv", "requests"):
        try:
            __import__(mod)
        except ImportError:
            missing.append(mod)
    if missing:
        print(f"Installing missing program dependencies: {', '.join(missing)} ...")
        subprocess.check_call(
            [sys.executable, "-m", "pip", "install", "-r", str(BRIDGE_DIR / "requirements.txt")]
        )


def run_bridge(port):
    print()
    print("Starting the program...")
    print(f"  http://localhost:{port}/docs  (API docs)")
    cmd = [sys.executable, "-m", "uvicorn", "main:app",
           "--host", "0.0.0.0", "--port", str(port)]
    return subprocess.Popen(cmd, cwd=str(BRIDGE_DIR))


def ensure_spotify_login():
    cache = BRIDGE_DIR / ".cache"
    if cache.exists():
        print("  Spotify already logged in (.cache present).")
        return
    print()
    print("  Before you log in, please make sure:")
    print("    1. You log in with a Spotify account that has PREMIUM")
    print("       (controlling playback requires a Premium account).")
    print("    2. The developer has authorized your Spotify account email")
    print()
    print("Opening your browser to log in to Spotify...")
    webbrowser.open(f"http://localhost:{DEFAULT_PORT}/login")
    print("Finish the login in the browser, then wait here.")
    deadline = time.time() + 180
    while time.time() < deadline and not cache.exists():
        time.sleep(2)
    if cache.exists():
        print("  Authenticated! You can close the browser tab.")
    else:
        print("  Still waiting for the Spotify login - check the browser.")


def main():
    banner()

    action = ask("What do you want to do?\n  [1] Set up the Pico for this network, then run the program\n  [2] Just run the program (Pico already configured)", "1")
    port = DEFAULT_PORT

    if action.strip().startswith("2"):
        print("Skipping Wi-Fi setup (using whatever is saved on the Pico).")
    else:
        ssid = ask("2.4 GHz Wi-Fi network name (SSID)")
        password = getpass.getpass("Wi-Fi password: ")
        lan = detect_lan_ip()
        if not lan:
            print("Could not auto-detect this computer's IP address.")
            ip = ask("Type this computer's IP address on the local network")
        else:
            ip = lan
            print(f"Using this computer's IP address: {ip}")

        ensure_pyserial()
        if not find_and_configure(ssid, password, ip, port):
            print()
            print("Could not reach a Pico in setup mode.")
            again = ask("Is the Pico already configured? Run the program anyway?", "n").lower()
            if not again.startswith("y"):
                print("Exiting. Hold the button while powering the Pico on to")
                print("force setup mode, then run setup again.")
                return
        else:
            result = wait_for_wifi_result()
            if result is False:
                print()
                print("The Pico could not join the Wi-Fi. Check the SSID and")
                print("password, then run setup again.")
                return
            if result is None:
                print("Couldn't confirm the Pico joined Wi-Fi (timed out).")
                print("It may still connect - continuing anyway.")
            else:
                print("Pico is connected to Wi-Fi!")
        print()
        print("Note: allow Python through the Windows firewall if prompted so")
        print("the Pico can reach the program.")

    ensure_env_file()
    ensure_bridge_deps()
    proc = run_bridge(port)
    ensure_spotify_login()
    try:
        proc.wait()
    except KeyboardInterrupt:
        proc.terminate()
        proc.wait()
    print("Program stopped.")


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nAborted.")