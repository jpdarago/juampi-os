#ifndef __SPINLOCK_H
#define __SPINLOCK_H

// A minimal test-and-set spinlock — the kernel's first real concurrency
// primitive, needed once the application processors run (the old
// cli-as-a-lock assumption no longer holds with more than one core). Header
// only: built on GCC's __atomic builtins, no heap, usable anywhere.

typedef struct {
    volatile int locked;
} spinlock;

static inline void spin_lock(spinlock* l)
{
    // Acquire: take the lock, then spin read-only (with PAUSE) while it's held,
    // so the cache line isn't bounced by repeated failed exchanges.
    while (__atomic_exchange_n(&l->locked, 1, __ATOMIC_ACQUIRE)) {
        while (__atomic_load_n(&l->locked, __ATOMIC_RELAXED)) {
            __builtin_ia32_pause();
        }
    }
}

static inline void spin_unlock(spinlock* l)
{
    __atomic_store_n(&l->locked, 0, __ATOMIC_RELEASE);
}

#endif
