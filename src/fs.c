#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "include/fs.h"
#include "include/vdisk.h"
#include "include/error.h"

#define BLOCK_SIZE 1024
#define INODE_SIZE 32
#define INODES_PER_BLOCK (BLOCK_SIZE / INODE_SIZE)
#define POINTERS_PER_BLOCK (BLOCK_SIZE / sizeof(uint32_t))
#define MAGIC_NUMBER "\xf0\x55\x4c\x49\x45\x47\x45\x49\x4e\x46\x4f\x30\x39\x34\x30\x0f"


/*************************/
/* Data structures       */
/*************************/

// Super block structure
typedef struct
{
    uint8_t magic[16];         // Magic # for SSFS
    uint32_t num_blocks;       // Total # of blocks
    uint32_t num_inode_blocks; // Number of inode blocks
    uint32_t block_size;       // Block size in bytes (1024)
} superblock_t;


// Inode structure (32 bytes)
typedef struct
{
    uint8_t valid;                  // 0 if free, 1 if allocated
    uint32_t size;                  // File size in bytes
    uint32_t direct_blocks[4];      // Direct block pointers
    uint32_t indirect_block;        // Single indirect block pointer
    uint32_t double_indirect_block; // Double indirect block pointer
} inode_t;


// File system state
static bool disk_mounted = false;
static DISK disk; // Defined in vdisk.h
static superblock_t superblock;
static uint32_t *block_bitmap = NULL; // For tracking free blocks
static char *mounted_disk = NULL;


/*************************/
/* Forward declarations  */
/*************************/

static int read_inode(int inode_num, inode_t *inode, bool bypass_mount_check);
static int write_inode(int inode_num, inode_t *inode);
static void free_block(int block_num);
static int find_free_block(void);
static int get_block_for_offset(inode_t *inode, int offset, bool allocate);


/*************************/
/* Core functions        */
/*************************/

/*
 * NB: before formatting the image, we mut rightfully create it.
 * -> Can do so using the `dd` command as such:
 *      `dd if=/dev/zero of=mydisk.img bs=1024 count=100`
 *  where,
 *      - dd: data duplicator/converter command
 *      - if=/dev/zero: Input File - reads from /dev/zero, which is a special file
 *                      that provides unlimited stream of zero bytes
 *      - of=mydisk.img: Output File - writes to file named mydisk.img
 *      - bs=1024: Block Size - to be set at 1024 bytes as per the statement
 *      - count=100: Number of blocks to copy - copies exactly 100 blocks
 */
int format(char *disk_name, int inodes)
{
    // Precondition: Check if disk already mounted
    if (disk_mounted)
    {
        return E_DISK_ALREADY_MOUNTED;
    }

    // Precondition: Adjust inodes to be at least 1
    if (inodes <= 0)
    {
        inodes = 1;
    }

    // Open disk image file
    DISK format_disk;
    int result = vdisk_on(disk_name, &format_disk);
    if (result != 0)
    {
        return result;
    }

    // Get required # of inode blocks (ceiling division)
    int num_inode_blocks = (inodes + INODES_PER_BLOCK - 1) / INODES_PER_BLOCK;
    if (num_inode_blocks <= 0)
    {
        num_inode_blocks = 1;
    }

    // Calculate total # of blocks available on disk
    uint32_t total_blocks = format_disk.size_in_sectors;

    // Ensure enough space for at least one data block
    //  +1 to account for the superblock!
    if ((uint32_t)num_inode_blocks + 1 >= total_blocks)
    {
        vdisk_off(&format_disk);
        return E_OUT_OF_SPACE; // can't fit inode b + superb + (>=1) one data b
    }

    // Init superblock
    superblock_t sb;
    memcpy(sb.magic, MAGIC_NUMBER, 16);
    sb.num_blocks = total_blocks;
    sb.num_inode_blocks = num_inode_blocks;
    sb.block_size = BLOCK_SIZE;

    // Write superblock to block 0
    uint8_t block_buffer[BLOCK_SIZE] = {0};
    memcpy(block_buffer, &sb, sizeof(superblock_t));
    result = vdisk_write(&format_disk, 0, block_buffer);
    if (result != 0)
    {
        vdisk_off(&format_disk);
        return result;
    }

    // Init inode blocks (starting at block idx 1)
    memset(block_buffer, 0, BLOCK_SIZE); // zero-out buffer
    for (int i = 1; i <= num_inode_blocks; i++)
    {
        result = vdisk_write(&format_disk, i, block_buffer);
        if (result != 0)
        {
            vdisk_off(&format_disk);
            return result;
        }
    }

    // Sync to ensure all changes are written to disk
    result = vdisk_sync(&format_disk);
    if (result != 0)
    {
        vdisk_off(&format_disk);
        return result;
    }

    // Close the disk and return success
    vdisk_off(&format_disk);
    return 0;
}

