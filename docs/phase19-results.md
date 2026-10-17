# Phase 19 results: ORBIT Link (spike)

Coded on 2026-10-16. **The spike has not run on the console yet.**

## Checked off the console (2026-10-16, i7-1165G7 / Iris Xe, ffmpeg 8.0)

- gdigrab + `mpeg2video` at 512x448 / 30 fps, `-bf 0 -g 30 -b:v 6M`: speed 0.998x, 150 pictures in 5 s, 2.0 s user +
  2.2 s system CPU (the desktop BitBlt). `mpeg2_qsv`: speed 0.98x. ViGEmBus service running.
- `link.elf` builds (ps2dev v2.0.0, gcc 15.2), 2 005 204 bytes; `make test` passes (gfx, ini, net, mpegps).
- `server/check.py` against `server/link.py`: `HTTP/1.1 200 OK`, MPEG program stream (pack header first), 60 pad
  packets counted at 12/s with 0 lost; ffmpeg killed and the server back to waiting after the disconnect.
- Found on the way: with ffmpeg's own `-listen 1`, gdigrab started at ffmpeg's start and the first connection got
  `frame=177 ... dup=124 speed=2.81x`, seconds of stale frames; link.py now serves the socket itself and starts
  ffmpeg when the PS2 connects: `frame=73 fps=30 dup=9 speed=0.999x` over the same 2.5 s.
- Found on the way: `ffmpeg` in PATH is a Chocolatey shim (392 KB) that runs the real ffmpeg as a child; `Popen.kill()`
  left it alive on a full pipe for as long as the test ran. link.py closes the pipe and `taskkill /T`s the tree.
- Found on the way: on Windows `SO_REUSEADDR` let an orphaned `ffmpeg -listen 1` share port 8099 and take the
  connection; link.py sets it only off Windows.

## Console gate (to do)

1. On the PC: `pip install vgamepad`, then `python server/link.py`. Windows asks to allow Python (UDP 8098) and ffmpeg
   (TCP 8099) through the firewall: allow on private networks.
2. `mass0:/orbit/config.ini`: `[link] servidor = <PC IP>` (and `aspecto = 16:9`). `[red]` as the launcher.
3. `wsl bash build.sh`, copy `link.elf` + `title.cfg` to `mass0:/APPS/Link/`, start it from the launcher or uLaunchELF.
4. **Latency**: open a millisecond stopwatch full screen on the PC (any web stopwatch). Photograph the PC screen and
   the TV in one shot, ten times; the difference of the two readings is the glass-to-glass latency. Write the ten
   numbers here. **GO at ≤ 100 ms average.**
5. **Sustained**: play something for 5 minutes (a Steam game in a window, or PCSX2). Press L1+L2+R1+R2 + SELECT for
   the numbers. Send `mass0:/link.txt` back. GO if shown/s ≥ 29, decode average < 30 ms, ring mostly under
   `atraso_kb`, no `stall` lines, pad ≈ 60/s with 0 lost on the PC's console.
6. If NO-GO on decode time: `python server/link.py --size 480x448` then `--fps 25`; note which passes.

## Numbers

(to fill on the console)
