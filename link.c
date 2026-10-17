// ORBIT Link (phase 19, go / no-go spike): the PS2 as a streaming client of the PC. server/link.py has ffmpeg capture
// the PC's screen and encode MPEG-2 with I and P pictures only, served as an MPEG program stream over HTTP; the PS2
// decodes it on the IPU and shows every picture as soon as it is decoded; its pad goes back over UDP, where link.py
// turns it into a virtual Xbox 360 controller (ViGEm). Latency is the gate: docs/phase19-results.md.
// - mass0:/orbit/config.ini [link] servidor = <PC IP>, aspecto = 16:9 (the PC screen's shape; the stream is
//   anamorphic), atraso_kb; [red] and [ui] idioma as the launcher
// - video: http://<servidor>:8099/ -> http.c -> mpegps.c -> ring -> IPU through ps2sdk libmpeg (RGBA32 pictures in
//   16x16 macroblocks) -> gfx_mb32 (DMA to the GS, bilinear, fitted into 1280x720). orbit-jellyfin's jfplay.c without
//   its prebuffer, audio and audio clock: a picture is drawn when it is decoded; with more than atraso_kb waiting in
//   the ring it is decoded (the next P pictures need it) and not drawn. No sound yet
// - pad: 16 bytes at ~60 Hz to UDP <servidor>:8098 (buttons and both sticks), from its own thread
// - L1+L2+R1+R2 + SELECT toggles the gate numbers on screen; L1+L2+R1+R2 + START leaves (Exit: the OSD, and FMCB's
//   autoboot brings ORBIT back)
// - every 5 s a line goes to mass0:/link.txt (and the EE serial port): pictures decoded / shown / skipped, decode ms,
//   network KB/s, ring KB, pad packets/s
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <time.h>
#include <unistd.h>
#include <kernel.h>
#include <tamtypes.h>
#include <dma.h>
#include <libpad.h>
#include <libmpeg.h>
#include <sio.h>
#include <netdb.h>
#include <ps2ip.h>
#include "lang.h"
#include "gfx.h"
#include "iop.h"
#include "ini.h"
#include "net.h"
#include "http.h"
#include "mpegps.h"

#define TEXT 0xEEF3FF
#define TEXT2 0xA9B6D3
#define LOG "mass0:/link.txt"
#define VIDEO_PORT 8099
#define PAD_PORT 8098
#define COMBO (PAD_L1 | PAD_L2 | PAD_R1 | PAD_R2)
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

// ---- ring: one writer (network thread), one reader (libmpeg); size a power of two ----
typedef struct { u8 *b; int size; volatile unsigned rd, wr; } ring; // free-running counters: differences wrap safely
static int ring_used(ring *r) { return (int)(r->wr - r->rd); }
static int ring_put(ring *r, const u8 *p, int n, volatile int *stop) // blocks while full
{
	while (n > 0 && !*stop) {
		int room = r->size - ring_used(r);
		if (!room) { usleep(2000); continue; }
		int k = n < room ? n : room, o = r->wr & (r->size - 1), first = r->size - o < k ? r->size - o : k;
		memcpy(r->b + o, p, first), memcpy(r->b, p + first, k - first);
		__asm__ volatile("" ::: "memory");
		r->wr += k, p += k, n -= k;
	}
	return !*stop;
}

static ring vring;
static volatile int net_eof, stop, leave, net_kb, net_total_kb, pics, pad_sent, show_osd, main_at; // main_at: 1 decode, 2 draw
static int behind_kb = 48, pad_rate; // [link] atraso_kb: more than this waiting in the ring = behind, pictures are not drawn

