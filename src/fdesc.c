#include <fdesc.h>
#include <utils.h>

file_object * get_file_object(process_info * p, int fd)
{
    if(fd < 0 || fd >= MAX_FDS) return NULL;
    return &p->fds[fd];
}

bool unused_fd(process_info * p, int fd)
{
    if(fd < 0 || fd >= MAX_FDS) return false;
    return p->fds[fd].inode == NULL;
}

void set_fd(process_info * p, int fd, file_object * f)
{
    memcpy(&p->fds[fd],f,sizeof(file_object));
}

int add_file_object(process_info * p, file_object * f)
{
    for(int i = 0; i < MAX_FDS; i++) {
        if(unused_fd(p,i)) {
            set_fd(p,i,f);
            return i;
        }
    }
    return -ENOFDSPACE;
}

int remove_file_object(process_info * p, int fd)
{
    if(fd < 0 || fd >= MAX_FDS) return -EINVFD;
    memset(&p->fds[fd],0,sizeof(file_object));
    return 0;
}

void copy_file_descriptors(process_info * dst,process_info * src)
{
    for(int i = 0; i< MAX_FDS; i++)
        memcpy(&dst->fds[i],&src->fds[i],sizeof(file_object));
}
