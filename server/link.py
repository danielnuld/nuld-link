#!/usr/bin/env python3
"""ORBIT Link, the PC side (phase 19 spike): serves the screen to the PS2 and turns its pad into an Xbox 360 controller.

  python server/link.py                                    # the desktop, 512x448, 30 fps, 6 Mbit/s, software MPEG-2
  python server/link.py --encoder mpeg2_qsv --title PCSX2  # Intel Quick Sync, one window (gdigrab title=)
  python server/link.py --size 480x448 --fps 25            # if the PS2 cannot keep up (docs/phase19-results.md)

Video: this script listens on TCP 8099; when the PS2 (link.elf) connects it answers the HTTP request and starts ffmpeg,
which captures the screen (gdigrab, Windows), scales it to WxH, encodes MPEG-2 with I and P pictures only and writes an
MPEG program stream to a pipe that is relayed to the PS2, which decodes it on the IPU. ffmpeg starts with the
connection on purpose: started earlier (ffmpeg -listen 1) gdigrab hands over every frame it missed in a burst, seconds
of latency at the start. When the PS2 disconnects ffmpeg is killed and the script waits for the next connection.
Audio: what Windows plays (WASAPI loopback of the default speakers, `pip install soundcard`) goes to ffmpeg's stdin
as 48 kHz s16 stereo and out as MP2 over UDP, one 24 ms frame per datagram, from the same ffmpeg as a second output
(to 127.0.0.1; audio_relay() sends it on from UDP 8097 to wherever the PS2's "OLA1" hellos come from: PCSX2's
Sockets network and NATs only let in answers to what the PS2 sent). Not in the program stream: its muxer emits a pack only when every stream has data
(mpegenc.c output_packet) and the generic interleaver waits for both (mux.c), so the video left in bursts paced by the
audio frames, +150 ms and skipped pictures on the PS2. Without soundcard there is no audio.
Pad: UDP port 8098, 16 bytes per packet from the PS2 (link.c pad_pkt): "OLK1", sequence, buttons, lx, ly, rx, ry.
Needs ffmpeg in PATH. The pad needs the ViGEmBus driver (github.com/nefarius/ViGEmBus/releases) and
`pip install vgamepad`; without vgamepad the packets are counted and not applied.
Catalog (the PS2's menu): GET /catalog = one "SOURCE\tTitle" line per installed game (Epic, Steam), read from the
launchers' own files at each request; GET /cover/<n> = its cover as 256x368 CT16 (RGB555, bit 15 set, little-endian,
what gfx_image draws), 404 without one; GET /launch/<n> opens it through the launcher's URI. GET / is the video.
Check without a PS2: `ffplay -fflags nobuffer -flags low_delay http://127.0.0.1:8099` while this runs.
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


def small(c, path):
    """The menu's requests: /catalog, /cover/<n>, /launch/<n>. One answer, then the connection closes."""
    global games
    body, code = b"", 200
    try:
        if path == "/catalog":
            games = sorted(epic_games() + steam_games(), key=lambda g: g[1].lower())
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
            os.startfile(games[int(m[1])][2])
        else:
            code = 404
    except Exception as e: # an index past the catalog, a cover that would not download or decode...
        print(f"link: {path}: {type(e).__name__}: {e}", file=sys.stderr, flush=True)
        body, code = b"", 404
    head = f"HTTP/1.1 {code} {'OK' if code == 200 else 'Not Found'}\r\nContent-Length: {len(body)}\r\nConnection: close\r\n\r\n"
    c.sendall(head.encode() + body)


