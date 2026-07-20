// Quicksort — O(n log n) average. The fast counterpart to insertion.c; same
// input and same checksum, far fewer cycles. See insertion.c for the setup.
#include <lab.h>

static unsigned long fill(int* a, long n)
{
    unsigned long x = 0x2545F4914F6CDD1DUL;
    for (long i = 0; i < n; i++) {
        x = x * 6364136223846793005UL + 1442695040888963407UL;
        a[i] = (int)(x >> 33);
    }
    return x;
}

static unsigned long checksum(const int* a, long n)
{
    unsigned long s = 0;
    for (long i = 0; i < n; i++) {
        s = s * 31 + (unsigned)a[i];
    }
    return s;
}

// Hoare-partition quicksort; recurse into the smaller side and loop on the
// larger so stack depth stays O(log n).
static void quick(int* a, long lo, long hi)
{
    while (lo < hi) {
        int pivot = a[(lo + hi) / 2];
        long i = lo, j = hi;
        while (i <= j) {
            while (a[i] < pivot) {
                i++;
            }
            while (a[j] > pivot) {
                j--;
            }
            if (i <= j) {
                int t = a[i];
                a[i] = a[j];
                a[j] = t;
                i++;
                j--;
            }
        }
        if (j - lo < hi - i) {
            quick(a, lo, j);
            lo = i;
        } else {
            quick(a, i, hi);
            hi = j;
        }
    }
}

long bench(const lab_api* api, long arg)
{
    long n = arg > 0 ? arg : 1000;
    int* a = api->alloc((unsigned long)n * sizeof(int));
    if (a == 0) {
        return -1;
    }
    fill(a, n);
    quick(a, 0, n - 1);
    unsigned long sum = checksum(a, n);
    api->free(a);
    return (long)sum;
}
