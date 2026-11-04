// NULD loading screen for link.elf (2026-11-03): the "NULD — identidad" canvas drawn by the GS. A thread at a lower
// priority than the main one draws it while the main thread sleeps in the IOP reset, DHCP, the HTTP connects and
// the cover downloads: a perspective floor and a wire sphere (gfx_line, alpha fading with depth), the four logo
// variants (A ring, B "> nulld", C NULD//, D label) 3 s each with a glitch cut (the logo redrawn in scissored bands,
// shifted), the status line with a blinking cursor, scanlines. The launcher has no Michroma atlas, so the
// wordmark is drawn as strokes (gfx_stroke).
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <kernel.h>
#include "gfx.h"
#include "splash.h"

#define BG 0x06070A
#define FG 0xE6E8EC
#define MUTED 0x8A919C
#define ACID 0xC8FF2E
#define BLUE 0x2B4BFF
#define PI 3.14159265f

static volatile int run, done, tid = -1;
static char msg[200];
static unsigned char stack[0x8000] __attribute__((aligned(16)));

// ---- strokes: a point list, then gfx_stroke ----
static float px[160], py[160];
static int np;
static void P(float x, float y) { if (np < 160) px[np] = x, py[np] = y, np++; }
static void arc(float cx, float cy, float r, float a0, float a1)
{
	for (int i = 0; i <= 10; i++) {
		float a = a0 + (a1 - a0) * i / 10;
		P(cx + r * cosf(a), cy + r * sinf(a));
	}
}
static void stroke(float t, unsigned c) { gfx_stroke(px, py, np, t, c); np = 0; }
static void seg(float x0, float y0, float x1, float y1, float t, unsigned c) { np = 0, P(x0, y0), P(x1, y1), stroke(t, c); }

// ---- the wordmark, Michroma-like (wide, rounded corners): letters return their advance ----
static float N_(float x, float y, float H, float t, unsigned c)
{
	float W = 1.22f * H, h = t / 2;
	seg(x + h, y, x + h, y + H, t, c);
	seg(x + W - h, y, x + W - h, y + H, t, c);
	seg(x + h, y + h, x + W - h, y + H - h, t, c);
	return W;
}
static float U_(float x, float y, float H, float t, unsigned c)
{
	float W = 1.22f * H, h = t / 2, R = 0.34f * H;
	np = 0, P(x + h, y);
	arc(x + R, y + H - R, R - h, PI, PI / 2);
	arc(x + W - R, y + H - R, R - h, PI / 2, 0);
	P(x + W - h, y), stroke(t, c);
	return W;
}
static float L_(float x, float y, float H, float t, unsigned c)
{
	float W = 1.0f * H, h = t / 2, R = 0.9f * t;
	np = 0, P(x + h, y);
	arc(x + R, y + H - R, R - h, PI, PI / 2);
	P(x + W, y + H - h), stroke(t, c);
	return W;
}
static float D_(float x, float y, float H, float t, unsigned c)
{
	float W = 1.22f * H, h = t / 2, R = 0.34f * H;
	seg(x + h, y, x + h, y + H, t, c);
	np = 0, P(x, y + h);
	arc(x + W - R, y + R, R - h, -PI / 2, 0);
	arc(x + W - R, y + H - R, R - h, 0, PI / 2);
	P(x, y + H - h), stroke(t, c);
	return W;
}
#define GAP 0.3f
float splash_word_w(float H) { return (1.22f * 3 + 1.0f + 3 * GAP) * H; }
void splash_word(float x, float y, float H, float t, unsigned c)
{
	x += N_(x, y, H, t, c) + GAP * H;
	x += U_(x, y, H, t, c) + GAP * H;
	x += L_(x, y, H, t, c) + GAP * H;
	D_(x, y, H, t, c);
}

