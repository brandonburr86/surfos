/* Randomized review test for round fs-fix (d7452f4): random FAT operations on a RAM disk that refuses
   writes (all, or a block range) at random times, under cache pressure from reads of another disk.
   (10/16/26, round fs-fix-2: written by the independent review of B5-B11 as fsrev-tests/fuzz.c, and kept
   here as it was but for the run below (and an unused helper, release(), gone: a warning): with no
   arguments, `make unittest` runs a fixed set of seeds (about 1 s natively, 10 s in the devenv's emulated
   container), and a short write that changed
   more than it returned counts as a failure, audit B15.
   The review's campaigns ran it by hand: test_fatfuzz <seed> <ops> [rbad] [blocks] [32].)
   Checks, after every operation:
     - the oracle of tests/host/test_fatfs.c at every block the disk stores (no entry or FAT link on the
       way reaches a cluster the first FAT says is free; since round fs-fix-5, no named file shows another
       file's bytes: each block carries its file's tag; since round fs-fix-6 exactly: no block of a named
       file's range carries the tag of a file that never had that name in that directory);
     - bcache_audit() of both disks (B9);
     - an operation that failed with EIO (rename, unlink, truncate to a smaller size, create, mkdir)
       changed nothing that can be seen through the file system (B5);
   and at each clean point (device working, sync returns 0) the image, mounted afresh from a copy,
   holds exactly the model's tree and bytes.
   usage: fuzz <seed> <ops> [rbad]   (FUZZ_VERBOSE=1 prints every operation) */
#define main fatfs_main
#include "test_fatfs.c"
#undef main

#ifdef BEFORE
int bcache_audit(struct bdev *dev) { (void)dev; return 0; }
#endif

void *realloc(void *p, unsigned long n);
int atoi(const char *s);

static u32 rng_state;
static u32 rnd(void) { rng_state = rng_state * 1103515245u + 12345u; return (rng_state >> 8) & 0xFFFFFF; }
static u32 rn(u32 n) { return n ? rnd() % n : 0; }

#define MAXF 48
#define MAXD 4
#define MAXSZ 40000

struct mfile { int used, isdir, dir; char name[100]; u8 *data; u32 size, id; };   /* id: round fs-fix-5's tag */
static u32 file_ids;
/* 10/16/26 (round fs-fix-5): stale data. Every 512-byte block of a file's bytes starts with the file's tag (8 bytes:
   a mark and the file's number, the same whatever wrote it: a write that covers those bytes writes them), and after
   every write the disk stores, each named file's range is looked at: every block in it whose tag lies within the size
   and is whole carries the same one (the number twice, once inverted: a tag a later write or a gap's zeros cut into
   is no tag; zeros carry none). Another file's tag there is a block the file never wrote: the
   card names it over bytes it held before (B20's and B34's class). A file keeps its number through a rename */
#define TAG_MARK0 0xA5
#define TAG_MARK1 0x5A
static u8 tag_byte(u32 id, u32 k) {
    static const u8 mark[8] = { TAG_MARK0, TAG_MARK1, 0, 0, 0, 0, 'T', 'G' };
    if(k == 2 || k == 3) return (u8)(id >> (8 * (k - 2)));
    if(k == 4 || k == 5) return (u8)(~id >> (8 * (k - 4)));
    return mark[k];
}
static void tag_fill(u8 *b, u32 off, u32 len, u32 id) {
    u32 i;
    for(i = 0; i < len; i++) if((off + i) % 512 < 8) b[i] = tag_byte(id, (off + i) % 512);
}

static struct mfile mf[MAXF];          /* dir: index of the parent directory entry in mf (-1: root) */
static u32 g_viol;
static char g_first[200];
static int fverbose, op_no, failures_seen, oracle_reported, b5_bad, audit_bad, img_bad, sync_bad, wextra;
static char last_ops[16][160];
static int nlast;

static void note(const char *fmt, ...) {
    va_list ap;
    char line[160];
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    strlcpy(last_ops[nlast++ % 16], line, sizeof(last_ops[0]));
    if(fverbose) printf("  %5d %s\n", op_no, line);
}

static void dump_last(void) {
    int i, n = nlast < 16 ? nlast : 16;
    for(i = 0; i < n; i++) printf("      ... %s\n", last_ops[(nlast - n + i) % 16]);
}

