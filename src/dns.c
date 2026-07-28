// A minimal DNS resolver: one A-record query over UDP to the server learned
// from DHCP (net_dns_server), parsing the first A answer. Handles the 0xC0 name
// compression pointers found in real replies. Poll-driven and BSP-only. See
// docs/networking.md.

#include <dns.h>
#include <net.h>
#include <net_internal.h> // htons / ntohs
#include <str.h>

#include <stddef.h> // NULL

// DNS message layout (RFC 1035). The header is fixed; questions and resource
// records that follow it carry variable-length names, so only their fixed parts
// are structs. All multi-byte fields are big-endian (htons/ntohs).
struct dns_header {
    uint16_t id;
    uint16_t flags;
    uint16_t qdcount; // questions
    uint16_t ancount; // answer RRs
    uint16_t nscount; // authority RRs
    uint16_t arcount; // additional RRs
} __attribute__((packed));

struct dns_question { // the fixed trailer after a question's name
    uint16_t qtype;
    uint16_t qclass;
} __attribute__((packed));

struct dns_rr { // the fixed part of a resource record, after its name
    uint16_t type;
    uint16_t rclass;
    uint32_t ttl;
    uint16_t rdlength;
} __attribute__((packed));

#define DNS_TYPE_A 1       // IPv4 address record
#define DNS_CLASS_IN 1     // Internet class
#define DNS_FLAG_RD 0x0100 // recursion desired
#define DNS_RCODE_MASK 0x000F

static uint16_t nonce16(void)
{
    uint32_t lo;
    __asm__ __volatile__("rdtsc" : "=a"(lo) : : "edx");
    return (uint16_t)(lo ^ (lo >> 16));
}

// Encode `host` as a DNS query into `q`; return its length, or -1 on a bad
// name.
static int build_query(uint8_t* q, uint16_t id, const char* host)
{
    struct dns_header* h = (struct dns_header*)q;
    h->id = htons(id);
    h->flags = htons(DNS_FLAG_RD);
    h->qdcount = htons(1);
    h->ancount = 0;
    h->nscount = 0;
    h->arcount = 0;
    int o = sizeof(struct dns_header);
    str rest = str_from(host);
    while (rest.len > 0) {
        str label;
        str_cut_ch(rest, '.', &label, &rest); // no '.': label = rest, rest = ""
        if (label.len == 0 || label.len > 63 || o + 1 + (int)label.len > 500) {
            return -1;
        }
        q[o++] = (uint8_t)label.len;
        for (size_t i = 0; i < label.len; i++) {
            q[o++] = (uint8_t)label.data[i];
        }
    }
    q[o++] = 0; // root label
    struct dns_question* qn = (struct dns_question*)(q + o);
    qn->qtype = htons(DNS_TYPE_A);
    qn->qclass = htons(DNS_CLASS_IN);
    return o + (int)sizeof(struct dns_question);
}

// Advance past a DNS name at offset `o` (labels or a compression pointer).
static int skip_name(const uint8_t* m, int len, int o)
{
    while (o < len) {
        uint8_t b = m[o];
        if ((b & 0xC0) == 0xC0) {
            return o + 2; // 2-byte pointer ends the name
        }
        if (b == 0) {
            return o + 1;
        }
        o += 1 + b;
    }
    return -1;
}

static bool parse_answer(const uint8_t* m, int len, uint16_t id,
                         uint32_t* out_ip)
{
    if (len < (int)sizeof(struct dns_header)) {
        return false;
    }
    const struct dns_header* h = (const struct dns_header*)m;
    if (ntohs(h->id) != id) {
        return false;
    }
    if ((ntohs(h->flags) & DNS_RCODE_MASK) != 0) {
        return false; // non-zero RCODE (e.g. NXDOMAIN)
    }
    int qd = ntohs(h->qdcount);
    int an = ntohs(h->ancount);
    int o = sizeof(struct dns_header);
    for (int i = 0; i < qd; i++) {
        o = skip_name(m, len, o);
        if (o < 0 || o + (int)sizeof(struct dns_question) > len) {
            return false;
        }
        o += sizeof(struct dns_question); // QTYPE + QCLASS
    }
    for (int i = 0; i < an; i++) {
        o = skip_name(m, len, o);
        if (o < 0 || o + (int)sizeof(struct dns_rr) > len) {
            return false;
        }
        const struct dns_rr* rr = (const struct dns_rr*)(m + o);
        int type = ntohs(rr->type);
        int rdlen = ntohs(rr->rdlength);
        o += sizeof(struct dns_rr);
        if (o + rdlen > len) {
            return false;
        }
        if (type == DNS_TYPE_A && rdlen == 4) { // A record
            // Assemble the 4 big-endian RDATA bytes into a host-order address by
            // hand — an ntohl that stays endian-agnostic and avoids an unaligned
            // 4-byte load into the middle of the packet buffer.
            *out_ip = ((uint32_t)m[o] << 24) | ((uint32_t)m[o + 1] << 16) |
                      ((uint32_t)m[o + 2] << 8) | (uint32_t)m[o + 3];
            return true;
        }
        o += rdlen;
    }
    return false;
}

bool net_resolve(const char* host, uint32_t timeout_ms, uint32_t* out_ip)
{
    if (net_aton(host, out_ip)) {
        return true; // already a dotted-quad literal
    }
    int sock = net_udp_open();
    if (sock < 0) {
        return false;
    }
    uint8_t q[512];
    uint16_t id = nonce16();
    int qlen = build_query(q, id, host);
    if (qlen < 0) {
        net_udp_close(sock);
        return false;
    }

    // Try the DHCP-provided resolver first, then a public one (8.8.8.8, reached
    // via the gateway). QEMU's user-mode forwarder (10.0.2.3) relays to the
    // host's resolver, which isn't always reachable; the public fallback keeps
    // resolution working either way.
    const uint32_t server_ips[2] = {
            net_dns_server(),
            (8u << 24) | (8u << 16) | (8u << 8) | 8u,
    };
    uint32_t slot = timeout_ms / 4;
    if (slot < 500) {
        slot = 500;
    }
    bool ok = false;
    for (int s = 0; s < 2 && !ok; s++) {
        for (int attempt = 0; attempt < 2 && !ok; attempt++) {
            if (!net_udp_sendto(sock, server_ips[s], 53, q, (uint16_t)qlen)) {
                break; // e.g. ARP failed for this server; try the next
            }
            uint8_t r[512];
            int n = net_udp_recvfrom(sock, slot, r, sizeof r, NULL, NULL);
            if (n >= (int)sizeof(struct dns_header)) {
                ok = parse_answer(r, n, id, out_ip);
            }
        }
    }
    net_udp_close(sock);
    return ok;
}
