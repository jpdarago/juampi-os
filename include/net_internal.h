#ifndef __NET_INTERNAL_H
#define __NET_INTERNAL_H

// Private glue shared between the stack's translation units (net.c, udp.c,
// tcp.c) — not a public API. net.c owns the core (Ethernet/ARP/IPv4/ICMP) and
// provides these helpers to the transport modules.

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define IP_PROTO_ICMP 1
#define IP_PROTO_TCP 6
#define IP_PROTO_UDP 17

static inline uint16_t htons(uint16_t x)
{
    return (uint16_t)__builtin_bswap16(x);
}
static inline uint16_t ntohs(uint16_t x)
{
    return (uint16_t)__builtin_bswap16(x);
}
static inline uint32_t htonl(uint32_t x)
{
    return __builtin_bswap32(x);
}
static inline uint32_t ntohl(uint32_t x)
{
    return __builtin_bswap32(x);
}

// RFC 1071 Internet checksum, split so a caller spanning several buffers (a
// pseudo-header + segment) can accumulate before folding. Defined in net.c.
uint32_t csum_add(uint32_t sum, const void* data, uint32_t len);
uint16_t csum_fold(uint32_t sum);

// Send an L4 payload as one IPv4 datagram to `dst` (host byte order), resolving
// the next hop via ARP. Returns false if unreachable or not confirmed on the
// wire. Defined in net.c.
bool ip_send(uint32_t dst, uint8_t proto, const void* l4, uint16_t l4len);

// (Our IPv4 address for pseudo-header checksums comes from net_ip() in net.h.)

// Allocate the next ephemeral local port (49152..65535, wrapping). Defined in
// net.c so UDP and TCP share one counter.
uint16_t net_next_ephemeral(void);

#endif
