# NULD Link

The PlayStation 2 as a streaming client of your PC: the PC captures its screen and encodes it, the PS2 decodes it on
its IPU and shows it on the TV, and the PS2 controller comes back as a virtual Xbox 360 pad. Steam, PCSX2, any
emulator, on the TV the PS2 is plugged into, with the PS2 pad. A companion of the
[ORBIT launcher](https://github.com/danielnuld/ps2-launcher), built on [NULD Jellyfin](https://github.com/danielnuld/nuld-jellyfin)'s player.

**1280x720 at 30 pictures a second, 70-100 ms from the PC's screen to the TV** (stopwatch photos on a SCPH-75001, TV in
game mode, 2026-11-15), with [nuld-av](https://github.com/danielnuld/nuld-av). The first version (512x448 through
ps2sdk's libmpeg over TCP) is in `docs/phase19-results.md` and on the `master` branch.

## How it works

- **PC** (`server/link.py`): when the PS2 asks, ffmpeg captures the screen (gdigrab), scales it to 1280x720 and encodes
  MPEG-2 with I and P pictures only; nuld-av's `psvpack` repacks each picture for the PS2's IPU as it leaves the
  encoder and it goes out at once by UDP (nuld-av's fast path, with lost datagrams sent again). What Windows plays
  goes in the same stream as MP2. A thread receives the pad over UDP (port 8098) and drives a virtual Xbox 360
  controller (ViGEm). The menu's catalog, covers and launches are small HTTP requests on TCP 8099.
- **PS2** (`link.elf`): nuld-av's player shows every picture as soon as it is decoded (no buffer, no clock); the sound
  goes through libmad to audsrv. TCP/IP runs on the IOP and the stream is taken straight from the Ethernet driver
  there. The pad is read 60 times a second and sent as 16-byte packets.

## Build

nuld-av, built, beside this folder (`../nuld-av`, or `make NULDAV=<folder>`):

```
wsl bash ../nuld-av/build.sh   # libnuldav.a and netring.irx
wsl bash build.sh              # link.elf
wsl bash build.sh test         # host selftests of the shared pieces
```

## Install

Copy `link.elf` and `title.cfg` to `mass0:/APPS/Link/`: the launcher lists it under APPS. It also runs on its own
from uLaunchELF. Add to the launcher's `mass0:/nuld/config.ini` (`mass0:/orbit/` with launchers up to v1.1):

```
[red]
ip = dhcp

[link]
servidor = 192.168.1.10   ; the PC running server/link.py: its address, not a name
directo = 0               ; 1: the desktop's stream at once, the menu only after it ends
```

The log goes to `mass0:/link.txt` (and the EE serial port): every 5 s, pictures shown and dropped, decode ms, what
waits to be decoded and the network's KB/s.

## PC

Needs ffmpeg in `PATH`, Python 3, nuld-av beside this repository with its `tools/psvpack.exe` built
(`tools/build_psvpack.bat` there), the [ViGEmBus](https://github.com/nefarius/ViGEmBus/releases) driver and
`pip install vgamepad` (without it the pad is counted, not applied), and `pip install soundcard numpy` for the sound.
Windows asks once to allow Python through the firewall (UDP 8098 and 8099, TCP 8099 in).

```
python server/link.py                    # the desktop
python server/link.py --title PCSX2      # one window
python server/link.py --rate 16M         # more bits (the PS2's network takes ~20M)
```

Put the TV in game mode: its own processing was most of the delay.

## Controls

Everything goes to the PC: ✕ A, ○ B, □ X, △ Y, L1/R1 shoulders, L2/R2 triggers, SELECT Back, START Start, sticks and
L3/R3, D-pad. On the PS2 itself: **L1+L2+R1+R2 + TRIANGLE** goes back to the menu; **L1+L2+R1+R2 + START** leaves.

## Limits

- 16:9 only: the PC's screen is scaled to 1280x720 whatever its shape.
- If the PS2 or the network falls behind, pictures wait on the PC and the delay grows until it catches up; there
  is no jump to the newest picture yet.
- 720p output only (the launcher's HDMI set-up); 480i / 240p for a CRT is not done.
