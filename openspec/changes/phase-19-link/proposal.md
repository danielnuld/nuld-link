## Why

After the Deckard research (2026-10-16) the user wants something new for the PS2, from newer consoles, that ORBIT can
ship: the inverse of Remote Play. The PC streams its screen to the PS2, which shows it on the TV it is plugged into
(a CRT, eventually, at 480i / 240p) and sends its pad back: Steam, PCSX2, any emulator, with the PS2 controller.
Nobody has a Moonlight / Steam Link client for the PS2. The risk is latency, so this phase starts with a go / no-go
spike built from ORBIT Jellyfin's player, which already decodes MPEG-2 on the IPU from the network.

## What Changes

- **New repo `orbit-link`** with the shared pieces copied unchanged from orbit-jellyfin (`gfx`, `net`, `iop`, `ini`,
  `mpegps`, fonts) and jellyfin.c's HTTP client lifted into **`http.c`**.
- **Spike `link.elf`** (`link.c`): `http://<servidor>:8099/` → demuxer → ring → IPU (libmpeg) → `gfx_mb32`, a picture
  drawn the moment it is decoded; no prebuffer, no audio, no clock; pictures decoded but not drawn while more than
  `atraso_kb` waits in the ring. A pad thread sends 16 bytes at ~60 Hz to UDP 8098. Gate numbers every 5 s in
  `mass0:/link.txt` and on screen (L1+L2+R1+R2 + SELECT); L1+L2+R1+R2 + START leaves.
- **PC `server/link.py`**: an HTTP server that starts ffmpeg (gdigrab → scale 512x448 → `mpeg2video` / `mpeg2_qsv`,
  `-bf 0`, `-g fps`, `-flush_packets 1`, `-muxdelay 0`, MPEG program stream to a pipe) when the PS2 connects and
  relays it (`TCP_NODELAY`); UDP pad → vgamepad (ViGEm) Xbox 360 controller; kills ffmpeg when the PS2 disconnects.
  `server/check.py` exercises both ports as the PS2 does.
- `config.ini [link] servidor / aspecto / atraso_kb`.
- After the gate: 480i / 240p output for a CRT (the launcher's `gfx.c` video modes), MP2 audio with a ~100 ms queue,
  UDP transport only if TCP jitter shows in the numbers, an entry in the launcher.

## Capabilities

### New Capabilities
- `link`: the PS2 as a low-latency streaming client of the PC, with its pad as the PC's controller.

## Impact

- New repo; nothing in the launcher or orbit-jellyfin changes.
- Links ps2sdk `libmpeg` and `libps2ip` / `netman`; `wolfssl` only because the shared `net.c` carries `https_get`.
