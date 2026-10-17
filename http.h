#pragma once
// HTTP/1.1 client over BSD sockets (lwIP on the PS2, the host's for a selftest): one request per connection, the
// body read as a stream, Content-Length or chunked. Lifted from orbit-jellyfin's jellyfin.c (phase 15), where it was
// checked against Jellyfin 12.1; ffmpeg's `-listen 1` server answers the same way (chunked).
typedef struct {
	int s, status, chunked, eof;
	long long left;          // bytes left in the body (Content-Length) or in the current chunk; -1 = to EOF
	int pos, len;            // buffered bytes buf[pos..len)
	char buf[4096];
} http_stream;
enum { HTTP_ERR_CONNECT = -21, HTTP_ERR_HTTP = -22 };
// method "GET" / "POST"; extra: more header lines ("Name: v\r\n"...) or NULL; body NULL for none. Returns the status,
// or < 0 (HTTP_ERR_*). On >= 0 the body is read with http_read and the stream closed with http_close.
int http_open(http_stream *h, const char *host, int port, const char *method, const char *path, const char *extra,
              const char *body);
int http_read(http_stream *h, void *dst, int n); // body bytes (chunks decoded); 0 = end, < 0 = error
void http_close(http_stream *h);
