// Insertion sort — O(n^2). Benchmark it against quick.c on the same input:
//   lab.bench("insertion.elf", 2000)  vs  lab.bench("quick.elf", 2000)
// Both fill `arg` ints from the same LCG and return the same order-sensitive
// checksum (so a matching result proves both sort correctly), but the cycle
// counts differ by orders of magnitude.
#include <lab.h>

static unsigned long fill(int* a, long n)
{
    unsigned long x = 0x2545F4914F6CDD1DUL;
    for (long i = 0; i < n; i++) {
        x = x * 6364136223846793005UL + 1442695040888963407UL;
        a[i] = (int)(x >> 33); // 31-bit, always positive
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

long bench(const lab_api* api, long arg)
{
    long n = arg > 0 ? arg : 1000;
    int* a = api->alloc((unsigned long)n * sizeof(int));
    if (a == 0) {
        return -1;
    }
    fill(a, n);

    for (long i = 1; i < n; i++) {
        int key = a[i];
        long j = i - 1;
        while (j >= 0 && a[j] > key) {
            a[j + 1] = a[j];
            j--;
        }
        a[j + 1] = key;
    }

    unsigned long sum = checksum(a, n);
    api->free(a);
    return (long)sum;
}
