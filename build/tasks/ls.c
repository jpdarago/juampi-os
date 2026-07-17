#include "syscall_wrappers.h"
#include "stdio.h"
#include "parser.h"
#include "utils.h"

char dirbuf[FS_MAXLEN];
int main(int argc, const char* argv[])
{
    const char* dir;
    if (argc == 1) {
        get_cwd(dirbuf);
        dir = dirbuf;
    } else {
        dir = argv[1];
    }

    int fd = open(dir, FS_RD);
    if (fd < 0) {
        printf("Opening directory %s failed\n", dir);
        exit();
    }
    dirent d;
    for (int res = readdir(fd, &d);; res = readdir(fd, &d)) {
        if (res < 0) {
            printf("Read failed: ERROR %d\n", -res);
            break;
        }
        if (res == 0)
            break;
        printf("%s\t", d.name);
    }

    putchar('\n');
    close(fd);
    exit();
    return 0;
}
