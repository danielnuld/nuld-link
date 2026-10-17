## 1. Pieces (host-checked)

- [x] 1.1 Repo from orbit-jellyfin's skeleton; `http.c` lifted from jellyfin.c; `make test` passes
- [x] 1.2 `server/link.py`: HTTP server, ffmpeg started at the connection and relayed, UDP pad → vgamepad
- [x] 1.3 `link.elf` builds (ps2dev v2.0.0, gcc 15.2): 2 005 204 bytes, no warnings of its own
- [x] 1.4 PC alone: `server/check.py` gets 200 + an MPEG program stream and link.py counts its 60 pad packets, 0 lost

## 2. Gate (console, SCPH-75001 + HDMI adapter)

- [ ] 2.1 `link.elf` connects, shows the PC's desktop, the pad moves a virtual Xbox 360 controller (joy.cpl)
- [ ] 2.2 Glass to glass: a millisecond stopwatch on the PC, one photo of the PC screen and the TV side by side,
      ten photos averaged. GO at ≤ 100 ms
- [ ] 2.3 `mass0:/link.txt` over 5 minutes of a game: ≥ 29 shown/s, decode average under 30 ms, ring under
      `atraso_kb` almost always, 0 stalls, pad ~60/s with 0 lost

## 3. After a GO

- [ ] 3.1 480i / 240p output for a CRT (the launcher's `gfx.c` modes, `[video] pantalla`)
- [ ] 3.2 Audio: MP2 through libmad + audsrv with a ~100 ms queue (`-c:a mp2` in link.py)
- [ ] 3.3 UDP transport and resync at the next I picture, only if TCP jitter shows in the numbers
- [ ] 3.4 Entry in the launcher (APPS, or its own tile)
