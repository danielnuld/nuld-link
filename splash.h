#pragma once
// NULD loading screen: a thread draws it while the main thread waits (IOP modules, network, the PC, covers)
void splash_start(void);              // draws until splash_stop; nothing else may use gfx meanwhile
void splash_msg(const char *s);       // the status line(s) under the logo ('\n' splits lines)
void splash_stop(void);               // waits for the last frame
void splash_mark(float cx, float cy, float r); // the ∅ mark (ring + acid slash), for other screens' headers
// the NULD look for the other screens (the canvas "NULD — identidad"): palette, backdrop, wordmark, HUD corners
#define NULD_BG 0x06070A
#define NULD_STEEL 0x1A1D23
#define NULD_MUTED 0x8A919C
#define NULD_FG 0xE6E8EC
#define NULD_ACID 0xC8FF2E
void splash_floor(float s, unsigned rgb, int a); // clears to NULD_BG, then the perspective floor (lines at alpha a)
void splash_scan(void);                          // CRT scanlines over everything: draw it last
float splash_word_w(float H);                    // the drawn "NULD" wordmark, H px tall, stroke t
void splash_word(float x, float y, float H, float t, unsigned rgb);
void splash_hud(int x, int y, int w, int h, int len); // acid corner brackets around a box