int mount(char *disk_name)
{
    // 1. Check if disk already mounted
    if (disk_mounted)
    {
        return E_DISK_ALREADY_MOUNTED;
    }

    // 2. Open disk image file
    int result = vdisk_on(disk_name, &disk);
    if (result != 0)
    {
        return result;
    }

    // 3. Read superblock (Block 0) and copy its data
    uint8_t block_buffer[BLOCK_SIZE];
    result = vdisk_read(&disk, 0, block_buffer);
    if (result != 0)
    {
        vdisk_off(&disk);
        return result;
    }

    memcpy(&superblock, block_buffer, sizeof(superblock_t));

    // 4. Verify the magic #
    if (memcmp(superblock.magic, MAGIC_NUMBER, 16) != 0)
    {
        vdisk_off(&disk);
        return E_CORRUPT_DISK;
    }

    // 5. Allocate mem for the block bitmap
    block_bitmap = (uint32_t *)calloc(superblock.num_blocks, sizeof(uint32_t));
    if (block_bitmap == NULL)
    {
        vdisk_off(&disk);
        return E_OUT_OF_SPACE;  // see error.h
    }

    // 6. Init block bitmap - mark superblock and inode blocks as used
    block_bitmap[0] = 1;
    for (uint32_t i = 1; i <= superblock.num_inode_blocks; i++)
    {
        block_bitmap[i] = 1;
    }

    // Scan all inodes to mark data blocks as used if allocated
    for (uint32_t i = 0; i < superblock.num_inode_blocks * INODES_PER_BLOCK; i++)
    {
        inode_t inode;
        int result = read_inode(i, &inode, true);
        if (result != 0)
        {
            free(block_bitmap);
            block_bitmap = NULL;
            vdisk_off(&disk);
            return result;
        }

        if (inode.valid)
        {
            // Mark direct blocks
            for (int j = 0; j < 4; j++)
            {
                if (inode.direct_blocks[j] != 0)
                {
                    block_bitmap[inode.direct_blocks[j]] = 1;
                }
            }

            // Mark indirect block
            if (inode.indirect_block != 0)
            {
                block_bitmap[inode.indirect_block] = 1;

                uint8_t indirect_block[BLOCK_SIZE];
                result = vdisk_read(&disk, inode.indirect_block, indirect_block);
                if (result != 0)
                {
                    free(block_bitmap);
                    block_bitmap = NULL;
                    vdisk_off(&disk);
                    return result;
                }

                // Set non-zero entries in indirect block as used
                uint32_t *pointers = (uint32_t *)indirect_block;
                for (uint32_t k = 0; k < POINTERS_PER_BLOCK; k++)
                {
                    if (pointers[k] != 0)
                    {
                        block_bitmap[pointers[k]] = 1;
                    }
                }
            }

            // Mark double indirect block
            if (inode.double_indirect_block != 0)
            {
                block_bitmap[inode.double_indirect_block] = 1;

                uint8_t double_indirect_block[BLOCK_SIZE];
                result = vdisk_read(&disk, inode.double_indirect_block, double_indirect_block);
                if (result != 0)
                {
                    free(block_bitmap);
                    block_bitmap = NULL;
                    vdisk_off(&disk);
                    return result;
                }

                // Process pointer in the double indirect block
                uint32_t *indirect_pointers = (uint32_t *)double_indirect_block;
                for (uint32_t j = 0; j < POINTERS_PER_BLOCK; j++)
                {
                    if (indirect_pointers[j] != 0)
                    {
                        // Mark indir block as used
                        block_bitmap[indirect_pointers[j]] = 1;

                        uint8_t curr_indirect_block[BLOCK_SIZE];
                        result = vdisk_read(&disk, indirect_pointers[j], curr_indirect_block);
                        if (result != 0)
                        {
                            free(block_bitmap);
                            block_bitmap = NULL;
                            vdisk_off(&disk);
                            return result;
                        }

                        // Set non-zero entries in this indirect block
                        uint32_t *data_pointers = (uint32_t *)curr_indirect_block;
                        for (uint32_t k = 0; k < POINTERS_PER_BLOCK; k++)
                        {
                            if (data_pointers[k] != 0)
                            {
                                block_bitmap[data_pointers[k]] = 1;
                            }
                        }
                    }
                }
            }
        }
    }

    // 7. Store disk name
    int name_length = strlen(disk_name) + 1;
    mounted_disk = (char *)malloc(name_length);
    if (mounted_disk == NULL)
    {
        free(block_bitmap);
        block_bitmap = NULL;
        vdisk_off(&disk);
        return E_OUT_OF_SPACE; // see error.h
    }
    strcpy(mounted_disk, disk_name);

    // 8. Set disk_mounted flag
    disk_mounted = true;

    return 0; // Success
}

