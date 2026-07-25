#ifndef __HTTP_H
#define __HTTP_H

// Minimal HTTP/1.1 client. http_get() parses an `http://host[:port]/path` URL,
// resolves the host (net_resolve), opens a TCP connection, sends a GET with
// `Connection: close`, reads the whole response, parses it with picohttpparser,
// and copies up to `cap` bytes of the body into `body`.
//
// Returns the HTTP status code (e.g. 200 or 404) on success and sets *body_len
// to the number of body bytes copied. On failure returns a negative error:
//   -1 malformed/non-http URL   -2 DNS failure   -3 connect failure
//   -4 no/!parseable response
// Plaintext only; https:// (BearSSL) is a later round.
int http_get(const char* url, char* body, int cap, int* body_len);

#endif
