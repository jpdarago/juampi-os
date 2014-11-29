#include <fs_minix.h>
#include <hdd.h>
#include <bitset.h>
#include <exception.h>
#include <memory.h>
#include <buffer_cache.h>
#include <memory.h>
#include <dev_table.h>

/* TODO:

 *  Considerar escribir el super block a disco
    despues de un cierto tiempo (no inmediatamente).
 *  Considerar guardar la posicion de la entrada de directorio
    para los inodos
 *  Fijarse el tema de contadores de uso para los inodos
    y los files.
 *	Actualmente el filesystem no soporta archivos de lectura
    y escritura. De todos modos esto no es complicado.
 */

static inode * minix_create_disk_inode(super_block *);
static void release_inode_blocks(inode *, super_block *);

static disk_position inode_entry_position(ushort inode_num,
                                          minix_fs_data * mfd)
{
    disk_position p; inode_num--;
    uint inode_block = inode_num / MINIX_INODES_PER_BLOCK;
    uint inode_offset = inode_num % MINIX_INODES_PER_BLOCK;
    p.block = mfd->inode_entries_start + inode_block;
    p.offset = inode_offset*sizeof(minix_inode);
    return p;
}

static int read_inode_entry(ushort inode_number,
                            minix_inode * data, minix_fs_data * mfd)
{
    disk_position p = inode_entry_position(inode_number,mfd);
    buffered_read(p.block,p.offset,sizeof(minix_inode),data);

    return 0;
}

static minix_inode * get_disk_inode(inode * ino)
{
    if(ino->super_block->magic != MINIX_MAGIC)
        return NULL;
    return (minix_inode *) ino->info_disk;
}

// Consigue un bloque de disco libre del super bloque minix
static ushort request_free_zone(minix_fs_data * mfd)
{
    uint pos = bitset_search(&mfd->inode_bitmap);
    if(pos == (uint) -1) return 0;
    bitset_set(&mfd->inode_bitmap,pos);

    return pos;
}

#define disk_pos(b,o) \
    (disk_position) { .block = (b), .offset = (o) }
static ushort read_data_entry(uint block, uint offset)
{
    ushort res;
    buffered_read(block,offset*sizeof(ushort),sizeof(res),&res);
    return res;
}

static void write_data_entry(uint block, uint offset,ushort res)
{
    buffered_read(block,offset*sizeof(ushort),sizeof(res),&res);
}

// Determina que posicion en disco tiene el bloque numero
// block_index del contenido fisico del inodo ino. create
// indica si debe ser creado o no
static disk_position find_block_ptr(inode * ino,
                                    uint block_index,
                                    bool create)
{
    minix_inode * mi = get_disk_inode(ino);
    minix_fs_data * mfd = ino->super_block->fs_data;
    block_index -= MINIX_ZONES;
    disk_position p;
    if(block_index < MINIX_BLOCK_PTRS) {
        p.block = mi->indirect_block;
        p.offset = block_index;
    } else {
        block_index -= MINIX_BLOCK_PTRS;
        uint ind_block_index = block_index / MINIX_BLOCK_PTRS;

        p.block = read_data_entry(mi->doubly_indirect_block,
                                  ind_block_index);
        if(!p.block) {
            if(!create) return disk_pos(0,0);
            p.block = request_free_zone(mfd);
        }
        p.offset = block_index % MINIX_BLOCK_PTRS;
    }
    return p;
}

// Determina la posicion en disco donde empieza el contenido
// del inodo ino, tomando el offset en bytes. create indica
// si la zona debe ser creada en caso de que no exista
static disk_position inode_offset_position(inode * ino,
                                           uint offset,
                                           bool create)
{
    minix_inode * mi = get_disk_inode(ino);
    minix_fs_data * mfd = ino->super_block->fs_data;
    uint block_index  = offset / MINIX_BLOCK_SIZE;
    uint block_offset = offset % MINIX_BLOCK_SIZE;
    ushort block_ptr;

    if(block_index < MINIX_ZONES) {
        if(!mi->zones[block_index]) {
            if(!create) return disk_pos(0,0);
            mi->zones[block_index] = request_free_zone(mfd);
            ino->is_dirty = true;
        }

        return disk_pos(mi->zones[block_index],block_offset);
    }

    disk_position p = find_block_ptr(ino,block_index,create);
    block_ptr = read_data_entry(p.block,p.offset);
    if(!block_ptr) {
        if(!create) return disk_pos(0,0);
        block_ptr = request_free_zone(mfd);
        write_data_entry(p.block,p.offset,block_ptr);
    }

    return disk_pos(block_ptr,block_offset);
}