int unmount(void)
{
    // 1. Check if disk is currently mounted
    if (!disk_mounted)
    {
        return E_DISK_NOT_MOUNTED;
    }

    // 2. Sync any pending changes to disk
    int result = vdisk_sync(&disk);
    // we actually don't check the result here
    // because we want to clean up even if sync fails
    // -> will check in the final return

    // 3. Free memory allocated for block bitmap
    if (block_bitmap != NULL)
    {
        free(block_bitmap);
        block_bitmap = NULL;
    }

    // 4. Free memory allocated for mounted disk name
    if (mounted_disk != NULL)
    {
        free(mounted_disk);
        mounted_disk = NULL;
    }

    // 5. Close virtual disk and reset flag
    vdisk_off(&disk);
    disk_mounted = false;

    // Return 0 for success or err code from vdisk_sync if it failed
    return (result == 0) ? 0 : result;
}

int create(void)
{
    // 1. Check for disk mounted
    if (!disk_mounted)
    {
        printf("Disk not mounted\n");
        return E_DISK_NOT_MOUNTED;
    }

    // 2. Iterate through all inodes
    int max_inodes = superblock.num_inode_blocks * INODES_PER_BLOCK;
    for (int inode_num = 0; inode_num < max_inodes; inode_num++)
    {
        // 3. Get curr inode
        inode_t inode;
        int result = read_inode(inode_num, &inode, false);
        if (result != 0)
        {
            return result;
        }

        // 4. Check if inode is free (valid = 0)
        if (inode.valid == 0)
        {
            // Init inode fields
            inode.valid = 1; // mark as allocated
            inode.size = 0;  // empty file

            // Init all block pointers to 0
            for (int i = 0; i < 4; i++)
            {
                inode.direct_blocks[i] = 0;
            }
            inode.indirect_block = 0;
            inode.double_indirect_block = 0;

            // 5. Write inode back to disk
            result = write_inode(inode_num, &inode);
            if (result != 0)
            {
                return result;
            }

            return inode_num;
        }
    }

    // 7. If we get here -> no free inodes found
    return E_OUT_OF_INODES;
}

