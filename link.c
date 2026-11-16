// NULD Link: the PS2 as a streaming client of the PC. server/link.py captures the PC's screen, encodes it and sends
// it as PS2V (nuld-av: MPEG-2 pictures repacked for the IPU) by the network's fast path; the PS2 shows 1280x720 at 30
// pictures a second, each as soon as it is decoded, and its pad goes back over UDP, where link.py turns it into a
// virtual Xbox 360 controller (ViGEm). 70-100 ms from the PC's screen to the TV on a SCPH-75001 (stopwatch photos,
// 2026-11-15); before nuld-av it was 512x448 through ps2sdk's libmpeg over TCP.
// - mass0:/nuld/config.ini (orbit/ until the launcher's v1.1) [link] servidor = <PC IP> (an address, not a name);
//   [red] and [ui] idioma as the launcher
// - video and sound: nav_net_open + nav_play_live (nuld-av). The player takes the GS, the IPU and this thread while a
//   stream plays; the sound comes in the same stream as MP2 frames (nav_live_audio) -> ring -> libmad -> audsrv.
//   audsrv reads its ~106 ms ring at 48 kHz whether it holds data or not (overtaken, it replays stale samples), so
//   it is kept full and sets the pace; over AUDIO_MS of MP2 waiting here, one cut to the newest
// - pad: 16 bytes at ~60 Hz to UDP <servidor>:8098 (buttons and both sticks), from its own thread, which also
//   watches for the combos while a stream plays (the main thread is the player's then)
// - menu first: the PC's desktop, then the games link.py finds installed (Epic, Steam) with their covers, in a row;
//   X launches the game on the PC (/launch/<n>) and starts the stream. The menu's requests are HTTP/1.0 GETs through
//   nuld-av's netring (nav_net_tcp): TCP/IP runs on the IOP, the EE has no stack of its own
// - L1+L2+R1+R2 + TRIANGLE back to the menu; L1+L2+R1+R2 + START leaves (Exit: the OSD, and FMCB's autoboot brings
//   NULD back)
// - every 5 s the player's line goes to mass0:/link.txt (and the EE serial port)
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <malloc.h>
#include <time.h>
#include <unistd.h>
#include <kernel.h>
#include <tamtypes.h>
#include <libpad.h>
#include <audsrv.h>
#include <mad.h>
#include <sio.h>
#include <ps2ips.h>
#include <nuldav.h>
#include "lang.h"
#include "gfx.h"
#include "iop.h"
#include "ini.h"
#include "net.h"
#include "ui_data.h"
#include "splash.h"

#define TEXT 0xEEF3FF
#define TEXT2 0xA9B6D3
#define LOG "mass0:/link.txt"
#define VIDEO_PORT 8099
#define PAD_PORT 8098
#define FAST_KBS 2600 // what the fast path may send, KB/s: a SCPH-75001 took this without trouble (nuld-av)
#define COMBO (PAD_L1 | PAD_L2 | PAD_R1 | PAD_R2)
#define AUDIO_MS 150 // MP2 waiting before audsrv; ponytail: fixed, an [link] key if the console wants another
int lang_en; // lang.h

