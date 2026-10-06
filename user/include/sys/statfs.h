/*
SurfOS user library: file system statistics (df)
----------------------
File: statfs.h  Date: 10/5/26
----------------------
The kernel's layout (include/fs/vfs.h). A file system that does not report its
size leaves unit, units and free_units at 0.
*/

#ifndef _SYS_STATFS_H
#define _SYS_STATFS_H

struct statfs {
    unsigned int unit;          /* bytes per allocation unit (a FAT cluster) */
    unsigned int units;         /* allocation units in all */
    unsigned int free_units;    /* and free */
    unsigned int flags;         /* ST_RDONLY */
    char fs[8];                 /* file system type name */
    char dev[16];               /* block device name, "" for none */
    char label[12];             /* volume label, "" for none */
    char path[64];              /* where it is mounted */
};

#define ST_RDONLY 1             /* the kernel's MNT_RDONLY */

int statfs(const char *path, struct statfs *st);   /* the mounted file system that holds path */

#endif