int delete(int inode_num)
{
    // 1. Check for disk mounted
    if (!disk_mounted)
    {
        return E_DISK_NOT_MOUNTED;
    }

    // 2. Check if inode # is valid
    if (inode_num < 0 || (uint32_t)inode_num >= superblock.num_inode_blocks * INODES_PER_BLOCK)
    {
        return E_INVALID_INODE;
    }

    // 3. Read inode
    inode_t inode;
    int result = read_inode(inode_num, &inode, false);
    if (result != 0)
    {
        return result;
    }

    // 4. Check if inode is allocated
    if (inode.valid == 0)
    {
        return E_INVALID_INODE; // inode already free
    }

    // 5. Free direct blocks
    for (int i = 0; i < 4; i++)
    {
        if (inode.direct_blocks[i] != 0)
        {
            free_block(inode.direct_blocks[i]);
            inode.direct_blocks[i] = 0;
        }
    }

    // 6. Free indirect block and all blocks it points to
    if (inode.indirect_block != 0)
    {
        // Read the indirect block
        uint8_t indirect_block[BLOCK_SIZE];
        result = vdisk_read(&disk, inode.indirect_block, indirect_block);
        if (result != 0)
        {
            return result;
        }

        // Free all referenced data blocks
        uint32_t *pointers = (uint32_t *)indirect_block;
        for (uint32_t i = 0; i < POINTERS_PER_BLOCK; i++)
        {
            if (pointers[i] != 0)
            {
                free_block(pointers[i]);
            }
        }

        // Free indirect block itself
        free_block(inode.indirect_block);
        inode.indirect_block = 0;
    }

    // 7. Free double indirect block and all blocks it points to
    if (inode.double_indirect_block != 0)
    {
        // Read the double indirect block
        uint8_t double_indirect_block[BLOCK_SIZE];
        result = vdisk_read(&disk, inode.double_indirect_block, double_indirect_block);
        if (result != 0)
        {
            return result;
        }

        // Process pointer in the double indirect block
        uint32_t *indirect_pointers = (uint32_t *)double_indirect_block;
        for (uint32_t i = 0; i < POINTERS_PER_BLOCK; i++)
        {
            if (indirect_pointers[i] != 0)
            {
                // Read this indirect block
                uint8_t indirect_block[BLOCK_SIZE];
                result = vdisk_read(&disk, indirect_pointers[i], indirect_block);
                if (result != 0)
                {
                    return result;
                }

                // Free all referenced data blocks
                uint32_t *data_pointers = (uint32_t *)indirect_block;
                for (uint32_t j = 0; j < POINTERS_PER_BLOCK; j++)
                {
                    if (data_pointers[j] != 0)
                    {
                        free_block(data_pointers[j]);
                    }
                }

                // Free indirect block itself
                free_block(indirect_pointers[i]);
            }
        }

        // Free double indirect block itself
        free_block(inode.double_indirect_block);
        inode.double_indirect_block = 0;
    }

    // 8. Mark inode as free
    inode.valid = 0;
    inode.size = 0;

    // 9. Write back to disk
    result = write_inode(inode_num, &inode);
    if (result != 0)
    {
        return result;
    }

    return 0;
}

int stat(int inode_num)
{
    // 1. Check for disk mounted
    if (!disk_mounted)
    {
        return E_DISK_NOT_MOUNTED;
    }

    // 2. Check if inode # is valid
    if (inode_num < 0 || (uint32_t)inode_num >= superblock.num_inode_blocks * INODES_PER_BLOCK)
    {
        return E_INVALID_INODE;
    }

    // 3. Read inode
    inode_t inode;
    int result = read_inode(inode_num, &inode, false);
    if (result != 0)
    {
        return result;
    }

    // 4. Check if inode is allocated/valid
    if (inode.valid == 0)
    {
        return E_INVALID_INODE;
    }

    return inode.size;
}

