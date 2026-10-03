/*
SurfOS tar File System
--------------------
File: tarfs.c   Date: 10/3/26 (roadmap F1)
--------------------
A read-only file system over a ustar archive on a block device, which is how
the initial RAM disk is built (tools/mkimage.py). The directory tree is read
once at mount time; file data is read from the device through the block cache.
*/

#include <surfos/types.h>
#include <surfos/console.h>
#include <sys/bdev.h>
#include <fs/vfs.h>
#include <fs/bcache.h>
#include <mm/kalloc.h>
#include <blibc_common.h>

struct tar_node {
    char *name;
    int type;
    u32 size, mtime, data_lba, ino;
    struct tar_node *parent, *children, *next;
};

struct tar_fs {
    struct bdev *dev;
    struct tar_node *root;
    u_int nnodes;
};

static const struct vnode_ops tar_vops;

static char *kstrndup(const char *s, size_t n) {
    char *d = (char *)kalloc(n + 1);
    if(!d) return NULL;
    memcpy(d, s, n);
    d[n] = 0;
    return d;
}

static u32 octal(const u8 *s, int n) {
    u32 v = 0;
    while(n-- && *s) {
        if(*s >= '0' && *s <= '7') v = v * 8 + (*s - '0');
        else if(*s != ' ') break;
        s++;
    }
    return v;
}

static struct tar_node *find_child(struct tar_node *dir, const char *name, size_t n) {
    struct tar_node *c;
    for(c = dir->children; c; c = c->next) {
        if(strlen(c->name) == n && !memcmp(c->name, name, n)) return c;
    }
    return NULL;
}

static struct tar_node *add_child(struct tar_fs *fs, struct tar_node *dir, const char *name, size_t n, int type) {
    struct tar_node *c = (struct tar_node *)kcalloc(1, sizeof(struct tar_node)), **pp;
    if(!c) return NULL;
    c->name = kstrndup(name, n);
    if(!c->name) { kfree(c); return NULL; }
    c->type = type;
    c->parent = dir;
    c->ino = ++fs->nnodes;
    for(pp = &dir->children; *pp; pp = &(*pp)->next);   /* archive order */
    *pp = c;
    return c;
}

/* "etc/version": create the directories on the way, return the last component's node */
static struct tar_node *insert_path(struct tar_fs *fs, const char *path, int type) {
    struct tar_node *dir = fs->root, *n = NULL;
    while(*path) {
        const char *s = path;
        size_t len;
        bool last;
        while(*path && *path != '/') path++;
        len = path - s;
        while(*path == '/') path++;
        last = (*path == 0);
        if(!len || (len == 1 && s[0] == '.')) continue;
        if(len > VFS_NAME_MAX) return NULL;
        n = find_child(dir, s, len);
        if(!n) n = add_child(fs, dir, s, len, last ? type : VN_DIR);
        if(!n) return NULL;
        if(last) n->type = type;
        dir = n;
    }
    return n;
}

static void free_tree(struct tar_node *n) {
    struct tar_node *c = n->children;
    while(c) {
        struct tar_node *next = c->next;
        free_tree(c);
        c = next;
    }
    kfree(n->name);
    kfree(n);
}

/**** vnodes ****/

static struct vnode *tar_vnode(struct superblock *sb, struct tar_node *n) {
    struct vnode *v = vnode_alloc(sb, &tar_vops, n->type);
    if(!v) return NULL;
    v->size = n->size;
    v->mtime = n->mtime;
    v->ino = n->ino;
    v->priv = n;
    return v;
}

static int tar_lookup(struct vnode *dir, const char *name, struct vnode **out) {
    struct tar_node *n = find_child((struct tar_node *)dir->priv, name, strlen(name));
    if(!n) return -ENOENT;
    *out = tar_vnode(dir->sb, n);
    return *out ? 0 : -ENOMEM;
}

static int tar_readdir(struct vnode *dir, u_int index, struct dirent *de) {
    struct tar_node *c;
    for(c = ((struct tar_node *)dir->priv)->children; c; c = c->next) {
        if(index-- == 0) {
            strlcpy(de->name, c->name, sizeof(de->name));
            de->type = c->type;
            de->size = c->size;
            de->mtime = c->mtime;
            return 0;
        }
    }
    return 1;
}

