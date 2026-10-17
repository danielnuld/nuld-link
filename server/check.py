#!/usr/bin/env python3
"""Checks the PC side the way the PS2 uses it, without a PS2: `python server/link.py` in one terminal, this in another.

Connects to the video port with the request link.c sends, expects 200 (a body to EOF, or chunked), reads 2 s of it
and checks it is an MPEG program stream (a pack header, 00 00 01 BA, first); then sends 60 pad packets (✕ held, left
stick up) to the pad port: link.py's console must report 12 packets/s over its 5 s window with 0 lost. Exit code 0 =
all fine.
"""
import socket
import struct
import sys
import time

host = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
PAD, CROSS = struct.Struct("<4sIHBBBBxx"), 1 << 14

s = socket.create_connection((host, 8099), timeout=5)
s.sendall(f"GET / HTTP/1.1\r\nHost: {host}:8099\r\nConnection: close\r\n\r\n".encode())
head = b""
while b"\r\n\r\n" not in head:
    head += s.recv(4096)
status, _, body = head.partition(b"\r\n\r\n")
lines = status.decode(errors="replace").split("\r\n")
ok = lines[0].startswith("HTTP/1.") and " 200" in lines[0]
chunked = any(l.lower().startswith("transfer-encoding:") and "chunked" in l.lower() for l in lines)
while len(body) < 64:
    body += s.recv(4096)
first = body[body.find(b"\r\n") + 2:] if chunked else body # the first chunk's payload
ps = first[:4] == b"\x00\x00\x01\xba"
t0, n = time.time(), len(body)
while time.time() - t0 < 2:
    d = s.recv(65536)
    if not d:
        break
    n += len(d)
s.close()
print(f"{lines[0]} | {'chunked' if chunked else 'body to EOF'} | {'MPEG program stream' if ps else 'NOT a program stream: ' + first[:4].hex()} | {n / 2 / 1024:.0f} KB/s")

u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
for seq in range(60):
    u.sendto(PAD.pack(b"OLK1", seq, CROSS, 128, 0, 128, 128), (host, 8098))
    time.sleep(1 / 60)
print("pad: 60 packets sent (cross held, left stick up); link.py must report 12 packets/s, 0 lost")
sys.exit(0 if ok and ps else 1)
