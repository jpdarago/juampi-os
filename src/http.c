// Minimal HTTP/1.1 client (see http.h). Parses the URL, resolves the host,
// opens a TCP connection, sends a GET with Connection: close, reads the whole
// response to EOF, and parses the status line + headers with the vendored
// picohttpparser. The body is bounded by the server closing the connection;
// Transfer-Encoding: chunked bodies are de-chunked in place
// (phr_decode_chunked).

#include <http.h>
#include <net.h>
#include <tls.h>
#include <str.h>
#include <utils.h> // memset

#include <printf/printf.h> // snprintf (kernel provides the standard name)
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "http/picohttpparser.h"

#define HTTP_PORT 80   // default port for http:// URLs
#define HTTPS_PORT 443 // default port for https:// URLs

// Case-insensitive: does header `h`'s name equal `name`?
static bool hdr_name_is(const struct phr_header* h, str name)
{
    return str_eq_ci(str_span(h->name, h->name_len), name);
}

// Case-insensitive substring search of `needle` in header `h`'s value.
static bool hdr_value_has(const struct phr_header* h, str needle)
{
    if (needle.len == 0 || h->value_len < needle.len) {
        return needle.len == 0;
    }
    for (size_t i = 0; i + needle.len <= h->value_len; i++) {
        size_t j = 0;
        while (j < needle.len &&
               to_lower(h->value[i + j]) == to_lower(needle.data[j])) {
            j++;
        }
        if (j == needle.len) {
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

    // Parse [http|https]://<authority>/<path> into host, port, and path.
    str u = str_from(url);
    bool https;
    if (str_has_prefix(u, S("https://"))) {
        https = true;
        u = str_trim_prefix(u, S("https://"));
    } else if (str_has_prefix(u, S("http://"))) {
        https = false;
        u = str_trim_prefix(u, S("http://"));
    } else {
        return -1;
    }

    // Authority is everything up to the first '/'; the rest (kept off the '/')
    // becomes the request path with the slash restored.
    str authority, rest;
    str_cut_ch(u, '/', &authority,
               &rest); // on no '/', authority = u, rest = ""
    char path[512];
    path[0] = '/';
    str_copy(path + 1, sizeof path - 1, rest);

    // Authority is host[:port].
    str host_v, port_v;
    uint16_t port = https ? HTTPS_PORT : HTTP_PORT;
    if (str_cut_ch(authority, ':', &host_v, &port_v)) {
        uint32_t pv;
        if (str_to_u32(port_v, &pv)) {
            port = (uint16_t)pv;
        }
    } else {
        host_v = authority;
    }
    if (host_v.len == 0) {
        return -1;
    }
    char host[256];
    str_copy(host, sizeof host, host_v);

    uint32_t host_ip;
    if (!net_resolve(host, 4000, &host_ip)) {
        return -2;
    }
    // Transport: TLS for https:// (validated against the curated CA set), plain
    // TCP otherwise. Both are driven through the same send/recv below.
    tls_conn* tls = NULL;
    int conn = -1;
    if (https) {
        tls = tls_connect(a, host_ip, port, host);
        if (tls == NULL) {
            return -3;
        }
    } else {
        conn = net_tcp_connect(host_ip, port, 5000);
        if (conn < 0) {
            return -3;
        }
    }

    char req[600];
    int rn = snprintf(req, sizeof req,
                      "GET %s HTTP/1.1\r\n"
                      "Host: %s\r\n"
                      "User-Agent: juampiOS/1.0\r\n"
                      "Accept: */*\r\n"
                      "Connection: close\r\n\r\n",
                      path, host);
    int sent = https ? tls_send(tls, req, rn)
                     : net_tcp_send(conn, req, (uint32_t)rn);
    if (sent < 0) {
        // e.g. the TLS handshake or certificate validation failed.
        if (https) {
            tls_close(tls);
        } else {
            net_tcp_close(conn);
        }
        return -3;
    }

    // Response buffer comes from the caller's allocator (no static state), so
    // concurrent callers with their own arenas/heaps don't collide.
    char* recvbuf = new (a, char, HTTP_RECV_MAX);

    // Read until the server closes (EOF) or we run out of buffer.
    int total = 0;
    while (total < HTTP_RECV_MAX) {
        int r = https ? tls_recv(tls, recvbuf + total, HTTP_RECV_MAX - total)
                      : net_tcp_recv(conn, recvbuf + total,
                                     (uint32_t)(HTTP_RECV_MAX - total), 8000);
        if (r <= 0) {
            break; // 0 = clean EOF, -1 = timeout/error
        }
        total += r;
    }
    if (https) {
        tls_close(tls);
    } else {
        net_tcp_close(conn);
    }
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
        if (hdr_name_is(&headers[i], S("transfer-encoding")) &&
            hdr_value_has(&headers[i], S("chunked"))) {
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
