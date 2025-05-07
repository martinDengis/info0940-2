#ifndef ERROR_H
#define ERROR_H

extern const int vdisk_ENODISK ;
extern const int vdisk_EACCESS ;
extern const int vdisk_ENOEXIST;
extern const int vdisk_EEXCEED ;
extern const int vdisk_ESECTOR ;

#define E_DISK_NOT_MOUNTED      -100  // Disk not mounted
#define E_DISK_ALREADY_MOUNTED  -101  // Disk already mounted
#define E_INVALID_INODE         -102  // Invalid inode number
#define E_OUT_OF_SPACE          -103  // No space left on disk
#define E_OUT_OF_INODES         -104  // No free inodes
#define E_CORRUPT_DISK          -105  // Corrupt disk image
#define E_INVALID_OFFSET        -106  // Invalid offset

#endif
