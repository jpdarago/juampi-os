#ifndef KLIBC_ASSERT_H
#define KLIBC_ASSERT_H
// Lua's default build does not enable internal asserts; keep this a no-op.
#define assert(x) ((void)0)
#endif
