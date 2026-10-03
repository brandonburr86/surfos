/*
SurfOS Virtual File System
--------------------
File: vfs.c     Date: 10/3/26 (roadmap F1)
--------------------
Mount table, path resolution, open files and the per-task descriptor table.
See include/fs/vfs.h. The root ("/") is a built-in file system whose only
entries are the mount points below it.
*/

#include <surfos/types.h>
#include <surfos/console.h>
#include <surfos/task.h>
#include <surfos/sync.h>
#include <surfos/irq.h>
#include <sys/bdev.h>
#include <fs/vfs.h>
#include <fs/bcache.h>
#include <mm/kalloc.h>
#include <blibc_common.h>

static const struct fs_type *fs_types;
static struct mount *mounts;
static mutex_t mount_lock = MUTEX_INIT("mounts");

/**** vnodes ****/

struct vnode *vnode_alloc(struct superblock *sb, const struct vnode_ops *ops, int type) {
    struct vnode *v = (struct vnode *)kcalloc(1, sizeof(struct vnode));
    if(!v) return NULL;
    v->sb = sb;
    v->ops = ops;
    v->type = type;
    v->refs = 1;
    return v;
}

struct vnode *vget(struct vnode *v) {
    u_long flags = irq_save();
    v->refs++;
    irq_restore(flags);
    return v;
}

void vput(struct vnode *v) {
    u_long flags;
    bool last;
    if(!v) return;
    flags = irq_save();
    last = (--v->refs == 0);
    irq_restore(flags);
    if(last) {
        if(v->ops && v->ops->release) v->ops->release(v);
        kfree(v);
    }
}

/**** the root file system: a directory of mount points ****/

static int rootfs_lookup(struct vnode *dir, const char *name, struct vnode **out) {
    struct mount *m;
    char want[VFS_PATH_MAX];
    snprintf(want, sizeof(want), "/%s", name);
    for(m = mounts; m; m = m->next) {
        if(!strcmp(m->path, want)) { *out = vget(m->sb->root); return 0; }
    }
    return -ENOENT;
}

static int rootfs_readdir(struct vnode *dir, u_int index, struct dirent *de) {
    struct mount *m;
    for(m = mounts; m; m = m->next) {
        if(!strcmp(m->path, "/") || strchr(m->path + 1, '/')) continue;   /* only the top level */
        if(index-- == 0) {
            strlcpy(de->name, m->path + 1, sizeof(de->name));
            de->type = VN_DIR;
            de->size = 0;
            de->mtime = 0;
            return 0;
        }
    }
    return 1;
}

static const struct vnode_ops rootfs_vops = { rootfs_lookup, rootfs_readdir, NULL, NULL, NULL, NULL, NULL, NULL };

static int rootfs_mount(struct superblock *sb) {
    sb->root = vnode_alloc(sb, &rootfs_vops, VN_DIR);
    sb->readonly = true;
    return sb->root ? 0 : -ENOMEM;
}

static const struct fs_type rootfs_type = { "rootfs", NULL, rootfs_mount, NULL };

/**** file system types and mounts ****/

int vfs_register(const struct fs_type *type) {
    const struct fs_type *t;
    for(t = fs_types; t; t = t->next) if(!strcmp(t->name, type->name)) return -EEXIST;
    ((struct fs_type *)type)->next = fs_types;
    fs_types = type;
    return 0;
}

static const struct fs_type *find_type(const char *name) {
    const struct fs_type *t;
    for(t = fs_types; t; t = t->next) if(!strcmp(t->name, name)) return t;
    return NULL;
}

static const struct fs_type *probe_type(struct bdev *dev) {
    const struct fs_type *t;
    for(t = fs_types; t; t = t->next) if(t->probe && t->probe(dev)) return t;
    return NULL;
}

const struct mount *vfs_mounts(void) {
    return mounts;
}