// lower case for "> nulld" (monospaced: every letter 0.9 H wide), x-height from 0.32 H
#define XH 0.32f
static void n_(float x, float y, float H, float t, unsigned c)
{
	float W = 0.9f * H, h = t / 2, R = 0.3f * H, xh = y + XH * H;
	seg(x + h, xh, x + h, y + H, t, c);
	np = 0, P(x + h, xh + R);
	arc(x + R, xh + R, R - h, PI, 1.5f * PI);
	arc(x + W - R, xh + R, R - h, 1.5f * PI, 2 * PI);
	P(x + W - h, y + H), stroke(t, c);
}
static void u_(float x, float y, float H, float t, unsigned c)
{
	float W = 0.9f * H, h = t / 2, R = 0.3f * H, xh = y + XH * H;
	np = 0, P(x + h, xh);
	arc(x + R, y + H - R, R - h, PI, PI / 2);
	arc(x + W - R, y + H - R, R - h, PI / 2, 0);
	stroke(t, c);
	seg(x + W - h, xh, x + W - h, y + H, t, c);
}
static void l_(float x, float y, float H, float t, unsigned c)
{
	float W = 0.9f * H, h = t / 2;
	seg(x + W * 0.5f, y, x + W * 0.5f, y + H, t, c);
	seg(x + W * 0.16f, y + h, x + W * 0.5f + h, y + h, t, c);
	seg(x + W * 0.1f, y + H - h, x + W * 0.9f, y + H - h, t, c);
}
static void d_(float x, float y, float H, float t, unsigned c)
{
	float W = 0.9f * H, h = t / 2, R = 0.3f * H, xh = y + XH * H;
	seg(x + W - h, y, x + W - h, y + H, t, c);
	np = 0, P(x + W, xh + h);
	arc(x + R, xh + R, R - h, 1.5f * PI, PI);
	arc(x + R, y + H - R, R - h, PI, PI / 2);
	P(x + W, y + H - h), stroke(t, c);
}

void splash_mark(float cx, float cy, float r)
{
	float t = r * 0.18f;
	np = 0;
	for (int i = -1; i <= 49; i++) P(cx + (r - t / 2) * cosf(i * 2 * PI / 48), cy + (r - t / 2) * sinf(i * 2 * PI / 48));
	stroke(t, FG);
	float d = r * 0.985f;
	seg(cx - d, cy + d, cx + d, cy - d, t, ACID);
}

// ---- the four variants, centred on (640, 300); dx shifts them for the glitch ----
static int blink(float s) { return (int)(s * 1.9f) & 1; }
static void variant(int k, float dx, float s)
{
	if (k == 0) { // A: ring + wordmark
		float H = 88, ww = splash_word_w(H), x = 640 - (120 + 36 + ww) / 2 + dx;
		splash_mark(x + 60, 300, 60);
		splash_word(x + 156, 300 - H / 2, H, 14, FG);
	} else if (k == 1) { // B: > nulld + cursor
		float H = 110, t = 12, a = 0.9f * H + 0.18f * H, x = 640 - (0.75f * H + 5 * a + 0.5f * H) / 2 + dx, y = 300 - H / 2;
		float xh = y + XH * H, mid = (xh + y + H) / 2;
		seg(x, xh, x + 0.5f * H, mid, t, ACID);
		seg(x + 0.5f * H, mid, x, y + H, t, ACID);
		x += 0.75f * H;
		n_(x, y, H, t, FG), x += a;
		u_(x, y, H, t, FG), x += a;
		l_(x, y, H, t, FG), x += a;
		l_(x, y, H, t, FG), x += a;
		d_(x, y, H, t, FG), x += a;
		if (!blink(s)) gfx_rect((int)x, (int)(y + 0.1f * H), (int)(0.45f * H), (int)(0.9f * H), ACID);
	} else if (k == 2) { // C: NULD//
		float H = 96, t = 15, ww = splash_word_w(H), x = 640 - (ww + GAP * H + 0.95f * H) / 2 + dx, y = 300 - H / 2;
		splash_word(x, y, H, t, FG);
		float sx = x + ww + GAP * H;
		seg(sx, y + H, sx + 0.42f * H, y, t, ACID);
		seg(sx + 0.5f * H, y + H, sx + 0.92f * H, y, t, ACID);
	} else { // D: the hardware label
		int bw = 640, x = 320 + (int)dx, y = 196, top = 150, bot = 64;
		gfx_rect(x, y, bw, 2, FG), gfx_rect(x, y + top + bot, bw, 2, FG);
		gfx_rect(x, y, 2, top + bot + 2, FG), gfx_rect(x + bw - 2, y, 2, top + bot + 2, FG);
		gfx_rect(x, y + top, bw, 2, FG);
		splash_mark(x + 34 + 46, y + top / 2 + 4, 46);
		splash_word(x + 34 + 92 + 30, y + top / 2 + 4 - 31, 62, 11, FG);
		static const unsigned char bars[30] = {3, 1, 2, 1, 4, 1, 1, 3, 2, 1, 1, 2, 4, 1, 3, 1, 2, 2, 1, 4, 1, 1, 3, 1, 2, 1, 4, 2, 1, 3};
		int bx = x + 18;
		for (int i = 0; i < 30; i++) gfx_rect(bx, y + top + 14, bars[i], bot - 26, FG), bx += bars[i] + 2;
		gfx_rect(bx + 14, y + top, 2, bot, FG);
		gfx_text(&gfx_font_mono, bx + 32, y + top + 12, "SCPH-75001 · EE 294.912 MHz", 0xC9CED6);
		gfx_text(&gfx_font_mono, bx + 32, y + top + 34, "v1.0 · 2026 · nuld.local", ACID);
		int sx = x + (int)fmodf(s * 175, (float)bw); // the scanner passing over the label
		gfx_alpha(0x60), gfx_rect(sx, y + 2, 2, top + bot - 2, ACID), gfx_alpha(0x80);
	}
}
static const int variant_top[4] = {240, 230, 245, 190}, variant_bot[4] = {360, 370, 355, 420};