int read(int inode_num, uint8_t *data, int len, int offset)
{
    // 1. Check for disk  mounted
    if (!disk_mounted)
    {
        return E_DISK_NOT_MOUNTED;
    }

    // 2. Check if inode # is valid
    if (inode_num < 0 || (uint32_t)inode_num >= superblock.num_inode_blocks * INODES_PER_BLOCK)
    {
        return E_INVALID_INODE;
    }

    // 3. Read inode
    inode_t inode;
    int result = read_inode(inode_num, &inode, false);
    if (result != 0)
    {
        return result;
    }

    // 4. Check if inode is allocated/valid)
    if (inode.valid == 0)
    {
        return E_INVALID_INODE;
    }

    // 5. Determine actual # of bytes to read
    int bytes_to_read = 0;
    if ((uint32_t)offset < inode.size)
    {
        bytes_to_read = inode.size - offset;
        if (bytes_to_read > len)
        {
            bytes_to_read = len;
        }
    }

    // 6. If no bytes to read, return 0
    if (bytes_to_read <= 0)
    {
        return 0;
    }

    // 7. Init counter for total bytes read
    int bytes_read = 0;
    uint32_t current_offset = offset;

    // 8. Read block by block
    while (bytes_read < bytes_to_read)
    {
        // Get curr block idx and offset w/in the block
        // Then get physical block # for curr offset
        int block_offset = current_offset % BLOCK_SIZE;
        int block_num = get_block_for_offset(&inode, current_offset, false);

        // If <=0, that means null pointer or error
        if (block_num <= 0)
        {
            break;
        }

        // Read the block into temp buffer
        uint8_t block[BLOCK_SIZE];
        result = vdisk_read(&disk, block_num, block);
        if (result != 0)
        {
            // but if some data has already been read, return the count
            // else, return the error
            return (bytes_read > 0) ? bytes_read : result;
        }

        // Calculate how many bytes to copy from this block
        int bytes_to_copy = BLOCK_SIZE - block_offset;
        if (bytes_to_copy > (bytes_to_read - bytes_read))
        {
            bytes_to_copy = bytes_to_read - bytes_read;
        }

        // Copy data from block to user buffer
        memcpy(data + bytes_read, block + block_offset, bytes_to_copy);

        // Update counters
        bytes_read += bytes_to_copy;
        current_offset += bytes_to_copy;
    }

    return bytes_read;  // # of bytes actually read
}

