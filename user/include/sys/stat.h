/*
SurfOS user library: file status
----------------------
File: stat.h    Date: 10/5/26
----------------------
struct stat is the kernel's own layout (include/fs/vfs.h). Its type field holds
S_IFREG or S_IFDIR (the kernel's VN_FILE and VN_DIR) rather than POSIX mode
bits; st_mode, st_size, st_mtime and st_ino name the same fields for code
written the POSIX way, so S_ISDIR(st.st_mode) and S_ISDIR(st.type) both work.
*/

#ifndef _SYS_STAT_H
#define _SYS_STAT_H

#include <sys/types.h>

struct stat {
    int type;                   /* S_IFREG or S_IFDIR */
    unsigned int size;          /* bytes */
    unsigned int mtime;         /* seconds since 1970 by the CMOS clock (local time) */
    unsigned int ino;           /* identifies the file inside its file system */
    char fs[8];                 /* file system type: "fat", "tarfs", "devfs", "rootfs" */
    char dev[16];               /* block device name, "" for none */
};

#define st_mode  type
#define st_size  size
#define st_mtime mtime
#define st_ino   ino

#define S_IFREG 1
#define S_IFDIR 2
#define S_ISREG(m) ((m) == S_IFREG)
#define S_ISDIR(m) ((m) == S_IFDIR)

int stat(const char *path, struct stat *st);
int fstat(int fd, struct stat *st);
int mkdir(const char *path, mode_t mode);       /* the mode is ignored */

#endif
