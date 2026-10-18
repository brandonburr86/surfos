/*
SurfOS Virtual File System
--------------------
File: vfs.c     Date: 10/3/26 (roadmap F1)
--------------------
Mount table, path resolution, open files and the per-task descriptor table.
See include/fs/vfs.h. The root ("/") is a built-in file system whose only
entries are the mount points below it.

10/5/26 (USB storage): umount refuses a mount whose vnodes are in use (an
open file, an open directory, the current directory of the task asking)
instead of freeing it under them; vfs_busy() says who.

10/6/26: whoever goes on using a mount after mount_lock is let go (a path
lookup, statfs, sync) holds its root vnode first, under the lock: umount then
says the mount is busy instead of freeing it under them. vfs_open_vnode() makes
a file of a vnode that has no path (a network connection, net/sock.c).

10/16/26 (audit B10): the last close of a file that was written returns the
error of its sync (it returned 0 whatever happened, so cp and edit said a file
was saved that was only in the cache); umount says how many changed blocks it
gave up because they could not be written (vfs_umount_lost()).

10/17/26 (audit B46): an umount finishes once it has begun (fs_finish, surfos/task.h):
Ctrl+C, a kill or a hangup cut its syncs short (B39), and it went on to give up the
changes the card would have taken and leave a working card marked as not cleanly
unmounted. Its waits are not cut short now, and a kill that waited comes after it.

10/18/26 (audit B51): the mount leaves the list before the umount's syncs (so that no
path reaches it any more), and vfs_bdev_busy() looked at the list only: mount, mkfs and
fdisk could take the device while its umount still wrote (a second mount's file given up
by the old umount's binvalidate(), the card marked clean under a live mount, mkfs's new
FATs crossed with the old volume's writes). The device is counted as being unmounted
(struct bdev's unmounting) from the moment its mount leaves the list until the umount
has let go of it, and vfs_bdev_busy() counts that too; vfs_bdev_unmounting() says so.

10/18/26 (audit B56, B58): vfs_bdev_busy() also counts a request issued on the device
that has not ended (struct bdev's requests, kept by driver/bdev.c), so no mount, mkfs
or fdisk takes a device a late write may still land on, and a mount under way (B58:
struct bdev's mounting). A mount takes its device under mount_lock in the same step as
it looks whether it is busy, and an eject takes its disk the same way for its whole
run (vfs_unmount_begin()): a mount could look, find the disk free, and append itself to
the list after the eject had listed the mounts it would unmount, or come in during the
eject's write-out; the eject then took that volume's blocks and said "safe to remove".
Both are counted inside (fs_enter()) while they hold such a count, so a kill waits for
them to let go of it. vfs_bdev_busy_why() says what keeps a device busy.
*/

#include <surfos/types.h>
#include <surfos/console.h>
#include <surfos/task.h>
#include <surfos/sync.h>
#include <surfos/irq.h>
#include <surfos/multiboot.h>
#include <sys/bdev.h>
#include <fs/vfs.h>
#include <fs/bcache.h>
#include <mm/kalloc.h>
#include <blibc_common.h>

static const struct fs_type *fs_types;
static struct mount *mounts;
static mutex_t mount_lock = MUTEX_INIT("mounts");

/* (mount_lock held) 10/18/26 (audit B56, B58): is a count of d's set nonzero, d itself, its disk (d a partition) or one
   of its partitions (d a disk): umounts and ejects (unmounting), mounts under way (mounting), or requests not ended */
static bool counted(const struct bdev *d, int which) {
    const struct bdev *p;
#define COUNT(x) (which == 0 ? (x)->unmounting : which == 1 ? (x)->mounting : (x)->requests)
    if(COUNT(d) || (d->parent && COUNT(d->parent))) return true;
    for(p = bdev_first(); p; p = p->next) if(p->parent == d && COUNT(p)) return true;
#undef COUNT
    return false;
}
#define claimed(d, unmount) counted(d, (unmount) ? 0 : 1)

