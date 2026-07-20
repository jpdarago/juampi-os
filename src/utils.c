#include <utils.h>

// Portable C replacements for the old 32-bit optutils.asm routines, with the
// standard C signatures (GCC may emit implicit calls to them for aggregate
// copies even under -fno-builtin, and it assumes the standard ABI).
//
// Both process 8 bytes per iteration with a byte tail — an order of magnitude
// faster than a byte loop for the bulk copies the framebuffer and filesystem
// do. The may_alias word type keeps the wide accesses legal under strict
// aliasing; x86 permits the unaligned 64-bit loads/stores.
typedef uint64_t __attribute__((may_alias)) word;

void* memset(void* dest, int val, size_t count)
{
    uint8_t* d = dest;
    uint8_t b = (uint8_t)val;
    word w = (word)0x0101010101010101ull * b;
    size_t i = 0;
    for (; i + 8 <= count; i += 8) {
        *(word*)(d + i) = w;
    }
    for (; i < count; i++) {
        d[i] = b;
    }
    return dest;
}

void* memcpy(void* dest, const void* src, size_t count)
{
    uint8_t* d = dest;
    const uint8_t* s = src;
    size_t i = 0;
    for (; i + 8 <= count; i += 8) {
        *(word*)(d + i) = *(const word*)(s + i);
    }
    for (; i < count; i++) {
        d[i] = s[i];
    }
    return dest;
}
