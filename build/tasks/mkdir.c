#include "syscall_wrappers.h"
#include "stdio.h"

int main(int argc, char* argv[])
{
    if (argc < 2) {
        printf("usage: mkdir <dir>\n");
        exit();
    }
    for (int i = 1; i < argc; i++) {
        int res = mkdir(argv[i]);
        if (res < 0)
            printf("mkdir: could not create %s: error %d\n", argv[i], res);
    }
    exit();
    return 0;
}