/* (mount_lock held) 10/18/26 (audit B56, B58): what keeps d from being mounted, formatted or partitioned, or NULL: a
   mount of it, of its disk or of one of its partitions; an umount or eject of them under way, a mount under way, or a
   request issued on them that has not ended (a late write may still land) */
static const char *busy_why(const struct bdev *d) {
    const struct mount *m;
    for(m = mounts; m; m = m->next) {
        const struct bdev *x = m->sb->dev;
        if(x && (x == d || x->parent == d || d->parent == x)) return VFS_BUSY_MOUNTED;   /* itself, a partition, its disk */
    }
    if(claimed(d, true)) return VFS_BUSY_UNMOUNTING;
    if(claimed(d, false)) return VFS_BUSY_MOUNTING;
    if(counted(d, 2)) return VFS_BUSY_REQUEST;
    return NULL;
}
static struct file *open_files;             /* 10/5/26: every open file, for vfs_busy() */

/**** vnodes ****/

struct vnode *vnode_alloc(struct superblock *sb, const struct vnode_ops *ops, int type) {
    struct vnode *v = (struct vnode *)kcalloc(1, sizeof(struct vnode));
    u_long flags;
    if(!v) return NULL;
    v->sb = sb;
    v->ops = ops;
    v->type = type;
    v->refs = 1;
    flags = irq_save();
    if(sb) sb->nvnodes++;
    irq_restore(flags);
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
        struct superblock *sb = v->sb;
        if(v->ops && v->ops->release) v->ops->release(v);
        kfree(v);
        flags = irq_save();
        if(sb && sb->nvnodes) sb->nvnodes--;
        irq_restore(flags);
    }
}

/**** the root file system: a directory of mount points ****/

static int rootfs_lookup(struct vnode *dir, const char *name, struct vnode **out) {
    struct mount *m;
    char want[VFS_PATH_MAX];
    int r = -ENOENT;
    snprintf(want, sizeof(want), "/%s", name);
    mutex_lock(&mount_lock);                    /* 10/6/26: the list, and the root held, under the lock */
    for(m = mounts; m; m = m->next) {
        if(!strcmp(m->path, want)) { *out = vget(m->sb->root); r = 0; break; }
    }
    mutex_unlock(&mount_lock);
    return r;
}