static const char *short_names[] = { "a", "b", "c", "d", "e1", "e2", "f.txt", "g.c", "h" };
static void rand_name(char *out, size_t size) {
    if(rn(3)) strlcpy(out, short_names[rn(9)], size);
    else {
        u32 k = rn(4);
        snprintf(out, size, "a long file name %u %s", (unsigned)k, k == 3 ? "that goes on and on to need more than two sectors of entries, nearly" : "x");
    }
}

static int lname_eq(const char *a, const char *b) {
    while(*a && *b) { int x = *a, y = *b; if(x >= 'A' && x <= 'Z') x += 32; if(y >= 'A' && y <= 'Z') y += 32; if(x != y) return 0; a++; b++; }
    return *a == *b;
}

/* 10/17/26 (round fs-fix-6): the exact check (the review of B34-B38). Round fs-fix-5's check only saw a range holding
   two different files' tags: a file whose blocks all carry one other file's tag passed. Now every name a file or a
   directory ever had (created, or moved to), with the directory it was in (0: the root, else that directory's own
   number), is recorded before the operation that could put that name on the disk, and a block within a named file's
   range carrying the tag of a file that never had that name in that directory (a directory of the root that ever
   had the name the image shows) is a block the file never wrote: stale */
static int name_index(const char *name) {
    int i;
    for(i = 0; i < 9; i++) if(lname_eq(name, short_names[i])) return i;
    if(!strncmp(name, "a long file name ", 17) && name[17] >= '0' && name[17] <= '3' && name[18] == ' ') return 9 + name[17] - '0';
    return -1;
}
struct hnode { u32 dir; int ni, next; };
static struct hnode *hpool;
static int hn, hcap, hhead[65536];
static u32 exact_checked, exact_viol;
static void hist_reset(void) { memset(hhead, 0xFF, sizeof(hhead)); hn = 0; exact_checked = exact_viol = 0; }
static void hist_add(u32 id, u32 dir, const char *name) {
    int ni = name_index(name);
    if(ni < 0 || !id) return;
    if(hn == hcap) {
        hcap = hcap ? 2 * hcap : 4096;
        hpool = (struct hnode *)realloc(hpool, hcap * sizeof(*hpool));
        if(!hpool) die("out of memory");
    }
    hpool[hn].dir = dir;
    hpool[hn].ni = ni;
    hpool[hn].next = hhead[id & 0xFFFF];
    hhead[id & 0xFFFF] = hn++;
}
static int hist_had(u32 id, u32 dir, int ni) {          /* did file id ever have name ni in directory dir */
    int k;
    for(k = hhead[id & 0xFFFF]; k >= 0; k = hpool[k].next) if(hpool[k].ni == ni && hpool[k].dir == dir) return 1;
    return 0;
}
static int hist_had_in(u32 id, int ni, int dni) {       /* name ni in a directory of the root that ever had name dni */
    int k;
    for(k = hhead[id & 0xFFFF]; k >= 0; k = hpool[k].next)
        if(hpool[k].ni == ni && hpool[k].dir && hist_had(hpool[k].dir, 0, dni)) return 1;
    return 0;
}

static int find_in(int dir, const char *name) {
    int i;
    for(i = 0; i < MAXF; i++) if(mf[i].used && mf[i].dir == dir && lname_eq(mf[i].name, name)) return i;
    return -1;
}

static int children(int dir) {
    int i, n = 0;
    for(i = 0; i < MAXF; i++) if(mf[i].used && mf[i].dir == dir) n++;
    return n;
}

static int free_slot(void) {
    int i;
    for(i = 0; i < MAXF; i++) if(!mf[i].used) return i;
    return -1;
}

static struct superblock *sb;
static struct ram *x, *y;
static u32 arm_rbad;                    /* a block unreadable during the next file system call only */
#define ARMED(call) (x->rbad = arm_rbad, arm_rbad = 0, rr_tmp = (call), x->rbad = 0, rr_tmp)
static int rr_tmp;

/* the vnode of model directory dir (-1: root), held */
static struct vnode *dir_vnode(int dir) {
    struct vnode *v;
    if(dir < 0) { sb->root->refs++; return sb->root; }
    v = lookup(sb->root, mf[dir].name);
    return v;
}