// Procesa un inodo dada una accion: el codigo es para generalizar
// read y write usando el mismo loop
static int minix_inode_data_read(inode * ino,uint offset,
                                 uint bytes, void * _data)
{
    char * data = _data;
    int processed = 0;
    disk_position p;

    for(; bytes; ) {
        p = inode_offset_position(ino,offset,false);
        if(!p.block) return -ENOSPACE;

        uint rem = MINIX_BLOCK_SIZE - p.offset;
        uint process = (rem < bytes) ? rem : bytes;

        buffered_read(p.block,p.offset,process,data);

        offset += process; bytes -= process;
        data += process; processed += process;
    }

    return processed;
}

static int minix_inode_write_process(inode * ino,
                                     uint offset, uint bytes,
                                     void * _data)
{
    char * data = _data;
    int processed = 0;
    disk_position p;

    for(; bytes; ) {
        p = inode_offset_position(ino,offset,true);
        if(!p.block) return -ENOSPACE;

        uint rem = MINIX_BLOCK_SIZE - p.offset;
        uint process = (rem < bytes) ? rem : bytes;

        buffered_write(p.block,p.offset,process,
                       ino->inode_number,data);

        offset += process; bytes -= process;
        data += process; processed += process;
    }

    return processed;
}

static int minix_inode_data_write(inode * ino,
                                  uint offset, uint bytes, void * data)
{
    int written = minix_inode_write_process(ino,
                                            offset,bytes,data);
    if(written < 0) {
        return written;
    }
    if(offset + written > ino->file_size)
        ino->file_size = offset + written;
    if(written > 0) ino->is_dirty = true;
    return written;
}

// Convierte un tipo de archivo dado en formato minix
// al formato del VFS
static fs_type minix_type(ushort mode)
{
    switch(MINIX_TYPE_ONLY(mode)) {
    case MINIX_IFREG:
        return FS_FILE;
    case MINIX_IFBLK:
        return FS_BLOCKDEV;
    case MINIX_IFDIR:
        return FS_DIR;
    case MINIX_IFCHR:
        return FS_CHARDEV;
    default:
        return FS_ERROR;
    }
}

// Reverso de la anterior
static fs_type to_minix_type(uint inode_type)
{
    switch(inode_type) {
    case FS_FILE:       return MINIX_IFREG;
    case FS_BLOCKDEV:   return MINIX_IFBLK;
    case FS_DIR:        return MINIX_IFDIR;
    case FS_CHARDEV:    return MINIX_IFCHR;
    default:            return FS_ERROR;
    }
}

static const inode_ops minix_default_inode_ops = { 0 };
static const fs_ops minix_default_file_ops = { 0 };

static int minix_open_file(inode * ino, file_object * file)
{
    if(ino == NULL)
        return -EINVINODE;


    file->inode = ino;
    file->f_ops = ino->f_ops;
    file->file_read_offset = 0;

    if(file->access_mode & FS_WR) {
        if(ino->inode_type == FS_DIR)
            return -EINVTYPE;
        // Para escritura se lee por default en modo append
        file->file_write_offset = file->inode->file_size;
        if(file->flags & FS_TRUNC) {
            file->file_write_offset = 0;
            release_inode_blocks(ino,ino->super_block);
            ino->file_size = 0;
        }
    }

    return 0;
}

static int minix_read_file(file_object * file,
                           uint bytes, void * _buffer)
{
    if(!(file->access_mode & FS_RDBIT))
        return -EPERMS;

    uint file_offset = file->file_read_offset;
    uint remaining = file->inode->file_size - file_offset;
    if(bytes > remaining) bytes = remaining;
    int read = minix_inode_data_read(file->inode,
                                     file_offset,bytes,_buffer);
    file->file_read_offset += read;
    return read;
}

