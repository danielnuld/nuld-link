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

## PCSX2 2.x (SCPH-75001 BIOS, DEV9 "Sockets", 64 MB FAT32 USB image with only orbit/config.ini), 2026-11-03

`link.elf` run with `-elf`: USB mounted, config read, DHCP from PCSX2, connected to 192.168.100.8:8099, 512x448 at
33 ms per picture. First 5 s windows: 27.6 then 30.6 shown/s, skipped 0, decode 5 ms avg (22 / 8 max), ring 3 KB,
pad 62/s at the PS2 end and on link.py. The TV window shows the PC desktop (a mirror tunnel of PCSX2 itself).
Timing in PCSX2 is not cycle-accurate: decode time and latency still belong to the console gate.

## Catalog in PCSX2, 2026-11-03

Fake catalog (scratchpad wrapper over link.py: Fortnite with its catcache cover, one entry without a cover): the menu
shows Desktop, the cover and the drawn card; right + X launched the entry and the stream started (29.9 shown/s, pad
62/s); L1+L2+R1+R2+TRIANGLE stopped it (511 decoded, 511 shown) and the menu came back on the same entry. Bug found:
the eased selection never reached `sel` exactly, so the big card sat in the row's right half (snap under 0.01;
orbit-jellyfin's jfplay browse has the same formula).

## Real catalog and the devlog take, PCSX2 2026-11-03

Epic's launcher (Nov 2026) keeps no `Data/Manifests/*.item`: installs are listed in
`ProgramData/Epic/EpicOnlineServicesShared/InstallHelper/InstalledItems/*.egi` (state, dir, launchExe, sandboxId =
namespace, itemId, artifactId = app name); names, categories and covers come from `catcache.bin` by itemId. Catalog:
Among Us, Fortnite, Sonic Mania, all three with covers.

Take (`%USERPROFILE%/Videos/ORBIT Link/orbit-link-sonic-2026-11-03.mp4`, 3:10, 1920x1080 60 fps, h264_qsv): Sonic
Mania windowed at the left half (client 0,270 960x540), PCSX2 16:9 at the right half (display 960,270 960x540),
`link.py --region 0,270,960x540`, the desktop recorded and the two halves cropped side by side. Boot -> menu ->
Sonic Mania launched from the PS2 -> SEGA, title, intro, Green Hill played with the PS2 pad -> L1+L2+R1+R2+TRIANGLE
back to the menu. Stream during play: 30.0 decoded/s, 29.4-29.9 shown/s, 2-3 skipped per 5 s, decode 6-8 ms avg,
512 KB/s, pad 62/s. Sonic's own timer read 0'31"42 on the PC and 0'31"27 on the PS2 in the same frame (~150 ms,
PCSX2 + capture; the console gate still decides). Sonic Mania keeps drawing without focus, so PCSX2 can hold it.
No sound in the take (Link has none yet; gdigrab records none).

## Audio, PCSX2 2026-11-03

