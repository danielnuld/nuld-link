// ORBIT Link (phase 19, go / no-go spike): the PS2 as a streaming client of the PC. server/link.py has ffmpeg capture
// the PC's screen and encode MPEG-2 with I and P pictures only, served as an MPEG program stream over HTTP; the PS2
// decodes it on the IPU and shows every picture as soon as it is decoded; its pad goes back over UDP, where link.py
// turns it into a virtual Xbox 360 controller (ViGEm). Latency is the gate: docs/phase19-results.md.
// - mass0:/orbit/config.ini [link] servidor = <PC IP>, aspecto = 16:9 (the PC screen's shape; the stream is
//   anamorphic), atraso_kb; [red] and [ui] idioma as the launcher
// - video: http://<servidor>:8099/ -> http.c -> mpegps.c -> ring -> IPU through ps2sdk libmpeg (RGBA32 pictures in
//   16x16 macroblocks) -> gfx_mb32 (DMA to the GS, bilinear, fitted into 1280x720). orbit-jellyfin's jfplay.c without
//   its prebuffer, audio and audio clock: a picture is drawn when it is decoded; with more than atraso_kb waiting in
//   the ring it is decoded (the next P pictures need it) and not drawn
// - audio: the PC's sound as MP2 over UDP 8097, a frame per datagram (in the program stream, ffmpeg's muxer held the
//   video for the audio: bursts, +150 ms) -> ring -> libmad -> audsrv (jfplay's audio thread without its
//   clock). audsrv reads its ~106 ms ring at 48 kHz whether it holds data or not (overtaken, it replays stale
//   samples), so it is kept full and sets the pace; over AUDIO_MS of MP2 waiting here, one cut to the newest
// - pad: 16 bytes at ~60 Hz to UDP <servidor>:8098 (buttons and both sticks), from its own thread
// - menu first: the PC's desktop, then the games link.py finds installed (Epic, Steam) with their covers, in a row as
//   jfplay's browse screen; X launches the game on the PC (/launch/<n>) and starts the stream
// - L1+L2+R1+R2 + SELECT toggles the gate numbers on screen; L1+L2+R1+R2 + TRIANGLE back to the menu;
//   L1+L2+R1+R2 + START leaves (Exit: the OSD, and FMCB's autoboot brings ORBIT back)
// - every 5 s a line goes to mass0:/link.txt (and the EE serial port): pictures decoded / shown / skipped, decode ms,
//   network KB/s, ring KB, pad packets/s
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <malloc.h>
#include <time.h>
#include <unistd.h>
#include <kernel.h>
#include <tamtypes.h>
#include <dma.h>
#include <libpad.h>
#include <libmpeg.h>
#include <audsrv.h>
#include <mad.h>
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
#include "ui_data.h"

#define TEXT 0xEEF3FF
#define TEXT2 0xA9B6D3
#define LABEL 0x8FA0C4
#define ICE 0x7FE7FF
#define NAVY 0x0D1736
#define NIGHT 0x04060E
#define CHROME_T 0xF4F8FF
#define CHROME_B 0xAAB7D2
#define IRIS 0xC08BFF
#define INK 0x0A1026
#define LOG "mass0:/link.txt"
#define VIDEO_PORT 8099
#define PAD_PORT 8098
#define AUDIO_PORT 8097
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

static ring vring, aring;
static volatile int pad_live; // the pad goes to the PC only while streaming: not the menu's presses
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
static void on_audio(void *c, const u8 *p, int n, long long t) // none in the program stream: the audio comes by UDP
{
	(void)c;
	(void)p;
	(void)n;
	(void)t;
}