int vfs_mount(struct bdev *dev, const char *path, const char *fstype) {
    const struct fs_type *type = fstype ? find_type(fstype) : (dev ? probe_type(dev) : NULL);
    struct superblock *sb;
    struct mount *m, **pp;
    char abs[VFS_PATH_MAX];
    int r;

    if(!type) return fstype ? -ENODEV : -EINVAL;
    r = vfs_normalize("/", path, abs, sizeof(abs));
    if(r) return r;
    for(m = mounts; m; m = m->next) if(!strcmp(m->path, abs)) return -EBUSY;

    sb = (struct superblock *)kcalloc(1, sizeof(struct superblock));
    m = (struct mount *)kcalloc(1, sizeof(struct mount));
    if(!sb || !m) { kfree(sb); kfree(m); return -ENOMEM; }
    sb->type = type;
    sb->dev = dev;
    sb->readonly = dev ? dev->readonly : false;
    mutex_init(&sb->lock, type->name);
    r = type->mount(sb);
    if(r) { kfree(sb); kfree(m); return r; }
    if(!sb->root) { kfree(sb); kfree(m); return -EIO; }

    strlcpy(m->path, abs, sizeof(m->path));
    m->sb = sb;
    mutex_lock(&mount_lock);
    for(pp = &mounts; *pp; pp = &(*pp)->next);   /* append: the listing keeps mount order */
    *pp = m;
    mutex_unlock(&mount_lock);
    return 0;
}

int vfs_umount(const char *path) {
    struct mount *m, **pp;
    struct superblock *sb;
    char abs[VFS_PATH_MAX];
    int r = vfs_normalize("/", path, abs, sizeof(abs));
    if(r) return r;
    if(!strcmp(abs, "/")) return -EBUSY;
    mutex_lock(&mount_lock);
    for(pp = &mounts; (m = *pp) != NULL; pp = &m->next) if(!strcmp(m->path, abs)) break;
    if(!m) { mutex_unlock(&mount_lock); return -ENOENT; }
    if(m->sb->root->refs > 1) { mutex_unlock(&mount_lock); return -EBUSY; }   /* open files or a cwd */
    *pp = m->next;
    mutex_unlock(&mount_lock);
    sb = m->sb;
    if(sb->ops && sb->ops->sync) sb->ops->sync(sb);
    if(sb->dev) bsync(sb->dev);
    vput(sb->root);
    if(sb->ops && sb->ops->unmount) sb->ops->unmount(sb);
    if(sb->dev) binvalidate(sb->dev);
    kfree(sb);
    kfree(m);
    return 0;
}

int vfs_sync(void) {
    struct mount *m;
    int r = 0;
    for(m = mounts; m; m = m->next) {
        if(m->sb->ops && m->sb->ops->sync && m->sb->ops->sync(m->sb) != 0) r = -EIO;
    }
    if(bsync(NULL) != 0) r = -EIO;
    return r;
}

/**** paths ****/

/* "/a/b" from cwd and a relative or absolute path: no empty, "." or ".." components remain */
int vfs_normalize(const char *cwd, const char *path, char *out, size_t size) {
    char tmp[VFS_PATH_MAX];
    size_t len;
    if(!path) return -EINVAL;
    if(path[0] == '/') tmp[0] = 0;
    else strlcpy(tmp, (cwd && cwd[0]) ? cwd : "/", sizeof(tmp));
    if(!strcmp(tmp, "/")) tmp[0] = 0;
    while(*path) {
        const char *s = path;
        size_t n;
        while(*path && *path != '/') path++;
        n = path - s;
        while(*path == '/') path++;
        if(n == 0 || (n == 1 && s[0] == '.')) continue;
        if(n == 2 && s[0] == '.' && s[1] == '.') {
            char *p = strrchr(tmp, '/');
            if(p) *p = 0;
            continue;
        }
        if(n > VFS_NAME_MAX) return -ENAMETOOLONG;
        len = strlen(tmp);
        if(len + 1 + n + 1 > sizeof(tmp)) return -ENAMETOOLONG;
        tmp[len] = '/';
        memcpy(tmp + len + 1, s, n);
        tmp[len + 1 + n] = 0;
    }
    if(!tmp[0]) strcpy(tmp, "/");
    if(strlen(tmp) + 1 > size) return -ENAMETOOLONG;
    strcpy(out, tmp);
    return 0;
}

