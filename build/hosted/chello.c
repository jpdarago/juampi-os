// A hosted C program for juampiOS: ordinary ANSI C using newlib's stdio, built
// with few/no kernel-specific changes. Proves the crt0 + newlib + libgloss +
// int-0x80 chain end to end (printf -> stdout -> _write -> console).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char** argv)
{
    printf("hello from a newlib-hosted C program on juampiOS\n");
    printf("argc=%d argv[0]=%s\n", argc, argc > 0 ? argv[0] : "(none)");

    // Exercise malloc + string + formatted output.
    char* buf = malloc(64);
    if (buf == NULL) {
        fprintf(stderr, "malloc failed\n");
        return 1;
    }
    snprintf(buf, 64, "%d + %d = %d, pi ~= %.4f", 2, 40, 2 + 40, 3.14159);
    printf("computed: %s\n", buf);
    free(buf);

    for (int i = 1; i <= 3; i++) {
        printf("  line %d of 3\n", i);
    }
    puts("HOSTED_OK");
    return 0;
}
