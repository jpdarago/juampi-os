// TLS client on the vendored BearSSL (src/bearssl/). Wraps a net_tcp connection
// in a TLS 1.2 session: SNI, cert-chain validation against a curated CA set
// (tls_trust_anchors.inc), and blocking read/write via br_sslio over the TCP
// send/recv calls. Entropy is drawn from the CPU RNG (RDRAND, TSC fallback)
// since we don't wire an OS seeder. BSP-only, like the rest of the net stack.
//
// Limitation: BearSSL x509_minimal checks certificate validity against a
// wall-clock time, and fails closed (reports "expired") if none is set. We have
// no RTC, so we feed it a fixed build-date constant (see tls_connect); the
// chain is otherwise cryptographically verified against the trust anchors.

#include <tls.h>
#include <bearssl.h>
#include <net.h>

#include <stdint.h>
#include <stddef.h>

// Curated CA trust anchors, compiled with -w in tls_trust_anchors.c (the
// generated table casts away const, which our gauntlet rejects).
extern const br_x509_trust_anchor* const tls_trust_anchors;
extern const size_t tls_trust_anchors_count;

// --- self-test --------------------------------------------------------------

bool tls_selftest(void)
{
    // SHA-256("abc") = ba7816bf 8f01cfea 414140de 5dae2223
    //                  b00361a3 96177a9c b410ff61 f20015ad
    static const unsigned char expect[32] = {
            0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40,
            0xde, 0x5d, 0xae, 0x22, 0x23, 0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17,
            0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad};
    br_sha256_context ctx;
    br_sha256_init(&ctx);
    br_sha256_update(&ctx, "abc", 3);
    unsigned char out[32];
    br_sha256_out(&ctx, out);
    for (int i = 0; i < 32; i++) {
        if (out[i] != expect[i]) {
            return false;
        }
    }
    return true;
}

// --- entropy (CPU RNG) ------------------------------------------------------

static uint64_t cpu_ticks(void)
{
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static bool cpu_has_rdrand(void)
{
    uint32_t ecx;
    __asm__ __volatile__("cpuid" : "=c"(ecx) : "a"(1) : "ebx", "edx");
    return (ecx & (1u << 30)) != 0;
}

// Fill `buf` with entropy: RDRAND when present (a few retries per word), mixed
// with the TSC as a fallback so we always produce a non-degenerate seed.
static void gather_entropy(unsigned char* buf, size_t n)
{
    bool rd = cpu_has_rdrand();
    for (size_t i = 0; i < n;) {
        uint64_t v = cpu_ticks();
        if (rd) {
            uint64_t r = 0;
            unsigned char ok = 0;
            for (int t = 0; t < 10 && !ok; t++) {
                __asm__ __volatile__("rdrand %0; setc %1" : "=r"(r), "=qm"(ok));
            }
            v ^= r;
        }
        size_t take = (n - i < 8) ? (n - i) : 8;
        for (size_t j = 0; j < take; j++) {
            buf[i + j] = (unsigned char)(v >> (8 * j));
        }
        i += take;
    }
}

// --- TCP-backed low-level I/O for br_sslio ---------------------------------

static int sock_read(void* ctx, unsigned char* buf, size_t len)
{
    int tcp = *(int*)ctx;
    int r = net_tcp_recv(tcp, buf, (uint32_t)len, 15000);
    return r > 0 ? r : -1; // 0 (EOF) / -1 (timeout) both end the record read
}

static int sock_write(void* ctx, const unsigned char* buf, size_t len)
{
    int tcp = *(int*)ctx;
    int r = net_tcp_send(tcp, buf, (uint32_t)len);
    return r > 0 ? r : -1;
}

// --- TLS connection ---------------------------------------------------------

struct tls_conn {
    br_ssl_client_context sc;
    br_x509_minimal_context xc;
    br_sslio_context ioc;
    int tcp;
    unsigned char iobuf[BR_SSL_BUFSIZE_MONO];
};

tls_conn* tls_connect(allocator* a, uint32_t ip, uint16_t port,
                      const char* host)
{
    int tcp = net_tcp_connect(ip, port, 8000);
    if (tcp < 0) {
        return NULL;
    }
    tls_conn* c = new (a, tls_conn, 1);
    c->tcp = tcp;

    // Full client profile: all standard cipher suites + hashes, X.509 minimal
    // validator seeded with our trust anchors.
    br_ssl_client_init_full(&c->sc, &c->xc, tls_trust_anchors,
                            tls_trust_anchors_count);

    // Certificate validity is checked against "now". With no RTC we use a fixed
    // build-date constant — without it BearSSL fails closed (every cert reads
    // as expired). BearSSL's epoch is days since 0000-01-01 (= unix/86400 +
    // 719528) plus seconds-of-day. Bump this, or wire the CMOS RTC, as it
    // drifts.
    br_x509_minimal_set_time(&c->xc, 740187u /* ~2026-07-25 */,
                             43200u /*noon*/);

    // Half-duplex buffer (request fully sent before the response is read).
    br_ssl_engine_set_buffer(&c->sc.eng, c->iobuf, sizeof c->iobuf, 0);

    unsigned char seed[32];
    gather_entropy(seed, sizeof seed);
    br_ssl_engine_inject_entropy(&c->sc.eng, seed, sizeof seed);

    br_ssl_client_reset(&c->sc, host, 0); // SNI = host, no session resumption
    br_sslio_init(&c->ioc, &c->sc.eng, sock_read, &c->tcp, sock_write, &c->tcp);
    return c;
}

int tls_send(tls_conn* c, const void* data, int len)
{
    if (br_sslio_write_all(&c->ioc, data, (size_t)len) != 0) {
        return -1; // includes a failed handshake (e.g. cert not trusted)
    }
    return br_sslio_flush(&c->ioc) == 0 ? len : -1;
}

int tls_recv(tls_conn* c, void* buf, int cap)
{
    int r = br_sslio_read(&c->ioc, buf, (size_t)cap);
    if (r < 0) {
        // A clean close_notify leaves last_error == BR_ERR_OK — report EOF.
        return br_ssl_engine_last_error(&c->sc.eng) == BR_ERR_OK ? 0 : -1;
    }
    return r;
}

int tls_error(tls_conn* c)
{
    return br_ssl_engine_last_error(&c->sc.eng);
}

void tls_close(tls_conn* c)
{
    br_sslio_close(&c->ioc); // best-effort close_notify
    net_tcp_close(c->tcp);
}