/* the mount that owns an absolute path (longest prefix); rest is the path inside it */
static struct superblock *find_mount(const char *abs, const char **rest) {
    struct mount *m, *best = NULL;
    size_t bestlen = 0;
    mutex_lock(&mount_lock);
    for(m = mounts; m; m = m->next) {
        size_t l = strlen(m->path);
        if(l == 1) { if(!best) { best = m; bestlen = 1; } continue; }
        if(l > bestlen && !strncmp(abs, m->path, l) && (abs[l] == '/' || abs[l] == 0)) { best = m; bestlen = l; }
    }
    mutex_unlock(&mount_lock);
    if(!best) return NULL;
    *rest = abs + bestlen;
    while(**rest == '/') (*rest)++;
    return best->sb;
}

static int walk(struct superblock *sb, const char *rest, struct vnode **out) {
    struct vnode *v = vget(sb->root);
    char comp[VFS_NAME_MAX + 1];
    while(*rest) {
        const char *s = rest;
        struct vnode *next;
        size_t n;
        int r;
        while(*rest && *rest != '/') rest++;
        n = rest - s;
        while(*rest == '/') rest++;
        if(n > VFS_NAME_MAX) { vput(v); return -ENAMETOOLONG; }
        memcpy(comp, s, n);
        comp[n] = 0;
        if(v->type != VN_DIR || !v->ops->lookup) { vput(v); return -ENOTDIR; }
        r = v->ops->lookup(v, comp, &next);
        vput(v);
        if(r) return r;
        v = next;
    }
    *out = v;
    return 0;
}

static int lookup_abs(const char *abs, struct vnode **out) {
    const char *rest;
    struct superblock *sb = find_mount(abs, &rest);
    if(!sb) return -ENOENT;
    return walk(sb, rest, out);
}

const char *vfs_getcwd(void) {
    return (curTask && curTask->cwd[0]) ? curTask->cwd : "/";
}

int vfs_lookup(const char *path, struct vnode **out) {
    char abs[VFS_PATH_MAX];
    int r = vfs_normalize(vfs_getcwd(), path, abs, sizeof(abs));
    if(r) return r;
    return lookup_abs(abs, out);
}

/* the directory that holds the last component of path, and that component's name */
static int lookup_parent(const char *path, struct vnode **dir, char *name) {
    char abs[VFS_PATH_MAX], *p;
    int r = vfs_normalize(vfs_getcwd(), path, abs, sizeof(abs));
    if(r) return r;
    if(!strcmp(abs, "/")) return -EINVAL;
    p = strrchr(abs, '/');
    strlcpy(name, p + 1, VFS_NAME_MAX + 1);
    if(p == abs) p[1] = 0;      /* the parent is "/" */
    else *p = 0;
    r = lookup_abs(abs, dir);
    if(r) return r;
    if((*dir)->type != VN_DIR) { vput(*dir); return -ENOTDIR; }
    return 0;
}

int vfs_stat(const char *path, struct stat *st) {
    struct vnode *v;
    int r = vfs_lookup(path, &v);
    if(r) return r;
    st->type = v->type;
    st->size = v->size;
    st->mtime = v->mtime;
    st->ino = v->ino;
    strlcpy(st->fs, v->sb->type->name, sizeof(st->fs));
    strlcpy(st->dev, v->sb->dev ? v->sb->dev->name : "", sizeof(st->dev));
    vput(v);
    return 0;
}

int vfs_chdir(const char *path) {
    char abs[VFS_PATH_MAX];
    struct vnode *v;
    int r;
    if(!curTask) return -EINVAL;
    r = vfs_normalize(vfs_getcwd(), path, abs, sizeof(abs));
    if(r) return r;
    r = lookup_abs(abs, &v);
    if(r) return r;
    if(v->type != VN_DIR) { vput(v); return -ENOTDIR; }
    vput(v);
    strlcpy(curTask->cwd, abs, sizeof(curTask->cwd));
    return 0;
}

/**** files ****/

