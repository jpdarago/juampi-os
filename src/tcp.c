// TCP over the IPv4 stack: an active/passive-open connection with the full
// state machine, seq/ack tracking, an in-order receive ring, stop-and-wait
// send with an RTO retransmit timer, and FIN close. Both client
// (net_tcp_connect) and server (net_tcp_listen/accept). The core stack lives in
// net.c; this module plugs in via tcp_input/tcp_tick and ip_send. See
// docs/networking.md.

#include <tcp.h>
#include <net.h>
#include <net_internal.h>
#include <ktime.h>
#include <utils.h>

typedef struct __attribute__((packed)) {
    uint16_t sport;
    uint16_t dport;
    uint32_t seq;
    uint32_t ack;
    uint8_t off;   // data offset: (header words) << 4
    uint8_t flags; // TCP_FIN..TCP_ACK
    uint16_t window;
    uint16_t csum;
    uint16_t urg;
} tcp_hdr;

#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10

// --- TCP (active-open client) -----------------------------------------------

#define TCP_CONNS 2
#define TCP_MSS 536
#define TCP_RXBUF 8192
#define TCP_RTO_MS 400
#define TCP_MAX_RETX 6

enum tcp_state {
    TCP_CLOSED,
    TCP_LISTEN,   // passive open, awaiting a SYN
    TCP_SYN_SENT, // active open, sent SYN
    TCP_SYN_RCVD, // got a SYN, sent SYN-ACK, awaiting its ACK
    TCP_ESTABLISHED,
    TCP_FIN_WAIT, // we sent FIN, awaiting its ACK and the peer's FIN
    TCP_DONE      // connection finished or reset; recv drains, then closed
};

typedef struct {
    bool in_use;
    enum tcp_state state;
    uint32_t peer_ip;
    uint16_t local_port, peer_port;
    uint32_t snd_una, snd_nxt; // oldest unacked / next seq to send
    uint32_t rcv_nxt;          // next in-order seq we expect
    bool peer_fin;             // peer has sent FIN

    uint8_t rx[TCP_RXBUF]; // in-order received data ring
    uint32_t rx_head, rx_len;

    // Single outstanding (retransmittable) segment — stop-and-wait.
    uint8_t retx[sizeof(tcp_hdr) + TCP_MSS];
    uint16_t retx_len;   // total segment length, 0 if nothing outstanding
    uint32_t retx_seq;   // seq the segment ends at (what an ACK must reach)
    uint64_t retx_at_ms; // last (re)send time
    int retx_tries;
} tcp_conn;

static tcp_conn tcp_conns[TCP_CONNS];

static bool seq_lt(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) < 0;
}

static uint16_t tcp_checksum(uint32_t src, uint32_t dst, const void* seg,
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
    ph.proto = IP_PROTO_TCP;
    ph.len = htons(seglen);
    uint32_t sum = csum_add(0, &ph, sizeof(ph));
    sum = csum_add(sum, seg, seglen);
    return csum_fold(sum);
}

// Build and send one segment. If it carries a SYN/FIN or data it is recorded as
// the outstanding segment for retransmission (seq-consuming flags advance nxt).
static bool tcp_emit(tcp_conn* c, uint8_t flags, const void* data, uint16_t len)
{
    if (len > TCP_MSS) {
        len = TCP_MSS;
    }
    uint16_t seglen = (uint16_t)(sizeof(tcp_hdr) + len);
    uint8_t seg[sizeof(tcp_hdr) + TCP_MSS];
    tcp_hdr* t = (tcp_hdr*)seg;
    memset(t, 0, sizeof(tcp_hdr));
    t->sport = htons(c->local_port);
    t->dport = htons(c->peer_port);
    t->seq = htonl(c->snd_nxt);
    t->ack = htonl(c->rcv_nxt);
    t->off = (uint8_t)((sizeof(tcp_hdr) / 4) << 4);
    t->flags = flags;
    t->window = htons((uint16_t)(TCP_RXBUF - c->rx_len));
    if (len) {
        memcpy(seg + sizeof(tcp_hdr), data, len);
    }
    t->csum = tcp_checksum(net_ip(), c->peer_ip, seg, seglen);

    uint32_t consumed = len;
    if (flags & (TCP_SYN | TCP_FIN)) {
        consumed += 1; // SYN and FIN each occupy one sequence number
    }
    if (consumed) { // remember it for retransmission
        memcpy(c->retx, seg, seglen);
        c->retx_len = seglen;
        c->retx_seq = c->snd_nxt + consumed;
        c->retx_at_ms = ktime_ms();
        c->retx_tries = 0;
        c->snd_nxt += consumed;
    }
    return ip_send(c->peer_ip, IP_PROTO_TCP, seg, seglen);
}

