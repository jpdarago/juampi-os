#ifndef __TCP_H
#define __TCP_H

#include <stdint.h>
#include <stdbool.h>

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

// Dispatched from ip_input() and net_poll() in net.c. Internal.
void tcp_input(uint32_t src, const uint8_t* data, uint16_t len);
void tcp_tick(void);

#endif
