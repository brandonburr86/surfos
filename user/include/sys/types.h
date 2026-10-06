/*
SurfOS user library: system data types
----------------------
File: types.h   Date: 10/5/26
----------------------
*/

#ifndef _SYS_TYPES_H
#define _SYS_TYPES_H

#include <stddef.h>

typedef int ssize_t;
typedef long off_t;             /* files are at most 4 GB - 1 (FAT); lseek takes 32 bits */
typedef int pid_t;
typedef unsigned int mode_t;    /* accepted and ignored: FAT has no permissions */
typedef long time_t;            /* seconds since 1970 */
typedef unsigned int ino_t;
typedef unsigned int useconds_t;

#endif
