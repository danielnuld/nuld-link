# ORBIT Link

The PlayStation 2 as a streaming client of your PC: the PC captures its screen and encodes it, the PS2 decodes it on
its IPU and shows it on the TV, and the PS2 controller comes back as a virtual Xbox 360 pad. Steam, PCSX2, any
emulator, on the TV the PS2 is plugged into, with the PS2 pad. A companion of the
[ORBIT launcher](https://github.com/danielnuld/ps2-launcher), built on [NULD Jellyfin](https://github.com/danielnuld/nuld-jellyfin)'s player.

**Phase 19 spike (go / no-go): latency.** Video only, 512x448 at 30 fps; the gate is in `docs/phase19-results.md`.

## How it works

- **PC** (`server/link.py`): listens on TCP 8099; when the PS2 connects it starts ffmpeg, which captures the screen
  (gdigrab), scales it to 512x448, encodes MPEG-2 with I and P pictures only (no B pictures: nothing to reorder) and
  writes an MPEG program stream to a pipe relayed to the PS2 (`TCP_NODELAY`). Capture starts with the connection on
  purpose: started earlier, gdigrab hands over every frame it missed in a burst. A thread receives the pad over UDP
  (port 8098) and drives a virtual Xbox 360 controller (ViGEm).
- **PS2** (`link.elf`): reads the stream (lwIP), demuxes it, decodes every picture on the IPU (ps2sdk libmpeg) and
  draws it the moment it is decoded: no prebuffer, no clock. If more than `atraso_kb` of video is waiting, pictures
  are decoded but not drawn until the PS2 catches up. The pad is read 60 times a second and sent as 16-byte packets.

Measured on the PC side (i7-1165G7, 2026-10-16): capture + MPEG-2 encode at 512x448 / 30 fps runs in real time with
~14 ms of CPU per picture (`mpeg2video`); `mpeg2_qsv` works too. The PS2 side is what the gate measures.

## Build

```
wsl bash build.sh          # link.elf (ps2dev v2.0.0)
wsl bash build.sh test     # host selftests of the shared pieces
```

## Install

Copy `link.elf` and `title.cfg` to `mass0:/APPS/Link/`: the launcher lists it under APPS. It also runs on its own
from uLaunchELF. Add to the launcher's `mass0:/nuld/config.ini` (`mass0:/orbit/` with launchers up to v1.1):

```
[red]
ip = dhcp

[link]
servidor = 192.168.1.10   ; the PC running server/link.py
aspecto = 16:9            ; the PC screen's shape (the stream is anamorphic); 4:3 for a 4:3 window
atraso_kb = 48            ; more than this waiting in the ring = behind: pictures decoded, not drawn
```

The log goes to `mass0:/link.txt` (and the EE serial port): every 5 s, pictures decoded / shown / skipped, decode ms
(average and worst), network KB/s, ring KB and pad packets/s.

## PC

Needs ffmpeg in `PATH`, Python 3, the [ViGEmBus](https://github.com/nefarius/ViGEmBus/releases) driver and
`pip install vgamepad` (without it the pad is counted, not applied). Windows asks once to allow Python through the
firewall (UDP 8098 in) and ffmpeg (TCP 8099 in).

```
python server/link.py                                    # the desktop
python server/link.py --encoder mpeg2_qsv --title PCSX2  # Intel Quick Sync, one window
python server/link.py --size 480x448 --fps 25            # lighter, if the PS2 cannot keep up
```

Without a PS2: `python server/check.py` connects as the PS2 does (200, MPEG program stream, 60 pad packets), or
`ffplay -fflags nobuffer -flags low_delay http://127.0.0.1:8099` shows the stream.

## Controls

Everything goes to the PC: ✕ A, ○ B, □ X, △ Y, L1/R1 shoulders, L2/R2 triggers, SELECT Back, START Start, sticks and
L3/R3, D-pad. On the PS2 itself: **L1+L2+R1+R2 + SELECT** shows the gate numbers; **L1+L2+R1+R2 + START** leaves.

## Limits of the spike

- No sound. Video only, 512x448 (896 macroblocks: ps2sdk libmpeg never finishes pictures over 1023 macroblocks,
  found in the Jellyfin spike, so 640x448 is out until that is fixed).
- 720p output (the launcher's HDMI setup). 480i / 240p for a CRT comes after the gate, from the launcher's `gfx.c`.
- TCP: if the IPU falls behind, the ring fills and TCP holds ffmpeg back, so latency grows on the PC side; the
  numbers in the log show it (ring KB, skipped). No jump to the next I picture yet.