/* what the file system shows of directory dir: names, sizes and bytes, against the model */
static int compare_dir(struct vnode *dv, int dir, const char *why, int deep) {
    struct dirent de;
    u_int i;
    int n = 0, bad = 0, k;
    static u8 back[MAXSZ + 1024];
    for(i = 0; dv->ops->readdir(dv, i, &de) == 0; i++) {
        int m = find_in(dir, de.name);
        n++;
        if(m < 0) { if(!bad++) printf("    %s: '%s' is there, not in the model\n", why, de.name); continue; }
        if(mf[m].isdir != (de.type == VN_DIR)) { if(!bad++) printf("    %s: '%s' type differs\n", why, de.name); continue; }
        if(!mf[m].isdir) {
            struct vnode *v = lookup(dv, de.name);
            if(!v) { if(!bad++) printf("    %s: '%s' cannot be looked up\n", why, de.name); continue; }
            if(v->size != mf[m].size) { if(!bad++) printf("    %s: '%s' size %u, model %u\n", why, de.name, v->size, mf[m].size); }
            else if(mf[m].size && (v->ops->read(v, 0, back, mf[m].size) != (int)mf[m].size || memcmp(back, mf[m].data, mf[m].size))) {
                if(!bad++) printf("    %s: '%s' bytes differ\n", why, de.name);
            }
            vput(v);
        } else if(deep) {
            struct vnode *v = lookup(dv, de.name);
            if(!v) { if(!bad++) printf("    %s: dir '%s' cannot be looked up\n", why, de.name); continue; }
            bad += compare_dir(v, m, why, 0);
            vput(v);
        }
    }
    k = children(dir);
    if(n != k && !bad++) printf("    %s: %d entries, the model has %d\n", why, n, k);
    return bad;
}

/* the image, mounted afresh from a copy, against the model */
static void image_check(const char *why) {
    struct ram *c = ram_new("copy", x->bd.nblocks);
    struct superblock *s2;
    struct superblock *keep = sb;
    memcpy(c->mem, x->mem, (size_t)x->bd.nblocks * 512);
    s2 = (struct superblock *)calloc(1, sizeof(*s2));
    s2->type = fat; s2->dev = &c->bd;
    mutex_init(&s2->lock, "fat2");
    if(fat->mount(s2) != 0) { printf("    %s: the image does not mount\n", why); img_bad++; dump_last(); return; }
    sb = s2;
    if(compare_dir(s2->root, -1, why, 1)) { img_bad++; printf("    (op %d) image differs from the model\n", op_no); dump_last(); }
    sb = keep;
    vput(s2->root);
    s2->ops->unmount(s2);
    c->bd.gone = true;                          /* its cached blocks go */
    binvalidate(&c->bd);
    nmade--;                                     /* (not audited at the end: it is gone) */
    free(s2);
    free(c->mem);
    free(c);
}

static void check_audit(void) {
    int a = bcache_audit(&x->bd) + bcache_audit(&y->bd);
    if(a && !audit_bad++) { printf("    (op %d) bcache_audit: %d counters off\n", op_no, a); dump_last(); }
}

static void check_oracle(void) {
    if(g_viol && !oracle_reported) {
        oracle_reported = 1;
        printf("    (op %d) ORACLE: %s\n", op_no, g_first);
        dump_last();
    }
}

static u8 buf1[MAXSZ + 1024];
/* the file's bytes as the file system has them now, into the model (after a write or a grow that may
   have stopped early); checks that bytes outside [lo, hi) kept their old values (zeros past the old end) */
static void reload(int m, u32 lo, u32 hi, const char *what) {
    struct vnode *dv = dir_vnode(mf[m].dir), *v;
    u32 i, n;
    if(!dv) return;
    v = lookup(dv, mf[m].name);
    vput(dv);
    if(!v) { printf("    (op %d) %s: file vanished\n", op_no, what); b5_bad++; return; }
    n = v->size;
    if(n > MAXSZ + 1000) { printf("    (op %d) %s: size %u\n", op_no, what, n); b5_bad++; vput(v); return; }
    if(n && v->ops->read(v, 0, buf1, n) != (int)n) { vput(v); return; }
    for(i = 0; i < n; i++) {
        if(i >= lo && i < hi) continue;
        if(i < mf[m].size ? buf1[i] != mf[m].data[i] : buf1[i] != 0) {
            printf("    (op %d) %s: byte %u outside the write changed\n", op_no, what, i);
            b5_bad++;
            break;
        }
    }
    mf[m].data = (u8 *)realloc(mf[m].data, n + 1);
    memcpy(mf[m].data, buf1, n);
    mf[m].size = n;
    vput(v);
}

/* the model's view of dir before an operation, to see that a failed one changed nothing */
static int unchanged(int dir, const char *what) {
    struct vnode *dv = dir_vnode(dir);
    int bad;
    if(!dv) { printf("    (op %d) %s: directory gone\n", op_no, what); return 1; }
    bad = compare_dir(dv, dir, what, 1);
    vput(dv);
    return bad;
}