PC: no Stereo Mix on this laptop, so link.py records the default speakers through WASAPI loopback (`pip install
soundcard`; it pads silence, so ffmpeg's audio input never starves) and writes 48 kHz s16 to ffmpeg's stdin; MP2
192 kbit/s in the same program stream, `-max_interleave_delta 1` (no interleaving hold) and `-packetsize 512`: vob's
2048-byte packs carry 85 ms of audio each, and the PS2 got it in 85 ms bursts.

PS2: jfplay's audio thread without the clock. Two wrong turns first: a cap on `audsrv_queued()` dropped frames all the
time and the queue still read 0 now and then. audsrv (iop/sound/audsrv/src/audsrv.c) has a fixed ring of 10 x 2048
bytes (106 ms at 48 kHz stereo) that its play thread reads at 48 kHz whether data is there or not: overtaken, it
replays stale samples; `audsrv_play_audio` cuts what does not fit; `audsrv_queued()` reads 0 when the ring is exactly
full. So the ring is kept full (`audsrv_wait_audio` before every frame, audsrv sets the pace) and the latency is
bounded before it: more than AUDIO_MS (150) of MP2 waiting = one cut to the newest data.

Result over 45 s of a looping Windows sound: 0 cuts, audsrv full the whole time, video unchanged (29-30 shown/s).
Echo test (PCSX2 volume 40, a 250 ms 1 kHz beep, loopback recorded): the PS2's copy came back 480 ms after the
original, 72 % of its energy at 1 kHz (the rest: the next echo already on top, MP2). That 480 ms holds loopback
capture, MP2 encode, network, ~150 ms worst on the PS2 side + 106 ms audsrv, and PCSX2's own audio output buffer.

## Audio moved out of the program stream, PCSX2 2026-11-03

The take with sound (`orbit-link-sonic-audio-2026-11-03.mp4`, audio still muxed) showed the PS2 side freezing and far
behind: 14-54 pictures skipped per 5 s (2-3 without audio), decode 6 -> 10-12 ms, Sonic's timer ~320 ms behind
(150 before). Not the audio thread's priority (0x28 -> 0x70, under the main thread: still 3-55 skipped, ~310 ms).
FFmpeg's program stream muxer emits a pack only when every stream's FIFO holds a packet's worth (mpegenc.c
output_packet ~1038-1049) and the generic interleaver waits for a packet of each stream (mux.c ~1012;
`-max_interleave_delta 1` does not bypass it): the video left in bursts paced by the audio frames.

Now: the same ffmpeg has a second output, MP2 to 127.0.0.1 one 24 ms frame per datagram; link.py relays it from UDP
8097 to where the PS2's "OLA1" hellos come from (once a second from link.c's port 8097: PCSX2's Sockets network
and NATs let in only answers). The video output has one stream again (`-packetsize` back to 2048). Result in PCSX2
with Sonic running: audsrv full, 0 cuts, the timer 150-220 ms behind. Pictures were still skipped (9-72 per 5 s),
and a baseline without audio was not taken in the same conditions (the PC was in use): PCSX2 under the load of
Sonic + two ffmpeg + the laptop's other work likely falls below real time, which the PS2 side reads as "behind".
The console decides.

Take with sound, audio over UDP (`orbit-link-sonic-audio-2026-11-03.mp4`, 2:22, 1080p60 + AAC of Windows' sound,
PCSX2 muted): audsrv full, 0 cuts; Sonic's timer 180-190 ms behind on the PS2; 11 pictures skipped per 5 s on
average (4-39). The PS2 counted 32 decoded pictures per second of its own clock for a 30 fps stream: PCSX2 ran ~6 %
under real time on the laptop (Sonic + two ffmpeg + recorder), and the PS2 side reads that as falling behind.

## NULD look, PCSX2 2026-11-03

The launcher is being renamed NULD (canvas "NULD — identidad"). link.elf's screens follow it: `splash.c` draws the
loading screen while the main thread waits (perspective floor + wire sphere, the four logo variants 3 s each with
a glitch cut in scissored bands, the status line), and the menu is black with a faint floor, the drawn wordmark,
an acid-bordered source tag, HUD corners on the selection and scanlines. There is no Michroma atlas on the PS2: the
wordmark was strokes at first and looked off; it is now vector: `tools/logo.py` bakes the real Michroma, Archivo
800 (125 %) and IBM Plex Mono 500 outlines, triangulated, into `logo_data.c`, drawn by `gfx_vmesh` (flat triangles + the
contours as AA1 line strips for the edges). The text atlas (`tools/font.py`, still 512x384: the 1280x720
framebuffers leave the GS 3200 words) is now Archivo 125 % / Archivo / IBM Plex Mono / a small Michroma NULD. Found on the way: AA1 lines take
their alpha from coverage, so a horizontal gfx_line has full alpha whatever it is given (the floor's far lines
crowded bright at the horizon): the floor's horizontal lines are 1 px rects. Still to see on the console: 1 px
lines on a CRT at 480i.

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
