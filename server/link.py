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
Pad: UDP port 8098, 16 bytes per packet from the PS2 (link.c pad_pkt): "OLK1", sequence, buttons, lx, ly, rx, ry.
Needs ffmpeg in PATH. The pad needs the ViGEmBus driver (github.com/nefarius/ViGEmBus/releases) and
`pip install vgamepad`; without vgamepad the packets are counted and not applied.
Check without a PS2: `ffplay -fflags nobuffer -flags low_delay http://127.0.0.1:8099` while this runs.
"""
import argparse
import socket
import struct
import subprocess
import sys
import threading
import time

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


def ffmpeg_cmd(a):
    src = ["-f", "gdigrab", "-framerate", str(a.fps), "-i", f"title={a.title}" if a.title else "desktop"]
    enc = ["-c:v", a.encoder, "-bf", "0", "-g", str(a.fps), "-b:v", a.bitrate, "-maxrate", a.maxrate, "-bufsize", a.bufsize]
    if a.low_delay:
        enc += ["-flags", "+low_delay"]
    return ["ffmpeg", "-hide_banner", "-loglevel", "warning", "-stats", *src,
            "-vf", f"scale={a.size.replace('x', ':')}:flags=bilinear", *enc, "-an",
            "-f", "vob", "-muxdelay", "0", "-muxpreload", "0", "-flush_packets", "1", "pipe:1"]


def serve(a, cmd):
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
        c.settimeout(None)
        c.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1) # every packet out at once: Nagle would hold them 40 ms
        c.sendall(b"HTTP/1.1 200 OK\r\nContent-Type: video/mpeg\r\nConnection: close\r\n\r\n")
        print(f"link: {peer[0]} connected, capture starts", flush=True)
        t0, sent, d = time.time(), 0, b"x"
        ff = subprocess.Popen(cmd, stdout=subprocess.PIPE)
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
    ap.add_argument("--port", type=int, default=8099, help="HTTP port the PS2 reads the video from")
    ap.add_argument("--pad-port", type=int, default=8098, help="UDP port the PS2 sends the pad to")
    ap.add_argument("--no-low-delay", dest="low_delay", action="store_false", help="without the MPEG-2 low_delay flag (if libmpeg minds it)")
    a = ap.parse_args()
    stats = {"pkts": 0, "lost": 0, "bad": 0}
    threading.Thread(target=pad_server, args=(a, stats), daemon=True).start()
    threading.Thread(target=report, args=(stats,), daemon=True).start()
    cmd = ffmpeg_cmd(a)
    print(" ".join(cmd), flush=True)
    try:
        serve(a, cmd)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
