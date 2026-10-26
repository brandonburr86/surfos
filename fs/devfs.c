/*
SurfOS Device Files
--------------------
File: devfs.c   Date: 10/3/26 (roadmap P2)
--------------------
/dev holds the console (the calling task's tty and console), null and zero,
so a program's descriptors 0, 1 and 2 are ordinary open files.
10/6/26: and kmsg: what a program writes there goes to the kernel log (dmesg),
and to no console; a read finds nothing (end of file).

10/6/26: devfs_is_console() tells a kernel command run by the user shell
(SYS_kcmd) whether descriptor 1 is the console or a redirection.
10/6/26: poll() (surfos/poll.h): the console is ready to read when its tty
has a key queued; the others are always ready.

10/25/26 (track A, A10): /dev/config holds surfsh's two files in surfos-config/ on the
stick SurfOS was started from, by the network manager's way to the stick (kernel/netman.c),
with no system call of their own: surfshrc, the owner's startup file, which surfsh runs, and
history, there only while the history setting is on. A file is read from the stick when its
name is looked up, so when it is opened, into memory, which its reads and writes then use.
History, once written, goes back to the stick when it is closed (fs/vfs.c calls the vnode's
flush), and close says EIO when it could not be saved, the reason in the kernel log.
surfshrc is never written. And hostname, the name DHCP requests give (the hostname
setting) and a newline, empty before the settings are read: surfsh's $HOSTNAME, which a
telnet session's prompt shows (the system calls' structures keep their sizes, so netinfo
could not carry it).
*/

#include <surfos/types.h>
#include <surfos/console.h>
#include <surfos/task.h>
#include <surfos/tty.h>
#include <surfos/poll.h>
#include <surfos/klog.h>
#include <fs/vfs.h>
#include <mm/kalloc.h>
#include <net/netcfg.h>
#include <net/netman.h>
#include <net/dhcp.h>
#include <blibc_common.h>

enum { DEV_CONSOLE = 1, DEV_TTY, DEV_NULL, DEV_ZERO, DEV_KMSG, DEV_CONFIG, CFG_SURFSHRC, CFG_HISTORY, CFG_HOSTNAME };

static const struct { const char *name; int id; } devices[] = {
    { "console", DEV_CONSOLE }, { "tty", DEV_TTY }, { "null", DEV_NULL }, { "zero", DEV_ZERO },
    { "kmsg", DEV_KMSG }, { "config", DEV_CONFIG },
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
    if(v->ino == DEV_KMSG) {                                 /* 10/6/26: the kernel log only, as it was written */
        const char *p = (const char *)buf;
        char chunk[128];
        u32 done = 0, n, i;
        while(done < len) {
            n = len - done < sizeof(chunk) - 1 ? len - done : sizeof(chunk) - 1;
            for(i = 0; i < n; i++) chunk[i] = p[done + i] ? p[done + i] : ' ';   /* a NUL would end the string */
            chunk[n] = 0;
            klog_write(chunk);
            done += n;
        }
    }
    return (int)len;
}

static struct vnode *dev_vnode(struct superblock *sb, int id) {
    struct vnode *v = vnode_alloc(sb, &dev_vops, VN_FILE);
    if(v) v->ino = id;
    return v;
}

/**** 10/25/26 (track A, A10): /dev/config ****/

struct cfg_file {
    char *buf;                  /* the file as read, then as written: len bytes of cap */
    u32 len, cap;
    bool written;               /* to go back to the stick when it is closed */
};

static const struct vnode_ops cfg_dir_vops, cfg_vops;

static int cfg_lookup(struct vnode *dir, const char *name, struct vnode **out) {
    struct cfg_file *f;
    struct vnode *v;
    char why[160];
    int id = !strcmp(name, NETCFG_SURFSHRC) ? CFG_SURFSHRC : !strcmp(name, NETCFG_HISTORY_FILE) ? CFG_HISTORY :
             !strcmp(name, "hostname") ? CFG_HOSTNAME : 0, r = 0;
    if(!id) return -ENOENT;
    /* the settings as the stick has them, at boot too; a stick still not there that could come (a USB one) after
       8 s: said on the caller's console, once for each surfsh (its startup file's lookup) */
    if(netman_wait_boot() && id == CFG_SURFSHRC)
        kprintf("surfsh: the stick SurfOS was started from was not ready: no surfshrc or history read\n");
    if(id == CFG_HISTORY && !netman_history_on()) return -ENOENT;
    f = (struct cfg_file *)kcalloc(1, sizeof(*f));
    if(f) {
        f->cap = id == CFG_HISTORY ? NETCFG_HISTORY_MAX : id == CFG_SURFSHRC ? NETCFG_SURFSHRC_MAX : NETCFG_HOSTNAME_MAX + 1;
        f->buf = (char *)kalloc(f->cap + 1);
    }
    if(!f || !f->buf) {
        if(f) kfree(f);
        return -ENOMEM;
    }
    if(id == CFG_HOSTNAME) {                                /* from memory, not the stick */
        dhcp_hostname(f->buf, f->cap);
        f->len = (u32)strlen(f->buf);
        if(f->len) f->buf[f->len++] = '\n';
    } else r = netman_file_read(name, f->buf, f->cap, &f->len, why, sizeof(why));
    if(r == -ENOENT && id == CFG_HISTORY) r = 0;        /* none yet: empty, to be written */
    v = r ? NULL : vnode_alloc(dir->sb, &cfg_vops, VN_FILE);
    if(!v) {
        kfree(f->buf);
        kfree(f);
        return r ? r : -ENOMEM;
    }
    v->ino = (u32)id;
    v->size = f->len;
    v->priv = f;
    *out = v;
    return 0;
}