int vfs_open(const char *path, int flags, struct file **out) {
    struct vnode *v;
    struct file *f;
    int acc = flags & O_ACCMODE;
    int r = vfs_lookup(path, &v);

    if(r == -ENOENT && (flags & O_CREAT)) {
        struct vnode *dir;
        char name[VFS_NAME_MAX + 1];
        r = lookup_parent(path, &dir, name);
        if(r) return r;
        if(dir->sb->readonly || !dir->ops->create) { vput(dir); return -EROFS; }
        r = dir->ops->create(dir, name, VN_FILE, &v);
        vput(dir);
        if(r) return r;
    } else if(r) {
        return r;
    } else if((flags & O_CREAT) && (flags & O_EXCL)) {
        vput(v);
        return -EEXIST;
    }
    if((flags & O_DIRECTORY) && v->type != VN_DIR) { vput(v); return -ENOTDIR; }
    if(v->type == VN_DIR && acc != O_RDONLY) { vput(v); return -EISDIR; }
    if(acc != O_RDONLY && v->sb->readonly) { vput(v); return -EROFS; }
    if((flags & O_TRUNC) && acc != O_RDONLY && v->type == VN_FILE && v->size) {
        if(!v->ops->truncate) { vput(v); return -EROFS; }
        r = v->ops->truncate(v, 0);
        if(r) { vput(v); return r; }
    }
    f = (struct file *)kcalloc(1, sizeof(struct file));
    if(!f) { vput(v); return -ENOMEM; }
    f->v = v;
    f->flags = flags;
    f->refs = 1;
    *out = f;
    return 0;
}

int vfs_close(struct file *f) {
    struct superblock *sb;
    if(!f) return -EBADF;
    if(--f->refs) return 0;
    sb = f->v->sb;
    if((f->flags & O_ACCMODE) != O_RDONLY) {      /* a written file reaches the disk when it is closed */
        if(sb->ops && sb->ops->sync) sb->ops->sync(sb);
        if(sb->dev) bsync(sb->dev);
    }
    vput(f->v);
    kfree(f);
    return 0;
}

int vfs_read(struct file *f, void *buf, u32 len) {
    int r;
    if(!f) return -EBADF;
    if(f->v->type == VN_DIR) return -EISDIR;
    if((f->flags & O_ACCMODE) == O_WRONLY) return -EBADF;
    if(!f->v->ops->read) return -EINVAL;
    if(!len) return 0;
    r = f->v->ops->read(f->v, f->pos, buf, len);
    if(r > 0) f->pos += r;
    return r;
}

int vfs_write(struct file *f, const void *buf, u32 len) {
    int r;
    if(!f) return -EBADF;
    if(f->v->type == VN_DIR) return -EISDIR;
    if((f->flags & O_ACCMODE) == O_RDONLY) return -EBADF;
    if(f->v->sb->readonly || !f->v->ops->write) return -EROFS;
    if(!len) return 0;
    if(f->flags & O_APPEND) f->pos = f->v->size;
    r = f->v->ops->write(f->v, f->pos, buf, len);
    if(r > 0) f->pos += r;
    return r;
}

int vfs_lseek(struct file *f, i32 off, int whence) {
    i32 base;
    if(!f) return -EBADF;
    base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (i32)f->pos : whence == SEEK_END ? (i32)f->v->size : -1;
    if(base < 0 || base + off < 0) return -EINVAL;
    f->pos = (u32)(base + off);
    return (int)f->pos;
}

int vfs_readdir(struct file *f, struct dirent *de) {
    int r;
    if(!f) return -EBADF;
    if(f->v->type != VN_DIR || !f->v->ops->readdir) return -ENOTDIR;
    r = f->v->ops->readdir(f->v, f->pos, de);
    if(r == 0) f->pos++;
    return r;
}

int vfs_mkdir(const char *path) {
    struct vnode *dir, *v;
    char name[VFS_NAME_MAX + 1];
    int r = vfs_lookup(path, &v);
    if(r == 0) { vput(v); return -EEXIST; }
    if(r != -ENOENT) return r;
    r = lookup_parent(path, &dir, name);
    if(r) return r;
    if(dir->sb->readonly || !dir->ops->create) { vput(dir); return -EROFS; }
    r = dir->ops->create(dir, name, VN_DIR, &v);
    if(r == 0) {
        vput(v);
        if(dir->sb->ops && dir->sb->ops->sync) dir->sb->ops->sync(dir->sb);
    }
    vput(dir);
    return r;
}

