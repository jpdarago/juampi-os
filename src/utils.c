#include <utils.h>

// memset/memcpy were hand-written in the 32-bit optutils.asm; the x86-64 port
// uses these portable C versions instead (the build passes -fno-builtin, so GCC
// will not rewrite the loops back into memset/memcpy calls).
void memset(void* dest, uchar val, uint count)
{
    uchar* d = dest;
    for (uint i = 0; i < count; i++) {
        d[i] = val;
    }
}

void memcpy(void* dest, const void* src, uint count)
{
    uchar* d = dest;
    const uchar* s = src;
    for (uint i = 0; i < count; i++) {
        d[i] = s[i];
    }
}