static void on_video(void *c, const u8 *p, int n, long long t)
{
	// drop sequence_end_code (00 00 01 B7): libmpeg hung on it instead of returning 0 (orbit-jellyfin, PCSX2)
	for (int i = 0; i + 4 <= n; i++)
		if (!p[i] && !p[i + 1] && p[i + 2] == 1 && p[i + 3] == 0xB7) {
			if (i) on_video(c, p, i, t);
			p += i + 4, n -= i + 4, i = -1, t = -1;
		}
	(void)c;
	(void)t; // no PTS: every picture is shown when it is decoded
	ring_put(&vring, p, n, &stop);
}
static void on_audio(void *c, const u8 *p, int n, long long t) // no sound yet (link.py sends none: -an)
{
	(void)c;
	(void)p;
	(void)n;
	(void)t;
}

static http_stream hs;
static ps_demux dmx;
static u8 net_stack[0x8000] __attribute__((aligned(16)));
static void net_thread(void *arg) // HTTP -> demuxer -> ring
{
	(void)arg;
	static u8 buf[32 << 10];
	int n;
	clock_t c0 = clock();
	long long total = 0;
	while (!stop && (n = http_read(&hs, buf, sizeof(buf))) > 0) {
		if (ps_feed(&dmx, buf, n) < 0) { logf_("link: not a program stream\n"); break; }
		total += n;
		net_total_kb = (int)(total >> 10);
		int ms = (int)((long long)(clock() - c0) * 1000 / CLOCKS_PER_SEC);
		if (ms > 0) net_kb = (int)(total * 1000 / 1024 / ms);
	}
	net_eof = 1;
	ExitThread();
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
		if (s >= 0 && padRead(0, 0, &b)) {
			k.buttons = 0xffff ^ b.btns; // pressed = 1, libpad's PAD_* bits
			k.lx = b.ljoy_h, k.ly = b.ljoy_v, k.rx = b.rjoy_h, k.ry = b.rjoy_v;
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

// ---- video: ring -> IPU in 2 KB DMA blocks (libmpeg pulls them) ----
static s64 cur_pts;
static int pending;                 // bytes of the block the IPU DMA may still be reading
static volatile long long wait_clk; // clock ticks spent waiting for data inside MPEG_Picture: not decode time
static int video_data(void *u)
{
	(void)u;
	dma_channel_wait(DMA_CHANNEL_toIPU, 0);
	vring.rd += pending, pending = 0;
	clock_t t0 = clock();
	while (ring_used(&vring) < 2048 && !stop && !net_eof) usleep(1000);
	wait_clk += clock() - t0;
	if (stop || !ring_used(&vring)) return 0; // at the stream's end what is left goes in, whatever its size
	int o = vring.rd & (vring.size - 1), n = ring_used(&vring) < 2048 ? (ring_used(&vring) + 15) & ~15 : 2048;
	SyncDCache(vring.b + o, vring.b + o + n);
	dma_channel_send_normal(DMA_CHANNEL_toIPU, vring.b + o, n >> 4, 0, 0);
	pending = n;
	return 1;
}

// libmpeg's _MPEG_Initialize resets the IPU and zeroes the D3 / D4 counts but leaves the channels started: after a
// stream cut mid-picture, toIPU still had a block in flight and, on the console, the next stream's first
// dma_channel_wait never returned (orbit-jellyfin). Stop both channels with the DMAC held (libmpeg's _ipu_suspend
// sequence), then reset the IPU
static void ipu_stop(void)
{
	volatile u32 *enabler = (u32 *)0x1000F520, *enablew = (u32 *)0x1000F590;
	volatile u32 *d3_chcr = (u32 *)0x1000B000, *d4_chcr = (u32 *)0x1000B400, *d4_qwc = (u32 *)0x1000B420;
	volatile u32 *ipu_ctrl = (u32 *)0x10002010;
	DI();
	u32 e = *enabler;
	*enablew = e | 0x10000;
	__asm__ volatile("sync.l");
	*d4_chcr &= ~0x100, *d3_chcr &= ~0x100, *d4_qwc = 0;
	*enablew = e;
	EI();
	*ipu_ctrl = 1u << 30;
	while (*ipu_ctrl & (1u << 31));
	pending = 0;
}

static MPEGSequenceInfo *seq;
static u8 *pic;               // decoded picture, RGBA32 in macroblocks
static void *video_init(void *u, MPEGSequenceInfo *si)
{
	(void)u;
	static int pic_size;
	int mbw = (si->m_Width + 15) >> 4, mbh = (si->m_Height + 15) >> 4;
	seq = si;
	if (pic_size < mbw * mbh * 1024) free(pic), pic = memalign(64, pic_size = mbw * mbh * 1024); // ponytail: no NULL check, libmpeg has none either
	logf_("link: sequence %dx%d, %d ms per picture, profile %d level %d\n", si->m_Width, si->m_Height,
	      si->m_MSPerFrame, si->m_Profile, si->m_Level);
	return pic;
}

// ---- screen ----
static int asp_w = 16, asp_h = 9; // [link] aspecto: the PC screen's shape; the stream is anamorphic (512x448)
static void draw_picture(int w, int h, const char *osd)
{
	gfx_begin();
	gfx_alpha(0x80);
	gfx_rect(0, 0, GFX_W, GFX_H, 0);
	int dh = GFX_H, dw = GFX_H * asp_w / asp_h;
	if (dw > GFX_W) dw = GFX_W, dh = GFX_W * asp_h / asp_w;
	gfx_mb32(pic, w, h, (GFX_W - dw) / 2, (GFX_H - dh) / 2, dw, dh); // straight from the IPU's output by DMA
	if (osd) { // the gate numbers, at the top
		gfx_alpha(0x50);
		gfx_rect(40, 96, 1200, 40, 0);
		gfx_alpha(0x80);
		gfx_text(&gfx_font_mono, 56, 104, osd, TEXT);
	}
	gfx_end();
	gfx_flip();
}

static void screen(const char *title, const char *body)
{
	gfx_begin();
	gfx_alpha(0x80);
	gfx_rect(0, 0, GFX_W, GFX_H, 0x04060E);
	gfx_text(&gfx_font_title, 64, 48, title, TEXT);
	gfx_text(&gfx_font_mono, 64, 120, body, TEXT2);
	gfx_end();
	gfx_flip();
}

static unsigned edges(unsigned b) // the buttons that went down since the last call
{
	static unsigned prev;
	unsigned p = b & ~prev;
	prev = b;
	return p;
}
static int leave_combo(unsigned b) { return (b & (COMBO | PAD_START)) == (COMBO | PAD_START); }

static u8 dog_stack[0x4000] __attribute__((aligned(16)));
static void watchdog(void *arg) // every 3 s while playing: a hang leaves a trace in the log
{
	(void)arg;
	int last = -1;
	while (!stop) {
		sleep(3);
		if (pics == last)
			logf_("link: stall: main at %d, ring %d KB, net eof %d, received %d KB\n", main_at, ring_used(&vring) >> 10, net_eof, net_total_kb);
		last = pics;
	}
	ExitThread();
}

static void play(void) // one stream: until it ends, or the user leaves
{
	static u8 vbuf[1 << 20] __attribute__((aligned(64))); // ~1.4 s at 6 Mbit/s; full, TCP holds the PC back
	char osd[160];
	vring = (ring){vbuf, sizeof(vbuf), 0, 0};
	net_eof = stop = net_kb = net_total_kb = pics = main_at = 0, pending = 0, wait_clk = 0, cur_pts = 0;
	ps_init(&dmx, on_video, on_audio, NULL);
	extern void *_gp;
	ee_thread_t nt = {.func = net_thread, .stack = net_stack, .stack_size = sizeof(net_stack), .gp_reg = &_gp, .initial_priority = 0x30};
	ee_thread_t wt = {.func = watchdog, .stack = dog_stack, .stack_size = sizeof(dog_stack), .gp_reg = &_gp, .initial_priority = 0x20};
	int ntid = CreateThread(&nt), wtid = CreateThread(&wt);
	StartThread(ntid, NULL), StartThread(wtid, NULL);
	ipu_stop();
	dma_channel_initialize(DMA_CHANNEL_toIPU, NULL, 0);
	MPEG_Initialize(video_data, NULL, video_init, NULL, &cur_pts);
	int decoded = 0, shown = 0, skipped = 0, win_dec = 0, win_shown = 0, win_skip = 0, ms_avg = 0;
	clock_t last_report = clock(), dec_clk = 0, dec_max = 0;
	for (;;) {
		unsigned b = pad_buttons(), p = edges(b);
		if (leave_combo(b)) { leave = 1; break; }
		if ((b & COMBO) == COMBO && (p & PAD_SELECT)) show_osd ^= 1;
		s64 t;
		main_at = 1;
		clock_t c0 = clock();
		long long w0 = wait_clk;
		if (!MPEG_Picture(pic, &t)) { logf_("link: end of video (eof %d)\n", seq ? seq->m_fEOF : -1); break; }
		clock_t d = clock() - c0 - (clock_t)(wait_clk - w0); // decode time: the waits for data taken out
		dec_clk += d;
		if (d > dec_max) dec_max = d;
		decoded++, win_dec++, pics++;
		// behind (the PC is more than atraso_kb ahead): decoded, since the next P pictures need it, not drawn (a draw
		// costs the picture upload and a vsync). ponytail: no jump to the next I picture; if the IPU cannot keep up the
		// ring fills and TCP holds the PC back, where the latency then grows
		if (ring_used(&vring) > behind_kb << 10) { skipped++, win_skip++; continue; }
		main_at = 2;
		if (win_dec) ms_avg = (int)((long long)dec_clk * 1000 / CLOCKS_PER_SEC / win_dec);
		snprintf(osd, sizeof(osd), L("dec %d  vis %d  salt %d  %d ms  red %d KB/s  buf %d KB  pad %d/s",
		         "dec %d  shown %d  skip %d  %d ms  net %d KB/s  buf %d KB  pad %d/s"),
		         decoded, shown, skipped, ms_avg, net_kb, ring_used(&vring) >> 10, pad_rate);
		draw_picture(seq->m_Width, seq->m_Height, show_osd ? osd : NULL);
		shown++, win_shown++;
		if (clock() - last_report > 5 * CLOCKS_PER_SEC) { // gate numbers, every 5 s
			float sec = (float)(clock() - last_report) / CLOCKS_PER_SEC;
			pad_rate = (int)(pad_sent / sec), pad_sent = 0;
			logf_("link: %.1f s: %.1f decoded/s, %.1f shown/s, skipped %d, decode %d ms avg %d max, net %d KB/s, ring %d KB, pad %d/s\n",
			      sec, win_dec / sec, win_shown / sec, win_skip, ms_avg, (int)((long long)dec_max * 1000 / CLOCKS_PER_SEC),
			      net_kb, ring_used(&vring) >> 10, pad_rate);
			win_dec = win_shown = win_skip = 0, dec_clk = dec_max = 0, last_report = clock();
		}
	}
	stop = 1;
	MPEG_Destroy();
	ipu_stop();
	// the network thread leaves by itself (it checks stop between reads; a full ring returns at once): terminated
	// inside lwIP's recv, the next connection could hang on lwIP's state (orbit-jellyfin)
	for (int t = 0; t < 300 && !net_eof; t++) usleep(10000);
	if (!net_eof) TerminateThread(ntid), logf_("link: network thread did not stop\n");
	http_close(&hs);
	TerminateThread(wtid);
	DeleteThread(ntid), DeleteThread(wtid);
	logf_("link: stopped: %d decoded, %d shown, %d skipped, demux skipped %d bytes\n", decoded, shown, skipped, dmx.skipped);
}

int main(void)
{
	static ini cfg;
	char msg[256], host[64], ps[8];
	// priorities as orbit-jellyfin: the HTTP reader (0x30) and the pad (0x40) block most of the time; lwIP's tcpip
	// thread (0x58) and netman (0x56-0x59) must preempt the decode + draw, which only this thread does
	ChangeThreadPriority(GetThreadId(), 0x60);
	if (!gfx_init()) printf("gfx_init: VRAM pool too small\n");
	screen("ORBIT Link", L("Cargando módulos...", "Loading modules..."));
	if (!iop_init()) { screen("ORBIT Link", L("Sin USB (mass0:)", "No USB (mass0:)")); SleepThread(); }
	ini_load(&cfg, "mass0:/orbit/config.ini");
	lang_en = !strcasecmp(ini_get(&cfg, "ui", "idioma", "es"), "en"); // the launcher's [ui] idioma
	snprintf(host, sizeof(host), "%s", ini_get(&cfg, "link", "servidor", ""));
	if (sscanf(ini_get(&cfg, "link", "aspecto", "16:9"), "%d:%d", &asp_w, &asp_h) != 2 || asp_w <= 0 || asp_h <= 0) asp_w = 16, asp_h = 9;
	behind_kb = atoi(ini_get(&cfg, "link", "atraso_kb", "48"));
	if (behind_kb < 4) behind_kb = 4;
	logf_("link: start, servidor %s, aspecto %d:%d, atraso %d KB\n", host, asp_w, asp_h, behind_kb);
	if (!*host) {
		screen("ORBIT Link", L("Falta [link] servidor = <IP de la PC> en mass0:/orbit/config.ini",
		                       "Missing [link] servidor = <PC IP> in mass0:/orbit/config.ini"));
		SleepThread();
	}
	screen("ORBIT Link", L("Conectando a la red...", "Connecting to the network..."));
	int r = net_up(ini_get(&cfg, "red", "ip", "dhcp"), ini_get(&cfg, "red", "mascara", "255.255.255.0"),
	               ini_get(&cfg, "red", "puerta", ""), ini_get(&cfg, "red", "dns", ""));
	if (r < 0) {
		snprintf(msg, sizeof(msg), L("Error de red %d (cable, DHCP o [red] de config.ini)", "Network error %d (cable, DHCP or [red] in config.ini)"), r);
		logf_("%s\n", msg);
		screen("ORBIT Link", msg);
		SleepThread();
	}
	logf_("link: mtu %d\n", net_mtu);
	pad_analog();
	struct addrinfo hints = {0}, *ai; // the pad's UDP target: the same host, port 8098
	hints.ai_family = AF_INET, hints.ai_socktype = SOCK_DGRAM;
	snprintf(ps, sizeof(ps), "%d", PAD_PORT);
	if (getaddrinfo(host, ps, &hints, &ai) == 0) memcpy(&pad_to, ai->ai_addr, sizeof(pad_to)), freeaddrinfo(ai);
	else logf_("link: cannot resolve %s for the pad\n", host);
	extern void *_gp;
	ee_thread_t pt = {.func = pad_thread, .stack = pad_stack, .stack_size = sizeof(pad_stack), .gp_reg = &_gp, .initial_priority = 0x40};
	StartThread(CreateThread(&pt), NULL);
	while (!leave) {
		for (;;) { // the PC may not be serving yet: try every 2 s
			int st = http_open(&hs, host, VIDEO_PORT, "GET", "/", "", NULL);
			if (st == 200) break;
			http_close(&hs);
			snprintf(msg, sizeof(msg), L("Esperando a la PC en %s:%d (%d)\nEn la PC: python server/link.py\nL1+L2+R1+R2+START sale",
			         "Waiting for the PC at %s:%d (%d)\nOn the PC: python server/link.py\nL1+L2+R1+R2+START leaves"), host, VIDEO_PORT, st);
			screen("ORBIT Link", msg);
			for (int t = 0; t < 20 && !leave; t++) {
				if (leave_combo(pad_buttons())) leave = 1;
				usleep(100000);
			}
			if (leave) break;
		}
		if (leave) break;
		logf_("link: connected to %s:%d%s\n", host, VIDEO_PORT, hs.chunked ? " (chunked)" : "");
		play();
	}
	logf_("link: leaving\n");
	gfx_shutdown();
	return 0; // Exit: back to the OSD, and FMCB's autoboot to ORBIT
}
