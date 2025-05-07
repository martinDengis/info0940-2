#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <bsd/string.h>

#ifndef __APPLE__
#include <stdio_ext.h>
#define fpurge __fpurge
#endif

#include "../include/error.h"
#include "../include/vdisk.h"

const int VDISK_SECTOR_SIZE = 1024;

int vdisk_on(char *filename, DISK *diskp) {
    FILE *vdisk = fopen(filename, "r+b");
    diskp->fp = vdisk;
    if (vdisk == NULL) {
        if (errno == EACCES) {
            return vdisk_EACCESS;
        }
        if (errno == ENOENT) {
            return vdisk_ENOEXIST;
        } else {
            return -1; // unknown error
        }
    }
    int filename_length = strlen(filename) + 1;
    diskp->name = malloc(filename_length);
    strlcpy(diskp->name, filename, filename_length);

    fseek(vdisk, 0L, SEEK_END);
    diskp->size_in_sectors = ftell(vdisk) / VDISK_SECTOR_SIZE;
    if (diskp->size_in_sectors == 0) {
        vdisk_off(diskp);
        return vdisk_ENODISK;
    }
    diskp->sector_size = VDISK_SECTOR_SIZE;
    return 0;
}

int seek_sector(DISK *diskp, uint32_t sector) {
    FILE *vdisk = diskp->fp;
    if (vdisk == NULL) {
        return vdisk_ENODISK;
    }
    if (sector >= diskp->size_in_sectors) {
        return vdisk_EEXCEED;
    }
    fseek(vdisk, sector * diskp->sector_size, SEEK_SET);
    return 0;
}

inline int vdisk_read(DISK *diskp, uint32_t sector, uint8_t *buffer) {
    int err = seek_sector(diskp, sector);
    if (err) {
        return err;
    }
    if (fread(buffer, 1, diskp->sector_size, diskp->fp) != diskp->sector_size) {
        return vdisk_ESECTOR;
    }
    return 0;
}

inline int vdisk_write(DISK *diskp, uint32_t sector, uint8_t *buffer) {
    int err = seek_sector(diskp, sector);
    if (err) {
        return err;
    }
    if (fwrite(buffer, 1, diskp->sector_size, diskp->fp) != diskp->sector_size) {
        return vdisk_ESECTOR;
    }
    return 0;
}

int vdisk_sync(DISK *diskp) {
    FILE *vdisk = diskp->fp;
    if (vdisk == NULL){
        return vdisk_ENODISK;
    }
    fflush(vdisk);
    fsync(fileno(vdisk));
    return 0;
}

void vdisk_off(DISK *diskp) {
    FILE *vdisk = diskp->fp;
    if (vdisk == NULL) {
        return;
    }
    fpurge(vdisk);
    fclose(vdisk);
    free(diskp->name);
    diskp->fp = NULL;
}
