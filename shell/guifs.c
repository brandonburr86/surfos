/*
SurfOS Desktop Files on the VFS
--------------------
File: guifs.c Date: 10/17/26
--------------------
struct gui_fs (include/sys/gui.h) for the kernel: Notepad and Files reach the file
systems through these, from the desktop's own task (cmd_gui), never while drawing.
Saving is edit's: the text is written to a new file beside the file and fsynced,
then renamed over it. The new file is <file>.tmp, or <file>.1.tmp and so on when
the user has a file of that name (O_EXCL: theirs is never touched). It is deleted
again only when the rename failed before it changed anything; after an I/O error
the rename may have half happened, and the text stays in it. Only where a file
system cannot rename, or no such name fits in a path, is the file written in
place. The volumes are the USB drives' FAT file systems, and the mounts of sticks
that were pulled out. Mount and Eject run the shell's own mount and eject with
their output kept (console_capture_begin()): the same rules (bdev_mount_readonly(),
busy volumes, the flush before "safe to remove") and the same words, of which the
last line is shown. Nothing here keeps any state, but for a desktop's read_at below.
10/18/26: each desktop has its own struct gui_fs (gui_kernel_fs_begin()), whose read_at
keeps the file it read last open until its frame's reads are done
(gui_kernel_fs_idle()): FAT remembers a file's place in its cluster chain only while
the file is open, so each read deep into a big file from a fresh open walked the chain
from its start, and a frame of the Image Viewer's or Hex's reads did that again and
again. The file is closed once the frame's tick and input are done, before the desktop
draws or sleeps, before anything it changes, mounts or ejects, and when it reads another
file. So it is open only while a frame's reads run: an eject on another console that
comes in those few milliseconds (the desktop's task preempted mid-read) finds the volume
busy, as it would during any single read.
*/
#include <stdio.h>
#include <string.h>
#include <surfos/task.h>
#include <surfos/console.h>
#include <sys/bdev.h>
#include <sys/part.h>
#include <fs/vfs.h>
#include <mm/kalloc.h>
#include <sys/gui.h>
#include "shell.h"

struct guifs_session {
    struct gui_fs fs;                       /* first: the desktop's struct gui_fs, whose ctx this is */
    struct file *f;                         /* read_at's file, open; NULL */
    char path[VFS_PATH_MAX];
};

/* the session's open file closed (a ctx of NULL: the shared struct gui_fs, which keeps none) */
static void forget(void *ctx) {
    struct guifs_session *s = (struct guifs_session *)ctx;
    if(s && s->f) {
        vfs_close(s->f);
        s->f = NULL;
    }
}

static int fs_list(void *ctx, const char *path, struct gui_entry *out, int max) {
    struct file *f;
    struct dirent de;
    int r, n = 0;
    r = vfs_open(path, O_RDONLY | O_DIRECTORY, &f);
    if(r) return r;
    while((r = vfs_readdir(f, &de)) == 0) {
        if(!strcmp(de.name, ".") || !strcmp(de.name, "..")) continue;
        if(n < max) {
            strlcpy(out[n].name, de.name, sizeof(out[n].name));
            out[n].type = de.type == VN_DIR ? GUI_DIR : GUI_FILE;
            out[n].size = de.size;
            out[n].mtime = de.mtime;
        }
        n++;
    }
    vfs_close(f);
    return r < 0 ? r : n;
}

static int fs_stat(void *ctx, const char *path, struct gui_entry *out, bool *device, bool *readonly) {
    struct stat st;
    struct statfs sf;
    const char *name = strrchr(path, '/');
    int r = vfs_stat(path, &st);
    if(r) return r;
    memset(out, 0, sizeof(*out));
    strlcpy(out->name, name && name[1] ? name + 1 : path, sizeof(out->name));
    out->type = st.type == VN_DIR ? GUI_DIR : GUI_FILE;
    out->size = st.size;
    out->mtime = st.mtime;
    *device = !strcmp(st.fs, "devfs");
    *readonly = *device || !strcmp(st.fs, "tarfs") || !strcmp(st.fs, "rootfs") ||
                (vfs_statfs(path, &sf) == 0 && (sf.flags & MNT_RDONLY));
    return 0;
}

