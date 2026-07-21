// A minimal IPv4 stack — Ethernet + ARP + IPv4 + ICMP, enough to ping — riding
// on the e1000 driver. Poll-driven and BSP-only, so no locking. IP addresses
// are host byte order internally; conversion happens only at the wire. See
// docs/networking.md.

#include <net.h>
#include <console.h>
#include <e1000.h>
#include <ktime.h>
#include <utils.h>

#define ETH_ARP 0x0806
#define ETH_IP 0x0800
#define IP_PROTO_ICMP 1

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

typedef struct __attribute__((packed)) {
    uint8_t dst[6];
    uint8_t src[6];
    uint16_t type;
} eth_hdr;

typedef struct __attribute__((packed)) {
    uint16_t htype;
    uint16_t ptype;
    uint8_t hlen;
    uint8_t plen;
    uint16_t oper;
    uint8_t sha[6];
    uint8_t spa[4];
    uint8_t tha[6];
    uint8_t tpa[4];
} arp_pkt;

typedef struct __attribute__((packed)) {
    uint8_t ver_ihl;
    uint8_t tos;
    uint16_t tot_len;
    uint16_t id;
    uint16_t frag;
    uint8_t ttl;
    uint8_t proto;
    uint16_t csum;
    uint32_t src;
    uint32_t dst;
} ip_hdr;

typedef struct __attribute__((packed)) {
    uint8_t type;
    uint8_t code;
    uint16_t csum;
    uint16_t id;
    uint16_t seq;
} icmp_hdr;

#define ICMP_ECHO_REQUEST 8
#define ICMP_ECHO_REPLY 0

static bool ready;
static uint32_t my_ip, my_mask, my_gw;
static uint8_t my_mac[6];
static uint16_t ip_id_ctr = 1;
static const uint8_t bcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// --- small helpers ----------------------------------------------------------

static bool mac_eq(const uint8_t* a, const uint8_t* b)
{
    for (int i = 0; i < 6; i++) {
        if (a[i] != b[i]) {
            return false;
        }
    }
    return true;
}

static void ip_to_bytes(uint32_t ip, uint8_t out[4])
{
    out[0] = (uint8_t)(ip >> 24);
    out[1] = (uint8_t)(ip >> 16);
    out[2] = (uint8_t)(ip >> 8);
    out[3] = (uint8_t)ip;
}

static uint32_t bytes_to_ip(const uint8_t in[4])
{
    return ((uint32_t)in[0] << 24) | ((uint32_t)in[1] << 16) |
           ((uint32_t)in[2] << 8) | in[3];
}

// RFC 1071 Internet checksum (endian-neutral: computed and stored in the same
// byte order).
static uint16_t inet_csum(const void* data, uint32_t len)
{
    const uint16_t* p = data;
    uint32_t sum = 0;
    while (len > 1) {
        sum += *p++;
        len -= 2;
    }
    if (len) {
        sum += *(const uint8_t*)p;
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

// --- ARP cache --------------------------------------------------------------

#define ARP_ENTRIES 8
static struct {
    uint32_t ip;
    uint8_t mac[6];
    bool valid;
} arp_cache[ARP_ENTRIES];

static const uint8_t* arp_lookup(uint32_t ip)
{
    for (int i = 0; i < ARP_ENTRIES; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            return arp_cache[i].mac;
        }
    }
    return NULL;
}

static void arp_insert(uint32_t ip, const uint8_t* mac)
{
    int slot = 0;
    for (int i = 0; i < ARP_ENTRIES; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            slot = i;
            goto set;
        }
        if (!arp_cache[i].valid) {
            slot = i;
        }
    }
set:
    arp_cache[slot].ip = ip;
    memcpy(arp_cache[slot].mac, mac, 6);
    arp_cache[slot].valid = true;
}

static bool eth_send(const uint8_t dst[6], uint16_t ethertype, const void* l3,
                     uint16_t l3len)
{
    static uint8_t frame[1600];
    eth_hdr* eth = (eth_hdr*)frame;
    memcpy(eth->dst, dst, 6);
    memcpy(eth->src, my_mac, 6);
    eth->type = htons(ethertype);
    memcpy(frame + sizeof(eth_hdr), l3, l3len);
    return e1000_tx(frame, (uint16_t)(sizeof(eth_hdr) + l3len));
}

static void arp_send_request(uint32_t target)
{
    arp_pkt a;
    a.htype = htons(1);
    a.ptype = htons(ETH_IP);
    a.hlen = 6;
    a.plen = 4;
    a.oper = htons(1); // request
    memcpy(a.sha, my_mac, 6);
    ip_to_bytes(my_ip, a.spa);
    memset(a.tha, 0, 6);
    ip_to_bytes(target, a.tpa);
    eth_send(bcast_mac, ETH_ARP, &a, sizeof(a));
}

