// HTTP/1.1 client (http.h): orbit-jellyfin's jellyfin.c HTTP part, unchanged but for the error names.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/time.h>
#include <netdb.h>
#ifdef _EE
#include <ps2ip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#endif
#include "http.h"

static int fill(http_stream *h) // more raw bytes into buf (after compacting); 0 at EOF / error
{
	if (h->pos > 0) memmove(h->buf, h->buf + h->pos, h->len - h->pos), h->len -= h->pos, h->pos = 0;
	if (h->len == (int)sizeof(h->buf)) return 1;
	int r = recv(h->s, h->buf + h->len, sizeof(h->buf) - h->len, 0);
	if (r <= 0) return 0;
	h->len += r;
	return 1;
}

static int line(http_stream *h, char *out, int max) // one header / chunk-size line without CRLF; 0 if none
{
	for (;;) {
		for (int i = h->pos; i + 1 < h->len; i++)
			if (h->buf[i] == '\r' && h->buf[i + 1] == '\n') {
				int n = i - h->pos < max - 1 ? i - h->pos : max - 1;
				memcpy(out, h->buf + h->pos, n), out[n] = 0;
				h->pos = i + 2;
				return 1;
			}
		if (h->len - h->pos >= (int)sizeof(h->buf) - 1 || !fill(h)) return 0; // a line longer than the buffer
	}
}

int http_open(http_stream *h, const char *host, int port, const char *method, const char *path, const char *extra,
              const char *body)
{
	char req[1536], l[256], ps[8];
	struct addrinfo hints, *ai;
	memset(h, 0, sizeof(*h));
	h->s = -1;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET, hints.ai_socktype = SOCK_STREAM;
	snprintf(ps, sizeof(ps), "%d", port);
	if (getaddrinfo(host, ps, &hints, &ai) != 0) return HTTP_ERR_CONNECT;
	h->s = socket(AF_INET, SOCK_STREAM, 0);
	int r = h->s < 0 ? -1 : connect(h->s, ai->ai_addr, ai->ai_addrlen);
	freeaddrinfo(ai);
	if (r < 0) { http_close(h); return HTTP_ERR_CONNECT; }
	struct timeval tv = {15, 0}; // a stalled server must not hang the caller forever
	setsockopt(h->s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	int n = snprintf(req, sizeof(req), "%s %s HTTP/1.1\r\nHost: %s:%d\r\nConnection: close\r\n%s%s", method, path, host,
	                 port, extra ? extra : "", body ? "Content-Type: application/json\r\n" : "");
	if (body) n += snprintf(req + n, sizeof(req) - n, "Content-Length: %d\r\n", (int)strlen(body));
	n += snprintf(req + n, sizeof(req) - n, "\r\n");
	if (n >= (int)sizeof(req) || send(h->s, req, n, 0) != n || (body && send(h->s, body, strlen(body), 0) != (int)strlen(body))) {
		http_close(h);
		return HTTP_ERR_HTTP;
	}
	if (!line(h, l, sizeof(l)) || strncmp(l, "HTTP/1.", 7) || strlen(l) < 12) { http_close(h); return HTTP_ERR_HTTP; }
	h->status = atoi(l + 9);
	h->left = -1;
	while (line(h, l, sizeof(l)) && *l) {
		if (!strncasecmp(l, "content-length:", 15)) h->left = atoll(l + 15);
		if (!strncasecmp(l, "transfer-encoding:", 18) && strstr(l + 18, "chunked")) h->chunked = 1;
	}
	if (h->chunked) h->left = 0; // read the first chunk size on the first read
	return h->status;
}

int http_read(http_stream *h, void *dst, int n)
{
	char l[32];
	if (h->eof || n <= 0) return 0;
	if (h->chunked && h->left == 0) {
		if (!line(h, l, sizeof(l))) return -1;
		if (!*l && !line(h, l, sizeof(l))) return -1; // the CRLF that ends the previous chunk
		h->left = strtoll(l, NULL, 16);
		if (h->left == 0) { h->eof = 1; return 0; }
	}
	if (h->left == 0) { h->eof = 1; return 0; } // Content-Length reached
	if (h->pos == h->len && !fill(h)) {
		h->eof = 1;
		return h->left == -1 ? 0 : -1; // the end of a to-EOF body, or cut short
	}
	int k = h->len - h->pos;
	if (k > n) k = n;
	if (h->left >= 0 && k > h->left) k = (int)h->left;
	memcpy(dst, h->buf + h->pos, k);
	h->pos += k;
	if (h->left > 0) h->left -= k;
	return k;
}

void http_close(http_stream *h)
{
	if (h->s >= 0) close(h->s);
	h->s = -1;
}
