# Simple and Safe File System (SSFS)

This repository contains the implementation of a Simple and Safe File System (SSFS) in C, developed as a project for the Operating Systems course (INFO0940-1). The file system interacts with a provided virtual disk driver to manage data on a disk image file.

## Project Overview

The SSFS is a user-space file system designed with simplicity and safety in mind. It utilizes a virtual disk that emulates a physical disk by dividing a file into fixed-size sectors. The file system structure includes a Super Block, I-node Blocks, and Data Blocks. A key safety feature is that all unused areas are zeroed out.

## Features

* **Virtual Disk Interaction:** Communicates with a provided virtual disk driver.
* **File System Structure:** Implements a Super Block, I-node Blocks, and Data Blocks.
* **Inode Management:** Supports creation, deletion, and status retrieval of files via inodes.
* **File Operations:** Provides functions for reading from and writing to files at specified offsets.
* **Allocation Strategy:** Uses a first-available strategy for allocating blocks and inodes.
* **Error Handling:** Includes specific error codes for various scenarios (e.g., disk not mounted, invalid inode, out of space).
* **Disk Management:** Includes functions for formatting, mounting, and unmounting the virtual disk.

## File Structure

The project follows a specific code organization:

```
src
|---- Makefile
|---- fs_test
|---- include
|    |---- error.h
|    |---- fs.h
|    |---- vdisk.h
|---- main.c
|---- vdisk
|    |---- vdisk.c
|---- error.c
```

## Building and Running

A `Makefile` is included to compile the application. The `make` command compiles the `main.c` file into an executable named `fs_test`.

To build the project, navigate to the `src` directory in your terminal and run:

```bash
make
```

Before running, you will need to create a disk image file. This can be done using the `dd` command:

```bash
dd if=/dev/zero of=mydisk.img bs=1024 count=100
```

This command creates a file named `mydisk.img` with a size of 100 blocks, each 1024 bytes in size.

The `main.c` file contains basic tests for the file system. You can run the tests with:

```bash
./fs_test
```

## SSFS Structure Details

* **Block Size:** The size of a block is equal to the virtual disk sector size, which is 1024 bytes.
* **Super Block:** Located at block 0, it contains a magic number, the total number of blocks, the number of i-node blocks, and the block size. The magic number is `f055 4c49 4547 4549 4e46 4f30 3934 300f`.
* **Inodes:** Each inode is a 32-byte structure. It contains a `valid` flag (0 for free, 1 for allocated), the file `size`, four direct block pointers, a single indirect block pointer, and a double indirect block pointer. Block pointers are represented by the block number, with 0 indicating a NULL pointer.
* **Allocation:** The file system uses a first-available allocation strategy for both inodes and data blocks, always selecting the one with the lowest number.

## SSFS API

The following functions are implemented as part of the SSFS API:

* `int format(char *disk_name, int inodes)`: Formats the virtual disk.
* `int mount(char *disk_name)`: Mounts the virtual disk.
* `int unmount()`: Unmounts the mounted volume.
* `int create()`: Creates a new file and returns its inode number.
* `int delete(int inode_num)`: Deletes the file associated with the given inode number.
* `int stat(int inode_num)`: Returns the size of the file associated with the given inode number.
* `int read(int inode_num, uint8_t *data, int len, int offset)`: Reads data from a file.
* `int write(int inode_num, uint8_t *data, int len, int offset)`: Writes data to a file.

Most functions return 0 on success and a negative integer on failure.

## Evaluation and Tests

The `main.c` file includes a basic testing suite (`run_basic_tests`) to verify the core functionality of the file system, including formatting, mounting, creating, writing, reading, stating, deleting, and unmounting files.

This project is a foundational implementation of a file system, providing a practical understanding of disk interaction, data structures, and allocation strategies at a low level.

## Contributors
- [@mdengis](https://github.com/martinDengis/)
- [@giooms](https://github.com/giooms)
