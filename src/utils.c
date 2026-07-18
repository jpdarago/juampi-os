#include <utils.h>

// Portable C replacements for the old 32-bit optutils.asm routines, with the
// standard C signatures (GCC may emit implicit calls to them for aggregate
// copies even under -fno-builtin, and it assumes the standard ABI).
void* memset(void* dest, int val, size_t count)
{
    uint8_t* d = dest;
    for (size_t i = 0; i < count; i++) {
        d[i] = (uint8_t)val;
    }
    return dest;
}

void* memcpy(void* dest, const void* src, size_t count)
{
    uint8_t* d = dest;
    const uint8_t* s = src;
    for (size_t i = 0; i < count; i++) {
        d[i] = s[i];
    }
    return dest;
}
