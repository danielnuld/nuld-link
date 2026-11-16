#pragma once
enum { NET_ERR_MODULES = -1, NET_ERR_LINK = -2, NET_ERR_DHCP = -3, NET_ERR_CONNECT = -5, NET_ERR_PROTO = -7 };

// ps2dev9, netman, the modular smap, ps2ip-nm, ps2ips and nuld-av's netring; ip "dhcp" or dotted. 0 or NET_ERR_*
int net_up(const char *ip, const char *mask, const char *gw);
unsigned net_aton(const char *s); // "a.b.c.d" -> network order; 0 if it is not one
int http_parse(const char *buf, int n, int *status, int *body, int *clen); // 1 if a full header was found
