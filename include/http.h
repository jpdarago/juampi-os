#ifndef __HTTP_H
#define __HTTP_H

#include <alloc.h>

// Largest response (headers + body) the client buffers; the scratch allocator
// passed to http_get must be able to supply this much.
#define HTTP_RECV_MAX (512 * 1024)

// Minimal HTTP/1.1 client. http_get() parses an `http://host[:port]/path` URL,
// resolves the host (net_resolve), opens a TCP connection, sends a GET with
// `Connection: close`, reads the whole response, and parses it with
// picohttpparser.
//
// The response is buffered in scratch allocated from `a` (pass an arena or a
// per-core heap — nothing here is static, so concurrent callers with their own
// allocators don't collide). On success *out_body points into that scratch
// (valid until `a` is reset/freed), *out_len is the de-chunked body length, and
// the return value is the HTTP status (e.g. 200 or 404). On failure returns a
// negative error:
//   -1 malformed/non-http URL   -2 DNS failure   -3 connect failure
//   -4 no/unparseable response
// Plaintext only; https:// (BearSSL) is a later round.
int http_get(struct allocator* a, const char* url, char** out_body,
             int* out_len);

#endif
