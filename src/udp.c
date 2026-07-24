// UDP over the IPv4 stack: datagram sockets with a pseudo-header checksum, a
// small socket table with per-socket receive queues, and ephemeral-port
// allocation. The core stack (Ethernet/ARP/IPv4) lives in net.c; this module
// plugs in via udp_input (dispatched from ip_input) and ip_send. See
// docs/networking.md.

#include <udp.h>
#include <net.h>
#include <net_internal.h>
#include <ktime.h>
#include <utils.h>

typedef struct __attribute__((packed)) {
    uint16_t sport;
    uint16_t dport;
    uint16_t len; // header + payload
    uint16_t csum;
} udp_hdr;

// --- UDP + sockets ----------------------------------------------------------

#define UDP_SOCKETS 8
#define UDP_QUEUE 4
#define UDP_MSG_MAX 1472 // max UDP payload for a non-fragmented IPv4/1500 frame

typedef struct {
    uint32_t src_ip;
    uint16_t src_port;
    uint16_t len;
    uint8_t data[UDP_MSG_MAX];
} udp_dgram;

typedef struct {
    bool in_use;
    uint16_t local_port; // 0 = unbound
    udp_dgram q[UDP_QUEUE];
    uint8_t head, count; // ring of received datagrams
} udp_socket;

static udp_socket udp_socks[UDP_SOCKETS];

// UDP checksum over the IPv4 pseudo-header + segment. A computed 0 is sent as
// 0xFFFF, since 0 on the wire means "no checksum".
static uint16_t udp_checksum(uint32_t src, uint32_t dst, const void* seg,
                             uint16_t seglen)
{
    struct __attribute__((packed)) {
        uint32_t src, dst;
        uint8_t zero, proto;
        uint16_t len;
    } ph;
    memset(&ph, 0, sizeof(ph));
    ph.src = htonl(src);
    ph.dst = htonl(dst);
    ph.proto = IP_PROTO_UDP;
    ph.len = htons(seglen);
    uint32_t sum = csum_add(0, &ph, sizeof(ph));
    sum = csum_add(sum, seg, seglen);
    uint16_t c = csum_fold(sum);
    return c ? c : 0xFFFF;
}

static bool udp_output(uint16_t sport, uint32_t dst_ip, uint16_t dport,
                       const void* data, uint16_t len)
{
    static uint8_t seg[sizeof(udp_hdr) + UDP_MSG_MAX];
    if (len > UDP_MSG_MAX) {
        len = UDP_MSG_MAX;
    }
    uint16_t seglen = (uint16_t)(sizeof(udp_hdr) + len);
    udp_hdr* u = (udp_hdr*)seg;
    u->sport = htons(sport);
    u->dport = htons(dport);
    u->len = htons(seglen);
    u->csum = 0;
    memcpy(seg + sizeof(udp_hdr), data, len);
    u->csum = udp_checksum(net_ip(), dst_ip, seg, seglen);
    return ip_send(dst_ip, IP_PROTO_UDP, seg, seglen);
}

void udp_input(uint32_t src, const uint8_t* data, uint16_t len)
{
    if (len < sizeof(udp_hdr)) {
        return;
    }
    const udp_hdr* u = (const udp_hdr*)data;
    uint16_t ulen = ntohs(u->len);
    if (ulen < sizeof(udp_hdr) || ulen > len) {
        return;
    }
    uint16_t dport = ntohs(u->dport);
    uint16_t plen = (uint16_t)(ulen - sizeof(udp_hdr));
    const uint8_t* payload = data + sizeof(udp_hdr);

    for (int i = 0; i < UDP_SOCKETS; i++) {
        udp_socket* s = &udp_socks[i];
        if (!s->in_use || s->local_port != dport) {
            continue;
        }
        if (s->count >= UDP_QUEUE) {
            return; // queue full — drop (UDP is lossy by design)
        }
        udp_dgram* d = &s->q[(s->head + s->count) % UDP_QUEUE];
        d->src_ip = src;
        d->src_port = ntohs(u->sport);
        d->len = plen > UDP_MSG_MAX ? UDP_MSG_MAX : plen;
        memcpy(d->data, payload, d->len);
        s->count++;
        return;
    }
    // No socket bound to this port: silently drop.
}

static bool port_in_use(uint16_t port)
{
    for (int i = 0; i < UDP_SOCKETS; i++) {
        if (udp_socks[i].in_use && udp_socks[i].local_port == port) {
            return true;
        }
    }
    return false;
}

int net_udp_open(void)
{
    for (int i = 0; i < UDP_SOCKETS; i++) {
        if (!udp_socks[i].in_use) {
            udp_socks[i].in_use = true;
            udp_socks[i].local_port = 0;
            udp_socks[i].head = 0;
            udp_socks[i].count = 0;
            return i;
        }
    }
    return -1;
}

bool net_udp_bind(int s, uint16_t port)
{
    if (s < 0 || s >= UDP_SOCKETS || !udp_socks[s].in_use) {
        return false;
    }
    if (port == 0) { // pick an ephemeral port
        for (int tries = 0; tries < 16384; tries++) {
            uint16_t p = net_next_ephemeral();
            if (!port_in_use(p)) {
                port = p;
                break;
            }
        }
        if (port == 0) {
            return false;
        }
    } else if (port_in_use(port)) {
        return false;
    }
    udp_socks[s].local_port = port;
    return true;
}

bool net_udp_sendto(int s, uint32_t dst_ip, uint16_t dport, const void* data,
                    uint16_t len)
{
    if (s < 0 || s >= UDP_SOCKETS || !udp_socks[s].in_use || !net_ready()) {
        return false;
    }
    if (udp_socks[s].local_port == 0 && !net_udp_bind(s, 0)) {
        return false; // auto-bind an ephemeral source port on first send
    }
    return udp_output(udp_socks[s].local_port, dst_ip, dport, data, len);
}

int net_udp_recvfrom(int s, uint32_t timeout_ms, void* buf, uint16_t cap,
                     uint32_t* src_ip, uint16_t* src_port)
{
    if (s < 0 || s >= UDP_SOCKETS || !udp_socks[s].in_use) {
        return -1;
    }
    uint64_t deadline = ktime_ms() + timeout_ms;
    while (udp_socks[s].count == 0) {
        net_poll();
        if (ktime_ms() >= deadline) {
            return -1;
        }
    }
    udp_socket* sk = &udp_socks[s];
    udp_dgram* d = &sk->q[sk->head];
    uint16_t copy = d->len > cap ? cap : d->len;
    memcpy(buf, d->data, copy);
    if (src_ip) {
        *src_ip = d->src_ip;
    }
    if (src_port) {
        *src_port = d->src_port;
    }
    sk->head = (uint8_t)((sk->head + 1) % UDP_QUEUE);
    sk->count--;
    return d->len; // true datagram length (may exceed cap if truncated)
}

void net_udp_close(int s)
{
    if (s >= 0 && s < UDP_SOCKETS) {
        udp_socks[s].in_use = false;
    }
}