// ---- background ----
void splash_floor(float s, unsigned rgb, int amax)
{
	gfx_alpha(0x80), gfx_rect(0, 0, GFX_W, GFX_H, BG);
	const float cx = 640, yh = 432, k = 288; // y = yh + k / z
	float ph = fmodf(s * 0.9f, 1);
	for (int i = 0; i < 16; i++) { // across, scrolling towards the viewer; far ones fade before they crowd
		float z = i + 1 - ph;
		if (z < 0.5f) continue;
		float y = yh + k / z, a = amax * (1 - (z - 1) / 9);
		// a 1 px rect, not gfx_line: AA1 lines take their alpha from coverage, and a horizontal one has full
		// coverage, so the depth fade was lost and the far lines crowded bright at the horizon (PCSX2, 2026-11-03)
		if (a >= 2 && y < GFX_H) gfx_alpha((int)a), gfx_rect(0, (int)y, GFX_W, 1, rgb);
	}
	gfx_alpha(0x80);
	for (int i = -14; i <= 14; i++) // depth lines to the vanishing point
		gfx_line(cx + 300.f * i / 0.6f, yh + k / 0.6f, cx + 300.f * i / 10, yh + k / 10, rgb, amax, 0);
}
static void sphere(float s)
{
	const float cx = 640, cy = 300, R = 196, ct = cosf(-0.28f), st = sinf(-0.28f);
	float rot = s * 2 * PI / 28;
	float x0 = 0, y0 = 0, a0 = 0;
	for (int m = 0; m < 9 + 5; m++)
		for (int i = 0, n = m < 9 ? 24 : 36; i <= n; i++) {
			float lat, lon;
			if (m < 9) lat = -PI / 2 + PI * i / n, lon = m * PI / 9 + rot;
			else lat = (m - 11) * PI / 6, lon = 2 * PI * i / n + rot;
			float x = cosf(lat) * cosf(lon), y = sinf(lat), z = cosf(lat) * sinf(lon);
			float y2 = y * ct - z * st, z2 = y * st + z * ct;
			float sx = cx + R * x, sy = cy - R * y2, a = 0x12 + (z2 + 1) * 0x1C;
			if (i) gfx_line(x0, y0, sx, sy, ACID, (int)a0, (int)a);
			x0 = sx, y0 = sy, a0 = a;
		}
}

void splash_scan(void)
{
	gfx_alpha(0x09);
	for (int sy = 0; sy < GFX_H; sy += 3) gfx_rect(0, sy, GFX_W, 1, FG);
	gfx_alpha(0x80);
}