static void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_(const char *fmt, ...)
{
	char b[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	printf("%s", b);
	sio_putsn(b); // the EE serial port too: PCSX2 logs it, and it works when the USB does not
	FILE *f = fopen(LOG, "a");
	if (f) fputs(b, f), fclose(f);
}

// ---- ring: one writer (the player's thread), one reader (the audio thread); size a power of two ----
typedef struct { u8 *b; int size; volatile unsigned rd, wr; } ring; // free-running counters: differences wrap safely
static int ring_used(ring *r) { return (int)(r->wr - r->rd); }
static void ring_put(ring *r, const u8 *p, int n) // the caller made sure it fits
{
	int o = r->wr & (r->size - 1), first = r->size - o < n ? r->size - o : n;
	memcpy(r->b + o, p, first), memcpy(r->b, p + first, n - first);
	__asm__ volatile("" ::: "memory");
	r->wr += n;
}

static ring aring;
static volatile int pad_live; // the pad goes to the PC only while streaming: not the menu's presses
static volatile int stop, leave, pad_sent;
static unsigned host_ip;      // [link] servidor, network order

static void on_audio(const unsigned char *p, int n) // the stream's MP2 frames, on the player's thread; full: dropped
{
	if (ring_used(&aring) + n <= aring.size) ring_put(&aring, p, n);
}

// ---- pad -> PC: 16 bytes at ~60 Hz to UDP 8098 (server/link.py PAD) ----
typedef struct { char magic[4]; u32 seq; u16 buttons; u8 lx, ly, rx, ry, pad[2]; } pad_pkt; // little-endian, no padding
_Static_assert(sizeof(pad_pkt) == 16, "pad_pkt is 16 bytes on the wire");
static struct sockaddr_in pad_to;
static u8 pad_stack[0x4000] __attribute__((aligned(16)));
static void pad_thread(void *arg)
{
	(void)arg;
	int s = socket(AF_INET, SOCK_DGRAM, 0);
	pad_pkt k = {{'O', 'L', 'K', '1'}, 0, 0, 128, 128, 128, 128, {0, 0}};
	struct padButtonStatus b;
	while (!leave) {
		if (pad_live && s >= 0 && padRead(0, 0, &b)) {
			k.buttons = 0xffff ^ b.btns; // pressed = 1, libpad's PAD_* bits
			k.lx = b.ljoy_h, k.ly = b.ljoy_v, k.rx = b.rjoy_h, k.ry = b.rjoy_v;
			// the combos, here while a stream plays: the main thread is inside the player
			if ((k.buttons & (COMBO | PAD_START)) == (COMBO | PAD_START)) leave = 1, nav_stop();
			else if ((k.buttons & (COMBO | PAD_TRIANGLE)) == (COMBO | PAD_TRIANGLE)) nav_stop(); // back to the menu
			if (sendto(s, &k, sizeof(k), 0, (struct sockaddr *)&pad_to, sizeof(pad_to)) == (int)sizeof(k)) pad_sent++;
			k.seq++;
		}
		usleep(16000); // ponytail: ~60 Hz by sleep, not locked to the vsync
	}
	if (s >= 0) close(s);
	ExitThread();
}

static void pad_analog(void) // the sticks need DualShock mode (ps2sdk samples/pad): set once the pad is stable
{
	for (int t = 0; t < 200; t++) {
		int st = padGetState(0, 0);
		if (st == PAD_STATE_STABLE || st == PAD_STATE_FINDCTP1) break;
		usleep(10000);
	}
	padSetMainMode(0, 0, PAD_MMODE_DUALSHOCK, PAD_MMODE_LOCK);
	for (int t = 0; t < 200 && padGetState(0, 0) != PAD_STATE_STABLE; t++) usleep(10000);
}

// ---- audio: MP2 -> PCM -> audsrv (jfplay.c's audio thread). audsrv's RPC client is not thread-safe: only this
// thread calls it while a stream plays ----
static volatile int a_on, a_done, a_free_ms, a_dropped; // a_free_ms: room left in audsrv, ~0 = full (good)
static u8 audio_stack[0x10000] __attribute__((aligned(16)));
static s16 clamp16(mad_fixed_t s)
{
	s += 1L << (MAD_F_FRACBITS - 16);
	if (s >= MAD_F_ONE) s = MAD_F_ONE - 1;
	if (s < -MAD_F_ONE) s = -MAD_F_ONE;
	return s >> (MAD_F_FRACBITS + 1 - 16);
}
static void audio_thread(void *arg)
{
	(void)arg;
	static u8 in[8192 + MAD_BUFFER_GUARD];
	static s16 pcm[1152 * 2];
	struct mad_stream st;
	struct mad_frame fr;
	struct mad_synth sy;
	int have = 0, rate = 48000, bytes_ms = 24; // 192 kbit/s until the first header
	mad_stream_init(&st), mad_frame_init(&fr), mad_synth_init(&sy);
	while (!stop) {
		int k = ring_used(&aring); // top up the decoder's input from the ring
		if (k > (int)sizeof(in) - MAD_BUFFER_GUARD - have) k = sizeof(in) - MAD_BUFFER_GUARD - have;
		for (int i = 0; i < k; i++) in[have + i] = aring.b[(aring.rd + i) & (aring.size - 1)];
		aring.rd += k, have += k;
		if (!have) { usleep(5000); continue; }
		mad_stream_buffer(&st, in, have);
		int frames = 0;
		for (;; frames++) {
			if (mad_frame_decode(&fr, &st)) {
				if (st.error == MAD_ERROR_BUFLEN || !MAD_RECOVERABLE(st.error)) break;
				continue; // a damaged frame: skip it
			}
			if (stop) break;
			mad_synth_frame(&sy, &fr);
			if (!a_on) {
				struct audsrv_fmt_t f = {sy.pcm.samplerate, 16, 2};
				rate = sy.pcm.samplerate;
				a_on = audsrv_set_format(&f) == 0;
				audsrv_set_volume(MAX_VOLUME);
			}
			if (fr.header.bitrate) bytes_ms = fr.header.bitrate / 8000;
			for (int i = 0; i < sy.pcm.length; i++)
				pcm[2 * i] = clamp16(sy.pcm.samples[0][i]), pcm[2 * i + 1] = clamp16(sy.pcm.samples[sy.pcm.channels > 1][i]);
			audsrv_wait_audio(sy.pcm.length * 4); // audsrv sets the pace: its ring stays full, never overtaken
			audsrv_play_audio((char *)pcm, sy.pcm.length * 4);
			a_free_ms = audsrv_available() / 4 * 1000 / rate; // audsrv_queued() reads 0 when its ring is exactly full
		}
		int used = st.next_frame ? st.next_frame - in : 0; // keep the partial frame
		memmove(in, in + used, have - used);
		have -= used;
		if (ring_used(&aring) + have > AUDIO_MS * bytes_ms) // behind the PC: one cut to the newest (libmad resyncs)
			aring.rd = aring.wr, have = 0, a_dropped++;
		if (have >= (int)sizeof(in) - MAD_BUFFER_GUARD) have = 0; // garbage that never decodes
		if (!frames && !k) usleep(5000); // half a frame, nothing new: this thread outranks lwIP, do not spin
	}
	mad_synth_finish(&sy); // an empty macro in libmad: separate statements
	mad_frame_finish(&fr);
	mad_stream_finish(&st);
	a_done = 1;
	ExitThread();
}

static void screen(const char *title, const char *body) // everything before the menu: the NULD splash (splash.c)
{
	(void)title;
	splash_msg(body);
	splash_start();
}

static unsigned edges(unsigned b) // the buttons that went down since the last call
{
	static unsigned prev;
	unsigned p = b & ~prev;
	prev = b;
	return p;
}
static int leave_combo(unsigned b) { return (b & (COMBO | PAD_START)) == (COMBO | PAD_START); }

static void player_says(const char *line) { logf_("%s", line); }

static void play(void) // one stream: until the PC ends it, or the user leaves (the pad thread calls nav_stop)
{
	static u8 abuf[64 << 10] __attribute__((aligned(64))); // ~2.7 s of MP2 at 192 kbit/s
	nav_source src;
	nav_stats st = {0};
	int r = nav_net_open(host_ip, VIDEO_PORT, FAST_KBS, &src);
	if (r < 0) { logf_("link: no stream from the PC (%d)\n", r); return; }
	aring = (ring){abuf, sizeof(abuf), 0, 0};
	a_on = a_done = a_free_ms = a_dropped = 0, stop = 0;
	extern void *_gp;
	// under the player's thread (0x60): libmad runs while that one waits, never in the middle of a picture
	ee_thread_t at = {.func = audio_thread, .stack = audio_stack, .stack_size = sizeof(audio_stack), .gp_reg = &_gp, .initial_priority = 0x70};
	int atid = CreateThread(&at);
	StartThread(atid, NULL);
	nav_live_audio(on_audio);
	pad_live = 1;
	r = nav_play_live(&src, player_says, &st); // takes the GS and this thread until the stream ends
	stop = 1, pad_live = 0;
	nav_live_audio(NULL);
	nav_net_close();
	// the audio thread leaves by itself: terminated inside an audsrv call, the next one hung (jfplay)
	for (int t = 0; t < 200 && !a_done; t++) usleep(10000);
	if (!a_done) TerminateThread(atid), logf_("link: audio thread did not stop\n");
	audsrv_stop_audio();
	DeleteThread(atid);
	logf_("link: stopped (%d): %d decoded, %d shown, %d dropped, %u datagrams sent again, audio %d cuts\n", r, st.decoded, st.shown,
	      st.dropped, nav_net_gaps(), a_dropped);
	gfx_init(); // the player left its own video mode and a black screen
}

// ---- menu: the desktop, then the PC's games (link.py /catalog), covers from /cover/<n> as 256x368 CT16 ----
#define MAXG 24
#define COVER_W 256 // jfplay's poster sizes (cover.h): big for the selection, the rest drawn at the small one
#define COVER_H 368
#define COVER_SW 184
#define COVER_SH 264
static char host[64], gname[MAXG][64], gsrc[MAXG][8];
static unsigned short *gcover[MAXG]; // NULL: none, a drawn card instead
static int ngames;

static int get_body(const char *path, void *dst, int max) // the whole body of GET path: its length, or < 0
{
	static char head[1024];
	nav_source src;
	char req[44];
	int hn = 0, st = 0, body = -1, clen, n = 0;
	if (snprintf(req, sizeof(req), "GET %s HTTP/1.0\r\n\r\n", path) >= (int)sizeof(req)) return NET_ERR_PROTO;
	if (nav_net_tcp(host_ip, VIDEO_PORT, req, &src) < 0) return NET_ERR_CONNECT;
	for (int idle = 0; idle < 5000;) { // 10 s without a byte: the PC is not answering
		const unsigned char *p;
		int k = src.pull(src.user, &p);
		if (k < 0) break; // the PC closed: the answer is whole
		if (!k) { usleep(2000), idle++; continue; }
		idle = 0;
		if (body < 0) { // still in the header
			int t = k < (int)sizeof(head) - hn ? k : (int)sizeof(head) - hn;
			memcpy(head + hn, p, t), hn += t;
			if (!http_parse(head, hn, &st, &body, &clen)) {
				if (hn == (int)sizeof(head)) break; // no header in the first KB: not HTTP
				continue;
			}
			p = (const unsigned char *)head + body, k = hn - body; // what came after it in the same pieces
			// ponytail: a piece that overflowed head[] would lose bytes here; answers' headers are ~70 bytes
		}
		if (n + k > max) k = max - n;
		memcpy((char *)dst + n, p, k), n += k;
	}
	nav_net_close();
	return body < 0 ? NET_ERR_PROTO : st == 200 ? n : -1;
}

static int load_catalog(void) // "SOURCE\tTitle" lines; the game count, or < 0 if the PC did not answer
{
	static char txt[MAXG * 80];
	int n = get_body("/catalog", txt, sizeof(txt) - 1);
	if (n < 0) return n;
	txt[n] = 0;
	ngames = 0;
	for (char *l = strtok(txt, "\n"), *tab; l && ngames < MAXG; l = strtok(NULL, "\n"))
		if ((tab = strchr(l, '\t'))) {
			*tab = 0;
			snprintf(gsrc[ngames], sizeof(gsrc[0]), "%s", l), snprintf(gname[ngames], sizeof(gname[0]), "%s", tab + 1);
			ngames++;
		}
	logf_("link: catalog, %d games\n", ngames);
	return ngames;
}

static void load_covers(void) // ponytail: all at once at the start (188 KB each, MAXG at most), not streamed in
{
	char path[24];
	for (int i = 0; i < ngames; i++) {
		unsigned short *c = memalign(64, COVER_W * COVER_H * 2);
		snprintf(path, sizeof(path), "/cover/%d", i);
		if (c && get_body(path, c, COVER_W * COVER_H * 2) == COVER_W * COVER_H * 2)
			SyncDCache(c, (u8 *)c + COVER_W * COVER_H * 2), gcover[i] = c;
		else free(c);
	}
}

// ---- menu, in the NULD look (the canvas "NULD — identidad", board "Inicio"): black, a faint floor, the drawn
// wordmark, an acid-bordered source tag, HUD corners on the selection, scanlines ----
static const char *entry(int i) { return i ? gname[i - 1] : L("Escritorio", "Desktop"); }

static void cut_to(char *t, int n, int w) // shorten to w px, whole UTF-8 letters. ponytail: cut, not wrapped
{
	while (n > 0 && gfx_text_width(&gfx_font_ui, t) > w) {
		t[--n] = 0;
		while (n > 0 && (t[n - 1] & 0xC0) == 0x80) t[--n] = 0;
		if (n > 0 && (unsigned char)t[n - 1] >= 0xC0) t[--n] = 0;
	}
}

static void card(int i, int x, int y, int big) // the cover, or a hatched panel with the name
{
	int w = big ? COVER_W : COVER_SW, h = big ? COVER_H : COVER_SH;
	if (i && gcover[i - 1]) gfx_image(gcover[i - 1], COVER_W, COVER_H, x, y, w, h);
	else {
		gfx_alpha(0x80), gfx_rect(x, y, w, h, 0x11141A);
		gfx_scissor(x, y, x + w - 1, y + h - 1);
		gfx_alpha(0x10);
		for (int d = -h; d < w; d += 12) {
			float lx[2] = {(float)(x + d), (float)(x + d + h)}, ly[2] = {(float)(y + h), (float)y};
			gfx_stroke(lx, ly, 2, 1, NULD_FG);
		}
		gfx_alpha(0x80);
		gfx_scissor(0, 0, GFX_W - 1, GFX_H - 1);
		gfx_text(&gfx_font_mono, x + 14, y + 12, i ? gsrc[i - 1] : "PC", NULD_MUTED);
		char t[64];
		cut_to(t, snprintf(t, sizeof(t), "%s", entry(i)), w - 28);
		gfx_text(&gfx_font_ui, x + 14, y + h - 34, t, NULD_FG);
	}
	unsigned edge = big ? 0x3A4150 : 0x232833; // 1 px frame
	gfx_rect(x, y, w, 1, edge), gfx_rect(x, y + h - 1, w, 1, edge), gfx_rect(x, y, 1, h, edge), gfx_rect(x + w - 1, y, 1, h, edge);
}

static void cross_hint(int xr, int y, const char *label) // the ✕ button hint, right-aligned at xr: ring + acid cross
{
	int lw = gfx_text_width(&gfx_font_ui, label), cx = xr - lw - 12 - 13, cy = y + 13;
	float rx[34], ry[34];
	for (int i = 0; i < 34; i++) rx[i] = cx + 12 * cosf(i * 6.2832f / 32), ry[i] = cy + 12 * sinf(i * 6.2832f / 32);
	gfx_stroke(rx, ry, 34, 1.2f, NULD_MUTED);
	float ax[2] = {cx - 4.5f, cx + 4.5f}, ay[2] = {cy - 4.5f, cy + 4.5f}, bx[2] = {cx + 4.5f, cx - 4.5f};
	gfx_stroke(ax, ay, 2, 1.8f, NULD_ACID), gfx_stroke(bx, ay, 2, 1.8f, NULD_ACID);
	gfx_text(&gfx_font_ui, xr - lw, y + 3, label, NULD_FG);
}

static void draw_menu(float s, int sel, float t)
{
	char a[32];
	gfx_begin();
	splash_floor(t * 0.5f, NULD_FG, 0x12);
	// header: the mark + wordmark, the links on the right
	splash_mark(81, 56, 17);
	splash_vtext(&vfont_michroma, 112, 64, 15, 0.06f, "NULD", NULD_FG); // Michroma 20 px: 15 px caps
	static const char *const st[3] = {"USB", "RED", "PC"};
	int x = GFX_W - 64;
	gfx_tracking(2);
	for (int i = 2; i >= 0; i--) {
		x -= gfx_text_width(&gfx_font_mono, st[i]);
		gfx_text(&gfx_font_mono, x, 49, st[i], 0xA9B0BA);
		if (i < 2 || ((int)(t * 1.25f) & 1) == 0) gfx_rect(x - 15, 53, 7, 7, NULD_ACID); // PC pulses: the PC is live
		x -= 15 + 26;
	}
	gfx_tracking(0);
	// the entry: title, source tag, what X does
	gfx_text(&gfx_font_title, 64, 112, entry(sel), NULD_FG);
	const char *src = sel ? gsrc[sel - 1] : "PC";
	int tw = gfx_text_width(&gfx_font_mono, src);
	gfx_rect(64, 168, tw + 20, 1, NULD_ACID), gfx_rect(64, 194, tw + 20, 1, NULD_ACID);
	gfx_rect(64, 168, 1, 27, NULD_ACID), gfx_rect(64 + tw + 19, 168, 1, 27, NULD_ACID);
	gfx_text(&gfx_font_mono, 74, 173, src, NULD_ACID);
	gfx_tracking(1);
	gfx_text(&gfx_font_mono, 64 + tw + 36, 173, sel ? L("SE ABRE EN LA PC", "OPENS ON THE PC") : L("LA PANTALLA DE LA PC", "THE PC'S SCREEN"), 0xA9B0BA);
	gfx_tracking(0);
	// the row: selected card big at x 64, the rest small to its right, bottoms aligned, sliding with s
	int py = 232;
	for (int i = 0; i <= ngames; i++) {
		float d = i - s;
		int big = i == sel && fabsf(d) < 0.5f;
		int px = 64 + (d <= 0 ? (int)(d * (COVER_SW + 24)) : COVER_W + 32 + (int)((d - 1) * (COVER_SW + 24)));
		if (px > GFX_W || px + COVER_W < 0) continue;
		card(i, px, big ? py : py + COVER_H - COVER_SH, big);
		if (big) splash_hud(px - 12, py - 12, COVER_W + 24, COVER_H + 24, 30);
	}
	snprintf(a, sizeof(a), "%02d / %02d", sel + 1, ngames + 1);
	gfx_tracking(2);
	gfx_text(&gfx_font_mono, 64, 668, a, NULD_MUTED);
	gfx_tracking(0);
	cross_hint(GFX_W - 64, 660, sel ? L("Jugar", "Play") : L("Ver", "View"));
	splash_scan();
	gfx_end();
	gfx_flip();
}

static int menu(void) // 0 = the desktop, n = game n; -1 = leave
{
	static int sel;
	float s = sel;
	clock_t t0 = clock();
	edges(pad_buttons()); // the combo that brought us here is not a press
	for (;;) {
		unsigned b = pad_buttons(), p = edges(b);
		if (leave_combo(b)) return -1;
		if (p & PAD_RIGHT && sel < ngames) sel++;
		if (p & PAD_LEFT && sel > 0) sel--;
		if (p & PAD_CROSS) return sel;
		s += (sel - s) * 0.22f;
		if (fabsf(sel - s) < 0.01f) s = sel; // a d of +0.0001 would put the big card in the row's right half
		draw_menu(s, sel, (float)(clock() - t0) / CLOCKS_PER_SEC);
	}
}

int main(void)
{
	static ini cfg;
	char msg[256];
	// priorities as orbit-jellyfin: the HTTP reader (0x30) and the pad (0x40) block most of the time; lwIP's tcpip
	// thread (0x58) and netman (0x56-0x59) must preempt the decode + draw, which only this thread does
	ChangeThreadPriority(GetThreadId(), 0x60);
	if (!gfx_init()) printf("gfx_init: VRAM pool too small\n");
	screen("ORBIT Link", L("Cargando módulos...", "Loading modules..."));
	if (!iop_init()) { screen("ORBIT Link", L("Sin USB (mass0:)", "No USB (mass0:)")); SleepThread(); }
	audsrv_init();
	if (!ini_load(&cfg, "mass0:/nuld/config.ini")) ini_load(&cfg, "mass0:/orbit/config.ini"); // the launcher's; orbit/ until v1.1
	lang_en = !strcasecmp(ini_get(&cfg, "ui", "idioma", "es"), "en"); // the launcher's [ui] idioma
	snprintf(host, sizeof(host), "%s", ini_get(&cfg, "link", "servidor", ""));
	host_ip = net_aton(host);
	logf_("link: start, servidor %s\n", host);
	if (!host_ip) {
		screen("ORBIT Link", L("Falta [link] servidor = <IP de la PC> en mass0:/nuld/config.ini",
		                       "Missing [link] servidor = <PC IP> in mass0:/nuld/config.ini"));
		SleepThread();
	}
	screen("ORBIT Link", L("Conectando a la red...", "Connecting to the network..."));
	int r = net_up(ini_get(&cfg, "red", "ip", "dhcp"), ini_get(&cfg, "red", "mascara", "255.255.255.0"),
	               ini_get(&cfg, "red", "puerta", ""));
	if (r < 0) {
		snprintf(msg, sizeof(msg), L("Error de red %d (cable, DHCP o [red] de config.ini)", "Network error %d (cable, DHCP or [red] in config.ini)"), r);
		logf_("%s\n", msg);
		screen("ORBIT Link", msg);
		SleepThread();
	}
	pad_analog();
	pad_to.sin_family = AF_INET, pad_to.sin_port = htons(PAD_PORT), pad_to.sin_addr.s_addr = host_ip;
	extern void *_gp;
	ee_thread_t pt = {.func = pad_thread, .stack = pad_stack, .stack_size = sizeof(pad_stack), .gp_reg = &_gp, .initial_priority = 0x40};
	StartThread(CreateThread(&pt), NULL);
	for (;;) { // the PC may not be serving yet: try every 2 s. ponytail: the catalog once per start
		int st = load_catalog();
		if (st >= 0) break;
		snprintf(msg, sizeof(msg), L("Esperando a la PC en %s:%d (%d)\nEn la PC: python server/link.py\nL1+L2+R1+R2+START sale",
		         "Waiting for the PC at %s:%d (%d)\nOn the PC: python server/link.py\nL1+L2+R1+R2+START leaves"), host, VIDEO_PORT, st);
		screen("ORBIT Link", msg);
		for (int t = 0; t < 20 && !leave; t++) {
			if (leave_combo(pad_buttons())) leave = 1;
			usleep(100000);
		}
		if (leave) break;
	}
	if (!leave) screen("ORBIT Link", L("Cargando portadas...", "Loading covers...")), load_covers();
	splash_stop(); // the menu and the stream draw for themselves
	// [link] directo = 1: the desktop's stream at once, the menu only after it ends (a console that is just a screen)
	int direct = atoi(ini_get(&cfg, "link", "directo", "0"));
	while (!leave) {
		int g = direct ? 0 : menu();
		direct = 0;
		if (g < 0) break;
		if (g) {
			snprintf(msg, sizeof(msg), "/launch/%d", g - 1);
			get_body(msg, msg, 0);
			logf_("link: launched %s\n", gname[g - 1]);
		}
		play();
	}
	logf_("link: leaving\n");
	gfx_shutdown();
	return 0; // Exit: back to the OSD, and FMCB's autoboot to ORBIT
}
