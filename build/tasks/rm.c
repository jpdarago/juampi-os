#include "syscall_wrappers.h"
#include "stdio.h"

int main(int argc, char* argv[])
{
    if (argc < 2) {
        printf("usage: rm <file>\n");
        exit();
    }
    for (int i = 1; i < argc; i++) {
        int res = unlink(argv[i]);
        if (res < 0)
            printf("rm: could not remove %s: error %d\n", argv[i], res);
    }
    exit();
    return 0;
}
