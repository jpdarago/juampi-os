// Single-threaded malloc lock hooks for the vendored newlib (juampiOS runs one
// hosted program at a time, cooperatively) — nothing to lock.
struct _reent;
void __malloc_lock(struct _reent* r)
{
    (void)r;
}
void __malloc_unlock(struct _reent* r)
{
    (void)r;
}
