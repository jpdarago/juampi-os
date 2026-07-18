#ifndef KLIBC_TIME_H
#define KLIBC_TIME_H
// Minimal <time.h>: Lua only uses these to seed its RNG, so they return the
// PIT tick count (klibc.c). Not a wall clock.
typedef long time_t;
typedef long clock_t;
#define CLOCKS_PER_SEC 100
time_t time(time_t* t);
clock_t clock(void);
#endif