static void op_create(int isdir) {
    int dir = rn(3) ? -1 : (int)rn(MAXD) - 1, m, r;
    u32 id;
    char name[100];
    struct vnode *dv, *v;
    if(dir >= 0 && (!mf[dir].used || !mf[dir].isdir)) dir = -1;
    if(isdir) dir = -1;                         /* directories only in the root */
    rand_name(name, sizeof(name));
    if(find_in(dir, name) >= 0 || (m = free_slot()) < 0) return;
    dv = dir_vnode(dir);
    if(!dv) return;
    id = 1 + file_ids++ % 65534;                        /* (16 bits, never 0) */
    hist_add(id, dir < 0 ? 0 : mf[dir].id, name);       /* (round fs-fix-6: before the name can reach the disk) */
    r = ARMED(dv->ops->create(dv, name, isdir ? VN_DIR : VN_FILE, &v));
    note("%s '%s' in %d: %d", isdir ? "mkdir" : "create", name, dir, r);
    if(r == 0) {
        mf[m].used = 1; mf[m].isdir = isdir; mf[m].dir = dir; strlcpy(mf[m].name, name, sizeof(mf[m].name));
        mf[m].id = id;
        mf[m].data = NULL; mf[m].size = 0;
        vput(v);
    } else if(unchanged(dir, "failed create")) { b5_bad++; printf("    (op %d) a failed create changed the directory\n", op_no); dump_last(); }
    vput(dv);
}

static int pick_file(void) {
    int i, k = rn(MAXF);
    for(i = 0; i < MAXF; i++) { int m = (k + i) % MAXF; if(mf[m].used && !mf[m].isdir) return m; }
    return -1;
}

static void op_write(void) {
    int m = pick_file(), w;
    struct vnode *dv, *v;
    u32 off, len, i, old;
    if(m < 0) return;
    dv = dir_vnode(mf[m].dir);
    if(!dv) return;
    v = lookup(dv, mf[m].name);
    vput(dv);
    if(!v) return;
    old = mf[m].size;
    off = rn(3) == 0 ? old : rn(old + 2000);
    len = 1 + rn(rn(4) == 0 ? 20000 : 3000);
    if(off + len > MAXSZ) { vput(v); return; }
    for(i = 0; i < len; i++) buf1[i] = (u8)rnd();
    tag_fill(buf1, off, len, mf[m].id);                 /* (round fs-fix-5: the file's tag where a block starts) */
    {
        static u8 oldb[MAXSZ + 1024];
        memcpy(oldb, mf[m].data ? mf[m].data : oldb, mf[m].size);
        w = ARMED(v->ops->write(v, off, buf1, len));
        note("write '%s' off %u len %u: %d (size %u -> %u)", mf[m].name, off, len, w, old, v->size);
        vput(v);
        {
            u32 hi = w > 0 ? off + (u32)w : off, newsize, j, changed = 0, first = 0;
            static u8 keep[MAXSZ + 1024];
            memcpy(keep, buf1, len);
            reload(m, off, off + len, "write");
            newsize = mf[m].size;
            if(w > 0 && (newsize < hi || memcmp(mf[m].data + off, keep, (u32)w))) {
                printf("    (op %d) write: the bytes written do not read back\n", op_no);
                b5_bad++;
            }
            for(j = hi; j < off + len && j < newsize; j++) {
                u8 was = j < old ? oldb[j] : 0;
                if(mf[m].data[j] != was) { if(!changed++) first = j; }
            }
            if(changed) {
                if(!wextra++ || fverbose) printf("    (op %d) write returned %d of %u, yet %u bytes past that changed (first at %u; size %u -> %u)\n",
                                                 op_no, w, len, changed, first, old, newsize);
            }
        }
    }
    return;
    w = v->ops->write(v, off, buf1, len);
    note("write '%s' off %u len %u: %d (size %u -> %u)", mf[m].name, off, len, w, old, v->size);
    vput(v);
    /* the model: what was written, then the rest checked against the old bytes */
    {
        u32 hi = w > 0 ? off + (u32)w : off, newsize;
        static u8 keep[MAXSZ + 1024];
        memcpy(keep, buf1, len);
        reload(m, off, hi, "write");
        newsize = mf[m].size;
        if(w > 0 && (newsize < hi || memcmp(mf[m].data + off, keep, (u32)w))) {
            printf("    (op %d) write: the bytes written do not read back\n", op_no);
            b5_bad++;
        }
    }
}