static int fs_read(void *ctx, const char *path, char *buf, u32 size, u32 *len) {
    struct file *f;
    char more;
    int r;
    *len = 0;
    r = vfs_open(path, O_RDONLY, &f);
    if(r) return r;
    while(*len < size && (r = vfs_read(f, buf + *len, size - *len)) > 0) *len += r;
    if(r >= 0 && *len == size && vfs_read(f, &more, 1) > 0) r = -EFBIG;
    vfs_close(f);
    return r < 0 ? r : 0;
}

/* 10/17/26 (the suite: Hex, Image Viewer): n bytes from offset off, and the file's size */
static int fs_read_at(void *ctx, const char *path, u32 off, char *buf, u32 n, u32 *len, u32 *size) {
    struct guifs_session *s = (struct guifs_session *)ctx;
    struct file *f;
    struct stat st;
    int r;
    *len = 0;
    *size = 0;
    r = vfs_stat(path, &st);
    if(r) { forget(s); return r; }
    if(st.type == VN_DIR) return -EISDIR;
    *size = st.size;
    if(off >= st.size || !n) return 0;
    if(s && s->f && !strcmp(s->path, path)) f = s->f;  /* 10/18/26: this frame's file: on from its place */
    else {
        forget(s);
        r = vfs_open(path, O_RDONLY, &f);
        if(r) return r;
        if(s && strlcpy(s->path, path, sizeof(s->path)) < sizeof(s->path)) s->f = f;
    }
    f->pos = off;                                   /* 10/18/26: not vfs_lseek(), whose i32 stops at 2 GB */
    while(!r && *len < n && (r = vfs_read(f, buf + *len, n - *len)) > 0) {
        *len += r;
        r = 0;
    }
    if(!s || s->f != f) vfs_close(f);
    else if(r < 0) forget(s);
    return r < 0 ? r : 0;
}

static int write_all(struct file *f, const char *data, u32 len) {
    int r;
    while(len) {
        r = vfs_write(f, data, len);
        if(r <= 0) return r ? r : -EIO;
        data += r;
        len -= r;
    }
    return 0;
}

/* the whole text, each \n as \r\n when crlf, then fsync; flags: O_EXCL for a new file, or O_TRUNC;
   *made: the file was opened (with O_EXCL: this call made it, so it is ours to delete) */
static int write_file(const char *path, int flags, const char *data, u32 len, bool crlf, bool *made) {
    struct file *f;
    char chunk[512];
    u32 used = 0, i;
    int r = vfs_open(path, O_WRONLY | O_CREAT | flags, &f), e;
    if(made) *made = !r;
    if(r) return r;
    if(!crlf) r = write_all(f, data, len);
    else {
        for(i = 0; i < len && !r; i++) {
            if(data[i] == '\n') chunk[used++] = '\r';
            chunk[used++] = data[i];
            if(used >= sizeof(chunk) - 1) { r = write_all(f, chunk, used); used = 0; }
        }
        if(!r && used) r = write_all(f, chunk, used);
    }
    if(!r) r = vfs_fsync(f);
    e = vfs_close(f);
    return r ? r : e;
}

/* a rename that failed before it changed anything (fs/vfs.c, fs/fat.c): its new file may go */
static bool nothing_changed(int r) {
    return r == -ENOSYS || r == -EXDEV || r == -EROFS || r == -EBUSY || r == -EINVAL || r == -ENOENT ||
           r == -ENOTDIR || r == -EISDIR || r == -ENOTEMPTY || r == -ENAMETOOLONG || r == -ENOSPC || r == -ENOMEM;
}

