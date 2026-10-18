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
last line is shown. Nothing here keeps any state.
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
    bool made = false;
    int r = -ENAMETOOLONG, k;
    if(kept && n) kept[0] = 0;
    for(k = 0; k < 10; k++) {
        if(snprintf(tmp, sizeof(tmp), k ? "%s.%d.tmp" : "%s.tmp", path, k) >= (int)sizeof(tmp)) break;
        r = write_file(tmp, O_EXCL, data, len, crlf, &made);
        if(r != -EEXIST) break;                         /* a file the user has under that name: the next name */
    }
    if(r == 0) {
        if((r = vfs_rename(tmp, path)) == 0) return 0;
        if(!nothing_changed(r)) {                       /* perhaps half done: the text stays in the new file */
            if(kept) strlcpy(kept, tmp, n);
            return r;
        }
        vfs_unlink(tmp);
        if(r != -ENOSYS) return r;
    } else if(made) {
        vfs_unlink(tmp);                                /* made by this call (O_EXCL): what of the text it got */
        return r;
    } else if(r != -ENAMETOOLONG) return r;           /* nothing was made, and nothing of the user's touched */
    return write_file(path, O_TRUNC, data, len, crlf, NULL);   /* no rename here, or no room for such a name: in place */
}

static int fs_mkdir(void *ctx, const char *path) {
    return vfs_mkdir(path);
}

static int fs_remove(void *ctx, const char *path, bool dir) {
    return dir ? vfs_rmdir(path) : vfs_unlink(path);
}

static int fs_rename(void *ctx, const char *from, const char *to) {
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
    return run("mount", dev, msg, n);
}

static int fs_eject(void *ctx, const char *dev, char *msg, size_t n) {
    return run("eject", dev, msg, n);
}

static const struct gui_fs vfs_ops = {
    NULL, fs_list, fs_stat, fs_read, fs_write, fs_mkdir, fs_remove, fs_rename, fs_volumes, fs_mount, fs_eject, strerror
};

const struct gui_fs *gui_kernel_fs(void) {
    return &vfs_ops;
}
