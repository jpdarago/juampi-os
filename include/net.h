#ifndef __NET_H
#define __NET_H

#include <stdint.h>
#include <stdbool.h>

// The IPv4 stack over the e1000 driver: this header is the core (bring-up,
// config, ICMP ping, the poll pump). The transport layers live in their own
// modules — see <udp.h> and <tcp.h>, included below so `#include <net.h>` still
// exposes the whole `net` surface. Poll-driven and BSP-only (see
// docs/networking.md).
//
// Convention: a uint32_t holding an IPv4 address is in host byte order
// (10.0.2.15 == 0x0A00020F) and its name ends in `_ip` — like `_ms` marks
// milliseconds — e.g. dst_ip, src_ip, gateway_ip. The bare `ip`/`my_ip` (our
// own address) and on-wire packed struct fields (read via ntohl) are the
// exceptions; `mask` is a netmask, not an address.

// Probe the NIC and apply the default static configuration (10.0.2.15/24 via
// 10.0.2.2, matching QEMU user-mode networking). No-op if no card is found.
void net_init(void);

// True once a NIC is up and configured.
bool net_ready(void);

// Drain the RX ring and run the stack over each frame. Cheap when idle; called
// from the console idle loop and from blocking calls like net_ping.
void net_poll(void);

// Override the static configuration (all host byte order).
void net_config(uint32_t ip, uint32_t mask, uint32_t gateway_ip);

uint32_t net_ip(void);        // our IPv4 address (host order), 0 if unset
void net_mac(uint8_t out[6]); // our MAC

// Send one ICMP echo request to `dst_ip` (host order) and wait up to
// `timeout_ms` for the reply. On success returns true and stores the round-trip
// time in `*rtt_us`. Returns false on timeout or if the stack is not ready.
bool net_ping(uint32_t dst_ip, uint32_t timeout_ms, uint64_t* rtt_us);

// Dotted-quad <-> host-order uint32 helpers.
bool net_aton(const char* s, uint32_t* out_ip);
void net_ntoa(uint32_t ip, char* buf); // buf must hold >= 16 bytes

// The transport layers. Included here so `#include <net.h>` yields the full
// API.
#include <udp.h>
#include <tcp.h>

// Configuration + application helpers built on the transports.
#include <dhcp.h> // DHCP client (net_dhcp, net_dns_server)
#include <dns.h>  // hostname resolver (net_resolve)

#endif