int write(int inode_num, uint8_t *data, int len, int offset)
{
    // 1. Check for disk mounted
    if (!disk_mounted)
    {
        return E_DISK_NOT_MOUNTED;
    }

    // 2. Check if inode # is valid
    if (inode_num < 0 || (uint32_t)inode_num >= superblock.num_inode_blocks * INODES_PER_BLOCK)
    {
        return E_INVALID_INODE;
    }

    // 3. Read the inode
    inode_t inode;
    int result = read_inode(inode_num, &inode, false);
    if (result != 0)
    {
        return result;
    }

    // 4. Check if inode is allocated/valid
    if (inode.valid == 0)
    {
        return E_INVALID_INODE;
    }

    // 5. If offset beyond curr file size, fill the gap with 0s
    if ((uint32_t)offset > inode.size)
    {
        int zero_fill_start = inode.size;
        int zero_fill_end = offset;
        uint8_t zeros[BLOCK_SIZE] = {0};

        for (int curr_offset = zero_fill_start; curr_offset < zero_fill_end; )
        {
            int block_offset = curr_offset % BLOCK_SIZE;
            int block_num = get_block_for_offset(&inode, curr_offset, true);

            if (block_num <= 0)
            {
                // Error allocating block
                // but potentially we've already modified the file
                // <=> update inode size to reflect changes so far
                inode.size = ((uint32_t)curr_offset > inode.size) ? (uint32_t)curr_offset : inode.size;
                write_inode(inode_num, &inode);
                return block_num; // err code
            }

            // Get how many bytes to fill in this block
            int bytes_to_fill = BLOCK_SIZE - block_offset;
            if (bytes_to_fill > (zero_fill_end - curr_offset))
            {
                bytes_to_fill = zero_fill_end - curr_offset;
            }

            // If block not empty / we're not writing a full block,
            // we need to read the existing block
            uint8_t block[BLOCK_SIZE];
            if (block_offset > 0 || bytes_to_fill < BLOCK_SIZE)
            {
                result = vdisk_read(&disk, block_num, block);
                if (result != 0)
                {
                    // If read fails -> update inode and return the error
                    inode.size = ((uint32_t)curr_offset > inode.size) ? (uint32_t)curr_offset : inode.size;
                    write_inode(inode_num, &inode);
                    return result;
                }
            }
            else
            {
                // If writing a full block, just use our 0-buffer
                memcpy(block, zeros, BLOCK_SIZE);
            }

            // Fill the appropriate portion of the block with 0s
            memset(block + block_offset, 0, bytes_to_fill);

            // Write block back to disk
            result = vdisk_write(&disk, block_num, block);
            if (result != 0)
            {
                // If write fails -> update inode and return the error
                inode.size = ((uint32_t)curr_offset > inode.size) ? (uint32_t)curr_offset : inode.size;
                write_inode(inode_num, &inode);
                return result;
            }

            // Update offset
            curr_offset += bytes_to_fill;
        }

        // Update inode size to new offset
        inode.size = offset;
    }

    // 6. Write data from user buffer
    int bytes_written = 0;
    int current_offset = offset;

    while (bytes_written < len)
    {
        // Get block idx and offset w/in the block
        int block_offset = current_offset % BLOCK_SIZE;
        int block_num = get_block_for_offset(&inode, current_offset, true); // pass allocate=true for potential new block

        // If error getting/allocating the block
        if (block_num <= 0)
        {
            // Update inode size to reflect changes so far
            if ((uint32_t)current_offset > inode.size)
            {
                inode.size = current_offset;
                write_inode(inode_num, &inode);
            }
            return (bytes_written > 0) ? bytes_written : block_num;
        }

        // Get how many bytes to write to this block
        int bytes_to_write = BLOCK_SIZE - block_offset;
        if (bytes_to_write > (len - bytes_written))
        {
            bytes_to_write = len - bytes_written;
        }

        // If not writing a full block or starting from the beginning of a block,
        // => then need to read the existing block to preserve data
        uint8_t block[BLOCK_SIZE];
        if (block_offset > 0 || bytes_to_write < BLOCK_SIZE)
        {
            result = vdisk_read(&disk, block_num, block);
            if (result != 0)
            {
                // If some data was already written, update size and rtn count
                if (bytes_written > 0)
                {
                    if ((uint32_t)current_offset > inode.size)
                    {
                        inode.size = current_offset;
                        write_inode(inode_num, &inode);
                    }
                    return bytes_written;
                }
                return result;
            }
        }

        // Copy data from user buffer to block
        memcpy(block + block_offset, data + bytes_written, bytes_to_write);

        // Write block back to disk
        result = vdisk_write(&disk, block_num, block);
        if (result != 0)
        {
            // If some data was already written, update size and rtn count
            if (bytes_written > 0)
            {
                if ((uint32_t)current_offset > inode.size)
                {
                    inode.size = current_offset;
                    write_inode(inode_num, &inode);
                }
                return bytes_written;
            }
            return result;
        }

        // Update counters
        bytes_written += bytes_to_write;
        current_offset += bytes_to_write;
    }

    // 7. Update inode size if the write extended the file
    if ((uint32_t)current_offset > inode.size)
    {
        inode.size = current_offset;
        result = write_inode(inode_num, &inode);
        if (result != 0)
        {
            // Even writing inode fails, we have written data,
            // so return count of bytes written so far
            return bytes_written;
        }
    }

    return bytes_written;
}




/*************************/
/* Helper functions      */
/*************************/

// Helper function to read an inode from disk
// bypass_mount_check: if true, skip the mounted disk check (used only during mount operation)
static int read_inode(int inode_num, inode_t *inode, bool bypass_mount_check)
{
    if (!disk_mounted && !bypass_mount_check)
    {
        return E_DISK_NOT_MOUNTED;
    }

    if (inode_num < 0 || (uint32_t)inode_num >= superblock.num_inode_blocks * INODES_PER_BLOCK)
    {
        return E_INVALID_INODE;
    }

    // Calculate block # and offset for the inode
    int block_num = 1 + (inode_num / INODES_PER_BLOCK); // +1 because block 0 is superblock
    int offset = (inode_num % INODES_PER_BLOCK) * INODE_SIZE;

    // Read the block containing the inode
    uint8_t block[BLOCK_SIZE];
    int result = vdisk_read(&disk, block_num, block);
    if (result != 0)
    {
        return result;
    }

    // Copy inode data
    memcpy(inode, block + offset, INODE_SIZE);

    return 0;
}

