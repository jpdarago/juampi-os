#ifndef __UDP_H
#define __UDP_H

#include <stdint.h>
#include <stdbool.h>

// UDP datagram sockets. net_udp_open() returns a socket handle (>= 0) or -1 if
// none are free; net_udp_close() releases it. IPs are host byte order.
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

// Receive entry point, dispatched from ip_input() in net.c. Internal.
void udp_input(uint32_t src, const uint8_t* data, uint16_t len);

#endif
