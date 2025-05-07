#ifndef VDISK_H
#define VDISK_H

#include <stdint.h>
#include <stdio.h>

typedef struct {
    uint32_t sector_size;
    uint32_t size_in_sectors;
    char *name;
    FILE *fp;
} DISK;

int vdisk_on(char *filename, DISK *diskp);
int vdisk_read(DISK *diskp, uint32_t sector, uint8_t *buffer);
int vdisk_write(DISK *diskp, uint32_t sector, uint8_t *buffer);
int vdisk_sync(DISK *diskp);
void vdisk_off(DISK *diskp);

#endif
