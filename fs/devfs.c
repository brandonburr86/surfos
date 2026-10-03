/*
SurfOS Device Files
--------------------
File: devfs.c   Date: 10/3/26 (roadmap P2)
--------------------
/dev holds the console (the calling task's tty and console), null and zero,
so a program's descriptors 0, 1 and 2 are ordinary open files.
*/

#include <surfos/types.h>
#include <surfos/console.h>
#include <surfos/task.h>
#include <surfos/tty.h>
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
    case DEV_TTY: {
        struct tty *t = (curTask && curTask->tty) ? curTask->tty : tty_active;
        char line[TTY_LINE_MAX];
        int n;
        if(!len) return 0;
        n = tty_readline(t, line, sizeof(line));          /* cooked: a line with its newline */
        if(n < 0) return n;
        line[n++] = '\n';
        if((u32)n > len) n = (int)len;
        memcpy(buf, line, n);
        return n;
    }
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

static const struct vnode_ops dev_vops = { dev_lookup, dev_readdir, dev_read, dev_write, NULL, NULL, NULL, NULL };

static int dev_mount(struct superblock *sb) {
    sb->root = vnode_alloc(sb, &dev_vops, VN_DIR);
    return sb->root ? 0 : -ENOMEM;
}

static const struct fs_type devfs_type = { "devfs", NULL, dev_mount, NULL };

void devfs_init(void) {
    vfs_register(&devfs_type);
}
