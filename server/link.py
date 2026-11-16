#!/usr/bin/env python3
"""NULD Link, the PC side: sends the screen to the PS2 and turns its pad into an Xbox 360 controller.

  python server/link.py                    # the desktop, 1280x720 at 30 pictures a second, 12 Mbit/s, on the GPU
  python server/link.py --grab gdigrab --encoder mpeg2video   # without an Intel GPU
  python server/link.py --title PCSX2      # one window (gdigrab title=)
  python server/link.py --region 0,0,1920x1080 --rate 16M

Video: when the PS2 (link.elf) asks on UDP 8099, ffmpeg starts capturing the screen (gdigrab, Windows) and encodes
MPEG-2 with I and P pictures; nuld-av's psvpack repacks each picture for the PS2's IPU as it leaves the encoder and
its fastsrv sends it by the network's fast path (nuld-av/tools/livesrv.py has the details). Capture starts with the
request on purpose: started earlier, gdigrab hands over every frame it missed in a burst. When the PS2 goes, it stops.
Audio: what Windows plays (WASAPI loopback of the default speakers, `pip install soundcard numpy`), MP2, in the same
stream. Without soundcard there is no audio.
Pad: UDP port 8098, 16 bytes per packet from the PS2 (link.c pad_pkt): "OLK1", sequence, buttons, lx, ly, rx, ry.
The pad needs the ViGEmBus driver (github.com/nefarius/ViGEmBus/releases) and `pip install vgamepad`; without
vgamepad the packets are counted and not applied.
Catalog (the PS2's menu), HTTP on TCP 8099: GET /catalog = one "SOURCE\tTitle" line per installed game (Epic, Steam),
read from the launchers' own files at each request; GET /cover/<n> = its cover as 256x368 CT16 (RGB555, bit 15 set,
little-endian, what gfx_image draws), 404 without one; GET /launch/<n> opens it through the launcher's URI.
Needs ffmpeg in PATH and nuld-av (github.com/danielnuld/nuld-av) beside this repository, or --nuldav <its folder>,
with its tools/psvpack.exe built (tools/build_psvpack.bat there).
"""
import argparse
import base64
import io
import json
import os
import re
import socket
import struct
import subprocess
import sys
import threading
import time
import urllib.parse
import urllib.request
from pathlib import Path

PAD = struct.Struct("<4sIHBBBBxx") # link.c pad_pkt: 16 bytes, little-endian
assert PAD.size == 16
# libpad's PAD_* bits, pressed = 1
SELECT, L3, R3, START, UP, RIGHT, DOWN, LEFT, L2, R2, L1, R1, TRIANGLE, CIRCLE, CROSS, SQUARE = (1 << i for i in range(16))
XBOX = [(CROSS, "XUSB_GAMEPAD_A"), (CIRCLE, "XUSB_GAMEPAD_B"), (SQUARE, "XUSB_GAMEPAD_X"), (TRIANGLE, "XUSB_GAMEPAD_Y"),
        (L1, "XUSB_GAMEPAD_LEFT_SHOULDER"), (R1, "XUSB_GAMEPAD_RIGHT_SHOULDER"), (SELECT, "XUSB_GAMEPAD_BACK"),
        (START, "XUSB_GAMEPAD_START"), (L3, "XUSB_GAMEPAD_LEFT_THUMB"), (R3, "XUSB_GAMEPAD_RIGHT_THUMB"),
        (UP, "XUSB_GAMEPAD_DPAD_UP"), (DOWN, "XUSB_GAMEPAD_DPAD_DOWN"), (LEFT, "XUSB_GAMEPAD_DPAD_LEFT"),
        (RIGHT, "XUSB_GAMEPAD_DPAD_RIGHT")]


def axis(v, invert=False):
    """A PS2 stick byte (0..255, 128 centre, 0 = up) to an Xbox axis (-32768..32767, positive = up)."""
    x = (v - 128) * 258
    if invert:
        x = -x
    return max(-32768, min(32767, x))


EPIC = Path(os.environ.get("PROGRAMDATA", r"C:\ProgramData"), "Epic/EpicGamesLauncher/Data")


