## Context

- orbit-jellyfin's `jfplay.c` (phase 15) decodes MPEG-2 Main 640x360 at 24 fps on the IPU with 3 late pictures out
  of 1 438 (`docs/phase15-results.md` there). Its latency is by design seconds long: ~1 MB prebuffer, a 1 MB audio
  ring, and pictures held until the audio clock reaches them. All three go.
- ps2sdk libmpeg never finishes pictures over 1023 macroblocks (its colour-conversion DMA handler swaps the two QWC
  registers on the second run; found in phase 15). 512x448 = 896 macroblocks; 640x448 = 1120 is out.
- `mpegps.c` takes MPEG-2 pack headers (14 bytes + stuffing), so ffmpeg's `-f vob` needs no change. The HTTP client
  decodes chunked bodies, which is how ffmpeg's `-listen 1` server answers.
- Measured on the PC (i7-1165G7, Iris Xe, ffmpeg 8.0, 2026-10-16): gdigrab + `mpeg2video` at 512x448 / 30 fps runs
  at speed 0.998x with 2.0 s user + 2.2 s system CPU per 5 s (the BitBlt of the desktop is the system part);
  `mpeg2_qsv` works at speed 0.98x. ViGEmBus was already installed.

## Decisions

1. **I and P pictures only (`-bf 0`)**: nothing to reorder, so a decoded picture is the newest one; with `-g fps`
   an I picture every second. libmpeg decoded Jellyfin's P (and B) pictures, so P is proven on the IPU.
2. **Show on decode.** The decode loop quantises to vsyncs: with a decode time D between 16.7 and 33.3 ms the loop
   runs at exactly 30 pictures/s; over 33.3 ms it drops to 20 and the ring grows. So D is the number that matters,
   logged as average and worst, with the waits for data taken out.
3. **Behind = ring over `atraso_kb` (48 KB ≈ 2 pictures at 6 Mbit/s): decode, do not draw.** P pictures need every
   picture decoded; skipping the draw saves the upload and a vsync. No jump to the next I picture in the spike.
4. **TCP over HTTP, as Jellyfin**: zero new transport code on the PS2; on the LAN the loss is nil and libmpeg hung
   on cut data in phase 15, so UDP waits until jitter shows in the numbers. link.py is the HTTP server and starts
   ffmpeg at the connection: with ffmpeg's own `-listen 1`, gdigrab opened at start and, when the PS2 connected 4 s
   later, delivered 177 frames in 2 s (124 duplicated): seconds of latency at the start. `TCP_NODELAY` on the relay.
5. **Aspect from config, not from the stream**: the capture is scaled anamorphic to 512x448; `[link] aspecto` says
   what shape to show it in (16:9 = the whole 1280x720 frame).
6. **Pad over UDP at ~60 Hz, 16 bytes**: buttons as libpad's bits (pressed = 1) and the four stick bytes; a sequence
   number counts losses on the PC. DualShock mode is set once the pad is stable, so the sticks read.
7. **Threads**: HTTP reader 0x30, pad 0x40, lwIP 0x56-0x59 (ps2sdk), decode + draw 0x60 (the main thread), the
   watchdog 0x20, as orbit-jellyfin.

## Risks

- Decode time at 30 fps: 896 macroblocks x 30 = 26 880 per second against the 22 080 phase 15 sustained. Fallbacks:
  480x448, 25 fps, or lower `-b:v`.
- `-flags +low_delay` sets the sequence extension's low_delay bit; if libmpeg minds it, `--no-low-delay`.
- If the IPU falls behind, TCP holds ffmpeg back and gdigrab queues frames on the PC: latency grows there, invisible
  to the PS2 but for the ring staying full. The gate's stopwatch catches it.
- gdigrab's cursor is drawn; a game in exclusive full screen may not capture (use a window or borderless).
- lwIP on the EE at ~750 KB/s while the EE decodes: phase 15 managed ~360 KB/s with audio decoding on top.
