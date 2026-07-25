// A minimal DNS resolver: one A-record query over UDP to the server learned
// from DHCP (net_dns_server), parsing the first A answer. Handles the 0xC0 name
// compression pointers found in real replies. Poll-driven and BSP-only. See
// docs/networking.md.

#include <dns.h>
#include <net.h>

#include <stddef.h> // NULL

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
    q[0] = (uint8_t)(id >> 8);
    q[1] = (uint8_t)id;
    q[2] = 0x01; // RD (recursion desired)
    q[3] = 0x00;
    q[4] = 0x00;
    q[5] = 0x01; // qdcount = 1
    q[6] = q[7] = q[8] = q[9] = q[10] = q[11] = 0;
    int o = 12;
    const char* p = host;
    while (*p) {
        int len = 0;
        while (p[len] && p[len] != '.') {
            len++;
        }
        if (len == 0 || len > 63 || o + 1 + len > 500) {
            return -1;
        }
        q[o++] = (uint8_t)len;
        for (int i = 0; i < len; i++) {
            q[o++] = (uint8_t)p[i];
        }
        p += len;
        if (*p == '.') {
            p++;
        }
    }
    q[o++] = 0;    // root label
    q[o++] = 0x00; // QTYPE = A
    q[o++] = 0x01;
    q[o++] = 0x00; // QCLASS = IN
    q[o++] = 0x01;
    return o;
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

static bool parse_answer(const uint8_t* m, int len, uint16_t id, uint32_t* out)
{
    if (len < 12 || (((uint16_t)m[0] << 8) | m[1]) != id) {
        return false;
    }
    if ((m[3] & 0x0F) != 0) {
        return false; // non-zero RCODE (e.g. NXDOMAIN)
    }
    int qd = (m[4] << 8) | m[5];
    int an = (m[6] << 8) | m[7];
    int o = 12;
    for (int i = 0; i < qd; i++) {
        o = skip_name(m, len, o);
        if (o < 0 || o + 4 > len) {
            return false;
        }
        o += 4; // QTYPE + QCLASS
    }
    for (int i = 0; i < an; i++) {
        o = skip_name(m, len, o);
        if (o < 0 || o + 10 > len) {
            return false;
        }
        int type = (m[o] << 8) | m[o + 1];
        int rdlen = (m[o + 8] << 8) | m[o + 9];
        o += 10;
        if (o + rdlen > len) {
            return false;
        }
        if (type == 1 && rdlen == 4) { // A record
            *out = ((uint32_t)m[o] << 24) | ((uint32_t)m[o + 1] << 16) |
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
    const uint32_t servers[2] = {
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
            if (!net_udp_sendto(sock, servers[s], 53, q, (uint16_t)qlen)) {
                break; // e.g. ARP failed for this server; try the next
            }
            uint8_t r[512];
            int n = net_udp_recvfrom(sock, slot, r, sizeof r, NULL, NULL);
            if (n >= 12) {
                ok = parse_answer(r, n, id, out_ip);
            }
        }
    }
    net_udp_close(sock);
    return ok;
}
