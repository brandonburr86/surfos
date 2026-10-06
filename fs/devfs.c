/*
SurfOS Device Files
--------------------
File: devfs.c   Date: 10/3/26 (roadmap P2)
--------------------
/dev holds the console (the calling task's tty and console), null and zero,
so a program's descriptors 0, 1 and 2 are ordinary open files.

10/6/26: devfs_is_console() tells a kernel command run by the user shell
(SYS_kcmd) whether descriptor 1 is the console or a redirection.
10/6/26: poll() (surfos/poll.h): the console is ready to read when its tty
has a key queued; the others are always ready.
*/

#include <surfos/types.h>
#include <surfos/console.h>
#include <surfos/task.h>
#include <surfos/tty.h>
#include <surfos/poll.h>
#include <fs/vfs.h>
#include <blibc_common.h>

enum { DEV_CONSOLE = 1, DEV_TTY, DEV_NULL, DEV_ZERO };

static const struct { const char *name; int id; } devices[] = {
    { "console", DEV_CONSOLE }, { "tty", DEV_TTY }, { "null", DEV_NULL }, { "zero", DEV_ZERO },
};
#define NDEVICES (sizeof(devices) / sizeof(devices[0]))

static const struct vnode_ops dev_vops;

static int dev_read(struct vnode *v, u32 off, void *buf, u32 len) {
    switch(v->ino) {
    case DEV_CONSOLE:
    case DEV_TTY:                                            /* a line, or in raw mode the keys (tty.c) */
        return tty_read((curTask && curTask->tty) ? curTask->tty : tty_active, (char *)buf, len);
    case DEV_ZERO: memset(buf, 0, len); return (int)len;
    default: return 0;                                       /* null: end of file */
    }
}

static int dev_write(struct vnode *v, u32 off, const void *buf, u32 len) {
    if(v->ino == DEV_CONSOLE || v->ino == DEV_TTY) {
        surf_console *con = (curTask && curTask->con) ? curTask->con : conActive;
        const char *p = (const char *)buf;
        u32 i;
        for(i = 0; i < len; i++) kputch(con, con->txtColor, p[i]);
    }
    return (int)len;
}

static struct vnode *dev_vnode(struct superblock *sb, int id) {
    struct vnode *v = vnode_alloc(sb, &dev_vops, VN_FILE);
    if(v) v->ino = id;
    return v;
}

static int dev_lookup(struct vnode *dir, const char *name, struct vnode **out) {
    u_int i;
    for(i = 0; i < NDEVICES; i++) {
        if(!strcmp(devices[i].name, name)) {
            *out = dev_vnode(dir->sb, devices[i].id);
            return *out ? 0 : -ENOMEM;
        }
    }
    return -ENOENT;
}

static int dev_readdir(struct vnode *dir, u_int index, struct dirent *de) {
    if(index >= NDEVICES) return 1;
    strlcpy(de->name, devices[index].name, sizeof(de->name));
    de->type = VN_FILE;
    de->size = 0;
    de->mtime = 0;
    return 0;
}

/* 10/6/26: the console (the polling task's tty) has keys queued, or not; the rest are always ready */
static int dev_poll(struct vnode *v, struct file *f) {
    (void)f;
    if(v->ino == DEV_CONSOLE || v->ino == DEV_TTY)
        return (tty_queued((curTask && curTask->tty) ? curTask->tty : tty_active) ? POLLIN : 0) | POLLOUT;
    return POLLIN | POLLOUT;
}

static const struct vnode_ops dev_vops = { dev_lookup, dev_readdir, dev_read, dev_write, NULL, NULL, NULL, NULL, NULL, dev_poll };

/* is f /dev/console or /dev/tty (the calling task's console)? */
bool devfs_is_console(const struct file *f) {
    return f && f->v && f->v->ops == &dev_vops && (f->v->ino == DEV_CONSOLE || f->v->ino == DEV_TTY);
}

static int dev_mount(struct superblock *sb) {
    sb->root = vnode_alloc(sb, &dev_vops, VN_DIR);
    return sb->root ? 0 : -ENOMEM;
}

static const struct fs_type devfs_type = { "devfs", NULL, dev_mount, NULL };

void devfs_init(void) {
    vfs_register(&devfs_type);
}