static int minix_write_file(file_object * file,
                            uint bytes, void * _buffer)
{
    if(!(file->access_mode & FS_WRBIT)) {
        return -EPERMS;
    }

    uint offset = file->file_write_offset;
    int written = minix_inode_data_write(file->inode,
                                         offset,bytes,_buffer);
    file->file_write_offset += written;
    return written;
}

static int minix_close_file(file_object * f)
{
    inode * ino = f->inode;
    ino->super_block->ops->release_inode(ino);
    return 0;
}

static int minix_flush(file_object * f)
{
    inode * ino = f->inode;
    buffers_flush(ino->inode_number);
    ino->super_block->ops->write_inode(ino);
    return 0;
}

static int minix_seek(file_object * f, uint offset)
{
    f->file_read_offset = offset;
    f->file_write_offset = offset;
    return 0;
}

static const fs_ops minix_file_file_ops = {
    .read       = minix_read_file,
    .write      = minix_write_file,
    .open       = minix_open_file,
    .close      = minix_close_file,
    .flush      = minix_flush,
    .seek       = minix_seek
};

static int minix_readdir(file_object * f, dirent * dir)
{
    minix_dir_entry d;
    int res = minix_read_file(f,sizeof(d),&d);
    if(res != sizeof(d))
        return res;
    dir->inode_number = d.inode;
    strcpy(dir->name,d.name);
    return sizeof(dirent);
}

static const fs_ops minix_dir_file_ops = {
    .open       = minix_open_file,
    .close      = minix_close_file,
    .readdir    = minix_readdir,
};

static disk_position
check_entries(minix_dir_entry entries[], char * name,uint block)
{
    for(int j = 0; j < MINIX_DIR_ENTRIES; j++) {
        if(!strcmp(name,entries[j].name)) {
            return disk_pos(block,j*sizeof(minix_dir_entry));
        }
    }
    return disk_pos(0,0);
}

static disk_position
minix_lookup_direct(minix_inode * mi, char * name)
{
    minix_dir_entry entries[MINIX_DIR_ENTRIES];
    disk_position res = disk_pos(0,0);
    for(int i = 0; i < MINIX_ZONES; i++) {
        if(!mi->zones[i]) return res;

        buffered_read(mi->zones[i],0,sizeof(entries),entries);
        res = check_entries(entries,name,mi->zones[i]);
        if(res.block) return res;
    }
    return res;
}

static disk_position
minix_lookup_indirect(ushort ind_block, char * name)
{
    ushort blocks[MINIX_BLOCK_PTRS];
    minix_dir_entry entries[MINIX_DIR_ENTRIES];

    disk_position res = disk_pos(0,0);
    buffered_read(ind_block,0,sizeof(blocks),blocks);
    for(int i = 0; i < MINIX_BLOCK_PTRS; i++) {
        if(!blocks[i]) return res;
        buffered_read(blocks[i],0,sizeof(entries),entries);
        res = check_entries(entries,name,blocks[i]);
        if(res.block) return res;
    }
    return res;
}

static disk_position
minix_lookup_doubly_indirect(ushort dind_block, char * name)
{
    ushort indirect_blocks[MINIX_BLOCK_PTRS];
    disk_position res = disk_pos(0,0);
    buffered_read(dind_block,0,sizeof(indirect_blocks),
                  indirect_blocks);
    for(int i = 0; i < MINIX_BLOCK_PTRS; i++) {
        if(!indirect_blocks[i]) return res;
        res = minix_lookup_indirect(indirect_blocks[i],name);
        if(res.block) return res;
    }
    return res;
}

static disk_position
disk_ptr_lookup(minix_inode * mi, char * dir)
{
    disk_position res = minix_lookup_direct(mi,dir);
    if(res.block) return res;
    if(mi->indirect_block) {
        res = minix_lookup_indirect(mi->indirect_block,dir);
        if(res.block) return res;
    }
    if(mi->doubly_indirect_block) {
        res = minix_lookup_doubly_indirect(
            mi->doubly_indirect_block,dir);
        if(res.block) return res;
    }
    return disk_pos(0,0);
}

