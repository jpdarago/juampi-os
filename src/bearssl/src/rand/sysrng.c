/*
 * Freestanding replacement for BearSSL's sysrng.c. The upstream file probes OS
 * entropy sources (/dev/urandom, getentropy, RDRAND via CPUID, Win32 CryptGen)
 * which don't apply in a ring-0 kernel. We report "no system seeder" here and
 * seed the TLS PRNG explicitly from the CPU RNG (see src/tls.c) via
 * br_ssl_engine_inject_entropy(). This keeps the symbol ssl_engine.c references
 * defined without dragging in any OS dependency.
 */

#include "inner.h"

/* see bearssl_rand.h */
br_prng_seeder
br_prng_seeder_system(const char **name)
{
	if (name != NULL) {
		*name = "none";
	}
	return 0;
}
