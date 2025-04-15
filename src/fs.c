#include <fs.h>
#include <tasks.h>
#include <utils.h>
#include <fs_minix.h>
#include <asserts.h>
#include <fdesc.h>
#include <buffer_cache.h>

static super_block disk_super_block;

int valid_path_char(char c)
{
    if(is_alpha(c)) return true;
    if(c == '_') return true;
    if(c == '-') return true;
    if(c == '.') return true;
    return false;
}

int invalid_path(const char * dir)
{
    uint l = strlen(dir);
    if(l > FS_MAXLEN) return -ETOOLONG;
    for(uint i = 0,j; i < l; ) {
        for(j = i+1; j < FILE_MAXLEN; j++) {
            if(dir[j] == '/') break;
            if(dir[j] == '\0') break;
            if(!valid_path_char(dir[j]))
                return -EINVPATH;
        }
        i = j+1;
    }
    return 0;
}

void check_disk_super_block(super_block * b)
{
    fail_if(b->block_size != 1024);
    fail_if(b->magic != MINIX_MAGIC);
    fail_if(b->is_dirty);
    fail_if(!b->root);
    fail_if(b->root->inode_number != MINIX_ROOT_INODE);
    fail_if(b->root->inode_type != FS_DIR);
}

void init_disk_super_block()
{
    memset(&disk_super_block,0,sizeof(super_block));
    fs_minix_init(&disk_super_block);
    check_disk_super_block(&disk_super_block);
}

super_block * get_disk_super_block()
{
    return &disk_super_block;
}

static int consume_part(char ** pathptr,char * buffer)
{
    int i;
    char * pathname = *pathptr;
    for(i = 0; i < FILE_MAXLEN; i++) {
        if(pathname[i] == '/') {
            break;
        }
        if(pathname[i] == '\0') break;
        buffer[i] = pathname[i];
    }
    buffer[i] = '\0';
    *pathptr += i;
    if(pathname[i] == '/')
        *pathptr += 1;
    return i;
}

static inode * process_path(char * pathname)
{
    if(invalid_path(pathname)) {
        return NULL;
    }

    process_info *  p = get_current_task();
    super_block *   s = get_disk_super_block();

    inode * ptr, * root_dir = s->root;
    if(pathname[0] == '/') {
        pathname++;
        ptr = root_dir;
    }else{
        ptr = process_path(p->cwd);
    }

    char chunk[FILE_MAXLEN+1];
    while(consume_part(&pathname,chunk) > 0) {
        if(!ptr) return NULL;
        if(ptr->inode_type != FS_DIR)
            return NULL;

        inode * prev = ptr;
        ptr = ptr->i_ops->lookup(ptr,chunk);
        prev->super_block->ops->release_inode(prev);
    }
    return ptr;
}

static inode * basedir_inode(char ** pathptr)
{
    char * pathname = *pathptr;
    int len = strlen(pathname);
    int i = len;
    for(i = len; i >= 0; i--)
        if(pathname[i] == '/')
            break;
    if(i < 0) {
        return process_path(get_current_task()->cwd);
    }
    char prev = pathname[i];
    pathname[i] = '\0';
    inode * ino = process_path(pathname);
    pathname[i] = prev;
    if(ino == NULL) return NULL;
    // Hacemos apuntar el pathname al ultimo
    // pedazo del directorio
    *pathptr = pathname+i+1;
    return ino;
}

uint get_flags(uint flags){
    uint res = 0;
    if(flags & FS_TRUNC) res |= FS_TRUNC;
    if(flags & FS_CREAT) res |= FS_CREAT;
    return res;
}

uint get_access_mode(uint flags){
    return flags & 0x3;
}

int do_creat(char * pathname)
{
    inode * parent = basedir_inode(&pathname);
    if(parent == NULL) return -ENODIR;

    int res = parent->i_ops->create(parent,pathname);
    parent->super_block->ops->release_inode(parent);

    if(res < 0) return res;
    return 0;
}

int do_open(char * pathname, uint flags)
{
    process_info * p = get_current_task();
    inode * ino = process_path(pathname);
    file_object f;
    uint aflags = get_flags(flags);
    if(ino == NULL) {
        if(!(aflags & FS_CREAT))
            return -ENODIR;
        int res = do_creat(pathname);
        if(res < 0) return res;
        ino = process_path(pathname);
        if(ino == NULL) return -ENODIR;
    }
    f.access_mode = get_access_mode(flags);
    f.flags = aflags;
    if(ino->f_ops->open == NULL)
        return -EINVOP;
    int res = ino->f_ops->open(ino,&f);
    if(res < 0) return res;
    res = add_file_object(p,&f);
    return res;
}

int do_read(int fd, int bytes, void * buffer)
{
    process_info * currtask = get_current_task();
    file_object * f = get_file_object(currtask,fd);
    if(f == NULL) return -EINVFD;
    if(f->f_ops == NULL || f->f_ops->read == NULL)
        return -EINVOP;
    return f->f_ops->read(f,bytes,buffer);
}

int do_write(int fd, int bytes, void * buffer)
{
    process_info * currtask = get_current_task();
    file_object * f = get_file_object(currtask,fd);
    if(f == NULL) return -EINVFD;
    if(f->f_ops == NULL || f->f_ops->write == NULL)
        return -EINVOP;
    return f->f_ops->write(f,bytes,buffer);
}

int do_readdir(int fd, dirent * d)
{
    process_info * currtask = get_current_task();
    file_object * f = get_file_object(currtask,fd);
    if(f == NULL) return -EINVFD;
    if(f->f_ops == NULL || f->f_ops->readdir == NULL)
        return -EINVOP;
    return f->f_ops->readdir(f,d);
}

int do_close(int fd)
{
    process_info * currtask = get_current_task();
    file_object * f = get_file_object(currtask,fd);
    if(f == NULL) return -EINVFD;
    if(f->f_ops == NULL || f->f_ops->close == NULL)
        return -EINVOP;
    int res = 0;
    if(f->f_ops->flush != NULL)
        res = f->f_ops->flush(f);
    if(res < 0) return res;
    res = f->f_ops->close(f);
    if(res < 0) return res;
    return remove_file_object(currtask,fd);
}

int do_mkdir(char * pathname)
{
    inode * ino = basedir_inode(&pathname);
    if(ino == NULL) return -EINVPATH;
    if(ino->inode_type != FS_DIR)
        return -EINVOP;
    if(ino->i_ops->mkdir == NULL)
        return -EINVOP;
    return ino->i_ops->mkdir(ino,pathname);
}

int do_unlink(char * pathname)
{
    inode * ino = basedir_inode(&pathname);
    if(ino == NULL) return -ENOFILE;
    if(ino->inode_type != FS_DIR)
        return -EINVOP;
    if(ino->i_ops->unlink == NULL)
        return -EINVOP;
    return ino->i_ops->unlink(ino,pathname);
}

int do_rmdir(char * pathname)
{
    inode * ino = basedir_inode(&pathname);
    if(ino == NULL) return -EINVPATH;
    if(ino->inode_type != FS_DIR)
        return -EINVOP;
    if(ino->i_ops->mkdir == NULL)
        return -EINVOP;
    return ino->i_ops->mkdir(ino,pathname);
}

uint get_file_size(int fd)
{
    process_info * p = get_current_task();
    file_object * f = get_file_object(p,fd);
    if(f == NULL) return (uint)-1;
    inode * ino = f->inode;
    if(ino == NULL) return (uint)-1;
    return ino->file_size;
}
