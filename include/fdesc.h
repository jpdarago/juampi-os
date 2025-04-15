#ifndef __FILE_DESCRIPTORS_H
#define __FILE_DESCRIPTORS_H

#include <tasks.h>
#include <types.h>
#include <vfs.h>

file_object * get_file_object(process_info *, int);
bool unused_fd(process_info *, int);
void set_fd(process_info *,int,file_object *);
int add_file_object(process_info *, file_object *);
int remove_file_object(process_info *, int);
void copy_file_descriptors(process_info *,process_info *);

#endif