// Bare ACK (carries no sequence space, never retransmitted).
static void tcp_ack(tcp_conn* c)
{
    uint8_t seg[sizeof(tcp_hdr)];
    tcp_hdr* t = (tcp_hdr*)seg;
    memset(t, 0, sizeof(tcp_hdr));
    t->sport = htons(c->local_port);
    t->dport = htons(c->peer_port);
    t->seq = htonl(c->snd_nxt);
    t->ack = htonl(c->rcv_nxt);
    t->off = (uint8_t)((sizeof(tcp_hdr) / 4) << 4);
    t->flags = TCP_ACK;
    t->window = htons((uint16_t)(TCP_RXBUF - c->rx_len));
    t->csum = tcp_checksum(net_ip(), c->peer_ip, seg, sizeof(seg));
    ip_send(c->peer_ip, IP_PROTO_TCP, seg, sizeof(seg));
}

static tcp_conn* tcp_find(uint32_t src, uint16_t sport, uint16_t dport)
{
    for (int i = 0; i < TCP_CONNS; i++) {
        tcp_conn* c = &tcp_conns[i];
        if (c->in_use && c->peer_ip == src && c->peer_port == sport &&
            c->local_port == dport) {
            return c;
        }
    }
    return NULL;
}

void tcp_input(uint32_t src, const uint8_t* data, uint16_t len)
{
    if (len < sizeof(tcp_hdr)) {
        return;
    }
    const tcp_hdr* t = (const tcp_hdr*)data;
    uint32_t hlen = (uint32_t)(t->off >> 4) * 4;
    if (hlen < sizeof(tcp_hdr) || hlen > len) {
        return;
    }
    uint16_t sport = ntohs(t->sport), dport = ntohs(t->dport);
    uint32_t seq = ntohl(t->seq);
    uint32_t ack = ntohl(t->ack);
    uint8_t flags = t->flags;
    const uint8_t* payload = data + hlen;
    uint16_t plen = (uint16_t)(len - hlen);

    tcp_conn* c = tcp_find(src, sport, dport);
    if (!c) {
        // A SYN to a listening socket opens the connection (passive open).
        if (flags & TCP_SYN) {
            for (int i = 0; i < TCP_CONNS; i++) {
                tcp_conn* l = &tcp_conns[i];
                if (l->in_use && l->state == TCP_LISTEN &&
                    l->local_port == dport) {
                    l->peer_ip = src;
                    l->peer_port = sport;
                    l->rcv_nxt = seq + 1;
                    l->snd_una = l->snd_nxt = (uint32_t)ktime_ns();
                    l->state = TCP_SYN_RCVD;
                    tcp_emit(l, TCP_SYN | TCP_ACK, NULL, 0);
                    return;
                }
            }
        }
        return;
    }

    if (flags & TCP_RST) {
        c->state = TCP_DONE;
        c->retx_len = 0;
        return;
    }

    // Clear the outstanding segment once its data is fully acknowledged.
    if ((flags & TCP_ACK) && c->retx_len && !seq_lt(ack, c->retx_seq)) {
        c->retx_len = 0;
    }
    if ((flags & TCP_ACK) && seq_lt(c->snd_una, ack)) {
        c->snd_una = ack;
    }

    // Passive open completes when our SYN-ACK is acknowledged.
    if (c->state == TCP_SYN_RCVD && (flags & TCP_ACK) && c->retx_len == 0) {
        c->state = TCP_ESTABLISHED;
    }

    if (c->state == TCP_SYN_SENT) {
        if ((flags & TCP_SYN) && (flags & TCP_ACK)) {
            c->rcv_nxt = seq + 1;
            c->state = TCP_ESTABLISHED;
            tcp_ack(c);
        }
        return;
    }

    // In-order data: append to the receive ring and acknowledge.
    if (plen && seq == c->rcv_nxt && c->rx_len + plen <= TCP_RXBUF) {
        for (uint16_t i = 0; i < plen; i++) {
            c->rx[(c->rx_head + c->rx_len + i) % TCP_RXBUF] = payload[i];
        }
        c->rx_len += plen;
        c->rcv_nxt += plen;
        tcp_ack(c);
    } else if (plen) {
        tcp_ack(c); // out of order or no room: re-ack what we have
    }

    if ((flags & TCP_FIN) && seq + plen == c->rcv_nxt) {
        c->rcv_nxt += 1; // FIN occupies one sequence number
        c->peer_fin = true;
        tcp_ack(c);
        if (c->state == TCP_FIN_WAIT) {
            c->state = TCP_DONE;
        }
    }
    if (c->state == TCP_FIN_WAIT && c->retx_len == 0 && c->peer_fin) {
        c->state = TCP_DONE;
    }
}

// Retransmit the outstanding segment if the RTO has elapsed; called from the
// poll pump for every open connection.
void tcp_tick(void)
{
    uint64_t now = ktime_ms();
    for (int i = 0; i < TCP_CONNS; i++) {
        tcp_conn* c = &tcp_conns[i];
        if (!c->in_use || c->retx_len == 0) {
            continue;
        }
        if (now - c->retx_at_ms < TCP_RTO_MS) {
            continue;
        }
        if (c->retx_tries >= TCP_MAX_RETX) {
            c->state = TCP_DONE; // peer unreachable
            c->retx_len = 0;
            continue;
        }
        c->retx_tries++;
        c->retx_at_ms = now;
        ip_send(c->peer_ip, IP_PROTO_TCP, c->retx, c->retx_len);
    }
}

