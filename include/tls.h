#ifndef __TLS_H
#define __TLS_H

#include <stdbool.h>

// Sanity-check the vendored BearSSL build with a SHA-256 known-answer test —
// proves the crypto runs correctly freestanding. Reported as a boot self-test.
// (This module will grow the HTTPS/TLS client.)
bool tls_selftest(void);

#endif