static void op_truncate(void) {
    int m = pick_file(), r;
    struct vnode *dv, *v;
    u32 ns, old;
    if(m < 0) return;
    dv = dir_vnode(mf[m].dir);
    if(!dv) return;
    v = lookup(dv, mf[m].name);
    vput(dv);
    if(!v) return;
    old = mf[m].size;
    ns = rn(3) == 0 ? 0 : rn(old + 3000);
    if(ns > MAXSZ) ns = MAXSZ;
    r = ARMED(v->ops->truncate(v, ns));
    note("truncate '%s' %u -> %u: %d (size now %u)", mf[m].name, old, ns, r, v->size);
    vput(v);
    if(r == 0) {
        mf[m].data = (u8 *)realloc(mf[m].data, ns + 1);
        if(ns > old) memset(mf[m].data + old, 0, ns - old);
        mf[m].size = ns;
        reload(m, 0, 0, "truncate");
    } else if(ns <= old) {
        if(unchanged(mf[m].dir, "failed truncate")) { b5_bad++; printf("    (op %d) a failed truncate to %u changed the file\n", op_no, ns); dump_last(); }
    } else reload(m, old, ns, "failed grow");
}

static void op_unlink(void) {
    int k = rn(MAXF), i, m = -1, r;
    struct vnode *dv;
    for(i = 0; i < MAXF; i++) { int j = (k + i) % MAXF; if(mf[j].used && (!mf[j].isdir || !children(j))) { m = j; break; } }
    if(m < 0) return;
    dv = dir_vnode(mf[m].dir);
    if(!dv) return;
    r = ARMED(dv->ops->unlink(dv, mf[m].name));
    note("%s '%s' in %d: %d", mf[m].isdir ? "rmdir" : "rm", mf[m].name, mf[m].dir, r);
    if(r == 0) { mf[m].used = 0; free(mf[m].data); mf[m].data = NULL; }
    else if(unchanged(mf[m].dir, "failed rm")) { b5_bad++; printf("    (op %d) a failed rm changed the directory\n", op_no); dump_last(); }
    vput(dv);
}

static void op_rename(void) {
    int k = rn(MAXF), i, m = -1, nd, d, r, bad;
    char name[100];
    struct vnode *odv, *ndv;
    for(i = 0; i < MAXF; i++) { int j = (k + i) % MAXF; if(mf[j].used) { m = j; break; } }
    if(m < 0) return;
    nd = mf[m].isdir ? -1 : (rn(2) ? -1 : (int)rn(MAXD) - 1);
    if(nd >= 0 && (!mf[nd].used || !mf[nd].isdir)) nd = -1;
    rand_name(name, sizeof(name));
    d = find_in(nd, name);
    if(d == m) return;
    if(d >= 0 && (mf[d].isdir != mf[m].isdir || (mf[d].isdir && children(d)))) return;
    odv = dir_vnode(mf[m].dir);
    ndv = dir_vnode(nd);
    if(!odv || !ndv) { if(odv) vput(odv); if(ndv) vput(ndv); return; }
    hist_add(mf[m].id, nd < 0 ? 0 : mf[nd].id, name);  /* (round fs-fix-6: before the new name can reach the disk) */
    r = ARMED(odv->ops->rename(odv, mf[m].name, ndv, name));
    note("mv '%s' (%d) -> '%s' (%d)%s: %d", mf[m].name, mf[m].dir, name, nd, d >= 0 ? " replacing" : "", r);
    if(r == 0) {
        if(d >= 0) { mf[d].used = 0; free(mf[d].data); mf[d].data = NULL; }
        mf[m].dir = nd;
        strlcpy(mf[m].name, name, sizeof(mf[m].name));
    } else {
        bad = unchanged(mf[m].dir, "failed mv (source)");
        if(nd != mf[m].dir) bad += unchanged(nd, "failed mv (target)");
        if(bad) { b5_bad++; printf("    (op %d) a failed mv changed a directory\n", op_no); dump_last(); }
    }
    vput(odv);
    vput(ndv);
}

/* the oracle for any FAT type: after every write the disk stores, no entry and no link on the way
   reaches a cluster the first FAT says is free (test_fatfs.c's oracle, which reads FAT16 only) */