def ffmpeg_cmd(a):
    src = ["-f", "gdigrab", "-framerate", str(a.fps)]
    if a.region: # X,Y,WxH of the desktop: one monitor, or a game window beside PCSX2 (no mirror tunnel)
        x, y, size = a.region.split(",")
        src += ["-offset_x", x, "-offset_y", y, "-video_size", size]
    src += ["-i", f"title={a.title}" if a.title else "desktop"]
    enc = ["-c:v", a.encoder, "-bf", "0", "-g", str(a.fps), "-b:v", a.bitrate, "-maxrate", a.maxrate, "-bufsize", a.bufsize]
    if a.low_delay:
        enc += ["-flags", "+low_delay"]
    aud = []
    if a.audio:
        src += ["-f", "s16le", "-ar", "48000", "-ac", "2", "-i", "pipe:0"] # audio_feed() writes it
        aud = ["-map", "1:a", "-c:a", "mp2", "-b:a", a.audio_bitrate, "-f", "mp2",
               f"udp://127.0.0.1:{a.relay_port}?pkt_size={a.audio_bitrate_bps * 1152 // 48000 // 8}"] # one MP2 frame each
    return ["ffmpeg", "-hide_banner", "-nostdin", "-loglevel", "warning", "-stats", *src,
            "-map", "0:v", "-vf", f"scale={a.size.replace('x', ':')}:flags=bilinear", *enc,
            "-f", "vob", "-packetsize", str(a.packet_size), "-muxdelay", "0", "-muxpreload", "0", "-flush_packets", "1", "pipe:1",
            *aud]


def audio_relay(a):
    """ffmpeg's MP2 datagrams (on a local port, returned) on to the PS2: from UDP audio_port, to the address its last
    "OLA1" hello came from (link.c sends one a second from its own port 8097)."""
    out = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    out.bind(("0.0.0.0", a.audio_port))
    local = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    local.bind(("127.0.0.1", 0))
    dest = [None]

    def hellos():
        while True:
            d, addr = out.recvfrom(64)
            if d[:4] == b"OLA1" and addr != dest[0]:
                print(f"link: audio to {addr[0]}:{addr[1]}", flush=True)
                dest[0] = addr

    def relay():
        while True:
            d = local.recv(4096)
            if dest[0]:
                try:
                    out.sendto(d, dest[0])
                except OSError:
                    pass

    threading.Thread(target=hellos, daemon=True).start()
    threading.Thread(target=relay, daemon=True).start()
    return local.getsockname()[1]


def audio_feed(pipe):
    """Windows' sound to ffmpeg, 10 ms at a time, until ffmpeg goes away. soundcard pads silence with zeros (WASAPI
    loopback itself sends nothing while nothing plays), so ffmpeg's audio input never starves the video."""
    import numpy as np
    import soundcard as sc
    try:
        mic = sc.get_microphone(sc.default_speaker().name, include_loopback=True)
        with mic.recorder(samplerate=48000, channels=2, blocksize=480) as r:
            while True:
                pipe.write((np.clip(r.record(numframes=480), -1, 1) * 32767).astype("<i2").tobytes())
    except (OSError, ValueError): # ffmpeg left (its stdin closed)
        pass
    except Exception as e:
        print(f"link: audio capture stopped ({type(e).__name__}: {e})", file=sys.stderr, flush=True)


