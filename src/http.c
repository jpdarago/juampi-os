// Minimal HTTP/1.1 client (see http.h). Parses the URL, resolves the host,
// opens a TCP connection, sends a GET with Connection: close, reads the whole
// response to EOF, and parses the status line + headers with the vendored
// picohttpparser. The body is bounded by the server closing the connection;
// Transfer-Encoding: chunked bodies are de-chunked in place
// (phr_decode_chunked).

#include <http.h>
#include <net.h>
#include <utils.h> // memset

#include <printf/printf.h> // snprintf (kernel provides the standard name)
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "http/picohttpparser.h"

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

// Case-insensitive: does header `h`'s name equal the NUL-terminated `name`?
static bool hdr_name_is(const struct phr_header* h, const char* name)
{
    size_t i = 0;
    for (; i < h->name_len && name[i]; i++) {
        if (lower(h->name[i]) != lower(name[i])) {
            return false;
        }
    }
    return i == h->name_len && name[i] == '\0';
}

// Case-insensitive substring search of `needle` in header `h`'s value.
static bool hdr_value_has(const struct phr_header* h, const char* needle)
{
    size_t nl = 0;
    while (needle[nl]) {
        nl++;
    }
    if (nl == 0 || h->value_len < nl) {
        return nl == 0;
    }
    for (size_t i = 0; i + nl <= h->value_len; i++) {
        size_t j = 0;
        while (j < nl && lower(h->value[i + j]) == lower(needle[j])) {
            j++;
        }
        if (j == nl) {
            return true;
        }
    }
    return false;
}

int http_get(allocator* a, const char* url, char** out_body, int* out_len)
{
    if (out_body) {
        *out_body = NULL;
    }
    if (out_len) {
        *out_len = 0;
    }

    // Require an http:// scheme.
    const char* pre = "http://";
    const char* p = url;
    for (int i = 0; i < 7; i++) {
        if (p[i] != pre[i]) {
            return -1;
        }
    }
    p += 7;

    // host[:port]
    char host[256];
    int hn = 0;
    while (*p && *p != ':' && *p != '/' && hn < (int)sizeof(host) - 1) {
        host[hn++] = *p++;
    }
    host[hn] = '\0';
    if (hn == 0) {
        return -1;
    }
    uint16_t port = 80;
    if (*p == ':') {
        p++;
        int v = 0;
        while (*p >= '0' && *p <= '9') {
            v = v * 10 + (*p - '0');
            p++;
        }
        port = (uint16_t)v;
    }
    const char* path = (*p == '/') ? p : "/";

    uint32_t host_ip;
    if (!net_resolve(host, 4000, &host_ip)) {
        return -2;
    }
    int conn = net_tcp_connect(host_ip, port, 5000);
    if (conn < 0) {
        return -3;
    }

    char req[600];
    int rn = snprintf(req, sizeof req,
                      "GET %s HTTP/1.1\r\n"
                      "Host: %s\r\n"
                      "User-Agent: juampiOS/1.0\r\n"
                      "Accept: */*\r\n"
                      "Connection: close\r\n\r\n",
                      path, host);
    net_tcp_send(conn, req, (uint32_t)rn);

    // Response buffer comes from the caller's allocator (no static state), so
    // concurrent callers with their own arenas/heaps don't collide.
    char* recvbuf = new (a, char, HTTP_RECV_MAX);

    // Read until the server closes (EOF) or we run out of buffer.
    int total = 0;
    while (total < HTTP_RECV_MAX) {
        int r = net_tcp_recv(conn, recvbuf + total,
                             (uint32_t)(HTTP_RECV_MAX - total), 8000);
        if (r <= 0) {
            break; // 0 = clean EOF, -1 = timeout
        }
        total += r;
    }
    net_tcp_close(conn);
    if (total <= 0) {
        return -4;
    }

    int minor = 0, status = 0;
    const char* msg = NULL;
    size_t msglen = 0;
    struct phr_header headers[32];
    size_t nheaders = sizeof(headers) / sizeof(headers[0]);
    int hdrlen = phr_parse_response(recvbuf, (size_t)total, &minor, &status,
                                    &msg, &msglen, headers, &nheaders, 0);
    if (hdrlen < 0) {
        return -4;
    }

    int blen = total - hdrlen;
    if (blen < 0) {
        blen = 0;
    }

    // De-chunk the body in place if the response was Transfer-Encoding: chunked
    // (HTTP/1.1's default framing — used by example.com and most servers).
    bool chunked = false;
    for (size_t i = 0; i < nheaders; i++) {
        if (hdr_name_is(&headers[i], "transfer-encoding") &&
            hdr_value_has(&headers[i], "chunked")) {
            chunked = true;
            break;
        }
    }
    if (chunked && blen > 0) {
        struct phr_chunked_decoder dec;
        memset(&dec, 0, sizeof dec);
        size_t sz = (size_t)blen;
        long rc = phr_decode_chunked(&dec, recvbuf + hdrlen, &sz);
        if (rc != -1) { // -2 (truncated) still leaves sz = bytes decoded so far
            blen = (int)sz;
        }
    }

    // Hand back a view into the scratch buffer; the caller owns its lifetime.
    if (out_body) {
        *out_body = recvbuf + hdrlen;
    }
    if (out_len) {
        *out_len = blen;
    }
    return status;
}
