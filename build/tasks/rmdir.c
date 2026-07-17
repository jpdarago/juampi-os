#include "syscall_wrappers.h"
#include "stdio.h"

int main(int argc, char* argv[])
{
    if (argc < 2) {
        printf("usage: rmdir <dir>\n");
        exit();
    }
    for (int i = 1; i < argc; i++) {
        int res = rmdir(argv[i]);
        if (res < 0)
            printf("rmdir: could not remove %s: error %d\n", argv[i], res);
    }
    exit();
    return 0;
}