def serve(a):
    """One PS2 at a time: answer its GET, start ffmpeg, relay its output until the PS2 goes away."""
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    if sys.platform != "win32": # on Windows SO_REUSEADDR lets two listeners share the port and steal connections
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", a.port))
    srv.listen(1)
    while True:
        print(f"link: waiting for the PS2 on http://0.0.0.0:{a.port}/ (video) and udp {a.pad_port} (pad)", flush=True)
        c, peer = srv.accept()
        c.settimeout(5)
        req = b""
        try:
            while b"\r\n\r\n" not in req:
                d = c.recv(1024)
                if not d:
                    break
                req += d
        except socket.timeout:
            pass
        if not req.startswith(b"GET "):
            c.close()
            continue
        path = req.split()[1].decode("latin-1")
        if path != "/":
            try:
                small(c, path)
            except OSError:
                pass
            c.close()
            continue
        c.settimeout(None)
        c.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1) # every packet out at once: Nagle would hold them 40 ms
        c.sendall(b"HTTP/1.1 200 OK\r\nContent-Type: video/mpeg\r\nConnection: close\r\n\r\n")
        print(f"link: {peer[0]} connected, capture starts", flush=True)
        t0, sent, d = time.time(), 0, b"x"
        cmd = ffmpeg_cmd(a)
        ff = subprocess.Popen(cmd, stdin=subprocess.PIPE if a.audio else None, stdout=subprocess.PIPE)
        if a.audio:
            threading.Thread(target=audio_feed, args=(ff.stdin,), daemon=True).start()
        try:
            while True:
                d = ff.stdout.read1(65536) # whatever ffmpeg has written so far
                if not d:
                    break
                c.sendall(d)
                sent += len(d)
            print(f"link: ffmpeg ended ({ff.wait()}) after {time.time() - t0:.0f} s; check its messages above", file=sys.stderr, flush=True)
            if time.time() - t0 < 2:
                sys.exit(1)
        except OSError as e:
            print(f"link: the PS2 disconnected ({type(e).__name__}) after {time.time() - t0:.0f} s, {sent >> 10} KB sent", flush=True)
        finally:
            ff.stdout.close() # ffmpeg's next write fails and it leaves on its own...
            if ff.stdin:
                try:
                    ff.stdin.close()
                except OSError:
                    pass
            if sys.platform == "win32": # ...and the whole tree goes: a shim in PATH (scoop, chocolatey) would survive ff.kill()
                subprocess.run(["taskkill", "/F", "/T", "/PID", str(ff.pid)], capture_output=True)
            ff.kill()
            ff.wait()
            c.close()


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
    ap.add_argument("--size", default="512x448", help="encoded size; 512x448 = 896 macroblocks, under libmpeg's 1023 limit")
    ap.add_argument("--fps", type=int, default=30)
    ap.add_argument("--bitrate", default="6M")
    ap.add_argument("--maxrate", default="8M")
    ap.add_argument("--bufsize", default="1M")
    ap.add_argument("--encoder", default="mpeg2video", help="mpeg2video (software) or mpeg2_qsv (Intel Quick Sync)")
    ap.add_argument("--title", default="", help="capture one window by title instead of the desktop")
    ap.add_argument("--region", default="", help="capture X,Y,WxH of the desktop (physical pixels), e.g. 0,0,1280x720")
    ap.add_argument("--port", type=int, default=8099, help="HTTP port the PS2 reads the video from")
    ap.add_argument("--pad-port", type=int, default=8098, help="UDP port the PS2 sends the pad to")
    ap.add_argument("--no-low-delay", dest="low_delay", action="store_false", help="without the MPEG-2 low_delay flag (if libmpeg minds it)")
    ap.add_argument("--no-audio", dest="audio", action="store_false", help="video only")
    ap.add_argument("--audio-bitrate", default="192k", help="MP2 bitrate")
    ap.add_argument("--audio-port", type=int, default=8097, help="UDP port the PS2 receives the MP2 audio on")
    ap.add_argument("--packet-size", type=int, default=2048, help="program stream pack size")
    a = ap.parse_args()
    a.audio_bitrate_bps = int(a.audio_bitrate.rstrip("kK")) * 1000 if a.audio_bitrate[-1] in "kK" else int(a.audio_bitrate)
    if a.audio:
        try:
            import soundcard # noqa: F401 (audio_feed)
        except ImportError:
            print("link: no audio (pip install soundcard)", file=sys.stderr)
            a.audio = False
    stats = {"pkts": 0, "lost": 0, "bad": 0}
    threading.Thread(target=pad_server, args=(a, stats), daemon=True).start()
    threading.Thread(target=report, args=(stats,), daemon=True).start()
    if a.audio:
        a.relay_port = audio_relay(a)
    print(" ".join(ffmpeg_cmd(a)), flush=True)
    try:
        serve(a)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