// --- TCP public API ---------------------------------------------------------

int net_tcp_connect(uint32_t dst_ip, uint16_t port, uint32_t timeout_ms)
{
    if (!net_ready()) {
        return -1;
    }
    tcp_conn* c = NULL;
    int id = -1;
    for (int i = 0; i < TCP_CONNS; i++) {
        if (!tcp_conns[i].in_use) {
            c = &tcp_conns[i];
            id = i;
            break;
        }
    }
    if (!c) {
        return -1;
    }
    memset(c, 0, sizeof(*c));
    c->in_use = true;
    c->state = TCP_SYN_SENT;
    c->peer_ip = dst_ip;
    c->peer_port = port;
    c->local_port = net_next_ephemeral();
    c->snd_una = c->snd_nxt = (uint32_t)ktime_ns();
    tcp_emit(c, TCP_SYN, NULL, 0);

    uint64_t deadline = ktime_ms() + timeout_ms;
    while (c->state == TCP_SYN_SENT) {
        net_poll();
        if (c->state == TCP_DONE || ktime_ms() >= deadline) {
            c->in_use = false;
            return -1; // refused, unreachable, or timed out
        }
    }
    return id;
}

int net_tcp_listen(uint16_t port)
{
    if (!net_ready()) {
        return -1;
    }
    for (int i = 0; i < TCP_CONNS; i++) {
        if (!tcp_conns[i].in_use) {
            tcp_conn* c = &tcp_conns[i];
            memset(c, 0, sizeof(*c));
            c->in_use = true;
            c->state = TCP_LISTEN;
            c->local_port = port;
            return i;
        }
    }
    return -1;
}

int net_tcp_accept(int id, uint32_t timeout_ms)
{
    if (id < 0 || id >= TCP_CONNS || !tcp_conns[id].in_use) {
        return -1;
    }
    tcp_conn* c = &tcp_conns[id];
    uint64_t deadline = ktime_ms() + timeout_ms;
    while (c->state != TCP_ESTABLISHED) {
        net_poll();
        if (c->state == TCP_DONE || ktime_ms() >= deadline) {
            return -1;
        }
    }
    return id;
}

int net_tcp_send(int id, const void* data, uint32_t len)
{
    if (id < 0 || id >= TCP_CONNS || !tcp_conns[id].in_use) {
        return -1;
    }
    tcp_conn* c = &tcp_conns[id];
    const uint8_t* p = data;
    uint32_t sent = 0;
    while (sent < len) {
        if (c->state != TCP_ESTABLISHED) {
            return sent ? (int)sent : -1;
        }
        uint16_t chunk =
                (uint16_t)((len - sent > TCP_MSS) ? TCP_MSS : (len - sent));
        tcp_emit(c, TCP_ACK | TCP_PSH, p + sent, chunk);
        // Stop-and-wait: block until this segment is acknowledged.
        uint64_t deadline = ktime_ms() + (TCP_RTO_MS * (TCP_MAX_RETX + 2));
        while (c->retx_len != 0) {
            net_poll();
            if (c->state == TCP_DONE || ktime_ms() >= deadline) {
                return sent ? (int)sent : -1;
            }
        }
        sent += chunk;
    }
    return (int)sent;
}

// Returns >0 bytes read, 0 at clean end-of-stream, or -1 on timeout.
int net_tcp_recv(int id, void* buf, uint32_t cap, uint32_t timeout_ms)
{
    if (id < 0 || id >= TCP_CONNS || !tcp_conns[id].in_use) {
        return -1;
    }
    tcp_conn* c = &tcp_conns[id];
    uint64_t deadline = ktime_ms() + timeout_ms;
    while (c->rx_len == 0) {
        if (c->peer_fin || c->state == TCP_DONE) {
            return 0; // no buffered data and the stream is finished
        }
        net_poll();
        if (ktime_ms() >= deadline) {
            return -1;
        }
    }
    uint32_t n = c->rx_len < cap ? c->rx_len : cap;
    uint8_t* out = buf;
    for (uint32_t i = 0; i < n; i++) {
        out[i] = c->rx[(c->rx_head + i) % TCP_RXBUF];
    }
    c->rx_head = (c->rx_head + n) % TCP_RXBUF;
    c->rx_len -= n;
    return (int)n;
}

void net_tcp_close(int id)
{
    if (id < 0 || id >= TCP_CONNS || !tcp_conns[id].in_use) {
        return;
    }
    tcp_conn* c = &tcp_conns[id];
    if (c->state == TCP_ESTABLISHED) {
        tcp_emit(c, TCP_FIN | TCP_ACK, NULL, 0);
        c->state = TCP_FIN_WAIT;
        uint64_t deadline = ktime_ms() + 1000;
        while (c->state == TCP_FIN_WAIT && ktime_ms() < deadline) {
            net_poll();
        }
    }
    c->in_use = false;
}