def epic_games():
    """Installed Epic games from the launcher's manifests; names, categories and covers from its catalog cache
    (base64 JSON, keyImages). Two install lists: Data/Manifests/*.item (older launchers) and EOS's InstallHelper
    InstalledItems/*.egi (the 2026 launcher writes only these)."""
    tall, cat = {}, {}
    try:
        for it in json.loads(base64.b64decode((EPIC / "Catalog/catcache.bin").read_bytes())):
            cat[it["id"]] = it
            urls = [k["url"] for k in it.get("keyImages", []) if k.get("type") in ("DieselGameBoxTall", "OfferImageTall")]
            if urls:
                tall[it["id"]] = urls[0]
    except (OSError, ValueError):
        pass
    out, seen = [], set()
    for f in sorted((EPIC / "../../EpicOnlineServicesShared/InstallHelper/InstalledItems").glob("*.egi")):
        try:
            v = json.loads(f.read_text(encoding="utf-8"))["v4"]
        except (OSError, ValueError, KeyError):
            continue
        it = cat.get(v.get("itemId"), {})
        if v.get("state") != "Installed" or "games" not in [c.get("path") for c in it.get("categories", [])]:
            continue # downloading, or not in the catalog cache as a game
        uri = f"com.epicgames.launcher://apps/{v['sandboxId']}%3A{v['itemId']}%3A{v['artifactId']}?action=launch&silent=true"
        out.append(("EPIC", it["title"], uri, tall.get(v["itemId"])))
        seen.add(v["itemId"])
    for f in sorted((EPIC / "Manifests").glob("*.item")):
        try:
            j = json.loads(f.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            continue
        if j.get("CatalogItemId") in seen or j.get("bIsIncompleteInstall") or "games" not in j.get("AppCategories", []) or j.get("MainGameAppName", j["AppName"]) != j["AppName"]:
            continue # still downloading, a tool (Unreal), or DLC
        uri = f"com.epicgames.launcher://apps/{j['CatalogNamespace']}%3A{j['CatalogItemId']}%3A{j['AppName']}?action=launch&silent=true"
        out.append(("EPIC", j["DisplayName"], uri, tall.get(j["CatalogItemId"])))
    return out


def steam_games():
    """Installed Steam games: libraryfolders.vdf -> appmanifest_*.acf; covers from librarycache, else Steam's CDN."""
    try:
        import winreg
        root = Path(winreg.QueryValueEx(winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam"), "SteamPath")[0])
        vdf = (root / "steamapps/libraryfolders.vdf").read_text(encoding="utf-8", errors="replace")
    except (OSError, ImportError):
        return []
    out, cache = [], root / "appcache/librarycache"
    for lib in re.findall(r'"path"\s+"([^"]+)"', vdf):
        for m in sorted(Path(lib.replace("\\\\", "\\"), "steamapps").glob("appmanifest_*.acf")):
            acf = m.read_text(encoding="utf-8", errors="replace")
            appid, name = re.search(r'"appid"\s+"(\d+)"', acf), re.search(r'"name"\s+"([^"]+)"', acf)
            if not appid or not name or appid[1] == "228980": # Steamworks Common Redistributables
                continue
            local = [p for p in (cache / f"{appid[1]}_library_600x900.jpg", cache / appid[1] / "library_600x900.jpg") if p.exists()]
            out.append(("STEAM", name[1], f"steam://rungameid/{appid[1]}",
                        local[0] if local else f"https://cdn.cloudflare.steamstatic.com/steam/apps/{appid[1]}/library_600x900.jpg"))
    return out


def gfn_games():
    """The user's GeForce NOW library, from the app's own cache of it (its "Library" panel as the service answered
    last: the app refreshes it when it opens). One entry per game with a shop chosen for it; launched through the
    app with the game's id, as the shortcuts the app makes do. Its stream is one more window to capture. Covers:
    Steam's tall one by the game's name, else the app's wide banner cropped."""
    home = Path(os.environ.get("LOCALAPPDATA", "")) / "NVIDIA Corporation/GeForceNOW"
    out, seen = [], set()

    def walk(o):
        if isinstance(o, list):
            for x in o:
                walk(x)
        elif isinstance(o, dict):
            mine = [v for v in o.get("variants") or [] if (v.get("gfn") or {}).get("library", {}).get("selected")]
            if mine and o.get("title") and o.get("id") not in seen:
                seen.add(o["id"])
                title = re.sub(r"[®™]", "", o["title"]).strip()
                route = f"#?cmsId={mine[0]['id']}&launchSource=External&shortName={title}&parentGameId={o['id']}"
                out.append(("GFN", title, [str(home / "CEF/GeForceNOW.exe"), "--url-route=" + route],
                            steam_cover(title) or (o.get("images") or {}).get("HERO_IMAGE")))
            for x in o.values():
                walk(x)
    for f in sorted((home / "CefCache/Default/Service Worker/CacheStorage").glob("*/*/*_0"), key=os.path.getmtime):
        try:
            b = f.read_bytes()
            if b"requestType=panels/Library" in b[:600]:
                walk(json.JSONDecoder().raw_decode(b[b.index(b'{"data"'):].decode("utf-8", "replace"))[0])
        except (OSError, ValueError):
            pass
    return out


found = {}  # steam_cover: by name, for this run


def steam_cover(name):
    """A game's tall cover from Steam's shop by its name (the first match of its search), or None."""
    if name not in found:
        try:
            q = urllib.request.urlopen("https://store.steampowered.com/api/storesearch/?cc=us&l=en&term=" +
                                       urllib.parse.quote(name), timeout=8).read()
            items = json.loads(q).get("items", [])
            found[name] = f"https://cdn.cloudflare.steamstatic.com/steam/apps/{items[0]['id']}/library_600x900.jpg" if items else None
        except (OSError, ValueError, KeyError):
            return None  # asked again at the next catalog
    return found[name]


COVER_W, COVER_H = 256, 368 # cover.h COVER_W x COVER_H, as the launcher's big cover


def c16(src):
    """A cover (URL or file) -> 256x368 CT16 bytes: cropped to fill, Lanczos, 4x4 ordered dither to RGB555."""
    import numpy as np
    from PIL import Image, ImageOps
    data = urllib.request.urlopen(src, timeout=15).read() if str(src).startswith("http") else Path(src).read_bytes()
    im = ImageOps.fit(Image.open(io.BytesIO(data)).convert("RGB"), (COVER_W, COVER_H), Image.LANCZOS)
    bayer = np.array([[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]) / 16 - 0.5
    v = np.clip(np.round(np.asarray(im, float) / 8 + np.tile(bayer, (COVER_H // 4, COVER_W // 4))[..., None]), 0, 31).astype(np.uint16)
    return (0x8000 | v[..., 0] | v[..., 1] << 5 | v[..., 2] << 10).astype("<u2").tobytes()


games, covers = [], {} # the last /catalog; covers by source URL / path, converted once per run


def windows():
    """The visible top-level windows with a title, at least 640 wide: {handle: process id}."""
    import ctypes
    from ctypes import wintypes
    u, out = ctypes.windll.user32, {}

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def each(h, _):
        r, pid = wintypes.RECT(), wintypes.DWORD()
        if u.IsWindowVisible(h) and u.GetWindowTextLengthW(h) and u.GetWindowRect(h, ctypes.byref(r)) and r.right - r.left >= 640:
            u.GetWindowThreadProcessId(h, ctypes.byref(pid))
            out[h] = pid.value
        return True
    u.EnumWindows(each, 0)
    return out


def exe(pid):
    """A process's program name, lower case; "" if it can't be asked."""
    import ctypes
    k = ctypes.windll.kernel32
    k.OpenProcess.restype = ctypes.c_void_p
    k.QueryFullProcessImageNameW.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_uint)]
    k.CloseHandle.argtypes = [ctypes.c_void_p]
    h, b, n = k.OpenProcess(0x1000, False, pid), ctypes.create_unicode_buffer(520), ctypes.c_uint(520)
    if not h:
        return ""
    k.QueryFullProcessImageNameW(h, 0, b, ctypes.byref(n))
    k.CloseHandle(h)
    return os.path.basename(b.value).lower()


def to_front(before, seconds=120):
    """A game launched from here opens behind whatever has the keyboard: Windows gives the foreground only to what
    the user started. For two minutes, each new window (the splash, then the game) is brought to the front; not the
    launchers' own. A tap of Alt first: it is what lets a background process call SetForegroundWindow."""
    import ctypes
    u = ctypes.windll.user32
    end = time.time() + seconds
    while time.time() < end:
        time.sleep(0.5)
        for h, pid in windows().items():
            if h not in before and not exe(pid).startswith(("epicgameslauncher", "steam", "explorer")):
                before[h] = pid
                u.keybd_event(0x12, 0, 0, 0), u.keybd_event(0x12, 0, 2, 0)
                if u.IsIconic(h):
                    u.ShowWindow(h, 9)
                u.SetForegroundWindow(h)
                print(f"link: {exe(pid)} to the front", flush=True)


def small(c, path):
    """The menu's requests: /catalog, /cover/<n>, /launch/<n>. One answer, then the connection closes."""
    global games
    body, code = b"", 200
    try:
        if path == "/catalog":
            games = sorted(epic_games() + steam_games() + gfn_games(), key=lambda g: g[1].lower())
            body = "".join(f"{src}\t{name}\n" for src, name, _, _ in games).encode("utf-8")
            print(f"link: catalog, {len(games)} games: {', '.join(g[1] for g in games)}", flush=True)
        elif m := re.fullmatch(r"/cover/(\d+)", path):
            src = games[int(m[1])][3]
            if src and src not in covers:
                covers[src] = c16(src)
            body = covers[src] if src else b""
            code = 200 if body else 404
        elif m := re.fullmatch(r"/launch/(\d+)", path):
            print(f"link: launching {games[int(m[1])][1]}", flush=True)
            before = windows()
            if games[int(m[1])][0] == "GFN":  # its window may be open already, behind: that one too
                before = {h: p for h, p in before.items() if not exe(p).startswith("geforcenow")}
            how = games[int(m[1])][2]
            subprocess.Popen(how) if isinstance(how, list) else os.startfile(how)
            threading.Thread(target=to_front, args=(before,), daemon=True).start()
        else:
            code = 404
    except Exception as e: # an index past the catalog, a cover that would not download or decode...
        print(f"link: {path}: {type(e).__name__}: {e}", file=sys.stderr, flush=True)
        body, code = b"", 404
    head = f"HTTP/1.1 {code} {'OK' if code == 200 else 'Not Found'}\r\nContent-Length: {len(body)}\r\nConnection: close\r\n\r\n"
    c.sendall(head.encode() + body)


def serve(a):
    """The menu's requests, one connection each."""
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    if sys.platform != "win32": # on Windows SO_REUSEADDR lets two listeners share the port and steal connections
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", a.port))
    srv.listen(4)
    print(f"link: waiting for the PS2 on {a.port} (menu: TCP, video: UDP) and udp {a.pad_port} (pad)", flush=True)
    while True:
        c, peer = srv.accept()
        c.settimeout(5)
        req = b""
        try:
            while b"\r\n\r\n" not in req:
                d = c.recv(1024)
                if not d:
                    break
                req += d
            if req.startswith(b"GET "):
                small(c, req.split()[1].decode("latin-1"))
        except OSError:
            pass
        c.close() # the PS2 reads until this (HTTP/1.0)


def pad_server(a, stats):
    try:
        import vgamepad as vg
        pad, B = vg.VX360Gamepad(), vg.XUSB_BUTTON
    except Exception as e: # not installed, or no ViGEmBus driver
        print(f"link: pad not applied ({e}); pip install vgamepad and the ViGEmBus driver", file=sys.stderr)
        pad = B = None
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("0.0.0.0", a.pad_port))
    s.settimeout(1.0)
    last_seq, held = None, False
    while True:
        try:
            data, _ = s.recvfrom(64)
        except socket.timeout:
            if pad and held: # the PS2 went quiet: release everything
                pad.reset(), pad.update()
                held = False
            continue
        if len(data) != PAD.size or data[:4] != b"OLK1":
            stats["bad"] += 1
            continue
        _, seq, btn, lx, ly, rx, ry = PAD.unpack(data)
        stats["pkts"] += 1
        if last_seq is not None and seq != (last_seq + 1) & 0xFFFFFFFF:
            stats["lost"] += 1
        last_seq = seq
        if not pad:
            continue
        for bit, name in XBOX:
            (pad.press_button if btn & bit else pad.release_button)(button=getattr(B, name))
        pad.left_trigger(value=255 if btn & L2 else 0)
        pad.right_trigger(value=255 if btn & R2 else 0)
        pad.left_joystick(x_value=axis(lx), y_value=axis(ly, True))
        pad.right_joystick(x_value=axis(rx), y_value=axis(ry, True))
        pad.update()
        held = True


def report(stats):
    last = 0
    while True:
        time.sleep(5)
        n = stats["pkts"]
        print(f"link: pad {(n - last) / 5:.0f} packets/s, {stats['lost']} lost, {stats['bad']} bad", flush=True)
        last = n


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--nuldav", default=str(Path(__file__).resolve().parents[2] / "nuld-av"), help="the nuld-av folder")
    ap.add_argument("--port", type=int, default=8099, help="the PS2's menu (TCP) and video (UDP) port")
    ap.add_argument("--pad-port", type=int, default=8098, help="UDP port the PS2 sends the pad to")
    ap.add_argument("--no-audio", dest="audio", action="store_false", help="video only")
    a, rest = ap.parse_known_args()
    sys.path.insert(0, str(Path(a.nuldav) / "tools"))
    try:
        import fastsrv
        import livesrv
    except ImportError:
        sys.exit(f"link: nuld-av's tools are not in {a.nuldav} (github.com/danielnuld/nuld-av; --nuldav <folder>)")
    live = argparse.ArgumentParser()
    livesrv.add_args(live) # --rate, --fps, --title, --region, --grab, --encoder, --audio-rate
    # the GPU captures, scales and encodes (Desktop Duplication, Quick Sync): what a game leaves of the CPU is not
    # enough for gdigrab and the software encoder (console tests with Fortnite, 2026-11-15/16)
    live.set_defaults(grab="ddagrab", encoder="mpeg2_qsv")
    v = live.parse_args(rest)
    v.audio = a.audio
    if v.audio:
        try:
            import soundcard # noqa: F401 (livesrv.audio_feed)
        except ImportError:
            print("link: no audio (pip install soundcard numpy)", file=sys.stderr)
            v.audio = False
    stats = {"pkts": 0, "lost": 0, "bad": 0}
    threading.Thread(target=pad_server, args=(a, stats), daemon=True).start()
    threading.Thread(target=report, args=(stats,), daemon=True).start()
    fastsrv.serve(a.port, lambda what: livesrv.Live(v), "link video")
    try:
        serve(a)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