static void arp_input(const uint8_t* data, uint16_t len)
{
    if (len < sizeof(arp_pkt)) {
        return;
    }
    const arp_pkt* a = (const arp_pkt*)data;
    if (ntohs(a->ptype) != ETH_IP || a->plen != 4) {
        return;
    }
    uint32_t spa = bytes_to_ip(a->spa);
    uint32_t tpa = bytes_to_ip(a->tpa);
    arp_insert(spa, a->sha); // learn the sender either way

    if (ntohs(a->oper) == 1 && tpa == my_ip) { // request for us -> reply
        arp_pkt r;
        r.htype = htons(1);
        r.ptype = htons(ETH_IP);
        r.hlen = 6;
        r.plen = 4;
        r.oper = htons(2); // reply
        memcpy(r.sha, my_mac, 6);
        ip_to_bytes(my_ip, r.spa);
        memcpy(r.tha, a->sha, 6);
        ip_to_bytes(spa, r.tpa);
        eth_send(a->sha, ETH_ARP, &r, sizeof(r));
    }
}

// Resolve `ip` to a MAC, sending a request and pumping the stack until it
// answers or `timeout_ms` elapses.
static bool arp_resolve(uint32_t ip, uint8_t out[6], uint32_t timeout_ms)
{
    const uint8_t* m = arp_lookup(ip);
    if (m) {
        memcpy(out, m, 6);
        return true;
    }
    arp_send_request(ip);
    uint64_t deadline = ktime_ms() + timeout_ms;
    while (ktime_ms() < deadline) {
        net_poll();
        m = arp_lookup(ip);
        if (m) {
            memcpy(out, m, 6);
            return true;
        }
    }
    return false;
}

// --- IPv4 output ------------------------------------------------------------

static bool ip_send(uint32_t dst, uint8_t proto, const void* l4, uint16_t l4len)
{
    uint32_t nexthop = ((dst & my_mask) == (my_ip & my_mask)) ? dst : my_gw;
    uint8_t dmac[6];
    if (!arp_resolve(nexthop, dmac, 1000)) {
        return false;
    }

    static uint8_t pkt[1600];
    ip_hdr* ip = (ip_hdr*)pkt;
    ip->ver_ihl = 0x45;
    ip->tos = 0;
    ip->tot_len = htons((uint16_t)(sizeof(ip_hdr) + l4len));
    ip->id = htons(ip_id_ctr++);
    ip->frag = htons(0x4000); // don't fragment
    ip->ttl = 64;
    ip->proto = proto;
    ip->csum = 0;
    ip->src = htonl(my_ip);
    ip->dst = htonl(dst);
    ip->csum = inet_csum(ip, sizeof(ip_hdr));
    memcpy(pkt + sizeof(ip_hdr), l4, l4len);
    return eth_send(dmac, ETH_IP, pkt, (uint16_t)(sizeof(ip_hdr) + l4len));
}

// --- ICMP -------------------------------------------------------------------

#define PING_ID 0x4A50 // 'JP'
#define PING_PAYLOAD 32
static volatile bool ping_got;
static uint16_t ping_seq;
static uint64_t ping_send_us;
static uint64_t ping_rtt_us;

static void icmp_input(uint32_t src, const uint8_t* data, uint16_t len)
{
    if (len < sizeof(icmp_hdr)) {
        return;
    }
    const icmp_hdr* ic = (const icmp_hdr*)data;

    if (ic->type == ICMP_ECHO_REQUEST) {
        // Echo it back: same id/seq/payload, type -> reply, recompute csum.
        static uint8_t rep[1500];
        if (len > sizeof(rep)) {
            return;
        }
        memcpy(rep, data, len);
        icmp_hdr* r = (icmp_hdr*)rep;
        r->type = ICMP_ECHO_REPLY;
        r->csum = 0;
        r->csum = inet_csum(rep, len);
        ip_send(src, IP_PROTO_ICMP, rep, len);
    } else if (ic->type == ICMP_ECHO_REPLY) {
        if (ntohs(ic->id) == PING_ID && ntohs(ic->seq) == ping_seq) {
            ping_rtt_us = ktime_us() - ping_send_us;
            ping_got = true;
        }
    }
}

// --- IPv4 input -------------------------------------------------------------

static void ip_input(const uint8_t* data, uint16_t len)
{
    if (len < sizeof(ip_hdr)) {
        return;
    }
    const ip_hdr* ip = (const ip_hdr*)data;
    if ((ip->ver_ihl >> 4) != 4) {
        return;
    }
    uint32_t ihl = (uint32_t)(ip->ver_ihl & 0x0F) * 4;
    if (ihl < sizeof(ip_hdr) || ihl > len) {
        return;
    }
    uint32_t dst = ntohl(ip->dst);
    if (dst != my_ip) {
        return; // not for us (we don't accept broadcast IP yet)
    }
    uint16_t tot = ntohs(ip->tot_len);
    if (tot > len || tot < ihl) {
        return;
    }
    if (ip->proto == IP_PROTO_ICMP) {
        icmp_input(ntohl(ip->src), data + ihl, (uint16_t)(tot - ihl));
    }
}