static int fs_write(void *ctx, const char *path, const char *data, u32 len, bool crlf, char *kept, size_t n) {
    char tmp[VFS_PATH_MAX];
    struct stat st;
    bool made = false;
    int r = -ENAMETOOLONG, k;
    forget(ctx);                                    /* 10/18/26: read_at's file is not open while this changes things */
    if(kept && n) kept[0] = 0;
    for(k = 0; k < 10; k++) {
        if(snprintf(tmp, sizeof(tmp), k ? "%s.%d.tmp" : "%s.tmp", path, k) >= (int)sizeof(tmp)) break;
        r = write_file(tmp, O_EXCL, data, len, crlf, &made);
        if(r != -EEXIST) break;                         /* a file the user has under that name: the next name */
    }
    if(r == -ENAMETOOLONG && k > 0) r = -EEXIST;        /* <file>.tmp is there, the next name is too long: not in place */
    if(r == 0) {
        if((r = vfs_rename(tmp, path)) == 0) return 0;
        if(!nothing_changed(r)) {                       /* perhaps half done: the text stays in the new file, if it is there */
            if(kept && vfs_stat(tmp, &st) == 0) strlcpy(kept, tmp, n);
            return r;
        }
        if(vfs_unlink(tmp) != 0 && r != -ENOSYS && kept) strlcpy(kept, tmp, n);   /* it would not go: the text is in it */
        if(r != -ENOSYS) return r;
    } else if(made) {
        vfs_unlink(tmp);                                /* made by this call (O_EXCL): what of the text it got */
        return r;
    } else if(r != -ENAMETOOLONG) return r;           /* nothing was made, and nothing of the user's touched */
    return write_file(path, O_TRUNC, data, len, crlf, NULL);   /* no rename here, or no room for such a name: in place */
}

static int fs_mkdir(void *ctx, const char *path) {
    forget(ctx);
    return vfs_mkdir(path);
}

static int fs_remove(void *ctx, const char *path, bool dir) {
    forget(ctx);
    return dir ? vfs_rmdir(path) : vfs_unlink(path);
}

static int fs_rename(void *ctx, const char *from, const char *to) {
    forget(ctx);
    return vfs_rename(from, to);
}

/* the USB disks' FAT volumes: each partition, or a disk without partitions */
static int fs_volumes(void *ctx, struct gui_volume *out, int max) {
    struct vfs_mount_view *v = kalloc(32 * sizeof(*v));
    struct bdev *d, *p;
    const char *type;
    int n = 0, nv = v ? vfs_mount_views(v, 32) : 0, j;
    bool parts;
    if(nv > 32) nv = 32;
    for(d = bdev_first(); d; d = d->next) {
        if(d->gone || bdev_disk(d)->bus != BDEV_BUS_USB) continue;
        for(parts = false, p = bdev_first(); p && !d->parent; p = p->next) if(p->parent == d) parts = true;
        if(parts || !(type = vfs_probe(d))) continue;
        if(n < max) {
            memset(&out[n], 0, sizeof(out[n]));
            strlcpy(out[n].dev, d->name, sizeof(out[n].dev));
            strlcpy(out[n].fs, strcmp(type, "fat") ? type : !d->type_name ? "FAT" :      /* the partition type says which FAT */
                    strstr(d->type_name, "32") ? "FAT32" : strstr(d->type_name, "16") ? "FAT16" :
                    strstr(d->type_name, "12") ? "FAT12" : "FAT", sizeof(out[n].fs));
            strlcpy(out[n].disk, bdev_disk(d)->name, sizeof(out[n].disk));
            part_fmt_size(d->nblocks, out[n].size, sizeof(out[n].size));
            out[n].readonly = bdev_mount_readonly(d) || d->readonly || bdev_disk(d)->readonly;
            out[n].boot = d->protect || bdev_disk(d)->protect;
            for(j = 0; j < nv; j++) if(v[j].dev == d) { strlcpy(out[n].path, v[j].path, sizeof(out[n].path)); out[n].readonly = v[j].readonly; break; }
        }
        n++;
    }
    for(j = 0; j < nv; j++) {                           /* pulled out while mounted: still to be ejected */
        struct bdev *x = v[j].dev;
        if(!x || !x->gone || bdev_disk(x)->bus != BDEV_BUS_USB) continue;
        if(n < max) {
            memset(&out[n], 0, sizeof(out[n]));
            strlcpy(out[n].dev, x->name, sizeof(out[n].dev));
            strlcpy(out[n].disk, bdev_disk(x)->name, sizeof(out[n].disk));
            strlcpy(out[n].fs, "FAT", sizeof(out[n].fs));
            part_fmt_size(x->nblocks, out[n].size, sizeof(out[n].size));
            strlcpy(out[n].path, v[j].path, sizeof(out[n].path));
            out[n].readonly = out[n].gone = true;
        }
        n++;
    }
    kfree(v);
    return n;
}