static int remove_entry(const char *path, int type) {
    struct vnode *dir, *v;
    char name[VFS_NAME_MAX + 1];
    int r = vfs_lookup(path, &v);
    if(r) return r;
    if(v->type != type) { vput(v); return type == VN_DIR ? -ENOTDIR : -EISDIR; }
    if(v == v->sb->root) { vput(v); return -EBUSY; }
    if(v->refs > 1) { vput(v); return -EBUSY; }          /* open somewhere */
    vput(v);
    r = lookup_parent(path, &dir, name);
    if(r) return r;
    if(dir->sb->readonly || !dir->ops->unlink) { vput(dir); return -EROFS; }
    r = dir->ops->unlink(dir, name);
    if(r == 0 && dir->sb->ops && dir->sb->ops->sync) dir->sb->ops->sync(dir->sb);
    vput(dir);
    return r;
}

int vfs_unlink(const char *path) {
    return remove_entry(path, VN_FILE);
}

int vfs_rmdir(const char *path) {
    return remove_entry(path, VN_DIR);
}

/**** descriptors ****/

int fd_install(struct file *f) {
    int i;
    if(!curTask) return -EINVAL;
    for(i = 0; i < NR_OPEN; i++) {
        if(!curTask->files[i]) { curTask->files[i] = f; return i; }
    }
    return -EMFILE;
}

struct file *fd_get(int fd) {
    if(!curTask || fd < 0 || fd >= NR_OPEN) return NULL;
    return curTask->files[fd];
}

int fd_close(int fd) {
    struct file *f = fd_get(fd);
    if(!f) return -EBADF;
    curTask->files[fd] = NULL;
    return vfs_close(f);
}

void fd_close_all(struct surf_task *t) {
    int i;
    for(i = 0; i < NR_OPEN; i++) {
        if(t->files[i]) { vfs_close(t->files[i]); t->files[i] = NULL; }
    }
}

/**** errors ****/

const char *strerror(int err) {
    if(err < 0) err = -err;
    switch(err) {
    case 0: return "Success";
    case ENOENT: return "No such file or directory";
    case EIO: return "I/O error";
    case EBADF: return "Bad file descriptor";
    case E2BIG: return "Argument list too long";
    case ENOMEM: return "Out of memory";
    case EFAULT: return "Bad address";
    case ERANGE: return "Result too large";
    case EACCES: return "Permission denied";
    case EBUSY: return "Device or resource busy";
    case EEXIST: return "File exists";
    case ENODEV: return "No such device";
    case ENOTDIR: return "Not a directory";
    case EISDIR: return "Is a directory";
    case EINVAL: return "Invalid argument";
    case EMFILE: return "Too many open files";
    case EFBIG: return "File too large";
    case ENOSPC: return "No space left on device";
    case EROFS: return "Read-only file system";
    case ENAMETOOLONG: return "File name too long";
    case ENOSYS: return "Function not implemented";
    case ENOTEMPTY: return "Directory not empty";
    }
    return "Unknown error";
}

/**** boot ****/

void init_fs(void) {
    struct bdev *d;
    bool have_initrd = false;
    int r;

    kprintf("\nFile Systems\n");
    init_bcache();
    vfs_register(&rootfs_type);
    tarfs_init();
    fatfs_init();
    devfs_init();
    r = vfs_mount(NULL, "/", "rootfs");
    if(r) { kprintf("*rootfs: %s\n", strerror(r)); return; }
    r = vfs_mount(NULL, "/dev", "devfs");
    if(r) kprintf("*devfs: %s\n", strerror(r));
    for(d = bdev_first(); d; d = d->next) {
        const struct fs_type *t = probe_type(d);
        char path[VFS_PATH_MAX];
        if(!t) continue;
        if(!strcmp(t->name, "tarfs") && !have_initrd) { strcpy(path, "/initrd"); have_initrd = true; }
        else snprintf(path, sizeof(path), "/%s", d->name);
        r = vfs_mount(d, path, t->name);
        if(r) kprintf("+VFS: %s on %s (%s): %s\n", d->name, path, t->name, strerror(r));
        else kprintf("+VFS: %s mounted on %s (%s%s)\n", d->name, path, t->name, d->readonly ? ", read-only" : "");
    }
    kprintf("*DONE\n");
}