// --- Ethernet input + poll pump --------------------------------------------

static void ether_input(const uint8_t* data, uint16_t len)
{
    if (len < sizeof(eth_hdr)) {
        return;
    }
    const eth_hdr* eth = (const eth_hdr*)data;
    // Accept broadcast and frames addressed to us; the card already filters,
    // but be defensive.
    if (!mac_eq(eth->dst, my_mac) && !mac_eq(eth->dst, bcast_mac)) {
        return;
    }
    uint16_t type = ntohs(eth->type);
    const uint8_t* payload = data + sizeof(eth_hdr);
    uint16_t plen = (uint16_t)(len - sizeof(eth_hdr));
    if (type == ETH_ARP) {
        arp_input(payload, plen);
    } else if (type == ETH_IP) {
        ip_input(payload, plen);
    }
}

void net_poll(void)
{
    if (!ready) {
        return;
    }
    e1000_frame f;
    while (e1000_rx_poll(&f)) {
        ether_input(f.data, f.len);
    }
}

// --- public API -------------------------------------------------------------

bool net_ping(uint32_t dst, uint32_t timeout_ms, uint64_t* rtt_us)
{
    if (!ready) {
        return false;
    }
    ping_seq++;
    ping_got = false;
    ping_send_us = ktime_us();

    uint8_t l4[sizeof(icmp_hdr) + PING_PAYLOAD];
    icmp_hdr* ic = (icmp_hdr*)l4;
    ic->type = ICMP_ECHO_REQUEST;
    ic->code = 0;
    ic->csum = 0;
    ic->id = htons(PING_ID);
    ic->seq = htons(ping_seq);
    for (int i = 0; i < PING_PAYLOAD; i++) {
        l4[sizeof(icmp_hdr) + i] = (uint8_t)i;
    }
    ic->csum = inet_csum(l4, sizeof(l4));

    if (!ip_send(dst, IP_PROTO_ICMP, l4, sizeof(l4))) {
        return false;
    }

    uint64_t deadline = ktime_ms() + timeout_ms;
    while (!ping_got) {
        net_poll();
        if (ktime_ms() >= deadline) {
            return false;
        }
    }
    if (rtt_us) {
        *rtt_us = ping_rtt_us;
    }
    return true;
}

void net_config(uint32_t ip, uint32_t mask, uint32_t gateway)
{
    my_ip = ip;
    my_mask = mask;
    my_gw = gateway;
}

uint32_t net_ip(void)
{
    return my_ip;
}

void net_mac(uint8_t out[6])
{
    memcpy(out, my_mac, 6);
}

bool net_ready(void)
{
    return ready;
}

bool net_aton(const char* s, uint32_t* out)
{
    uint32_t parts[4] = {0, 0, 0, 0};
    int idx = 0;
    bool any = false;
    for (const char* p = s;; p++) {
        if (*p >= '0' && *p <= '9') {
            parts[idx] = parts[idx] * 10 + (uint32_t)(*p - '0');
            if (parts[idx] > 255) {
                return false;
            }
            any = true;
        } else if (*p == '.') {
            if (!any || idx == 3) {
                return false;
            }
            idx++;
            any = false;
        } else if (*p == '\0') {
            break;
        } else {
            return false;
        }
    }
    if (idx != 3 || !any) {
        return false;
    }
    *out = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
    return true;
}

void net_ntoa(uint32_t ip, char* buf)
{
    uint8_t b[4];
    ip_to_bytes(ip, b);
    int n = 0;
    for (int i = 0; i < 4; i++) {
        uint8_t v = b[i];
        if (v >= 100) {
            buf[n++] = (char)('0' + v / 100);
        }
        if (v >= 10) {
            buf[n++] = (char)('0' + (v / 10) % 10);
        }
        buf[n++] = (char)('0' + v % 10);
        if (i < 3) {
            buf[n++] = '.';
        }
    }
    buf[n] = '\0';
}

void net_init(void)
{
    if (!e1000_init()) {
        console_print("juampiOS: net no NIC (e1000) found\n");
        return;
    }
    e1000_mac(my_mac);
    // QEMU user-mode networking defaults: 10.0.2.15/24 via 10.0.2.2.
    net_config((10u << 24) | (0u << 16) | (2u << 8) | 15u, 0xFFFFFF00u,
               (10u << 24) | (0u << 16) | (2u << 8) | 2u);
    ready = true;

    static const char hexd[] = "0123456789abcdef";
    char macs[18];
    int n = 0;
    for (int i = 0; i < 6; i++) {
        macs[n++] = hexd[my_mac[i] >> 4];
        macs[n++] = hexd[my_mac[i] & 0xF];
        if (i < 5) {
            macs[n++] = ':';
        }
    }
    macs[n] = '\0';
    char ips[16];
    net_ntoa(my_ip, ips);
    console_print("juampiOS: net e1000 up, mac=");
    console_print(macs);
    console_print(" ip=");
    console_print(ips);
    console_print("\n");
}
