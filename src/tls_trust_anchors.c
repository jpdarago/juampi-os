// Compiles the brssl-generated trust anchors (tls_trust_anchors.inc) with
// warnings off — the generated initialisers cast away const, which the kernel's
// -Wcast-qual gauntlet rejects — and re-exports them for tls.c. Built by a
// dedicated -w Makefile rule.

#include <bearssl.h>

#include "tls_trust_anchors.inc" // static const TAs[] + #define TAs_NUM

const br_x509_trust_anchor* const tls_trust_anchors = TAs;
const size_t tls_trust_anchors_count = TAs_NUM;