static u32 gle16(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8); }
static u32 gle32(const u8 *p) { return gle16(p) | (gle16(p + 2) << 16); }
struct ggeo { u32 spc, fat_lba, spf, root_lba, root_secs, data_lba, nclusters, root_cluster; int type; };
static void ggeometry(const u8 *bs, struct ggeo *g) {
    u32 rsv = gle16(bs + 14), nfats = bs[16], re = gle16(bs + 17), tot = gle16(bs + 19) ? gle16(bs + 19) : gle32(bs + 32);
    g->spc = bs[13];
    g->spf = gle16(bs + 22) ? gle16(bs + 22) : gle32(bs + 36);
    g->fat_lba = rsv;
    g->root_lba = rsv + nfats * g->spf;
    g->root_secs = (re * 32 + 511) / 512;
    g->data_lba = g->root_lba + g->root_secs;
    g->nclusters = (tot - g->data_lba) / g->spc;
    g->type = g->nclusters < 4085 ? 12 : g->nclusters < 65525 ? 16 : 32;
    g->root_cluster = g->type == 32 ? gle32(bs + 44) : 0;
}
static u32 gfat(const u8 *mem, const struct ggeo *g, u32 c) {
    const u8 *f = mem + g->fat_lba * 512;
    if(g->type == 12) { u32 v = gle16(f + c + c / 2); return (c & 1) ? v >> 4 : v & 0xFFF; }
    if(g->type == 16) return gle16(f + c * 2);
    return gle32(f + c * 4) & 0x0FFFFFFF;
}
static int geoc(const struct ggeo *g, u32 v) { return g->type == 12 ? v >= 0xFF8 : g->type == 16 ? v >= 0xFFF8 : v >= 0x0FFFFFF8; }
static void gviol(struct ram *r, const char *m) {
    if(!g_viol++) snprintf(g_first, sizeof(g_first), "after the disk's write %u: %s", r->writes, m);
}
static void gstale(struct ram *r, const struct ggeo *g, const u8 *p, const char *name, int ni, int dni) {
    u32 size = gle32(p + 28), c = gle16(p + 26) | (g->type == 32 ? gle16(p + 20) << 16 : 0), steps = 0, off = 0, id = 0, s;
    char m[200];
    while(c >= 2 && c < g->nclusters + 2 && off < size && steps++ < g->nclusters) {
        u32 v;
        for(s = 0; s < g->spc && off < size; s++, off += 512) {
            const u8 *d = r->mem + (g->data_lba + (c - 2) * g->spc + s) * 512;
            u32 t;
            if(off + 8 > size || d[0] != TAG_MARK0 || d[1] != TAG_MARK1 || d[6] != 'T' || d[7] != 'G' ||
               (u8)(d[2] ^ d[4]) != 0xFF || (u8)(d[3] ^ d[5]) != 0xFF) continue;   /* (whole, and within the size: the
                                                                                  bytes past the end are anybody's) */
            t = gle16(d + 2);
            if(ni >= 0 && dni != -2) {                  /* round fs-fix-6: the exact check */
                exact_checked++;
                if(dni < 0 ? !hist_had(t, 0, ni) : !hist_had_in(t, ni, dni)) {
                    exact_viol++;
                    snprintf(m, sizeof(m), "entry '%s': its block at %u (cluster %u) holds file %u's bytes, a file that never had"
                             " that name there (stale data, exact check)", name, off, c, t);
                    gviol(r, m);
                    return;
                }
            }
            if(!id) id = t;
            else if(t != id) {
                snprintf(m, sizeof(m), "entry '%s': its block at %u holds file %u's bytes, its others file %u's (stale data)",
                         name, off, t, id);
                gviol(r, m);
                return;
            }
        }
        v = gfat(r->mem, g, c);
        if(!v || geoc(g, v)) break;
        c = v;
    }
}
static u8 lfn_sum(const u8 *n11) {
    u8 sum = 0;
    int i;
    for(i = 0; i < 11; i++) sum = (u8)(((sum & 1) << 7) + (sum >> 1) + n11[i]);
    return sum;
}
/* dni: the name index of the directory of the root the entries are in (-1: the root itself; -2: a name not known) */
static void gentries(struct ram *r, const struct ggeo *g, const u8 *p, u32 n, int depth, int dni);
static void gdir(struct ram *r, const struct ggeo *g, u32 first, int depth, int dni) {
    u32 c = first, steps = 0;
    if(depth > 4) return;
    while(c >= 2 && c < g->nclusters + 2 && steps++ < 65536) {
        u32 next = gfat(r->mem, g, c);
        char m[120];
        if(!next) { snprintf(m, sizeof(m), "a directory's chain reaches cluster %u, which is free", c); gviol(r, m); return; }
        gentries(r, g, r->mem + (g->data_lba + (c - 2) * g->spc) * 512, g->spc * 16, depth, dni);
        if(geoc(g, next)) return;
        c = next;
    }
}
static void gentries(struct ram *r, const struct ggeo *g, const u8 *p, u32 n, int depth, int dni) {
    u32 i;
    char lfn[300];
    int lfn_max = 0, lfn_ok = 0;
    u8 lfn_ck = 0;
    for(i = 0; i < n; i++, p += 32) {
        u32 first, c, steps = 0;
        char m[120], name[12], full[300];
        if(p[0] == 0) return;
        if(p[0] == 0xE5) { lfn_ok = 0; continue; }
        if(p[11] == 0x0F) {                          /* a long name's part: 13 UTF-16 characters (ASCII here) */
            static const int at[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
            int seq = p[0] & 0x1F, k;
            if(p[0] & 0x40) { lfn_ok = 1; lfn_max = seq * 13; lfn_ck = p[13]; memset(lfn, 0, sizeof(lfn)); }
            if(!lfn_ok || seq < 1 || seq * 13 > (int)sizeof(lfn) - 1 || p[13] != lfn_ck) { lfn_ok = 0; continue; }
            for(k = 0; k < 13; k++) {
                u16 ch = (u16)(p[at[k]] | (p[at[k] + 1] << 8));
                lfn[(seq - 1) * 13 + k] = ch == 0xFFFF ? 0 : (char)ch;
            }
            continue;
        }
        if(p[11] & 0x08) { lfn_ok = 0; continue; }
        if(p[0] == '.' && (p[1] == ' ' || p[1] == '.')) { lfn_ok = 0; continue; }
        memcpy(name, p, 11); name[11] = 0;
        if(lfn_ok && lfn_ck == lfn_sum(p)) { lfn[lfn_max < (int)sizeof(lfn) ? lfn_max : (int)sizeof(lfn) - 1] = 0; strlcpy(full, lfn, sizeof(full)); }
        else {                                       /* "F       TXT" -> "f.txt" */
            int k, j = 0;
            for(k = 0; k < 8 && p[k] != ' '; k++) full[j++] = (char)(p[k] >= 'A' && p[k] <= 'Z' ? p[k] + 32 : p[k]);
            if(p[8] != ' ') { full[j++] = '.'; for(k = 8; k < 11 && p[k] != ' '; k++) full[j++] = (char)(p[k] >= 'A' && p[k] <= 'Z' ? p[k] + 32 : p[k]); }
            full[j] = 0;
        }
        lfn_ok = 0;
        first = gle16(p + 26) | (g->type == 32 ? gle16(p + 20) << 16 : 0);
        for(c = first; c >= 2 && c < g->nclusters + 2 && steps++ < 1000000; ) {
            u32 v = gfat(r->mem, g, c);
            if(!v) { snprintf(m, sizeof(m), "entry '%s' -> cluster %u (step %u of its chain) is free", name, c, steps - 1); gviol(r, m); break; }
            if((p[11] & 0x10) || geoc(g, v)) break;
            c = v;
        }
        if((p[11] & 0x10) && first) {
            int sub = name_index(full);
            gdir(r, g, first, depth + 1, depth == 0 && sub >= 0 ? sub : -2);
        } else if(!(p[11] & 0x10)) gstale(r, g, p, full, name_index(full), dni);
    }
}
static void goracle(struct ram *r, u32 lba, u32 count) {
    struct ggeo g;
    (void)lba; (void)count;
    ggeometry(r->mem, &g);
    if(g.type == 32) gdir(r, &g, g.root_cluster, 0, -1);
    else gentries(r, &g, r->mem + g.root_lba * 512, g.root_secs * 16, 0, -1);
}

static void pressure(void) {
    u32 i, base = rn(3000);
    struct buf *b;
    for(i = 0; i < 300 + rn(600); i++) if((b = bread(&y->bd, base + i)) != NULL) brelse(b);
}

/* one run: seed, operations, unreadable blocks or not, the volume's blocks, FAT32 or not; 0 when it found nothing */
static int fuzz_one(u32 seed, u32 ops, int use_rbad, u32 blocks, int fat32) {
    int failing = 0, i;
    u32 clean_points = 0, refused_ops = 0;
    for(i = 0; i < MAXF; i++) { free(mf[i].data); memset(&mf[i], 0, sizeof(mf[i])); }
    g_viol = 0; g_first[0] = 0;
    hist_reset();                                /* (round fs-fix-6) */
    op_no = failures_seen = oracle_reported = b5_bad = audit_bad = img_bad = sync_bad = wextra = 0;
    nlast = 0;
    arm_rbad = 0;
    rng_state = seed;
    init_bcache();
    nmade = 0;
    x = ram_new("fz", blocks);                   /* 8192: FAT16, 512-byte clusters: 256 FAT entries a sector */
    y = ram_new("fzy", 4096);
    if(fat32) {
        char msg[256];
        if(fat_mkfs(&x->bd, "TEST", 32, msg, sizeof(msg)) != 0) { printf("mkfs: %s\n", msg); return 2; }
    } else mkfs(x);
    sb = mount(x);
    if(fverbose) {
        struct ggeo g;
        ggeometry(x->mem, &g);
        printf("volume: FAT%d, %u clusters of %u blocks\n", g.type, g.nclusters, g.spc);
    }
    x->hook = goracle;
    for(op_no = 0; op_no < (int)ops; op_no++) {
        u32 what = rn(100);
        /* the device: refusing for a few operations now and then, or one block range */
        if(!failing && rn(12) == 0) {
            failing = 1 + rn(5);
            if(rn(2)) { x->fail_all = 1; note("device: refuses every write"); }
            else { x->bad_lo = 2 + rn(400); x->bad_hi = x->bad_lo + rn(4); note("device: refuses blocks %u-%u", x->bad_lo, x->bad_hi); }
            failures_seen++;
        }
        if(use_rbad && rn(40) == 0) { arm_rbad = 1 + rn(300); note("device: block %u unreadable for one op", arm_rbad); }
        if(what < 18) op_create(0);
        else if(what < 22) op_create(1);
        else if(what < 50) op_write();
        else if(what < 62) op_truncate();
        else if(what < 74) op_unlink();
        else if(what < 88) op_rename();
        else if(what < 94) pressure();
        else { int s = bsync(&x->bd); note("sync: %d", s); }
        x->rbad = 0;
        arm_rbad = 0;
        check_audit();
        check_oracle();
        if(x->fail_all || x->bad_hi) refused_ops++;
        if(failing && --failing == 0) {
            x->fail_all = 0; x->bad_hi = 0;
            note("device: works again");
        }
        if(!failing && (rn(15) == 0 || op_no == (int)ops - 1)) {     /* a clean point */
            int s1 = sb->ops->sync(sb), s2 = bsync(&x->bd);
            note("clean point: syncs %d %d", s1, s2);
            check_oracle();
            if(s1 || s2) { if(!sync_bad++) { printf("    (op %d) sync with the device working: %d %d\n", op_no, s1, s2); dump_last(); } }
            else { image_check("image"); clean_points++; }
        }
    }
    printf("FUZZ seed %u: %u ops (%u while refusing), %d refusal episodes, %u clean points: oracle %u, B5 %d, audit %d, image %d, sync %d; short writes that changed more: %d; tags checked exactly %u (stale %u)\n",
           seed, ops, refused_ops, failures_seen, clean_points, g_viol, b5_bad, audit_bad, img_bad, sync_bad, wextra,
           exact_checked, exact_viol);
    x->hook = NULL;
    return (g_viol || b5_bad || audit_bad || img_bad || sync_bad || wextra) ? 1 : 0;
}

int main(int argc, char **argv) {
    /* make unittest's fixed set: FAT16 with 512-byte clusters (also with blocks that cannot be read for one
       operation), a bigger FAT16, FAT32 */
    static const struct { u32 seed, ops; int rbad; u32 blocks; int fat32; } set[] = {
        { 100, 3000, 0, 8192, 0 }, { 202, 3000, 1, 8192, 0 }, { 300, 800, 0, 40000, 0 }, { 400, 500, 0, 70000, 1 },
    };
    int bad = 0;
    u32 i;
    fverbose = getenv("FUZZ_VERBOSE") != NULL;
    fatfs_init();
    if(argc > 1)
        return fuzz_one((u32)atoi(argv[1]), argc > 2 ? (u32)atoi(argv[2]) : 2000, argc > 3 && atoi(argv[3]) != 0,
                        argc > 4 ? (u32)atoi(argv[4]) : 8192, argc > 5 && atoi(argv[5]) == 32);
    for(i = 0; i < sizeof(set) / sizeof(set[0]); i++)
        if(fuzz_one(set[i].seed, set[i].ops, set[i].rbad, set[i].blocks, set[i].fat32)) bad++;
    printf("FATFUZZ: %u runs, %d with failures\n", (u32)(sizeof(set) / sizeof(set[0])), bad);
    return bad ? 1 : 0;
}
