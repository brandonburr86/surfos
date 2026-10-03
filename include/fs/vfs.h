/*
SurfOS Virtual File System
----------------------
File: vfs.h     Date: 10/3/26 (roadmap F1)
----------------------
Paths are resolved against a mount table (longest prefix wins) and then walked
one component at a time inside the file system that owns them. A vnode is an
open object (file or directory) that a file system hands out from lookup(),
create() or as its root; it is reference counted and released through vput().
A struct file is an open file: a vnode plus a position and the open flags.
Every call returns 0 or a negative errno value.
*/

#ifndef _FS_VFS_H
#define _FS_VFS_H

#include <surfos/types.h>
#include <surfos/sync.h>
#include <surfos/task.h>     /* NR_OPEN, the per-task cwd and descriptor table */

#define VFS_NAME_MAX 255
#define VFS_PATH_MAX 256

/* errno values (the numbers Linux uses, so a user libc can share them later) */
#define ENOENT        2
#define EIO           5
#define EBADF         9
#define ENOMEM       12
#define EACCES       13
#define EBUSY        16
#define EEXIST       17
#define ENODEV       19
#define ENOTDIR      20
#define EISDIR       21
#define EINVAL       22
#define EMFILE       24
#define EFBIG        27
#define ENOSPC       28
#define EROFS        30
#define ENAMETOOLONG 36
#define ENOSYS       38
#define ENOTEMPTY    39

/* open flags (Linux values) */
#define O_RDONLY    0x0000
#define O_WRONLY    0x0001
#define O_RDWR      0x0002
#define O_ACCMODE   0x0003
#define O_CREAT     0x0040
#define O_EXCL      0x0080
#define O_TRUNC     0x0200
#define O_APPEND    0x0400
#define O_DIRECTORY 0x10000

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* vnode types */
#define VN_FILE 1
#define VN_DIR  2

struct bdev;
struct superblock;
struct vnode;

struct dirent {
    char name[VFS_NAME_MAX + 1];
    int type;                   /* VN_FILE or VN_DIR */
    u32 size;
    u32 mtime;                  /* seconds since 1970 */
};

struct stat {
    int type;
    u32 size;
    u32 mtime;
    u32 ino;
    const char *fs;             /* file system type name */
    const char *dev;            /* block device name, "" for none */
};

struct vnode_ops {
    int (*lookup)(struct vnode *dir, const char *name, struct vnode **out);
    int (*readdir)(struct vnode *dir, u_int index, struct dirent *de);   /* 0 entry, 1 end, < 0 error */
    int (*read)(struct vnode *v, u32 off, void *buf, u32 len);          /* bytes, or < 0 */
    int (*write)(struct vnode *v, u32 off, const void *buf, u32 len);   /* bytes, or < 0 */
    int (*truncate)(struct vnode *v, u32 size);
    int (*create)(struct vnode *dir, const char *name, int type, struct vnode **out);
    int (*unlink)(struct vnode *dir, const char *name);                 /* files and empty directories */
    void (*release)(struct vnode *v);                                   /* last reference dropped */
};

struct vnode {
    struct superblock *sb;
    const struct vnode_ops *ops;
    int type;
    u32 size;
    u32 mtime;
    u32 ino;                    /* identifies the object inside its file system */
    u_int refs;
    void *priv;                 /* file system data */
};

struct superblock_ops {
    int (*sync)(struct superblock *sb);
    int (*unmount)(struct superblock *sb);     /* after the root vnode is released */
};

struct superblock {
    const struct fs_type *type;
    struct bdev *dev;           /* NULL for a device-less file system */
    struct vnode *root;
    const struct superblock_ops *ops;
    void *priv;
    bool readonly;
    mutex_t lock;               /* file systems serialize their metadata updates with this */
};

struct fs_type {
    const char *name;
    bool (*probe)(struct bdev *dev);                 /* does this device hold one of ours? */
    int (*mount)(struct superblock *sb);             /* fill in root, ops, priv */
    const struct fs_type *next;
};

struct mount {
    char path[VFS_PATH_MAX];
    struct superblock *sb;
    struct mount *next;
};

struct file {
    struct vnode *v;
    u32 pos;
    int flags;
    u_int refs;
};

/* vnodes */
struct vnode *vnode_alloc(struct superblock *sb, const struct vnode_ops *ops, int type);
struct vnode *vget(struct vnode *v);
void vput(struct vnode *v);

/* file systems and mounts */
void init_fs(void);                                  /* register the file systems, mount what is found */
int vfs_register(const struct fs_type *type);
int vfs_mount(struct bdev *dev, const char *path, const char *fstype);   /* fstype NULL: probe */
int vfs_umount(const char *path);
int vfs_sync(void);
const struct mount *vfs_mounts(void);

/* paths */
int vfs_normalize(const char *cwd, const char *path, char *out, size_t size);
int vfs_lookup(const char *path, struct vnode **out);             /* relative to the current task's cwd */
int vfs_stat(const char *path, struct stat *st);
int vfs_chdir(const char *path);
const char *vfs_getcwd(void);

/* files */
int vfs_open(const char *path, int flags, struct file **out);
int vfs_close(struct file *f);
int vfs_read(struct file *f, void *buf, u32 len);
int vfs_write(struct file *f, const void *buf, u32 len);
int vfs_lseek(struct file *f, i32 off, int whence);               /* new position, or < 0 */
int vfs_readdir(struct file *f, struct dirent *de);              /* 0 entry, 1 end */
int vfs_mkdir(const char *path);
int vfs_unlink(const char *path);                                /* a file */
int vfs_rmdir(const char *path);                                 /* an empty directory */

/* descriptors of the current task (the system call layer) */
int fd_install(struct file *f);                                  /* fd, or -EMFILE */
struct file *fd_get(int fd);
int fd_close(int fd);
void fd_close_all(struct surf_task *t);

const char *strerror(int err);

/* file system modules register themselves here (called by init_fs) */
void tarfs_init(void);

#endif
