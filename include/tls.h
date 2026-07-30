#ifndef __TLS_H
#define __TLS_H

#include <alloc.h>
#include <stdint.h>
#include <stdbool.h>

// TLS 1.2 client over the TCP stack, backed by the vendored BearSSL. Validates
// the server certificate chain against a curated CA set. See src/tls.c.

// Sanity-check the BearSSL build (SHA-256 known-answer); a boot self-test.
bool tls_selftest(void);

// An established (or establishing) TLS session. Allocated from `a`; its buffers
// live in that allocator, so free/reset it (or its arena) after use. The
// handshake is lazy — it runs on the first tls_send/tls_recv, so certificate
// errors surface there (tls_send returns -1; check tls_error()).
struct tls_conn;

// Open a TCP connection to (ip, port) and prepare a TLS session for `host`
// (used for SNI and certificate name validation). Returns NULL if the TCP
// connect fails.
struct tls_conn* tls_connect(struct allocator* a, uint32_t ip, uint16_t port,
                             const char* host);

// Send all `len` bytes as TLS application data (driving the handshake on first
// use). Returns `len`, or -1 on any TLS/socket error.
int tls_send(struct tls_conn* c, const void* data, int len);

// Read up to `cap` bytes of decrypted application data. Returns >0 bytes, 0 at
// a clean close_notify (end of stream), or -1 on error.
int tls_recv(struct tls_conn* c, void* buf, int cap);

// The last BearSSL engine error (0 == BR_ERR_OK). Useful after a failed send.
int tls_error(struct tls_conn* c);

// Send close_notify (best effort) and close the underlying TCP connection.
void tls_close(struct tls_conn* c);

#endif