static inode *
inode_from_dir_entry(super_block * s,disk_position p)
{
    minix_dir_entry d;
    buffered_read(p.block,p.offset,sizeof(d),&d);

    inode * ino = s->ops->alloc_inode(s);
    if(ino == NULL) return NULL;

    buffered_read(p.block,p.offset,sizeof(d),&d);
    ino->inode_number = d.inode;
    s->ops->read_inode(ino);
    return ino;
}

static inode * minix_lookup(inode * start, char * dir)
{
    super_block * s = start->super_block;
    if(s->magic != MINIX_MAGIC) return NULL;
    if(!start || start->inode_type != FS_DIR)
        return NULL;

    minix_inode * mi = get_disk_inode(start);
    disk_position res = disk_ptr_lookup(mi,dir);
    if(!res.block) return NULL;
    return inode_from_dir_entry(s,res);
}

static inode * minix_create_subfile(inode * parent, char * name)
{
    super_block * s = parent->super_block;
    inode * new_inode = s->ops->obtain_inode(s);
    if(new_inode == NULL) return new_inode;

    minix_dir_entry dir;

    dir.inode = new_inode->inode_number;
    strncpy(dir.name,name,MINIX_DIR_MAXLEN);
    int res = minix_inode_data_write(parent,
                                     parent->file_size,sizeof(dir),&dir);
    if(res != sizeof(dir)) return NULL;
    parent->super_block->ops->write_inode(parent);
    return new_inode;
}

static int minix_mkdir(inode * parent,char * name)
{
    super_block * s = parent->super_block;
    if(s->magic != MINIX_MAGIC)
        return -EINVFS;
    if(parent->inode_type != FS_DIR)
        return -EINVTYPE;

    inode * new_inode = minix_create_subfile(parent,name);
    if(!new_inode || new_inode->inode_number == 0)
        return -EINODES;

    new_inode->is_dirty = true;
    minix_dir_entry ode = { .name = ".",
                            .inode = new_inode->inode_number };
    minix_dir_entry pde = { .name = "..",
                            .inode = parent->inode_number };

    minix_inode_data_write(new_inode,0,sizeof(ode),&ode);
    minix_inode_data_write(new_inode,0,sizeof(pde),&pde);

    new_inode->inode_type = FS_DIR;
    new_inode->file_size = MINIX_DEF_DIRSIZE;
    s->ops->write_inode(new_inode);

    return 0;
}

static int minix_unlink(inode * parent, char * name)
{
    super_block * s = parent->super_block;
    if(s->magic != MINIX_MAGIC)
        return -EINVFS;
    if(parent->inode_type != FS_DIR)
        return -EINVTYPE;

    minix_inode * mi = parent->info_disk;
    disk_position p = disk_ptr_lookup(mi,name);
    if(p.block == 0) return -ENODIR;

    inode * rmed = inode_from_dir_entry(s,p);
    if( rmed->inode_type != FS_DIR &&
        rmed->inode_type != FS_FILE)
        return -EDIRNOTEMPTY;

    if( rmed->inode_type == FS_DIR &&
        rmed->file_size > MINIX_DEF_DIRSIZE)
        return -EINVTYPE;

    parent->file_size -= sizeof(minix_dir_entry);

    minix_dir_entry last;
    minix_inode_data_read(parent,parent->file_size,
                          sizeof(last),&last);

    buffered_write(p.block,p.offset,
                   sizeof(last),parent->inode_number,&last);

    memset(&last,0,sizeof(last));
    minix_inode_data_write(parent,parent->file_size,
                           sizeof(last),&last);

    int res = s->ops->delete_inode(rmed);
    return res ? res : 0;
}

static int minix_create(inode * parent, char * name)
{
    super_block * s = parent->super_block;
    if(s->magic != MINIX_MAGIC)
        return -EINVFS;
    if(parent->inode_type != FS_DIR)
        return -EINVTYPE;

    inode * ino = minix_create_subfile(parent,name);
    if(!ino || ino->inode_number == 0) return -EINODES;

    ino->inode_type = FS_FILE;
    ino->file_size = 0;
    ino->is_dirty = true;

    s->ops->write_inode(ino);
    return 0;
}

