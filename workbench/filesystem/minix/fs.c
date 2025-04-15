file_object * get_file_object(process_info * p, int fd){
	if(fd < 0 || fd >= MAX_FDS) return NULL;
	return p->fds[fd];
}

int add_file_object(process_info * p, file_object * f){
	file_object ** fptrs = p->fds;
	for(int i = 0; i < MAX_FDS; i++){
		if(fptrs[i] == NULL){
			fptrs[i] = f;
			return i;	
		}
	}
	return -ENOFDSPACE;
}

int remove_file_object(process_info * p, int fd){
	file_object * target = get_file_object(p,fd);	
	if(target == NULL) return -EINVFD;
	file_object ** pos = &fptrs[fd];
	if(*pos == NULL) return -EINVFD;
	kfree(*pos); *pos = NULL;	
	return 0;
}

int next_part(char ** pathptr,char * buffer){
	int i;	
	char * pathname = *pathptr;
	for(i = 0; i < FILE_MAXLEN;i++){
		if(pathname[i] == '/') break;
		if(pathname[i] == '\0') break;	
		buffer[i] = pathname[i];
	}	
	buffer[i] = '\0';
	*pathptr += i;
	return i;
}

inode * process_path(inode * start,char * pathname)
{	
	process_info * 	p = get_current_task();
	super_block *	s = get_disk_superblock();

	inode * root = s->root;	
	if(pathname[0] == '/'){
		pathname++;
		start = root;
	}else{
		start = root->i_ops->lookup(p->cwd);	
	}

	char chunk[FILE_MAXLEN+1];
	while(next_part(&pathname,chunk) > 0){
		if(!ino) return NULL;
	   	if(ino->inode_type != FS_DIR) 
			return NULL;
			
		inode * ino = start->i_ops->lookup(start,chunk);
		start = ino;
	}
	return ino;
}

int do_open(char * pathname, uint flags)
{
	inode * ino = process_path(pathname);
	if(ino == NULL) return -EINVPATH;
	file_object * f = kmalloc(sizeof(file_object));
	f->flags = get_flags(flags);
	f->access_mode = get_access_mode(flags);
	if(ino->f_ops->open == NULL)
		return -INVOP;	

	ino->f_ops->open(&ino,&f);
	return add_file_object(p,f);
}

int do_read(int fd, int bytes, void * buffer)
{
	file_object * f = get_file_object(get_current_task(),fd);
	if(f == NULL) return -INVFD;
	if(f->f_ops->read == NULL) return -INVOP;
	return f->f_ops->read(&f,bytes,buffer);	
}

int do_write(int fd, int bytes, void * buffer)
{
	file_object * f = get_file_object(get_current_task(),fd);
	if(f == NULL) return -INVFD;
	if(f->f_ops->write == NULL) return -INVOP;
	return f->f_ops->write(&f,bytes,buffer);
}

int do_readdir(int fd, dir_entry * d)
{
	file_object * f = get_file_object(get_current_task(),fd);
	if(f == NULL) return -INVFD;
	if(f->f_ops->readdir == NULL) return -INVOP;
	return f->f_ops->readdir(&f,&d);
}

int do_close(int fd)
{
	file_object * f = get_file_object(get_current_task(),fd);
	if(f == NULL) return -INVFD;
	if(f->f_ops->close == NULL) return -INVOP;
	int res = f->f_ops->close(&f);
	return remove_file_object(fd,f);
}

static inode * basedir_inode(char ** pathptr, int len)
{
	int i; char * pathname = *pathptr;	
	for(int i = len;i >= 0; i--)
		if(pathname[i] == '/')
			break;
	if(i < 0) return NULL;
	pathname[i] = '\0';
	inode * ino = process_path(pathname);
	if(ino == NULL) return NULL;
	//Hacemos apuntar el pathname al ultimo
	//pedazo del directorio
	*pathptr = pathname+i+1;	
	return ino;	
}

int do_mkdir(char * pathname,int len)
{
	inode * ino = basedir_inode(&pathname,len);
	if(ino == NULL) return -INVPATH;	
	if(ino->inode_type != FS_DIR)
		return -INVOP;
	if(ino->i_ops->mkdir == NULL)
		return -INVOP;
	return ino->i_ops->mkdir(ino,pathname);	
}

int do_unlink(char * pathname)
{
	inode * ino = basedir_inode(&pathname,len);
	if(ino == NULL) return -INVPATH;
	if(ino->inode_type != FS_DIR)
		return -INVOP;
	if(ino->i_ops->unlink == NULL)
		return -INVOP;
	return ino->i_ops->unlink(ino,pathname+i+1);
}

int do_rmdir(char * pathname)
{
	inode * ino = basedir_inode(&pathname,len);	
	if(ino == NULL) return -INVPATH;
	if(ino->inode_type != FS_DIR)
		return -INVOP;
	if(ino->i_ops->mkdir == NULL)
		return -INVOP;
	return ino->i_ops->mkdir(ino,pathname+i+1);
}
