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
    d->op = 1; // BOOTREQUEST
    d->htype = 1;
    d->hlen = 6;
    d->xid = htonl(xid);
    d->flags = htons(0x8000); // ask the server to broadcast its reply
    memcpy(d->chaddr, mac, 6);

    uint8_t* o = msg + DHCP_FIXED;
    *o++ = 0x63; // magic cookie 63 82 53 63
    *o++ = 0x82;
    *o++ = 0x53;
    *o++ = 0x63;
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
    u->sport = htons(68);
    u->dport = htons(67);
    u->len = htons(udplen);
    u->csum = 0; // optional for IPv4
    return (int)udplen;
}

// Parse a reply; require op==BOOTREPLY, matching xid, and option 53 == `want`.
// Fills the offered address and any options present (0 when absent).
static bool parse(const uint8_t* d, int len, uint32_t xid, uint8_t want,
                  uint32_t* yiaddr, uint32_t* mask, uint32_t* router,
                  uint32_t* dns, uint32_t* server_id)
{
    if (len < DHCP_FIXED + 4) {
        return false;
    }
    const dhcp_hdr* m = (const dhcp_hdr*)d;
    if (m->op != 2 || ntohl(m->xid) != xid) {
        return false;
    }
    const uint8_t* o = d + DHCP_FIXED;
    if (o[0] != 0x63 || o[1] != 0x82 || o[2] != 0x53 || o[3] != 0x63) {
        return false;
    }
    o += 4;
    *yiaddr = ntohl(m->yiaddr);
    *mask = *router = *dns = *server_id = 0;
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
            *mask = get32(o);
        } else if (code == OPT_ROUTER && l >= 4) {
            *router = get32(o);
        } else if (code == OPT_DNS && l >= 4) {
            *dns = get32(o); // first DNS server
        } else if (code == OPT_SERVERID && l >= 4) {
            *server_id = get32(o);
        }
        o += l;
    }
    return mtype == want;
}

// Send `type` (broadcast, src 0.0.0.0) and wait up to `slot_ms` for a matching
// reply. Returns true and fills the outputs on success.
static bool exchange(int sock, const uint8_t mac[6], uint32_t xid, uint8_t type,
                     uint8_t want, uint32_t req_ip, uint32_t server_id,
                     uint32_t slot_ms, uint32_t* yiaddr, uint32_t* mask,
                     uint32_t* router, uint32_t* dns, uint32_t* sid)
{
    for (int attempt = 0; attempt < 3; attempt++) {
        uint8_t out[600];
        int n = build(out, xid, type, req_ip, server_id, mac);
        ip_send_bcast(0, IP_PROTO_UDP, out, (uint16_t)n);

        uint8_t in[600];
        int r = net_udp_recvfrom(sock, slot_ms, in, sizeof in, NULL, NULL);
        if (r > 0 && parse(in, r, xid, want, yiaddr, mask, router, dns, sid)) {
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
    if (!net_udp_bind(sock, 68)) {
        net_udp_close(sock);
        return false;
    }

    uint32_t yi = 0, mask = 0, gw = 0, dns = 0, sid = 0;
    bool ok = exchange(sock, mac, xid, DHCP_DISCOVER, DHCP_OFFER, 0, 0, slot,
                       &yi, &mask, &gw, &dns, &sid);
    if (ok) {
        uint32_t ayi = 0, amask = 0, agw = 0, adns = 0, asid = 0;
        ok = exchange(sock, mac, xid, DHCP_REQUEST, DHCP_ACK, yi, sid, slot,
                      &ayi, &amask, &agw, &adns, &asid);
        if (ok) {
            yi = ayi;
            if (amask)
                mask = amask;
            if (agw)
                gw = agw;
            if (adns)
                dns = adns;
        }
    }
    net_udp_close(sock);
    if (!ok) {
        return false;
    }

    if (mask == 0) {
        mask = 0xFFFFFF00u;
    }
    net_config(yi, mask, gw);
    g_dns = dns;

    char ips[16], gws[16], dnss[16];
    net_ntoa(yi, ips);
    net_ntoa(gw, gws);
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
