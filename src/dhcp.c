// DHCP client (RFC 2131 DORA): DISCOVER -> OFFER -> REQUEST -> ACK. Runs over
// the existing UDP sockets for receive (bound to port 68) and the broadcast IP
// path (ip_send_bcast) for transmit, since replies and the initial request are
// broadcast and there is no lease yet. Poll-driven and BSP-only like the rest
// of the stack. See docs/networking.md.

#include <dhcp.h>
#include <net.h>
#include <net_internal.h>
#include <console.h>
#include <utils.h> // memset/memcpy

#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68
#define BOOTREQUEST 1                 // BOOTP op: client -> server
#define BOOTREPLY 2                   // BOOTP op: server -> client
#define DHCP_MAGIC_COOKIE 0x63825363u // precedes the options (RFC 2131)

#define DHCP_DISCOVER 1
#define DHCP_OFFER 2
#define DHCP_REQUEST 3
#define DHCP_ACK 5

#define OPT_MASK 1
#define OPT_ROUTER 3
#define OPT_DNS 6
#define OPT_MSGTYPE 53
#define OPT_REQIP 50
#define OPT_SERVERID 54
#define OPT_PARAMLIST 55
#define OPT_END 255

#define DHCP_FIXED 236 // op..file, before the magic cookie

// Fixed head of a BOOTP/DHCP message (options follow the magic cookie at 236).
typedef struct __attribute__((packed)) {
    uint8_t op, htype, hlen, hops;
    uint32_t xid;
    uint16_t secs, flags;
    uint32_t ciaddr, yiaddr, siaddr, giaddr;
    uint8_t chaddr[16];
    uint8_t sname[64];
    uint8_t file[128];
} dhcp_hdr;

typedef struct __attribute__((packed)) {
    uint16_t sport, dport, len, csum;
} udp_hdr_t;

static uint32_t g_dns; // DNS server learned from the lease (host order)

// A cheap 32-bit nonce for the transaction id (TSC low/high mixed).
static uint32_t nonce32(void)
{
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return lo ^ (hi << 13) ^ (hi >> 7);
}

static void put32(uint8_t* p, uint32_t v) // host -> network (big-endian)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}
static uint32_t get32(const uint8_t* p) // network -> host
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

// Build a UDP(68->67) + DHCP message into `buf`; return its total length.
static int build(uint8_t* buf, uint32_t xid, uint8_t type, uint32_t req_ip,
                 uint32_t server_id, const uint8_t mac[6])
{
    udp_hdr_t* u = (udp_hdr_t*)buf;
    uint8_t* msg = buf + sizeof(udp_hdr_t);
    memset(msg, 0, DHCP_FIXED);
    dhcp_hdr* d = (dhcp_hdr*)msg;
    d->op = BOOTREQUEST;
    d->htype = 1;
    d->hlen = 6;
    d->xid = htonl(xid);
    d->flags = htons(0x8000); // ask the server to broadcast its reply
    memcpy(d->chaddr, mac, 6);

    uint8_t* o = msg + DHCP_FIXED;
    put32(o, DHCP_MAGIC_COOKIE); // precedes the options
    o += 4;
    *o++ = OPT_MSGTYPE;
    *o++ = 1;
    *o++ = type;
    if (type == DHCP_REQUEST) {
        *o++ = OPT_REQIP;
        *o++ = 4;
        put32(o, req_ip);
        o += 4;
        *o++ = OPT_SERVERID;
        *o++ = 4;
        put32(o, server_id);
        o += 4;
    }
    *o++ = OPT_PARAMLIST;
    *o++ = 3;
    *o++ = OPT_MASK;
    *o++ = OPT_ROUTER;
    *o++ = OPT_DNS;
    *o++ = OPT_END;

    uint16_t msglen = (uint16_t)(o - msg);
    uint16_t udplen = (uint16_t)(sizeof(udp_hdr_t) + msglen);
    u->sport = htons(DHCP_CLIENT_PORT);
    u->dport = htons(DHCP_SERVER_PORT);
    u->len = htons(udplen);
    u->csum = 0; // optional for IPv4
    return (int)udplen;
}

// The address configuration a reply carries (0 for options the server omits).
typedef struct {
    uint32_t ip;         // yiaddr — offered/leased address
    uint32_t mask;       // option 1  — subnet mask
    uint32_t gateway_ip; // option 3  — router
    uint32_t dns_ip;     // option 6  — first DNS server
    uint32_t server_ip;  // option 54 — DHCP server identifier
} dhcp_lease;

// One DISCOVER/REQUEST round: the socket/identity context plus what to send and
// which reply type to accept.
typedef struct {
    int sock;
    const uint8_t* mac;
    uint32_t xid;
    uint32_t slot_ms;   // per-attempt receive timeout
    uint8_t type;       // message to send (DISCOVER / REQUEST)
    uint8_t want;       // reply type to accept (OFFER / ACK)
    uint32_t req_ip;    // option 50 (REQUEST only)
    uint32_t server_ip; // option 54 (REQUEST only)
} dhcp_req;