static int tar_read(struct vnode *v, u32 off, void *buf, u32 len) {
    struct tar_node *n = (struct tar_node *)v->priv;
    struct tar_fs *fs = (struct tar_fs *)v->sb->priv;
    u8 *p = (u8 *)buf;
    u32 done = 0;
    if(off >= n->size) return 0;
    if(len > n->size - off) len = n->size - off;
    while(done < len) {
        u32 pos = off + done, boff = pos % 512, chunk = 512 - boff;
        struct buf *b;
        if(chunk > len - done) chunk = len - done;
        b = bread(fs->dev, n->data_lba + pos / 512);
        if(!b) return done ? (int)done : -EIO;
        memcpy(p + done, b->data + boff, chunk);
        brelse(b);
        done += chunk;
    }
    return (int)done;
}

static const struct vnode_ops tar_vops = { tar_lookup, tar_readdir, tar_read, NULL, NULL, NULL, NULL, NULL };

/**** mount ****/

static bool tar_probe(struct bdev *dev) {
    struct buf *b = bread(dev, 0);
    bool ok;
    if(!b) return false;
    ok = !memcmp(b->data + 257, "ustar", 5);
    brelse(b);
    return ok;
}

static int tar_unmount(struct superblock *sb) {
    struct tar_fs *fs = (struct tar_fs *)sb->priv;
    if(fs) { free_tree(fs->root); kfree(fs); }
    return 0;
}

static const struct superblock_ops tar_sops = { NULL, tar_unmount };

static int tar_mount(struct superblock *sb) {
    struct tar_fs *fs = (struct tar_fs *)kcalloc(1, sizeof(struct tar_fs));
    char *longname = NULL;
    u32 lba = 0, files = 0;

    if(!fs) return -ENOMEM;
    if(!sb->dev) { kfree(fs); return -ENODEV; }
    fs->dev = sb->dev;
    fs->root = (struct tar_node *)kcalloc(1, sizeof(struct tar_node));
    if(!fs->root) { kfree(fs); return -ENOMEM; }
    fs->root->name = kstrndup("", 0);
    fs->root->type = VN_DIR;

    while(lba < fs->dev->nblocks) {
        struct buf *b = bread(fs->dev, lba);
        char name[VFS_PATH_MAX];
        u32 size, mtime, nblk;
        u8 type;
        if(!b) { free_tree(fs->root); kfree(fs); kfree(longname); return -EIO; }
        if(b->data[0] == 0 || memcmp(b->data + 257, "ustar", 5)) { brelse(b); break; }   /* end of archive */
        size = octal(b->data + 124, 12);
        mtime = octal(b->data + 136, 12);
        type = b->data[156];
        if(b->data[345]) snprintf(name, sizeof(name), "%.155s/%.100s", (char *)b->data + 345, (char *)b->data);
        else snprintf(name, sizeof(name), "%.100s", (char *)b->data);
        brelse(b);
        nblk = (size + 511) / 512;
        if(type == 'L') {                                   /* GNU long name: the data is the next entry's name */
            u32 got = 0;
            kfree(longname);
            longname = (char *)kcalloc(1, VFS_PATH_MAX);
            while(longname && got < size && got < VFS_PATH_MAX - 1) {
                struct buf *d = bread(fs->dev, lba + 1 + got / 512);
                u32 chunk = 512;
                if(!d) break;
                if(chunk > size - got) chunk = size - got;
                if(chunk > VFS_PATH_MAX - 1 - got) chunk = VFS_PATH_MAX - 1 - got;
                memcpy(longname + got, d->data, chunk);
                brelse(d);
                got += chunk;
            }
        } else if(type == '0' || type == 0 || type == '7' || type == '5') {
            const char *p = longname ? longname : name;
            struct tar_node *n;
            size_t l;
            while(p[0] == '.' && p[1] == '/') p += 2;
            while(*p == '/') p++;
            l = strlen(p);
            strlcpy(name, p, sizeof(name));
            while(l && name[l - 1] == '/') name[--l] = 0;
            if(l) {
                n = insert_path(fs, name, type == '5' ? VN_DIR : VN_FILE);
                if(n) { n->size = (type == '5') ? 0 : size; n->mtime = mtime; n->data_lba = lba + 1; files++; }
            }
            kfree(longname);
            longname = NULL;
        }
        lba += 1 + nblk;
    }
    kfree(longname);
    sb->priv = fs;
    sb->ops = &tar_sops;
    sb->readonly = true;
    sb->root = tar_vnode(sb, fs->root);
    if(!sb->root) { free_tree(fs->root); kfree(fs); return -ENOMEM; }
    kprintf("+TARFS: %s: %lu entries in %lu blocks\n", fs->dev->name, (u_long)files, (u_long)lba);
    return 0;
}

static const struct fs_type tarfs_type = { "tarfs", tar_probe, tar_mount, NULL };

void tarfs_init(void) {
    vfs_register(&tarfs_type);
}
