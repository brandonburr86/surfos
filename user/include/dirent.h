/*
SurfOS user library: directories
----------------------
File: dirent.h  Date: 10/5/26
----------------------
opendir() opens the directory with O_DIRECTORY; readdir() is the kernel's
readdir call, one entry at a time. struct dirent is the kernel's layout;
d_name and d_type name its fields the POSIX way. "." and ".." are not listed.
*/

#ifndef _DIRENT_H
#define _DIRENT_H

#include <sys/types.h>

struct dirent {
    char name[256];             /* NAME_MAX + 1 */
    int type;                   /* DT_REG or DT_DIR */
    unsigned int size;          /* bytes (0 for a directory) */
    unsigned int mtime;         /* seconds since 1970 */
};

#define d_name name
#define d_type type
#define DT_REG 1
#define DT_DIR 2

typedef struct _DIR DIR;

DIR *opendir(const char *path);
struct dirent *readdir(DIR *d);                 /* NULL at the end (errno unchanged) or on an error */
int closedir(DIR *d);
void rewinddir(DIR *d);
int dirfd(DIR *d);

#endif