// Parse a reply; require op==BOOTREPLY, matching xid, and option 53 == `want`.
// Fills `out` with the offered address and any options present.
static bool parse(const uint8_t* d, int len, uint32_t xid, uint8_t want,
                  dhcp_lease* out)
{
    if (len < DHCP_FIXED + 4) {
        return false;
    }
    const dhcp_hdr* m = (const dhcp_hdr*)d;
    if (m->op != BOOTREPLY || ntohl(m->xid) != xid) {
        return false;
    }
    const uint8_t* o = d + DHCP_FIXED;
    if (get32(o) != DHCP_MAGIC_COOKIE) {
        return false;
    }
    o += 4;
    out->ip = ntohl(m->yiaddr);
    out->mask = out->gateway_ip = out->dns_ip = out->server_ip = 0;
    uint8_t mtype = 0;
    const uint8_t* end = d + len;
    while (o < end && *o != OPT_END) {
        uint8_t code = *o++;
        if (code == 0) {
            continue; // pad
        }
        if (o >= end) {
            break;
        }
        uint8_t l = *o++;
        if (o + l > end) {
            break;
        }
        if (code == OPT_MSGTYPE && l >= 1) {
            mtype = o[0];
        } else if (code == OPT_MASK && l >= 4) {
            out->mask = get32(o);
        } else if (code == OPT_ROUTER && l >= 4) {
            out->gateway_ip = get32(o);
        } else if (code == OPT_DNS && l >= 4) {
            out->dns_ip = get32(o); // first DNS server
        } else if (code == OPT_SERVERID && l >= 4) {
            out->server_ip = get32(o);
        }
        o += l;
    }
    return mtype == want;
}

// Send `req->type` (broadcast, src 0.0.0.0) and wait up to `req->slot_ms` for a
// matching reply. Returns true and fills `*out` on success.
static bool exchange(const dhcp_req* req, dhcp_lease* out)
{
    for (int attempt = 0; attempt < 3; attempt++) {
        uint8_t buf[600];
        int n = build(buf, req->xid, req->type, req->req_ip, req->server_ip,
                      req->mac);
        ip_send_bcast(0, IP_PROTO_UDP, buf, (uint16_t)n);

        uint8_t reply[600];
        int r = net_udp_recvfrom(req->sock, req->slot_ms, reply, sizeof reply,
                                 NULL, NULL);
        if (r > 0 && parse(reply, r, req->xid, req->want, out)) {
            return true;
        }
    }
    return false;
}

bool net_dhcp(uint32_t timeout_ms)
{
    uint8_t mac[6];
    net_mac(mac);
    uint32_t xid = nonce32();
    uint32_t slot = timeout_ms / 6;
    if (slot < 200) {
        slot = 200;
    }

    int sock = net_udp_open();
    if (sock < 0) {
        return false;
    }
    if (!net_udp_bind(sock, DHCP_CLIENT_PORT)) {
        net_udp_close(sock);
        return false;
    }

    dhcp_req req = {
            .sock = sock,
            .mac = mac,
            .xid = xid,
            .slot_ms = slot,
            .type = DHCP_DISCOVER,
            .want = DHCP_OFFER,
    };
    dhcp_lease lease = {0};
    bool ok = exchange(&req, &lease);
    if (ok) {
        // Confirm the offer with a REQUEST; the ACK's options win where
        // present.
        req.type = DHCP_REQUEST;
        req.want = DHCP_ACK;
        req.req_ip = lease.ip;
        req.server_ip = lease.server_ip;
        dhcp_lease ack = {0};
        ok = exchange(&req, &ack);
        if (ok) {
            lease.ip = ack.ip;
            if (ack.mask)
                lease.mask = ack.mask;
            if (ack.gateway_ip)
                lease.gateway_ip = ack.gateway_ip;
            if (ack.dns_ip)
                lease.dns_ip = ack.dns_ip;
        }
    }
    net_udp_close(sock);
    if (!ok) {
        return false;
    }

    if (lease.mask == 0) {
        lease.mask = 0xFFFFFF00u;
    }
    net_config(lease.ip, lease.mask, lease.gateway_ip);
    g_dns = lease.dns_ip;

    char ips[16], gws[16], dnss[16];
    net_ntoa(lease.ip, ips);
    net_ntoa(lease.gateway_ip, gws);
    net_ntoa(net_dns_server(), dnss);
    console_print("juampiOS: net DHCP lease ");
    console_print(ips);
    console_print(" via ");
    console_print(gws);
    console_print(", dns ");
    console_print(dnss);
    console_print("\n");
    return true;
}

uint32_t net_dns_server(void)
{
    // QEMU user-mode DNS forwarder as the fallback (10.0.2.3).
    return g_dns ? g_dns : ((10u << 24) | (0u << 16) | (2u << 8) | 3u);
}