/* the shell's command on dev, its output kept: 0 when it worked; its last line in msg */
static int run(const char *command, const char *dev, char *msg, size_t n) {
    struct kcapture k, *kept = curTask->kcap;
    char word[8], name[BDEV_NAME_LEN], said[768], *argv[3], *line, *end;
    int r;
    strlcpy(word, command, sizeof(word));
    strlcpy(name, dev, sizeof(name));
    argv[0] = word;
    argv[1] = name;
    argv[2] = NULL;
    k.buf = said;
    k.len = 0;
    k.size = sizeof(said) - 1;
    k.cut = 0;
    console_capture_begin(&k);
    r = shell_run_command(2, argv);
    console_capture_end();
    if(kept) console_capture_begin(kept);
    said[k.len] = 0;
    for(end = said + k.len; end > said && (end[-1] == '\n' || end[-1] == ' ' || end[-1] == '\r'); end--) ;
    *end = 0;
    line = strrchr(said, '\n');
    line = line ? line + 1 : said;
    while(*line == ' ') line++;
    strlcpy(msg, *line ? line : (r == CMD_OK ? "done" : "failed"), n);
    return r == CMD_OK ? 0 : -1;
}

static int fs_mount(void *ctx, const char *dev, char *msg, size_t n) {
    forget(ctx);
    return run("mount", dev, msg, n);
}

static int fs_eject(void *ctx, const char *dev, char *msg, size_t n) {
    forget(ctx);                                    /* else its own read_at's file would keep the volume busy */
    return run("eject", dev, msg, n);
}

static const struct gui_fs vfs_ops = {
    NULL, fs_list, fs_stat, fs_read, fs_write, fs_mkdir, fs_remove, fs_rename, fs_volumes, fs_mount, fs_eject, strerror,
    fs_read_at,
};

const struct gui_fs *gui_kernel_fs(void) {
    return &vfs_ops;
}

/* 10/18/26: a desktop's own, whose read_at keeps its file open between gui_kernel_fs_idle() calls; NULL without
   memory (the shared one then serves, keeping nothing) */
struct gui_fs *gui_kernel_fs_begin(void) {
    struct guifs_session *s = (struct guifs_session *)kcalloc(1, sizeof(*s));
    if(!s) return NULL;
    s->fs = vfs_ops;
    s->fs.ctx = s;
    return &s->fs;
}

/* the frame's reads are done: read_at's file is closed */
void gui_kernel_fs_idle(const struct gui_fs *fs) {
    if(fs) forget(fs->ctx);
}

/* the desktop ends: its file closed, and its struct gui_fs freed (fs may be NULL, or the shared one) */
void gui_kernel_fs_end(struct gui_fs *fs) {
    if(!fs || !fs->ctx) return;
    forget(fs->ctx);
    kfree(fs->ctx);
}
