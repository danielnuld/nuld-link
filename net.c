// Network bring-up (DHCP or a fixed IP), TCP/IP on the IOP, and the HTTP answer's header parser.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <strings.h>
#include "net.h"

int http_parse(const char *buf, int n, int *status, int *body, int *clen)
{
	int end = -1;
	for (int i = 0; i + 3 < n; i++)
		if (!memcmp(buf + i, "\r\n\r\n", 4)) { end = i; break; }
	if (end < 0 || n < 12 || memcmp(buf, "HTTP/1.", 7) || !isdigit((unsigned char)buf[9])) return 0;
	*status = atoi(buf + 9);
	*body = end + 4;
	*clen = -1;
	for (int i = 0; i < end; i++) // header lines: "\r\n" + name, case-insensitive
		if (buf[i] == '\n' && i + 16 < end && !strncasecmp(buf + i + 1, "content-length:", 15))
			*clen = atoi(buf + i + 16);
	return 1;
}

#ifndef SELFTEST
#include <unistd.h>
#include <kernel.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <netman.h>
#include <ps2ips.h>

// TCP/IP runs on the IOP (ps2ip-nm.irx): nuld-av's netring.irx takes the stream straight from the Ethernet driver
// there (smap-modular-netman.irx, which it hooks), and the EE only makes calls: ps2ips for the address set-up and
// the pad's datagrams, nuld-av for everything that is read
#define IRX(m) extern unsigned char m##_irx[]; extern unsigned int size_##m##_irx
IRX(ps2dev9); IRX(netman); IRX(smap); IRX(ps2ip_nm); IRX(ps2ips); IRX(netring);

unsigned net_aton(const char *s) // "a.b.c.d" -> network order, as lwIP keeps it; 0 if it is not one
{
	unsigned a = 0, b = 0, c = 0, d = 0;
	if (sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return 0;
	return a | b << 8 | c << 16 | d << 24;
}

static int wait_for(int (*ok)(void)) // 10 s, as the ps2sdk sample
{
	for (int t = 0; t < 10; t++) {
		if (ok()) return 1;
		sleep(1);
	}
	return ok();
}
static int link_up(void) { return NetManIoctl(NETMAN_NETIF_IOCTL_GET_LINK_STATUS, NULL, 0, NULL, 0) == NETMAN_NETIF_ETH_LINK_STATE_UP; }
static int dhcp_bound(void)
{
	t_ip_info ip;
	return ps2ip_getconfig("sm0", &ip) >= 0 && ip.dhcp_status == DHCP_STATE_BOUND;
}

int net_up(const char *ip, const char *mask, const char *gw)
{
	struct { unsigned char *irx; unsigned *size; } mods[] = {{ps2dev9_irx, &size_ps2dev9_irx}, {netman_irx, &size_netman_irx},
		{smap_irx, &size_smap_irx}, {ps2ip_nm_irx, &size_ps2ip_nm_irx}, {ps2ips_irx, &size_ps2ips_irx},
		{netring_irx, &size_netring_irx}};
	for (unsigned i = 0; i < sizeof(mods) / sizeof(mods[0]); i++) {
		int r = 0;
		if (SifExecModuleBuffer(mods[i].irx, *mods[i].size, 0, NULL, &r) < 0 || r == 1) return NET_ERR_MODULES;
	}
	NetManInit();
	if (ps2ip_init() < 0) return NET_ERR_MODULES;
	int dhcp = !strcasecmp(ip, "dhcp");
	t_ip_info info;
	if (ps2ip_getconfig("sm0", &info) < 0) return NET_ERR_MODULES;
	info.dhcp_enabled = dhcp;
	if (!dhcp) {
		unsigned a = net_aton(ip), m = net_aton(mask), g = net_aton(gw);
		memcpy(&info.ipaddr, &a, 4), memcpy(&info.netmask, &m, 4), memcpy(&info.gw, &g, 4);
	}
	ps2ip_setconfig(&info);
	if (!wait_for(link_up)) return NET_ERR_LINK;
	if (dhcp && !wait_for(dhcp_bound)) return NET_ERR_DHCP;
	return 0;
}

#else // host check: `make test`
#include <assert.h>
int main(void)
{
	static const char ok[] = "HTTP/1.0 200 OK\r\nServer: x\r\ncontent-length: 5\r\n\r\nhello";
	int st, body, clen;
	assert(http_parse(ok, sizeof(ok) - 1, &st, &body, &clen) && st == 200 && clen == 5 && !memcmp(ok + body, "hello", 5));
	static const char nf[] = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
	assert(http_parse(nf, sizeof(nf) - 1, &st, &body, &clen) && st == 404 && clen == 0 && body == sizeof(nf) - 1);
	assert(!http_parse(ok, 20, &st, &body, &clen));                       // header cut short
	assert(!http_parse("FTP/1.0 200\r\n\r\n", 15, &st, &body, &clen));    // not HTTP
	static const char nolen[] = "HTTP/1.0 200 OK\r\n\r\nabc";
	assert(http_parse(nolen, sizeof(nolen) - 1, &st, &body, &clen) && clen == -1);
	puts("net selftest ok");
	return 0;
}
#endif
