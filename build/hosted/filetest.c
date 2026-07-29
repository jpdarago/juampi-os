// Hosted C program exercising newlib file I/O over ext2 (fopen/fwrite/fread).
// Uses unbuffered stderr markers so a crash still shows how far it got.

#include <stdio.h>
#include <string.h>

#define MARK(s) fputs(s, stderr)

int main(void)
{
    const char* path = "/hosted-scratch.txt";
    const char* msg = "written by a hosted C program: 2+40=42\n";

    MARK("m1\n");
    FILE* f = fopen(path, "w");
    MARK("m2\n");
    if (f == NULL) {
        MARK("fopen(w) failed\n");
        return 1;
    }
    fputs(msg, f);
    MARK("m3\n");
    fprintf(f, "second line %d\n", 123);
    MARK("m4\n");
    fclose(f); // flushes to ext2
    MARK("m5\n");

    char buf[256];
    f = fopen(path, "r");
    MARK("m6\n");
    if (f == NULL) {
        MARK("fopen(r) failed\n");
        return 1;
    }
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);
    MARK("m7\n");

    printf("read back %u bytes:\n%s", (unsigned)n, buf);
    fflush(stdout);
    if (strncmp(buf, msg, strlen(msg)) == 0) {
        printf("FILEIO_OK\n");
    } else {
        printf("FILEIO_MISMATCH\n");
    }
    fflush(stdout);
    return 0;
}