static const inode_ops minix_dir_inode_ops = {
    .lookup     = minix_lookup,
    .mkdir      = minix_mkdir,
    .rmdir      = minix_unlink,
    .create     = minix_create,
    .unlink     = minix_unlink
};

static int initialize_file_type(inode * data, ushort mt)
{
    data->inode_type= minix_type(mt);
    minix_inode * mi = data->info_disk;
    ushort tmp,major,minor;
    switch(data->inode_type) {
    case FS_FILE:
        data->i_ops = &minix_default_inode_ops;
        data->f_ops = &minix_file_file_ops;
        break;
    case FS_DIR:
        data->i_ops = &minix_dir_inode_ops;
        data->f_ops = &minix_dir_file_ops;
        break;
    case FS_CHARDEV:
        // TODO: Conseguir las funciones de char device
        data->info_cdev = kmalloc(sizeof(char_dev));
        tmp = mi->zones[0];
        major = tmp & 0xFF;
        minor= (tmp >> 8) & 0xFF;
        data->f_ops = get_fops_table(major,minor);
        data->i_ops = get_iops_table(major,minor);
        break;
    case FS_BLOCKDEV:
        // TODO: Conseguir las funciones de block device
        break;
    default:
        return -1;
    }
    return 0;
}

static void add_to_super_list(inode * data)
{
    list_add(&data->open_ptr,
             &data->super_block->open_inodes);
}

// Lee los datos del inodo desde disco duro,
// utiliza el inode_number pasado con data
static int minix_read_inode(inode * data)
{
    ushort ino_num = data->inode_number;
    super_block * sb = data->super_block;
    if(sb->magic != MINIX_MAGIC)
        return -1;

    minix_fs_data * mfd = sb->fs_data;
    minix_inode * ifd = get_disk_inode(data);

    read_inode_entry(ino_num,ifd,mfd);

    data->file_size = ifd->size;
    data->is_dirty = false;

    if(!initialize_file_type(data,ifd->mode))
        return -1;

    return 0;
}

static int write_inode_entry(ushort ino_num,
                             minix_inode * ifd, minix_fs_data * mfd)
{
    disk_position p = inode_entry_position(ino_num,mfd);
    buffered_write(p.block,p.offset,sizeof(minix_inode),ino_num,ifd);
    return 0;
}

// Flushea el inodo a disco duro, usando el inode_number
// pasado en data como parametro
static int minix_write_inode(inode * data)
{
    if(!data->is_dirty) return 0;

    ushort ino_num = data->inode_number;
    super_block * sb = data->super_block;
    if(sb->magic != MINIX_MAGIC)
        return -1;

    minix_fs_data * mfd = sb->fs_data;
    minix_inode * ifd = get_disk_inode(data);

    ifd->size = data->file_size;
    ifd->mode = to_minix_type(data->inode_type);

    write_inode_entry(ino_num,ifd,mfd);
    data->is_dirty = false;

    return 0;
}

static inode * find_free_inode(super_block * s)
{
    inode * res;
    list_head * ptr;
    list_for_each(ptr,&s->open_inodes) {
        res = list_entry(ptr,inode,open_ptr);
        if(!res->use_count) {
            res->use_count++;
            return res;
        }
    }
    return NULL;
}

// Devuelve un inodo inicializado
static inode * minix_alloc_inode(super_block * s)
{
    if(s->magic != MINIX_MAGIC)
        return NULL;

    inode * ino = find_free_inode(s);
    if(ino == NULL) {
        ino = kmalloc(sizeof(inode));
        memset(ino,0,sizeof(inode));
        ino->use_count = 1;
        ino->super_block = s;
        add_to_super_list(ino);
    }

    ino->i_ops = &minix_default_inode_ops;
    ino->f_ops = &minix_default_file_ops;

    minix_inode * minix_ino = kmalloc(sizeof(minix_inode));
    memset(minix_ino,0,sizeof(minix_inode));
    ino->info_disk = minix_ino;

    return ino;
}

