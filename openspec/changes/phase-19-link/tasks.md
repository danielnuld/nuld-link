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
- [x] 3.2 Audio (asked before the gate, 2026-11-03): WASAPI loopback (`soundcard`) -> ffmpeg stdin -> MP2 192k over
      UDP 8097 (relayed by link.py to the PS2's hellos; not muxed: ffmpeg's PS muxer held the video for the audio); PS2: libmad -> audsrv kept full (it sets the pace), one cut when > 150 ms of MP2 waits.
      PCSX2: 0 cuts, audsrv full; a beep came back 480 ms later through PCSX2's own output (console to measure)
- [ ] 3.3 UDP transport and resync at the next I picture, only if TCP jitter shows in the numbers
- [ ] 3.4 Entry in the launcher (APPS, or its own tile)

## 4. Catalog (2026-11-03, asked before the gate)

- [x] 4.1 `link.py` GET /catalog (Epic manifests + Steam appmanifests), /cover/<n> (256x368 CT16, cover from Epic's
      catcache.bin keyImages or Steam's librarycache / CDN), /launch/<n> (the launcher's URI)
- [x] 4.2 `link.c` menu first: Desktop + games in a row (jfplay's browse layout), X launches and streams,
      L1+L2+R1+R2+TRIANGLE back to the menu; the pad reaches the PC only while streaming
- [x] 4.3 PCSX2 with a fake catalog (Fortnite's cover from catcache, a game without one): menu, launch, stream, back
- [x] 4.4 Real Epic installs (Among Us, Fortnite, Sonic Mania) in the catalog with their covers; the 2026 launcher
      writes only EOS InstallHelper `*.egi` (no Data/Manifests), read as well. Sonic Mania launched from the PS2 menu
      in PCSX2 and played with the PS2 pad (video for the devlog, see docs/phase19-results.md)
- [ ] 4.5 A Steam game in the catalog (Steam not installed on this PC yet); launch from the console
