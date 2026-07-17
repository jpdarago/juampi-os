#include "syscall_wrappers.h"
#include "stdio.h"
#include "parser.h"
#include "utils.h"

char buf[1024];
int main(int argc, const char* argv[])
{
    if (argc != 3) {
        printf("Incorrect number of arguments\n");
        printf("\tUsage: cp <from> <to>\n");
        exit();
    }

    int fd1 = open(argv[1], FS_RD);
    if (fd1 < 0) {
        printf("Opening file %s failed\n", argv[1]);
        exit();
    }
    int fd2 = open(argv[2], FS_WR | FS_TRUNC | FS_CREAT);
    if (fd2 < 0) {
        printf("Opening file %s failed\n", argv[2]);
        exit();
    }
    int rd;
    for (rd = read(fd1, 1023, buf);; rd = read(fd1, 1023, buf)) {
        if (rd <= 0) {
            if (rd < 0)
                printf("Error in cp while reading: %d\n", rd);
            break;
        }

        int wr = write(fd2, rd, buf);
        if (wr < rd) {
            printf("Error in cp while writing: %d\n", wr);
        }
    }

    close(fd1);
    close(fd2);
    exit();
    return 0;
}