// ---- audio in: MP2 datagrams from link.py -> aring, while a stream plays (a_live). The pad thread sends "OLA1" from
// this socket once a second: link.py answers to where it comes from (PCSX2's Sockets network, NATs) ----
static volatile int a_live, a_sock = -1;
static struct sockaddr_in audio_to;
static u8 arx_stack[0x4000] __attribute__((aligned(16)));
static void audio_rx(void *arg) // for the whole run: blocks in recvfrom, nothing to stop
{
	(void)arg;
	static u8 b[2048];
	int s = socket(AF_INET, SOCK_DGRAM, 0);
	struct sockaddr_in at = {0};
	at.sin_family = AF_INET, at.sin_port = htons(AUDIO_PORT), at.sin_addr.s_addr = htonl(INADDR_ANY);
	if (s < 0 || bind(s, (struct sockaddr *)&at, sizeof(at)) < 0) { logf_("link: no audio socket\n"); ExitThread(); }
	a_sock = s;
	for (;;) {
		int n = recvfrom(s, b, sizeof(b), 0, NULL, NULL);
		if (n > 0 && a_live && ring_used(&aring) + n <= aring.size) ring_put(&aring, b, n, &stop); // full: dropped
	}
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
		if (pad_live && s >= 0 && padRead(0, 0, &b)) {
			k.buttons = 0xffff ^ b.btns; // pressed = 1, libpad's PAD_* bits
			k.lx = b.ljoy_h, k.ly = b.ljoy_v, k.rx = b.rjoy_h, k.ry = b.rjoy_v;
			if (sendto(s, &k, sizeof(k), 0, (struct sockaddr *)&pad_to, sizeof(pad_to)) == (int)sizeof(k)) pad_sent++;
			if (a_sock >= 0 && k.seq % 60 == 0) sendto(a_sock, "OLA1", 4, 0, (struct sockaddr *)&audio_to, sizeof(audio_to));
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
	static u8 abuf[64 << 10] __attribute__((aligned(64))); // ~2.7 s of MP2 at 192 kbit/s
	char osd[160];
	vring = (ring){vbuf, sizeof(vbuf), 0, 0}, aring = (ring){abuf, sizeof(abuf), 0, 0};
	a_on = a_done = a_free_ms = a_dropped = 0;
	a_live = 1;
	net_eof = stop = net_kb = net_total_kb = pics = main_at = 0, pending = 0, wait_clk = 0, cur_pts = 0;
	ps_init(&dmx, on_video, on_audio, NULL);
	extern void *_gp;
	ee_thread_t nt = {.func = net_thread, .stack = net_stack, .stack_size = sizeof(net_stack), .gp_reg = &_gp, .initial_priority = 0x30};
	ee_thread_t wt = {.func = watchdog, .stack = dog_stack, .stack_size = sizeof(dog_stack), .gp_reg = &_gp, .initial_priority = 0x20};
	ee_thread_t at = {.func = audio_thread, .stack = audio_stack, .stack_size = sizeof(audio_stack), .gp_reg = &_gp, .initial_priority = 0x70}; // under the main thread (0x60): libmad runs while it waits for the vsync, never in the middle of a picture
	int ntid = CreateThread(&nt), wtid = CreateThread(&wt), atid = CreateThread(&at);
	StartThread(ntid, NULL), StartThread(wtid, NULL), StartThread(atid, NULL);
	ipu_stop();
	dma_channel_initialize(DMA_CHANNEL_toIPU, NULL, 0);
	MPEG_Initialize(video_data, NULL, video_init, NULL, &cur_pts);
	pad_live = 1;
	int decoded = 0, shown = 0, skipped = 0, win_dec = 0, win_shown = 0, win_skip = 0, ms_avg = 0;
	clock_t last_report = clock(), dec_clk = 0, dec_max = 0;
	for (;;) {
		unsigned b = pad_buttons(), p = edges(b);
		if (leave_combo(b)) { leave = 1; break; }
		if ((b & COMBO) == COMBO && (p & PAD_SELECT)) show_osd ^= 1;
		if ((b & COMBO) == COMBO && (p & PAD_TRIANGLE)) break; // back to the menu
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
			logf_("link: %.1f s: %.1f decoded/s, %.1f shown/s, skipped %d, decode %d ms avg %d max, net %d KB/s, ring %d KB, pad %d/s, audio %d ms free in audsrv, %d cuts\n",
			      sec, win_dec / sec, win_shown / sec, win_skip, ms_avg, (int)((long long)dec_max * 1000 / CLOCKS_PER_SEC),
			      net_kb, ring_used(&vring) >> 10, pad_rate, a_free_ms, a_dropped);
			win_dec = win_shown = win_skip = 0, dec_clk = dec_max = 0, last_report = clock();
		}
	}
	stop = 1, pad_live = 0, a_live = 0;
	MPEG_Destroy();
	ipu_stop();
	// the network thread leaves by itself (it checks stop between reads; a full ring returns at once): terminated
	// inside lwIP's recv, the next connection could hang on lwIP's state (orbit-jellyfin)
	for (int t = 0; t < 300 && !net_eof; t++) usleep(10000);
	if (!net_eof) TerminateThread(ntid), logf_("link: network thread did not stop\n");
	http_close(&hs);
	// the audio thread leaves by itself too: terminated inside an audsrv call, the next one hung (jfplay)
	for (int t = 0; t < 200 && !a_done; t++) usleep(10000);
	if (!a_done) TerminateThread(atid), logf_("link: audio thread did not stop\n");
	audsrv_stop_audio();
	TerminateThread(wtid);
	DeleteThread(ntid), DeleteThread(wtid), DeleteThread(atid);
	logf_("link: stopped: %d decoded, %d shown, %d skipped, demux skipped %d bytes\n", decoded, shown, skipped, dmx.skipped);
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
	static http_stream h;
	int st = http_open(&h, host, VIDEO_PORT, "GET", path, "", NULL), n = 0, k;
	if (st == 200)
		while (n < max && (k = http_read(&h, (char *)dst + n, max - n)) > 0) n += k;
	http_close(&h);
	return st == 200 ? n : st < 0 ? st : -1;
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

// a button hint pill, right-aligned at x (jfplay.c / launcher.c hint(); action_icon -1 = none). Returns the next x
static int hint(int x, int y, int key_icon, const char *key_text, int action_icon, const char *label)
{
	int lw = gfx_text_width(&gfx_font_ui, label);
	int kw = key_icon >= 0 ? 30 : gfx_text_width(&gfx_font_mono, key_text) + 20, aw = action_icon >= 0 ? 28 : 0;
	int w = 8 + kw + 10 + aw + lw + 18, x0 = x - w;
	gfx_alpha(0x2D);
	gfx_rrect(x0, y, w, 44, 22, TEXT2, TEXT2);
	gfx_alpha(0x80);
	gfx_rrect(x0 + 1, y + 1, w - 2, 42, 21, 0x0B1430, 0x0A1128);
	int kx = x0 + 8;
	if (key_icon >= 0) {
		gfx_rrect(kx, y + 7, 30, 30, 15, CHROME_T, CHROME_B);
		gfx_icon(key_icon, kx + 8, y + 15, IRIS);
	} else {
		gfx_rrect(kx, y + 9, kw, 26, 13, CHROME_T, CHROME_B);
		gfx_text(&gfx_font_mono, kx + 10, y + 12, key_text, INK);
	}
	if (aw) gfx_icon(action_icon, kx + kw + 10, y + 13, ICE);
	gfx_text(&gfx_font_ui, kx + kw + 10 + aw, y + 11, label, TEXT);
	return x0 - 12;
}

static const char *entry(int i) { return i ? gname[i - 1] : L("Escritorio", "Desktop"); }

static void card(int i, int x, int y, int big) // the cover, or a drawn card with the name
{
	int w = big ? COVER_W : COVER_SW, h = big ? COVER_H : COVER_SH;
	if (i && gcover[i - 1]) { gfx_image(gcover[i - 1], COVER_W, COVER_H, x, y, w, h); return; }
	gfx_rrect(x, y, w, h, 10, 0x1B2850, 0x0D1530);
	if (!i) gfx_icon(UI_NET_18, x + w / 2 - 9, y + h / 2 - 30, ICE);
	char t[64];
	int n = snprintf(t, sizeof(t), "%s", entry(i));
	while (n > 0 && gfx_text_width(&gfx_font_ui, t) > w - 28) { // ponytail: cut to the card, not wrapped
		t[--n] = 0;
		while (n > 0 && (t[n - 1] & 0xC0) == 0x80) t[--n] = 0; // whole UTF-8 letters
		if (n > 0 && (unsigned char)t[n - 1] >= 0xC0) t[--n] = 0;
	}
	gfx_text(&gfx_font_ui, x + 14, y + h - 50, t, TEXT2);
}

static void draw_menu(float s, int sel)
{
	char a[32];
	gfx_begin();
	gfx_alpha(0x80);
	gfx_dither(1);
	gfx_grad(0, 0, GFX_W, GFX_H, NAVY, NIGHT, 1);
	gfx_dither(0);
	gfx_orb(64, 40, 56);
	gfx_tracking(2);
	gfx_text(&gfx_font_mono, 140, 52, "ORBIT LINK", ICE);
	gfx_rect(140, 74, gfx_text_width(&gfx_font_mono, "ORBIT LINK"), 2, ICE);
	gfx_tracking(0);
	gfx_text(&gfx_font_title, 64, 98, entry(sel), TEXT);
	const char *src = sel ? gsrc[sel - 1] : "PC";
	int cw = gfx_text_width(&gfx_font_ui, src) + 24;
	gfx_rrect(64, 150, cw, 28, 14, CHROME_T, CHROME_B);
	gfx_text(&gfx_font_ui, 76, 153, src, INK);
	int py = 232; // the row: selected card big at x 64, the rest small to its right, sliding with s
	for (int i = 0; i <= ngames; i++) {
		float d = i - s;
		int big = i == sel && fabsf(d) < 0.5f;
		int px = 64 + (d <= 0 ? (int)(d * (COVER_SW + 24)) : COVER_W + 32 + (int)((d - 1) * (COVER_SW + 24)));
		if (px > GFX_W || px + COVER_W < 0) continue;
		card(i, px, big ? py : py + (COVER_H - COVER_SH) / 2, big);
		if (big) gfx_rrect(px - 4, py - 4, COVER_W + 8, 3, 1, ICE, ICE);
	}
	snprintf(a, sizeof(a), "%02d / %02d", sel + 1, ngames + 1);
	gfx_text(&gfx_font_mono, 64, 664, a, LABEL);
	hint(GFX_W - 64, 652, UI_CROSS_14, NULL, UI_PLAY_18, sel ? L("Jugar", "Play") : L("Ver", "View"));
	gfx_end();
	gfx_flip();
}

static int menu(void) // 0 = the desktop, n = game n; -1 = leave
{
	static int sel;
	float s = sel;
	edges(pad_buttons()); // the combo that brought us here is not a press
	for (;;) {
		unsigned b = pad_buttons(), p = edges(b);
		if (leave_combo(b)) return -1;
		if (p & PAD_RIGHT && sel < ngames) sel++;
		if (p & PAD_LEFT && sel > 0) sel--;
		if (p & PAD_CROSS) return sel;
		s += (sel - s) * 0.22f;
		if (fabsf(sel - s) < 0.01f) s = sel; // a d of +0.0001 would put the big card in the row's right half
		draw_menu(s, sel);
	}
}

int main(void)
{
	static ini cfg;
	char msg[256], ps[8];
	// priorities as orbit-jellyfin: the HTTP reader (0x30) and the pad (0x40) block most of the time; lwIP's tcpip
	// thread (0x58) and netman (0x56-0x59) must preempt the decode + draw, which only this thread does
	ChangeThreadPriority(GetThreadId(), 0x60);
	if (!gfx_init()) printf("gfx_init: VRAM pool too small\n");
	screen("ORBIT Link", L("Cargando módulos...", "Loading modules..."));
	if (!iop_init()) { screen("ORBIT Link", L("Sin USB (mass0:)", "No USB (mass0:)")); SleepThread(); }
	audsrv_init();
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
	audio_to = pad_to, audio_to.sin_port = htons(AUDIO_PORT); // the audio hellos: same host, port 8097
	extern void *_gp;
	ee_thread_t pt = {.func = pad_thread, .stack = pad_stack, .stack_size = sizeof(pad_stack), .gp_reg = &_gp, .initial_priority = 0x40};
	StartThread(CreateThread(&pt), NULL);
	ee_thread_t rt = {.func = audio_rx, .stack = arx_stack, .stack_size = sizeof(arx_stack), .gp_reg = &_gp, .initial_priority = 0x30};
	StartThread(CreateThread(&rt), NULL);
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
	while (!leave) {
		int g = menu();
		if (g < 0) break;
		if (g) {
			snprintf(msg, sizeof(msg), "/launch/%d", g - 1);
			get_body(msg, msg, 0);
			logf_("link: launched %s\n", gname[g - 1]);
		}
		if (http_open(&hs, host, VIDEO_PORT, "GET", "/", "", NULL) != 200) { http_close(&hs); continue; }
		logf_("link: connected to %s:%d%s\n", host, VIDEO_PORT, hs.chunked ? " (chunked)" : "");
		play();
	}
	logf_("link: leaving\n");
	gfx_shutdown();
	return 0; // Exit: back to the OSD, and FMCB's autoboot to ORBIT
}