static int rootfs_readdir(struct vnode *dir, u_int index, struct dirent *de) {
    struct mount *m;
    int r = 1;
    mutex_lock(&mount_lock);
    for(m = mounts; m; m = m->next) {
        if(!strcmp(m->path, "/") || strchr(m->path + 1, '/')) continue;   /* only the top level */
        if(index-- == 0) {
            strlcpy(de->name, m->path + 1, sizeof(de->name));
            de->type = VN_DIR;
            de->size = 0;
            de->mtime = 0;
            r = 0;
            break;
        }
    }
    mutex_unlock(&mount_lock);
    return r;
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

/* 10/6/26 (vfs.h): a copy, for listings that may block while they print */
int vfs_mount_views(struct vfs_mount_view *out, int max) {
    struct mount *m;
    int n = 0;
    mutex_lock(&mount_lock);
    for(m = mounts; m; m = m->next, n++) {
        if(n >= max) continue;
        strlcpy(out[n].path, m->path, sizeof(out[n].path));
        out[n].type = m->sb->type->name;
        out[n].dev = m->sb->dev;
        out[n].readonly = m->sb->readonly;
    }
    mutex_unlock(&mount_lock);
    return n;
}

bool vfs_dev_mounted(const struct bdev *d) {
    struct mount *m;
    bool found = false;
    mutex_lock(&mount_lock);
    for(m = mounts; m && !found; m = m->next) found = m->sb->dev == d;
    mutex_unlock(&mount_lock);
    return found;
}

int vfs_mount(struct bdev *dev, const char *path, const char *fstype) {
    return vfs_mount_flags(dev, path, fstype, 0);
}

int vfs_mount_flags(struct bdev *dev, const char *path, const char *fstype, int flags) {
    const struct fs_type *type = fstype ? find_type(fstype) : (dev ? probe_type(dev) : NULL);
    struct superblock *sb;
    struct mount *m, **pp;
    char abs[VFS_PATH_MAX];
    int r;

    if(!type) return fstype ? -ENODEV : -EINVAL;
    r = vfs_normalize("/", path, abs, sizeof(abs));
    if(r) return r;
    fs_enter();                                     /* 10/18/26 (audit B58): counted inside while it holds the device's
                                                       count below, so a kill waits and never leaves it behind */
    mutex_lock(&mount_lock);
    for(m = mounts; m; m = m->next) if(!strcmp(m->path, abs)) break;
    if(m || (dev && busy_why(dev))) {               /* 10/5/26: two mounts of one device would each think they own it */
        mutex_unlock(&mount_lock);
        fs_leave();
        return -EBUSY;
    }
    if(dev) dev->mounting++;                        /* 10/18/26 (audit B58): taken in the same step: busy until below */
    mutex_unlock(&mount_lock);

    sb = (struct superblock *)kcalloc(1, sizeof(struct superblock));
    m = (struct mount *)kcalloc(1, sizeof(struct mount));
    r = !sb || !m ? -ENOMEM : dev && dev->gone ? -ENODEV : 0;
    if(r == 0) {
        sb->type = type;
        sb->dev = dev;
        sb->readonly = (dev ? dev->readonly : false) || (flags & MNT_RDONLY);
        mutex_init(&sb->lock, type->name);
        r = type->mount(sb);
        if(r == 0 && !sb->root) r = -EIO;
    }
    if(r) {
        kfree(sb);
        kfree(m);
        if(dev) {
            mutex_lock(&mount_lock);
            dev->mounting--;
            mutex_unlock(&mount_lock);
        }
        fs_leave();
        return r;
    }
    if(dev) bdev_get(dev);                          /* held until umount, even if unplugged meanwhile */

    strlcpy(m->path, abs, sizeof(m->path));
    m->sb = sb;
    mutex_lock(&mount_lock);
    for(pp = &mounts; *pp; pp = &(*pp)->next);   /* append: the listing keeps mount order */
    *pp = m;
    if(dev) dev->mounting--;                        /* (B58: on the list now, busy as a mount) */
    mutex_unlock(&mount_lock);
    fs_leave();
    return 0;
}

/* what keeps the mount m from going away (holds mount_lock): an open file (who opened it),
   other vnodes, or the current directory of the task asking; NULL when nothing does */
static const char *in_use(const struct mount *m, char *why, size_t size) {
    const struct superblock *sb = m->sb;
    const struct file *f;
    size_t l = strlen(m->path);
    const char *cwd = vfs_getcwd();
    u_int files = 0;
    u_long flags = irq_save();
    for(f = open_files; f; f = f->onext) {
        if(f->v->sb != sb) continue;
        if(!files++ && why) snprintf(why, size, "%s (pid %lu) has a file or directory open there", f->owner, f->pid);
    }
    irq_restore(flags);
    if(files > 1 && why) {
        size_t n = strlen(why);
        snprintf(why + n, size - n, ", %u open in all", files);
    }
    if(files) return why ? why : "";
    if(sb->nvnodes > 1 || sb->root->refs > 1) {
        if(why) snprintf(why, size, "a command or program is using it right now");
        return why ? why : "";
    }
    if(l > 1 && !strncmp(cwd, m->path, l) && (cwd[l] == '/' || cwd[l] == 0)) {
        if(why) snprintf(why, size, "it holds the current directory, %s (cd / first)", cwd);
        return why ? why : "";
    }
    return NULL;
}

int vfs_busy(const char *path, char *why, size_t size) {
    struct mount *m;
    char abs[VFS_PATH_MAX];
    int r = vfs_normalize("/", path, abs, sizeof(abs));
    if(r) return r;
    mutex_lock(&mount_lock);
    for(m = mounts; m; m = m->next) if(!strcmp(m->path, abs)) break;
    r = !m ? -ENOENT : in_use(m, why, size) ? -EBUSY : 0;
    mutex_unlock(&mount_lock);
    return r;
}

int vfs_umount(const char *path) {
    return vfs_umount_lost(path, NULL);
}

/* 10/16/26 (audit B10): umount gives up what could not be written (its blocks are dropped), and says
   how many blocks that was: in *lost, or on the console when the caller does not ask. 10/17/26 (B46): once the
   mount is off the list it finishes (fs_finish_enter()): its syncs are not cut short by Ctrl+C, a kill or a
   hangup, which before made it give up changes a working card would take and leave the card unclean (B42 then
   kept it so); a kill that waited comes after it (it counts as inside the file system). 10/18/26 (B51): and the
   device stays busy meanwhile (unmounting): vfs_bdev_busy() refuses it to mount, mkfs and fdisk */
int vfs_umount_lost(const char *path, u_int *lost) {
    struct mount *m, **pp;
    struct superblock *sb;
    char abs[VFS_PATH_MAX];
    u_int gave_up = 0;
    int r = vfs_normalize("/", path, abs, sizeof(abs));
    if(lost) *lost = 0;
    if(r) return r;
    if(!strcmp(abs, "/")) return -EBUSY;
    mutex_lock(&mount_lock);
    for(pp = &mounts; (m = *pp) != NULL; pp = &m->next) if(!strcmp(m->path, abs)) break;
    if(!m) { mutex_unlock(&mount_lock); return -ENOENT; }
    if(in_use(m, NULL, 0)) { mutex_unlock(&mount_lock); return -EBUSY; }   /* open files, a cwd: vfs_busy says which */
    fs_finish_enter();                              /* (before the mount leaves the list: no kill between) */
    *pp = m->next;
    if(m->sb->dev) m->sb->dev->unmounting++;        /* 10/18/26 (B51): busy until it is let go of, below */
    mutex_unlock(&mount_lock);
    sb = m->sb;
    if(sb->ops && sb->ops->sync) sb->ops->sync(sb);
    if(sb->dev) {
        bsync(sb->dev);
        bdev_flush(sb->dev);                        /* and out of the device's own cache */
    }
    vput(sb->root);
    if(sb->ops && sb->ops->unmount) sb->ops->unmount(sb);
    if(sb->dev) {
        gave_up = sb->dev->gone ? 0 : bdirty_count(sb->dev);
        binvalidate(sb->dev);
        vfs_unmount_end(sb->dev);                   /* (B51: nothing of the volume is left in the cache) */
        bdev_put(sb->dev);
    }
    if(lost) *lost = gave_up;
    else if(gave_up) kprintf("+VFS: %s: %u changed block%s could not be written and %s given up\n", abs, gave_up,
                             gave_up == 1 ? "" : "s", gave_up == 1 ? "was" : "were");
    kfree(sb);
    kfree(m);
    fs_finish_leave();                              /* (a kill that waited may come now: nothing is left behind) */
    return 0;
}

/* is a disk of this name, or one of its partitions, mounted (it may be unplugged and gone)? */
bool vfs_disk_name_mounted(const char *name) {
    struct mount *m;
    bool used = false;
    mutex_lock(&mount_lock);
    for(m = mounts; m && !used; m = m->next) {
        struct bdev *x = m->sb->dev;
        if(x && (!strcmp(x->name, name) || (x->parent && !strcmp(x->parent->name, name)))) used = true;
    }
    mutex_unlock(&mount_lock);
    return used;
}

/* is d, the disk it is on, or a partition of d mounted? */
bool vfs_bdev_busy(struct bdev *d) {
    return vfs_bdev_busy_why(d) != NULL;
}

/* 10/18/26 (audit B56, B58): what keeps d busy (vfs_bdev_busy()), for the messages of mount, mkfs, fdisk and sdmode */
const char *vfs_bdev_busy_why(struct bdev *d) {
    const char *why;
    if(!d) return NULL;
    mutex_lock(&mount_lock);
    why = busy_why(d);
    mutex_unlock(&mount_lock);
    return why;
}

/* 10/18/26 (audit B51): is d, its disk or one of its partitions being unmounted (vfs_umount_lost()'s steps once the
   mount is off the list, an eject of the disk: B58, all of it): still written, so taken by nobody else */
bool vfs_bdev_unmounting(struct bdev *d) {
    bool busy;
    if(!d) return false;
    mutex_lock(&mount_lock);
    busy = claimed(d, true);
    mutex_unlock(&mount_lock);
    return busy;
}

/* 10/18/26 (audit B51): d is being unmounted until vfs_unmount_end() (counted: an eject of its disk and an umount of
   one of its partitions may overlap). (B58) For an eject, all of it: -EBUSY when d, its disk or one of its partitions
   is being mounted or unmounted already, else taken under mount_lock in the same step as that look */
int vfs_unmount_begin(struct bdev *d) {
    if(!d) return 0;
    mutex_lock(&mount_lock);
    if(claimed(d, true) || claimed(d, false)) {
        mutex_unlock(&mount_lock);
        return -EBUSY;
    }
    d->unmounting++;
    mutex_unlock(&mount_lock);
    return 0;
}

void vfs_unmount_end(struct bdev *d) {
    if(!d) return;
    mutex_lock(&mount_lock);
    if(d->unmounting) d->unmounting--;
    mutex_unlock(&mount_lock);
}

const char *vfs_probe(struct bdev *dev) {
    const struct fs_type *t = dev ? probe_type(dev) : NULL;
    return t ? t->name : NULL;
}

/* 10/6/26: each file system is synced with its root held and mount_lock let go (a sync writes); a umount
   meanwhile finds it busy, and the mount stays on the list, where the walk goes on from */
int vfs_sync(void) {
    struct mount *m;
    int r = 0;
    mutex_lock(&mount_lock);
    for(m = mounts; m; m = m->next) {
        struct superblock *sb = m->sb;
        if(!sb->ops || !sb->ops->sync) continue;
        vget(sb->root);
        mutex_unlock(&mount_lock);
        if(sb->ops->sync(sb) != 0) r = -EIO;
        mutex_lock(&mount_lock);
        vput(sb->root);                             /* not the last reference: the mount keeps one */
    }
    mutex_unlock(&mount_lock);
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

/* the mount that owns an absolute path (longest prefix); rest is the path inside it. 10/6/26: its
   root vnode, held before mount_lock goes (a umount cannot free it then), for walk() */
static struct vnode *find_mount(const char *abs, const char **rest) {
    struct mount *m, *best = NULL;
    struct vnode *root = NULL;
    size_t bestlen = 0;
    mutex_lock(&mount_lock);
    for(m = mounts; m; m = m->next) {
        size_t l = strlen(m->path);
        if(l == 1) { if(!best) { best = m; bestlen = 1; } continue; }
        if(l > bestlen && !strncmp(abs, m->path, l) && (abs[l] == '/' || abs[l] == 0)) { best = m; bestlen = l; }
    }
    if(best) root = vget(best->sb->root);
    mutex_unlock(&mount_lock);
    if(!root) return NULL;
    *rest = abs + bestlen;
    while(**rest == '/') (*rest)++;
    return root;
}

/* from v (a reference that becomes walk's) down the components of rest */
static int walk(struct vnode *v, const char *rest, struct vnode **out) {
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
    struct vnode *root = find_mount(abs, &rest);
    if(!root) return -ENOENT;
    return walk(root, rest, out);
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
    r = vfs_open_vnode(v, flags, out);
    if(r) vput(v);
    return r;
}

/* 10/6/26: a file for a vnode that has no path (a network connection, net/sock.c); the file takes
   over the caller's reference to v, which stays the caller's when this fails (-ENOMEM) */
int vfs_open_vnode(struct vnode *v, int flags, struct file **out) {
    struct file *f = (struct file *)kcalloc(1, sizeof(struct file));
    u_long fl;
    if(!f) return -ENOMEM;
    f->v = v;
    f->flags = flags;
    f->refs = 1;
    if(curTask) {
        f->pid = curTask->pid;
        strlcpy(f->owner, curTask->name, sizeof(f->owner));
    }
    fl = irq_save();
    f->onext = open_files;
    if(open_files) open_files->oprev = f;
    open_files = f;
    irq_restore(fl);
    *out = f;
    return 0;
}

/* 10/16/26 (audit B10): a file system's state and its device's blocks out to the device; 0 when
   everything reached it by the end (a block refused by the first sync and written by the second
   counts as written), else -EIO */
static int write_back(struct superblock *sb) {
    int r = sb->ops && sb->ops->sync ? sb->ops->sync(sb) : 0;
    if(!sb->dev) return r ? -EIO : 0;
    return bsync(sb->dev) != 0 || sb->dev->gone ? -EIO : 0;
}

int vfs_close(struct file *f) {
    struct superblock *sb;
    u_long fl;
    bool last;
    int r = 0;
    if(!f) return -EBADF;
    fl = irq_save();                                /* tasks share files (spawnfd): the count is atomic */
    last = --f->refs == 0;
    if(last) {
        if(f->oprev) f->oprev->onext = f->onext;
        else open_files = f->onext;
        if(f->onext) f->onext->oprev = f->oprev;
    }
    irq_restore(fl);
    if(!last) return 0;
    sb = f->v->sb;
    if((f->flags & O_ACCMODE) != O_RDONLY) r = write_back(sb);   /* a written file reaches the disk when it is
                                                                     closed; 10/16/26 (B10): and close says
                                                                     when it did not */
    vput(f->v);
    kfree(f);
    return r;
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

/* 10/5/26: rename within one file system; the file system does the work */
int vfs_rename(const char *from, const char *to) {
    char afrom[VFS_PATH_MAX], ato[VFS_PATH_MAX], oname[VFS_NAME_MAX + 1], nname[VFS_NAME_MAX + 1];
    struct vnode *v, *odir, *ndir;
    size_t lf;
    int r = vfs_normalize(vfs_getcwd(), from, afrom, sizeof(afrom));
    if(r == 0) r = vfs_normalize(vfs_getcwd(), to, ato, sizeof(ato));
    if(r) return r;
    lf = strlen(afrom);
    if(!strncmp(ato, afrom, lf) && (ato[lf] == '/' || ato[lf] == 0)) return ato[lf] ? -EINVAL : 0;   /* into itself, or no change */
    r = lookup_abs(afrom, &v);
    if(r) return r;
    if(v == v->sb->root) { vput(v); return -EBUSY; }                /* a mount point */
    vput(v);
    r = lookup_parent(afrom, &odir, oname);
    if(r) return r;
    r = lookup_parent(ato, &ndir, nname);
    if(r) { vput(odir); return r; }
    if(odir->sb != ndir->sb) r = -EXDEV;
    else if(odir->sb->readonly) r = -EROFS;
    else if(!odir->ops->rename) r = -ENOSYS;
    else r = odir->ops->rename(odir, oname, ndir, nname);
    if(r == 0 && odir->sb->ops && odir->sb->ops->sync) odir->sb->ops->sync(odir->sb);
    vput(odir);
    vput(ndir);
    return r;
}

/* 10/6/26: the mount's path is copied and its root held under mount_lock; the file system answers
   (it may read the disk) with the lock let go and the root still held, so a umount meanwhile finds
   it busy instead of freeing it */
int vfs_statfs(const char *path, struct statfs *st) {
    char abs[VFS_PATH_MAX];
    struct mount *m, *best = NULL;
    struct superblock *sb = NULL;
    size_t bestlen = 0;
    int r = vfs_normalize(vfs_getcwd(), path, abs, sizeof(abs));
    if(r) return r;
    memset(st, 0, sizeof(*st));
    mutex_lock(&mount_lock);
    for(m = mounts; m; m = m->next) {
        size_t l = strlen(m->path);
        if(l == 1) { if(!best) { best = m; bestlen = 1; } continue; }
        if(l > bestlen && !strncmp(abs, m->path, l) && (abs[l] == '/' || abs[l] == 0)) { best = m; bestlen = l; }
    }
    if(best) {
        sb = best->sb;
        vget(sb->root);
        strlcpy(st->path, best->path, sizeof(st->path));
    }
    mutex_unlock(&mount_lock);
    if(!sb) return -ENOENT;
    strlcpy(st->fs, sb->type->name, sizeof(st->fs));
    strlcpy(st->dev, sb->dev ? sb->dev->name : "", sizeof(st->dev));    /* block devices are never freed */
    st->flags = sb->readonly ? MNT_RDONLY : 0;
    r = sb->ops && sb->ops->statfs ? sb->ops->statfs(sb, st) : 0;      /* sizes unknown: all zero */
    vput(sb->root);
    return r;
}

int vfs_ftruncate(struct file *f, u32 size) {
    int r;
    if(!f) return -EBADF;
    if(f->v->type != VN_FILE) return -EISDIR;
    if((f->flags & O_ACCMODE) == O_RDONLY) return -EBADF;
    if(f->v->sb->readonly || !f->v->ops->truncate) return -EROFS;
    r = f->v->ops->truncate(f->v, size);           /* the position stays where it is (POSIX) */
    return r;
}

int vfs_fsync(struct file *f) {
    struct superblock *sb;
    if(!f) return -EBADF;
    sb = f->v->sb;
    if(write_back(sb) != 0) return -EIO;            /* 10/16/26 (B10): as close says it */
    if(sb->dev && bdev_flush(sb->dev) != 0) return -EIO;
    return 0;
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
    case EINTR: return "Interrupted system call";
    case EIO: return "I/O error";
    case EBADF: return "Bad file descriptor";
    case E2BIG: return "Argument list too long";
    case ENOMEM: return "Out of memory";
    case EFAULT: return "Bad address";
    case ERANGE: return "Result too large";
    case EACCES: return "Permission denied";
    case EBUSY: return "Device or resource busy";
    case EXDEV: return "Invalid cross-device link";
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
    case EAGAIN: return "Try again";
    case ENFILE: return "File table overflow";
    case EPIPE: return "Broken pipe";
    case EADDRINUSE: return "Address already in use";
    case ENETUNREACH: return "Network is unreachable";
    case ECONNRESET: return "Connection reset by peer";
    case ENOTCONN: return "Transport endpoint is not connected";
    case ETIMEDOUT: return "Connection timed out";
    case ECONNREFUSED: return "Connection refused";
    case EHOSTUNREACH: return "No route to host";
    }
    return "Unknown error";
}

/**** boot ****/

void init_fs(void) {
    struct bdev *d;
    bool have_initrd = false;
    int r;

    poll_init();                        /* 10/6/26: poll()'s wait queue (fs/poll.c) */

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
        const struct fs_type *t;
        char path[VFS_PATH_MAX];
        if(d->bus == BDEV_BUS_USB) continue;    /* USB drives are mounted by hand (mount usb0p1), in either profile */
        t = probe_type(d);
        if(!t) continue;
        if(!strcmp(t->name, "tarfs") && !have_initrd) { strcpy(path, "/initrd"); have_initrd = true; }
        else if(bootinfo.laptop) {               /* native hardware: never write a disk nobody asked for */
            kprintf("+VFS: %s (%s) not mounted (profile=laptop mounts only the initrd; use mount)\n", d->name, t->name);
            continue;
        }
        else snprintf(path, sizeof(path), "/%s", d->name);
        r = vfs_mount_flags(d, path, t->name, d->protect ? MNT_RDONLY : 0);    /* the boot drive is only read */
        if(r) kprintf("+VFS: %s on %s (%s): %s\n", d->name, path, t->name, strerror(r));
        else kprintf("+VFS: %s mounted on %s (%s%s)\n", d->name, path, t->name, (d->readonly || d->protect) ? ", read-only" : "");
    }
    kprintf("*DONE\n");
}
