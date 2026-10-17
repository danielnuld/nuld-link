## ADDED Requirements

### Requirement: Server
`config.ini [link] servidor` SHALL name the PC (IP or host name) that serves the video on TCP 8099 and receives the
pad on UDP 8098. While the PC is not serving, the PS2 SHALL retry every 2 s and say so on screen; a missing
`servidor` or a network error SHALL be reported on screen.

#### Scenario: PC started later
- **WHEN** link.elf starts before `server/link.py`
- **THEN** it shows "Esperando a la PC" with the address, and plays within 2 s of link.py starting

### Requirement: Low-latency playback
Every decoded picture SHALL be drawn as soon as it is decoded, without a prebuffer or a clock. While more than
`atraso_kb` KB of video waits in the ring, pictures SHALL be decoded and not drawn. The stream's shape SHALL follow
`[link] aspecto` (16:9 by default).

#### Scenario: Keeping up
- **WHEN** the PC serves 512x448 at 30 fps for 5 minutes
- **THEN** the log shows at least 29 shown/s, decode time under 33 ms on average and the ring under `atraso_kb`

### Requirement: Pad to the PC
The pad on port 0 SHALL be read in DualShock mode about 60 times a second and sent to UDP 8098 as 16 bytes: "OLK1",
a sequence number, the buttons (libpad bits, pressed = 1) and the four stick bytes. link.py SHALL apply them to a
virtual Xbox 360 controller and release it when no packet arrives for a second.

#### Scenario: Controller on the PC
- **WHEN** the user holds ✕ and pushes the left stick up on the PS2
- **THEN** joy.cpl on the PC shows A pressed and the left stick up, and both release when the PS2 is turned off

### Requirement: Gate numbers
Every 5 s link.elf SHALL log pictures decoded / shown / skipped per second, decode ms (average and worst, waits for
data excluded), network KB/s, ring KB and pad packets/s to `mass0:/link.txt`; L1+L2+R1+R2 + SELECT SHALL show them on
screen and L1+L2+R1+R2 + START SHALL leave.

#### Scenario: Log line
- **WHEN** a stream has played for 5 s
- **THEN** `mass0:/link.txt` has a `link: 5.0 s: ... decoded/s, ... shown/s, skipped ..., decode ... ms avg ... max, net ... KB/s, ring ... KB, pad .../s` line
