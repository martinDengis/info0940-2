#ifndef FS_H
#define FS_H

#include <stdint.h>

int format(char *disk_name, int inodes);
int stat(int inode_num);
int mount(char *disk_name);
int unmount();
int create();
int delete(int inode_num);
int read(int inode_num, uint8_t *data, int len, int offset);
int write(int inode_num, uint8_t *data, int len, int offset);
#endif