static void clear_inode_entry(ushort ino_num, minix_fs_data * mfd)
{
    minix_inode data;
    memset(&data,0,sizeof(data));
    write_inode_entry(ino_num,&data,mfd);
}

static ushort get_unused_inode(super_block * s)
{
    minix_fs_data * mfd = s->fs_data;
    uint ino_num = bitset_search(&mfd->inode_bitmap);
    s->is_dirty = true;
    if(ino_num == (uint)-1)
        return 0;
    clear_inode_entry(ino_num,mfd);
    return ino_num+1; // Los inodos se cuentan desde 1
}

static void minix_release_incore_data(inode * data)
{
    kfree(data->info_disk);
    kfree(data->info_cdev);
    kfree(data->info_bdev);
    kfree(data->info_pipe);
}

// Libera el inodo inicializado (destructor)
static int minix_release_inode(inode * data)
{
    if(data == data->super_block->root)
        return 0;
    data->use_count--;
    if(data->use_count > 0)
        return 0;
    if(data->is_dirty) {
        data->is_dirty = false;
        minix_write_inode(data);
    }
    minix_release_incore_data(data);
    return 0;
}

// Obtiene un inodo asignandole un numero de inodo que este
// libre
static inode * minix_create_disk_inode(super_block * s)
{
    if(s->magic != MINIX_MAGIC)
        return NULL;
    inode * ino = NULL;
    ino = minix_alloc_inode(s);
    if(ino == NULL) return NULL;

    ushort inode_number = get_unused_inode(s);
    if(inode_number == 0)
        return NULL;

    ino->inode_number = inode_number;
    ino->is_dirty = true;
    return ino;
}

static void minix_flush_bitmaps(super_block * s)
{
    // Los bitmaps son consecutivos en memoria, para flushearlos
    // mas rapido a disco
    minix_fs_data * mfd = s->fs_data;
    buffered_write_several(
        mfd->inode_bitmap_start_block,
        mfd->inode_bitmap_blocks + mfd->zone_bitmap_blocks,
        0,
        mfd->inode_bitmap.start );
}

// Flushea el super bloque a disco
static int minix_write_super_block(super_block * s)
{
    if(s->magic != MINIX_MAGIC)
        return -1;
    if(!s->is_dirty) return 0;
    s->is_dirty = false;

    minix_flush_bitmaps(s);
    return 0;
}

static void minix_release_blocks(super_block * sb,
                                 ushort * entries,uint count,int depth)
{
    minix_fs_data * mfd = sb->fs_data;
    for(uint i = 0; i < count; i++) {
        ushort entry = entries[i];
        if(entry == 0) break;
        if(depth > 0) {
            ushort space[MINIX_BLOCK_PTRS];
            buffered_read(entry,0,MINIX_BLOCK_SIZE,space);
            minix_release_blocks(sb,space,
                                 MINIX_BLOCK_PTRS,depth-1);
        }
        bitset_set(&mfd->zone_bitmap,entry);
    }
}

void release_inode_blocks(inode * ino, super_block * sb)
{
    minix_inode * diskino = get_disk_inode(ino);
    int depth = 0;
    if(ino->inode_type == FS_FILE) {
        minix_release_blocks(sb,diskino->zones,MINIX_ZONES,0);
        depth++;
    }
    minix_release_blocks(sb,&diskino->indirect_block,1,depth);
    minix_release_blocks(sb,
                         &diskino->doubly_indirect_block,1,depth+1);
}

// Borra el inodo, no solo destruyendo la memoria misma sino
// que ademas libera el inodo en disco duro
static int minix_delete_inode(inode * data)
{
    if(data->super_block->magic != MINIX_MAGIC)
        return -1;
    super_block * sb = data->super_block;
    minix_fs_data * mfd = sb->fs_data;
    ushort inum = data->inode_number;

    bitset_clear(&mfd->inode_bitmap,inum-1);
    sb->is_dirty = true;

    release_inode_blocks(data,sb);
    minix_write_super_block(sb);
    data->is_dirty = false;
    minix_release_inode(data);

    return 0;
}

