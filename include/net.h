#ifndef __NET_H
#define __NET_H

#include <stdint.h>
#include <stdbool.h>

// Minimal IPv4 stack over the e1000 driver: Ethernet + ARP + IPv4 + ICMP, just
// enough to ping. Poll-driven and BSP-only (see docs/networking.md). IP
// addresses are passed around in host byte order (10.0.2.15 == 0x0A00020F).

// Probe the NIC and apply the default static configuration (10.0.2.15/24 via
// 10.0.2.2, matching QEMU user-mode networking). No-op if no card is found.
void net_init(void);

// True once a NIC is up and configured.
bool net_ready(void);

// Drain the RX ring and run the stack over each frame. Cheap when idle; called
// from the console idle loop and from blocking calls like net_ping.
void net_poll(void);

// Override the static configuration (all host byte order).
void net_config(uint32_t ip, uint32_t mask, uint32_t gateway);

uint32_t net_ip(void);        // our IPv4 address (host order), 0 if unset
void net_mac(uint8_t out[6]); // our MAC

// Send one ICMP echo request to `dst` (host order) and wait up to `timeout_ms`
// for the reply. On success returns true and stores the round-trip time in
// `*rtt_us`. Returns false on timeout or if the stack is not ready.
bool net_ping(uint32_t dst, uint32_t timeout_ms, uint64_t* rtt_us);

// UDP sockets. net_udp_open() returns a socket handle (>= 0) or -1 if none are
// free; net_udp_close() releases it. Datagrams are host byte order for the IP,
// network semantics otherwise.
int net_udp_open(void);
void net_udp_close(int sock);

// Bind a local port (0 picks an ephemeral one). False if the socket is invalid
// or the port is already taken.
bool net_udp_bind(int sock, uint16_t port);

// Send a datagram to (dst_ip, dport). Auto-binds an ephemeral source port on
// first use. Returns false if the socket is invalid or the send is not
// confirmed on the wire.
bool net_udp_sendto(int sock, uint32_t dst_ip, uint16_t dport, const void* data,
                    uint16_t len);

// Wait up to `timeout_ms` for a datagram, copying up to `cap` bytes into `buf`
// and the sender into *src_ip / *src_port (either may be NULL). Returns the
// true datagram length (may exceed `cap` if truncated), or -1 on timeout.
int net_udp_recvfrom(int sock, uint32_t timeout_ms, void* buf, uint16_t cap,
                     uint32_t* src_ip, uint16_t* src_port);

// TCP client (active open). net_tcp_connect performs the 3-way handshake to
// (dst_ip, port) and returns a connection handle (>= 0), or -1 on refusal,
// unreachability, or timeout.
int net_tcp_connect(uint32_t dst_ip, uint16_t port, uint32_t timeout_ms);

// TCP server (passive open, one connection per handle). net_tcp_listen binds a
// local port; net_tcp_accept blocks up to `timeout_ms` for a client to complete
// the handshake, after which the same handle is a connected socket usable with
// send/recv/close. Returns the handle, or -1.
int net_tcp_listen(uint16_t port);
int net_tcp_accept(int conn, uint32_t timeout_ms);

// Reliably send `len` bytes (stop-and-wait with retransmit). Returns the number
// of bytes acknowledged, or -1 if the connection failed before any went out.
int net_tcp_send(int conn, const void* data, uint32_t len);

// Read up to `cap` bytes of received stream data, waiting up to `timeout_ms`
// for some to arrive. Returns >0 bytes read, 0 at clean end-of-stream, or -1 on
// timeout.
int net_tcp_recv(int conn, void* buf, uint32_t cap, uint32_t timeout_ms);

// Send FIN and release the connection handle.
void net_tcp_close(int conn);

// Dotted-quad <-> host-order uint32 helpers.
bool net_aton(const char* s, uint32_t* out);
void net_ntoa(uint32_t ip, char* buf); // buf must hold >= 16 bytes

#endif