// Helper function to write an inode to disk
static int write_inode(int inode_num, inode_t *inode)
{
    if (!disk_mounted)
    {
        return E_DISK_NOT_MOUNTED;
    }

    if (inode_num < 0 || (uint32_t)inode_num >= superblock.num_inode_blocks * INODES_PER_BLOCK)
    {
        return E_INVALID_INODE;
    }

    // Calculate block # and offset for the inode
    int block_num = 1 + (inode_num / INODES_PER_BLOCK); // +1 because block 0 is superblock
    int offset = (inode_num % INODES_PER_BLOCK) * INODE_SIZE;

    // Read the block containing the inode
    uint8_t block[BLOCK_SIZE];
    int result = vdisk_read(&disk, block_num, block);
    if (result != 0)
    {
        return result;
    }

    // Update inode data in the block
    memcpy(block + offset, inode, INODE_SIZE);

    // Write the block back to disk
    result = vdisk_write(&disk, block_num, block);
    if (result != 0)
    {
        return result;
    }

    return 0;
}

// Helper function to find a free block
static int find_free_block()
{
    if (!disk_mounted)
    {
        return E_DISK_NOT_MOUNTED;
    }

    // Start from the first data block (after superblock and inode blocks)
    int first_data_block = 1 + superblock.num_inode_blocks;

    // Search for the first available block using first-available strategy
    for (uint32_t i = (uint32_t)first_data_block; i < superblock.num_blocks; i++)
    {
        if (block_bitmap[i] == 0)
        {
            // Mark the block as used
            block_bitmap[i] = 1;
            return i;
        }
    }

    return E_OUT_OF_SPACE; // No free blocks available
}

// Helper function to mark a block as free
static void free_block(int block_num)
{
    if (disk_mounted && block_num > 0 && (uint32_t)block_num < superblock.num_blocks)
    {
        // Mark the block as free in the bitmap
        block_bitmap[block_num] = 0;
    }
}