static const super_block_ops minix_super_block_ops = {
    .read_inode     = minix_read_inode,
    .write_inode    = minix_write_inode,
    .delete_inode   = minix_delete_inode,
    .alloc_inode    = minix_alloc_inode,
    .release_inode  = minix_release_inode,
    .write_super    = minix_write_super_block,
    .obtain_inode   = minix_create_disk_inode
};

static void minix_init_fs_ops(super_block * block)
{
    block->ops = &minix_super_block_ops;
}

static void minix_init_disk_info(super_block * block,
                                 minix_super_block * d)
{
    block->magic = d->magic;
    block->block_size = MINIX_BLOCK_SIZE;
    block->max_file_size = d->max_file_size;

    uint data_zone_size =
        d->data_zones * (1024 << d->data_zone_size_log);

    block->disk_size = data_zone_size;
    block->is_dirty = false;
}

static void * buffer_from_disk(minix_super_block * d,
                               uint start, uint blocks)
{
    void * buf = kmalloc(blocks*MINIX_BLOCK_SIZE);
    buffered_read_several(start,blocks,buf);
    return buf;
}

static void load_minix_specific_metadata(minix_fs_data * m,
                                         minix_super_block * d)
{
    m->number_inodes = d->number_inodes;
    m->data_zone_start = d->data_zone_start;
    m->inode_entries_start = MINIX_INODEBMAP_STARTB
                             + d->inode_map_bsize + d->zone_map_bsize;
}

static void load_minix_bitmaps(minix_fs_data * m,
                               minix_super_block * d)
{
    uint inodebmap_start = MINIX_INODEBMAP_STARTB;
    uint zonebmap_start = MINIX_ZONEBMAP_STARTB;

    void * bitmaps_buf = buffer_from_disk(d,inodebmap_start,
                                          d->inode_map_bsize + d->zone_map_bsize);

    m->inode_bitmap_start_block = inodebmap_start;
    m->zone_bitmap_start_block  = zonebmap_start;

    uint inodebmap_size  = d->inode_map_bsize*MINIX_BLOCK_SIZE;
    uint zonebmap_size   = d->zone_map_bsize*MINIX_BLOCK_SIZE;

    bitset_load(&m->inode_bitmap,bitmaps_buf,inodebmap_size);
    bitset_load(&m->zone_bitmap, (char *) bitmaps_buf +
                inodebmap_size,zonebmap_size);

    m->inode_bitmap_blocks = d->inode_map_bsize;
    m->zone_bitmap_blocks = d->zone_map_bsize;
}

static void minix_initialize_fs_specific(super_block * block,
                                         minix_super_block * d)
{
    minix_fs_data * m = kmalloc(sizeof(minix_fs_data));
    if(m == NULL)
        kernel_panic("No hay memoria para bitmaps de MINIX");

    load_minix_specific_metadata(m,d);
    load_minix_bitmaps(m,d);

    block->fs_data = m;
}

static void read_super_block(minix_super_block * res)
{
    char buffer[MINIX_BLOCK_SIZE];
    hdd_read(MINIX_SUPERBLOCK_START_BLOCK,
             MINIX_SUPERBLOCK_BSIZE, buffer);
    memcpy(res,buffer,sizeof(minix_super_block));
}

static void init_super_block(super_block * b)
{
    // TODO: Aca va lo que inicializa el super bloque
    INIT_LIST_HEAD(&b->open_inodes);
}

static void minix_obtain_root_inode(super_block * b)
{
    b->root = b->ops->alloc_inode(b);
    if(b->root == NULL)
        kernel_panic("No se pudo leer inodo raiz");
    b->root->inode_number = MINIX_ROOT_INODE;
    b->ops->read_inode(b->root);
}

int fs_minix_init(super_block * block)
{
    buffer_cache_init();

    minix_super_block b;
    read_super_block(&b);

    if(b.magic != MINIX_MAGIC)
        kernel_panic("Magic Number invalido en superbloque");

    init_super_block(block);
    minix_init_disk_info(block,&b);
    minix_initialize_fs_specific(block,&b);
    minix_init_fs_ops(block);
    minix_obtain_root_inode(block);

    return 0;
}