void splash_hud(int x, int y, int w, int h, int len)
{
	gfx_alpha(0x80);
	gfx_rect(x, y, len, 2, ACID), gfx_rect(x, y, 2, len, ACID);
	gfx_rect(x + w - len, y, len, 2, ACID), gfx_rect(x + w - 2, y, 2, len, ACID);
	gfx_rect(x, y + h - 2, len, 2, ACID), gfx_rect(x, y + h - len, 2, len, ACID);
	gfx_rect(x + w - len, y + h - 2, len, 2, ACID), gfx_rect(x + w - 2, y + h - len, 2, len, ACID);
}

static void frame(float s)
{
	int k = (int)(s / 3) & 3;
	float in = fmodf(s, 3);
	gfx_begin();
	gfx_alpha(0x80);
	splash_floor(s, ACID, 0x40);
	sphere(s);
	int cut = in < 0.18f || in > 2.9f; // the glitch at both ends of each variant
	if (!cut) variant(k, 0, s);
	else {
		static const signed char off[5] = {-10, 7, -4, 9, -6};
		int f = (int)(s * 60), top = variant_top[k] - 10, h = (variant_bot[k] - variant_top[k] + 20) / 5;
		gfx_scissor(0, 0, GFX_W - 1, top - 1), variant(k, 0, s);
		for (int b = 0; b < 5; b++) {
			gfx_scissor(0, top + b * h, GFX_W - 1, top + (b + 1) * h - 1);
			variant(k, off[(b + f) % 5], s);
		}
		gfx_scissor(0, top + 5 * h, GFX_W - 1, GFX_H - 1), variant(k, 0, s);
		gfx_scissor(0, 0, GFX_W - 1, GFX_H - 1);
		if (in < 0.05f) gfx_alpha(0x0C), gfx_rect(0, 0, GFX_W, GFX_H, FG), gfx_alpha(0x80); // a 3-frame flash on the cut in
	}
	// status line(s), centred under the logo, the last with a cursor
	char line[200];
	const char *p = msg;
	int y = 486;
	for (int l = 0; l < 4 && *p; l++, y += 24) {
		const char *e = strchr(p, '\n');
		int n = e ? (int)(e - p) : (int)strlen(p);
		snprintf(line, sizeof(line), "%.*s", n, p);
		int w = gfx_text_width(&gfx_font_mono, line), x = 640 - w / 2;
		gfx_alpha(0x80), gfx_rect(x - 12, y - 4, w + 24 + (e ? 0 : 14), 24, BG);
		gfx_text(&gfx_font_mono, x, y, line, l ? MUTED : FG);
		if (!e && !blink(s)) gfx_rect(x + w + 4, y, 8, 16, ACID);
		p = e ? e + 1 : p + n;
	}
	// HUD corners and the edge labels
	splash_hud(24, 24, GFX_W - 48, GFX_H - 48, 28);
	gfx_tracking(1);
	gfx_text(&gfx_font_mono, 64, 30, "NULD 1.0", MUTED);
	const char *r = "EE R5900 · 294.912 MHz";
	gfx_text(&gfx_font_mono, GFX_W - 64 - gfx_text_width(&gfx_font_mono, r), 30, r, MUTED);
	gfx_text(&gfx_font_mono, GFX_W - 64 - gfx_text_width(&gfx_font_mono, "SCPH-75001"), GFX_H - 44, "SCPH-75001", MUTED);
	gfx_tracking(0);
	splash_scan();
	gfx_end();
	gfx_flip();
}

static void splash_thread(void *arg)
{
	(void)arg;
	clock_t t0 = clock();
	while (run) frame((float)(clock() - t0) / CLOCKS_PER_SEC);
	done = 1;
	ExitThread();
}

void splash_msg(const char *s) { snprintf(msg, sizeof(msg), "%s", s); } // ponytail: a torn line for one frame at worst

void splash_start(void)
{
	if (run) return;
	extern void *_gp;
	// under the main thread (0x60): it runs while the main one sleeps on the IOP, the network or the PC
	ee_thread_t t = {.func = splash_thread, .stack = stack, .stack_size = sizeof(stack), .gp_reg = &_gp, .initial_priority = 0x70};
	run = 1, done = 0;
	tid = CreateThread(&t);
	StartThread(tid, NULL);
}

void splash_stop(void)
{
	if (!run) return;
	run = 0;
	while (!done) usleep(5000);
	DeleteThread(tid), tid = -1;
}