static int cfg_readdir(struct vnode *dir, u_int index, struct dirent *de) {
    static const char *const names[] = { "hostname", NETCFG_SURFSHRC, NETCFG_HISTORY_FILE };
    if(index >= (netman_history_on() ? 3u : 2u)) return 1;
    strlcpy(de->name, names[index], sizeof(de->name));
    de->type = VN_FILE;
    de->size = 0;                                           /* not read for a listing */
    de->mtime = 0;
    return 0;
}

static int cfg_read(struct vnode *v, u32 off, void *buf, u32 len) {
    struct cfg_file *f = (struct cfg_file *)v->priv;
    if(off >= f->len) return 0;
    if(len > f->len - off) len = f->len - off;
    memcpy(buf, f->buf + off, len);
    return (int)len;
}

static int cfg_write(struct vnode *v, u32 off, const void *buf, u32 len) {
    struct cfg_file *f = (struct cfg_file *)v->priv;
    if(v->ino != CFG_HISTORY) return -EROFS;               /* surfshrc is the owner's */
    if(off > f->cap || len > f->cap - off) return -EFBIG;
    f->written = true;
    if(off > f->len) memset(f->buf + f->len, 0, off - f->len);
    memcpy(f->buf + off, buf, len);
    if(off + len > f->len) f->len = off + len;
    v->size = f->len;
    return (int)len;
}

static int cfg_truncate(struct vnode *v, u32 size) {
    struct cfg_file *f = (struct cfg_file *)v->priv;
    if(v->ino != CFG_HISTORY) return -EROFS;
    if(size > f->cap) return -EFBIG;
    f->written = true;
    if(size > f->len) memset(f->buf + f->len, 0, size - f->len);
    f->len = v->size = size;
    return 0;
}

/* the file closed (fs/vfs.c): history, when it was written, back to the stick; -EIO when it could not be saved, the
   reason in the kernel log */
static int cfg_flush(struct vnode *v) {
    struct cfg_file *f = (struct cfg_file *)v->priv;
    char why[160], line[220];
    if(!f || !f->written) return 0;
    f->written = false;
    if(!netman_file_write(NETCFG_HISTORY_FILE, f->buf, f->len, why, sizeof(why))) return 0;
    snprintf(line, sizeof(line), "+NET: %s/%s was not saved: %s\n", NETCFG_DIR, NETCFG_HISTORY_FILE, why);
    klog_write(line);
    return -EIO;
}

static void cfg_release(struct vnode *v) {
    struct cfg_file *f = (struct cfg_file *)v->priv;
    if(!f) return;
    kfree(f->buf);
    kfree(f);
    v->priv = NULL;
}

static const struct vnode_ops cfg_dir_vops = { cfg_lookup, cfg_readdir, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL };
static const struct vnode_ops cfg_vops = { NULL, NULL, cfg_read, cfg_write, cfg_truncate, NULL, NULL, cfg_release, NULL,
                                           NULL, cfg_flush };

static int dev_lookup(struct vnode *dir, const char *name, struct vnode **out) {
    u_int i;
    for(i = 0; i < NDEVICES; i++) {
        if(!strcmp(devices[i].name, name)) {
            if(devices[i].id == DEV_CONFIG) {                  /* 10/25/26: a directory */
                *out = vnode_alloc(dir->sb, &cfg_dir_vops, VN_DIR);
                if(*out) (*out)->ino = DEV_CONFIG;
            } else *out = dev_vnode(dir->sb, devices[i].id);
            return *out ? 0 : -ENOMEM;
        }
    }
    return -ENOENT;
}

static int dev_readdir(struct vnode *dir, u_int index, struct dirent *de) {
    if(index >= NDEVICES) return 1;
    strlcpy(de->name, devices[index].name, sizeof(de->name));
    de->type = devices[index].id == DEV_CONFIG ? VN_DIR : VN_FILE;
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
