#include <tty.h>
#include <tasks.h>
#include <keyboard.h>
#include <scrn.h>
#include <utils.h>

process_info * terminal_owner;

int read_tty(file_object * obj, uint bytes, void * _data)
{
	char c, * data = _data;
   	int read = 0; ushort mode = scrn_getmode();
	while(bytes-- > 0){	
		while((c = keybuffer_consume()) == -1)
			do_sleep();
		data[read++] = c;
		if(c == '\n'){
			scrn_putc(c,mode);
			return read;
		}else if(c == '\b'){
			read -= 2;
			if(read < 0){ 
				read = 0;
			}else{
				scrn_move_back();
			}
		}else{
			scrn_putc(c,mode);
		}	
	}
	return read;
}

int write_tty(file_object * obj, uint bytes, void * _data)
{
	char * data = _data;
	ushort mode = scrn_getmode();
	for(uint i = 0; i < bytes; i++){
		scrn_putc(data[i],mode);
	}
	return bytes;
}

int open_tty(inode * ino, file_object * obj)
{
	if(!ino){
		kernel_panic("No hay inodo");
		return -EINVINODE;
	}
	if(ino->inode_type != FS_CHARDEV){
		kernel_panic("Inodo no es chardev");
		return -EINVINODETYPE;
	}

	char_dev * dev = ino->info_cdev;
	if(!dev || dev->major_number != TTY_MAJOR 
			|| dev->minor_number != TTY_MINOR){
		kernel_panic("Inodo no es terminal");	
		return -EINVINODETYPE;
	}	
	
	ino->use_count++;
	obj->f_ops = ino->f_ops;
	obj->inode = ino;

	obj->file_read_offset  = 
	obj->file_write_offset = 0;
	
	scrn_cls();	
	return 0;
}

fs_ops tty_fs_ops = {
	.open 	= open_tty,
	.read	= read_tty,
	.write	= write_tty
};
