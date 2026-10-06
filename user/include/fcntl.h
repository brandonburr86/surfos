/*
SurfOS user library: opening files
----------------------
File: fcntl.h   Date: 10/5/26
----------------------
The flags have the kernel's values (include/fs/vfs.h), which are Linux's.
*/

#ifndef _FCNTL_H
#define _FCNTL_H

#include <sys/types.h>

#define O_RDONLY    0x0000
#define O_WRONLY    0x0001
#define O_RDWR      0x0002
#define O_ACCMODE   0x0003
#define O_CREAT     0x0040
#define O_EXCL      0x0080
#define O_TRUNC     0x0200
#define O_APPEND    0x0400
#define O_DIRECTORY 0x10000

int open(const char *path, int flags, ...);     /* a mode argument is accepted and ignored */
int creat(const char *path, mode_t mode);       /* open(path, O_WRONLY | O_CREAT | O_TRUNC) */

#endif