// Helper function to get block # for a specific file offset
static int get_block_for_offset(inode_t *inode, int offset, bool allocate)
{
    if (!disk_mounted)
    {
        return E_DISK_NOT_MOUNTED;
    }
    if (offset < 0)
    {
        return E_INVALID_OFFSET;
    }

    // Calculate which block this offset falls into
    int block_index = offset / BLOCK_SIZE;

    // Direct blocks (0-3)
    if (block_index < 4)
    {
        if (inode->direct_blocks[block_index] == 0 && allocate)
        {
            // Need to allocate a new block
            int new_block = find_free_block();
            if (new_block < 0)
            {
                return new_block; // Error finding free block
            }

            // Init the new block with 0s
            uint8_t zeros[BLOCK_SIZE] = {0};
            int result = vdisk_write(&disk, new_block, zeros);
            if (result != 0)
            {
                free_block(new_block);
                return result;
            }

            inode->direct_blocks[block_index] = new_block;
        }
        return inode->direct_blocks[block_index];
    }

    // Indirect blocks (4-259)
    block_index -= 4;
    if ((uint32_t)block_index < POINTERS_PER_BLOCK)
    {
        // Check if we have an indirect block
        if (inode->indirect_block == 0)
        {
            if (!allocate)
            {
                return 0; // No block and not allocating
            }

            // Allocate new indirect block
            int new_block = find_free_block();
            if (new_block < 0)
            {
                return new_block;
            }

            // Init with 0s
            uint8_t zeros[BLOCK_SIZE] = {0};
            int result = vdisk_write(&disk, new_block, zeros);
            if (result != 0)
            {
                free_block(new_block);
                return result;
            }

            inode->indirect_block = new_block;
        }

        // Read the indirect block
        uint8_t indirect_block[BLOCK_SIZE];
        int result = vdisk_read(&disk, inode->indirect_block, indirect_block);
        if (result != 0)
        {
            return result;
        }

        uint32_t *pointers = (uint32_t *)indirect_block;

        // Check if we need to allocate a new data block
        if (pointers[block_index] == 0 && allocate)
        {
            int new_block = find_free_block();
            if (new_block < 0)
            {
                return new_block;
            }

            // Init with 0s
            uint8_t zeros[BLOCK_SIZE] = {0};
            result = vdisk_write(&disk, new_block, zeros);
            if (result != 0)
            {
                free_block(new_block);
                return result;
            }

            pointers[block_index] = new_block;

            // Write the updated indirect block back
            result = vdisk_write(&disk, inode->indirect_block, indirect_block);
            if (result != 0)
            {
                free_block(new_block);
                return result;
            }
        }

        return pointers[block_index];
    }

    // Double indirect blocks (260+)
    block_index -= POINTERS_PER_BLOCK;
    if ((uint32_t)block_index < (uint32_t)POINTERS_PER_BLOCK * POINTERS_PER_BLOCK)
    {
        // Check if we have a double indirect block
        if (inode->double_indirect_block == 0)
        {
            if (!allocate)
            {
                return 0; // No block and not allocating
            }

            // Allocate new double indirect block
            int new_block = find_free_block();
            if (new_block < 0)
            {
                return new_block;
            }

            // Init with 0s
            uint8_t zeros[BLOCK_SIZE] = {0};
            int result = vdisk_write(&disk, new_block, zeros);
            if (result != 0)
            {
                free_block(new_block);
                return result;
            }

            inode->double_indirect_block = new_block;
        }

        // Read the double indirect block
        uint8_t double_indirect_block[BLOCK_SIZE];
        int result = vdisk_read(&disk, inode->double_indirect_block, double_indirect_block);
        if (result != 0)
        {
            return result;
        }

        uint32_t *pointers = (uint32_t *)double_indirect_block;

        // Calculate which indirect block and entry within that block
        int indirect_index = block_index / POINTERS_PER_BLOCK;
        int entry_index = block_index % POINTERS_PER_BLOCK;

        // Check if we need to allocate a new indirect block
        if (pointers[indirect_index] == 0 && allocate)
        {
            int new_block = find_free_block();
            if (new_block < 0)
            {
                return new_block;
            }

            // Init with zeros
            uint8_t zeros[BLOCK_SIZE] = {0};
            int result = vdisk_write(&disk, new_block, zeros);
            if (result != 0)
            {
                free_block(new_block);
                return result;
            }

            pointers[indirect_index] = new_block;

            // Write the updated double indirect block back
            result = vdisk_write(&disk, inode->double_indirect_block, double_indirect_block);
            if (result != 0)
            {
                free_block(new_block);
                return result;
            }
        }
        else if (pointers[indirect_index] == 0)
        {
            return 0; // No block and not allocating
        }

        // Read the indirect block
        uint8_t indirect_block[BLOCK_SIZE];
        result = vdisk_read(&disk, pointers[indirect_index], indirect_block);
        if (result != 0)
        {
            return result;
        }

        uint32_t *sub_pointers = (uint32_t *)indirect_block;

        // Check if we need to allocate a new data block
        if (sub_pointers[entry_index] == 0 && allocate)
        {
            int new_block = find_free_block();
            if (new_block < 0)
            {
                return new_block;
            }

            // Init with zeros
            uint8_t zeros[BLOCK_SIZE] = {0};
            result = vdisk_write(&disk, new_block, zeros);
            if (result != 0)
            {
                free_block(new_block);
                return result;
            }

            sub_pointers[entry_index] = new_block;

            // Write the updated indirect block back
            result = vdisk_write(&disk, pointers[indirect_index], indirect_block);
            if (result != 0)
            {
                free_block(new_block);
                return result;
            }
        }

        return sub_pointers[entry_index];
    }

    return E_INVALID_OFFSET; // Offset too large for this file system
}
