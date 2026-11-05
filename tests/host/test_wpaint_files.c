/* Host tests for Paint's files (user/wpaint/files.c, 11/05/26, the desktop applications, track paint), over libsurf's
   codecs (user/libsurf/image*.c, inflate, deflate, CRC-32) and the model, all built with the host tests' allocator
   (tests/host/undertow/alloc.c) so that what they hold is counted and any allocation can be made to fail; files.c
   with tests/host/wpaint_host.h, which makes its file calls fail on request. In a folder of the test's own (under
   $TMPDIR, else /tmp), removed at the end. They check: the kinds by name and the name Save as offers; transparency on
   white; canvases saved as BMP and PNG and opened again pixel for pixel, at sizes up to the largest; what floats and
   what is typed saved too; a file replaced through saveutil.h and one made with O_EXCL; every failure of open, read,
   write, fsync, close, rename and unlink, with the file as it was or gone again and no .tmp left, and a rename that
   may have half happened kept; short writes; the pair care (a twin beside the file, no free name); folders, missing
   folders, kinds Paint does not write; every picture of tests/fixtures/wundertow/images/ opened as the decoders read
   it, or refused with the canvas as it was; empty, short, unreadable and too large files; every allocation failing in
   an open and in a save, with nothing left held; the history and the clipboard across an open. Natively under
   AddressSanitizer and UndefinedBehaviorSanitizer on the Mac; built and run by `make unittest`. */
#define _FILE_OFFSET_BITS 64    /* readdir(): in the Docker image the 32-bit call fails at once (EOVERFLOW), as the */
                                /* folders' offsets there are 64-bit; the large-file one reads them. macOS ignores it */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <utime.h>
#include <sys/stat.h>
#include "paint.h"
#include "files.h"
#include "alloc.h"
#define WPH_NAMES_ONLY
#include "wpaint_host.h"

int wph_at[WPH_CALLS], wph_errno[WPH_CALLS], wph_count[WPH_CALLS], wph_short;

static int checks, failed;
#define CHECK(x) do { checks++; if(!(x)) { failed++; printf("FAIL %s line %d: %s (%s)\n", __FILE__, __LINE__, #x, why); } } while(0)

#define FIXTURES "tests/fixtures/wundertow/images"

static struct paint P, *p = &P;
static char dir[400], why[PF_WHY], kept[600];

static const char *at(const char *name) {
    static char path[4][600];
    static int k;
    k = (k + 1) % 4;
    snprintf(path[k], sizeof(path[k]), "%s/%s", dir, name);
    return path[k];
}

static void calm(void) {                                /* every file call works again */
    memset(wph_at, 0, sizeof(wph_at));
    memset(wph_errno, 0, sizeof(wph_errno));
    memset(wph_count, 0, sizeof(wph_count));
    wph_short = 0;
}

static void fail_at(int op, int n, int e) {
    calm();
    wph_at[op] = n;
    wph_errno[op] = e;
}

static int exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static long file_size(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 ? (long)st.st_size : -1;
}

/* a file of these bytes, made here (not through files.c) */
static void make(const char *path, const void *data, size_t n) {
    FILE *f = fopen(path, "wb");
    if(n) fwrite(data, 1, n, f);
    fclose(f);
}

static unsigned char *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    unsigned char *b;
    long len;
    if(!f) return NULL;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = (unsigned char *)malloc(len > 0 ? (size_t)len : 1);
    *n = fread(b, 1, (size_t)len, f);
    fclose(f);
    return b;
}

static int same_file(const char *path, const unsigned char *data, size_t n) {
    size_t m;
    unsigned char *b = slurp(path, &m);
    int r = b && m == n && !memcmp(b, data, n);
    free(b);
    return r;
}

/* no new file beside path: no .tmp, no .N.tmp */
static int no_tmp(const char *path) {
    char t[700];
    int k;
    for(k = 0; k < 10; k++) {
        snprintf(t, sizeof(t), k ? "%s.%d.tmp" : "%s.tmp", path, k);
        if(exists(t)) return 0;
    }
    return 1;
}

/* the model holds what it says, and the allocator agrees */
static int counted(void) {
    return p->mem <= p->mem_max;
}

/* a canvas of w x h with something on every row: a gradient, and noise in a corner */
static void picture(int w, int h) {
    int x, y;
    unsigned s = 99;
    paint_free(p);
    CHECK(paint_init(p, w, h) == 0);
    for(y = 0; y < h; y++)
        for(x = 0; x < w; x++) {
            s = s * 1103515245u + 12345u;
            p->px[(size_t)y * w + x] = x < 40 && y < 40 ? (s >> 8) & 0xFFFFFF : (unsigned)(x * 255 / w) << 16 |
                                       (unsigned)(y * 255 / h) << 8 | (unsigned)((x + y) & 0xFF);
        }
}

static unsigned *copy_canvas(void) {
    unsigned *c = (unsigned *)malloc((size_t)p->w * p->h * 4);
    memcpy(c, p->px, (size_t)p->w * p->h * 4);
    return c;
}

/**** names and colours ****/

static void names(void) {
    char out[64];
    CHECK(pf_kind("a.bmp") == PF_BMP && pf_kind("/x/A.BMP") == PF_BMP && pf_kind("b.Png") == PF_PNG);
    CHECK(!pf_kind("c.jpg") && !pf_kind("d.gif") && !pf_kind("bmp") && !pf_kind("e.png.txt") && !pf_kind(""));
    pf_save_name("/d/photo.jpg", out, sizeof(out));
    CHECK(!strcmp(out, "/d/photo.png"));
    pf_save_name("/d/photo", out, sizeof(out));
    CHECK(!strcmp(out, "/d/photo.png"));
    pf_save_name("/d.x/photo", out, sizeof(out));
    CHECK(!strcmp(out, "/d.x/photo.png"));
    pf_save_name("/d/.hidden", out, sizeof(out));
    CHECK(!strcmp(out, "/d/.hidden.png"));
    pf_save_name("/d/keep.BMP", out, sizeof(out));
    CHECK(!strcmp(out, "/d/keep.BMP"));
    pf_save_name("/d/a-long-name.jpeg", out, 12);
    CHECK(strlen(out) == 11 && !strcmp(out + 7, ".png"));
}

static void flatten(void) {
    unsigned px[6] = { 0x00123456, 0xFF000000, 0xFF123456, 0x80000000, 0x80FFFFFF, 0x01FF0000 };
    pf_flatten(px, 6);
    CHECK(px[0] == 0x123456 && px[1] == 0xFFFFFF && px[2] == 0xFFFFFF);
    CHECK(px[3] == 0x808080 && px[4] == 0xFFFFFF && px[5] == 0xFF0101);
}

/**** saving and opening again ****/

static void round_trips(void) {
    static const int sizes[][2] = { { 1, 1 }, { 3, 2 }, { 33, 17 }, { 640, 400 }, { 4096, 3 }, { 1, 300 } };
    unsigned *want;
    int i, k, ok;
    long n;
    for(i = 0; i < (int)(sizeof(sizes) / sizeof(sizes[0])); i++)
        for(k = 0; k < 2; k++) {
            const char *f = at(k ? "round.png" : "round.bmp");
            picture(sizes[i][0], sizes[i][1]);
            want = copy_canvas();
            unlink(f);
            paint_set_tool(p, PAINT_PENCIL);
            paint_press(p, 0, 0, PAINT_LEFT, 0);            /* modified, so that saving can say otherwise */
            paint_release(p, 0, 0, 0);
            want[0] = 0;
            CHECK(paint_modified(p));
            CHECK(pf_save(p, f, why, sizeof(why), kept, sizeof(kept)) == 0 && !paint_modified(p) && no_tmp(f));
            n = file_size(f);
            if(!k) CHECK(n == 54 + (long)((sizes[i][0] * 3 + 3) / 4 * 4) * sizes[i][1]);
            paint_new(p, 5, 5);
            CHECK(pf_open(p, f, why, sizeof(why)) == 0 && p->w == sizes[i][0] && p->h == sizes[i][1]);
            ok = !memcmp(p->px, want, (size_t)p->w * p->h * 4);
            CHECK(ok && !paint_modified(p) && !paint_can_undo(p) && counted());
            if(!ok) printf("  %d x %d as %s differs\n", sizes[i][0], sizes[i][1], k ? "PNG" : "BMP");
            free(want);
        }
    /* what floats and what is being typed are saved, put down first */
    paint_new(p, 50, 50);
    paint_set_tool(p, PAINT_RECT);
    paint_set_filled(p, 1);
    paint_press(p, 0, 0, PAINT_LEFT, 0);
    paint_release(p, 9, 9, 0);
    paint_set_tool(p, PAINT_SELECT);
    paint_press(p, 0, 0, PAINT_LEFT, 0);
    paint_release(p, 9, 9, 0);
    paint_press(p, 5, 5, PAINT_LEFT, 0);
    paint_release(p, 25, 5, 0);                         /* floats at 20, 0 */
    CHECK(p->sel.buf && pf_save(p, at("float.png"), why, sizeof(why), kept, sizeof(kept)) == 0 && !p->sel.buf);
    paint_new(p, 5, 5);
    CHECK(pf_open(p, at("float.png"), why, sizeof(why)) == 0 && p->px[20] == 0 && p->px[0] == 0xFFFFFF);
    paint_set_tool(p, PAINT_TEXT);
    paint_press(p, 0, 0, PAINT_LEFT, 0);
    paint_key(p, 'W', 0);
    CHECK(paint_typing(p) && pf_save(p, at("float.png"), why, sizeof(why), kept, sizeof(kept)) == 0 && !paint_typing(p));
    paint_new(p, 5, 5);
    CHECK(pf_open(p, at("float.png"), why, sizeof(why)) == 0 && p->w == 50);
    for(ok = 0, i = 0; i < 32 * 16; i++) ok += p->px[i / 16 * 50 + i % 16] == 0;     /* the W, twice as large */
    CHECK(ok > 30);
    /* the largest canvas: 4000 x 4000, saved as PNG and as BMP, and opened again */
    paint_free(p);
    CHECK(paint_init(p, 4000, 4000) == 0);
    p->px[3999 * 4000 + 3999] = 0x123456;
    CHECK(pf_save(p, at("large.png"), why, sizeof(why), kept, sizeof(kept)) == 0);
    CHECK(pf_save(p, at("large.bmp"), why, sizeof(why), kept, sizeof(kept)) == 0 && file_size(at("large.bmp")) == 48000054);
    paint_new(p, 5, 5);
    CHECK(pf_open(p, at("large.png"), why, sizeof(why)) == 0 && p->w == 4000 && p->px[3999 * 4000 + 3999] == 0x123456);
    paint_new(p, 5, 5);
    CHECK(pf_open(p, at("large.bmp"), why, sizeof(why)) == 0 && p->h == 4000 && p->px[3999 * 4000 + 3999] == 0x123456);
    CHECK(counted());
    unlink(at("large.png"));
    unlink(at("large.bmp"));
    paint_new(p, 10, 10);
}

/**** replacing, making, and failing ****/

static void saving(void) {
    unsigned char old[] = "the user's file, as it was";
    char f[600], g[600];
    int n, r, writes;
    snprintf(f, sizeof(f), "%s", at("pic.png"));       /* at()'s own buffers are used again by the next calls */
    snprintf(g, sizeof(g), "%s", at("new.bmp"));
    picture(64, 48);
    /* a kind Paint does not write; a folder of that name; a folder that is not there */
    CHECK(pf_save(p, at("pic.jpg"), why, sizeof(why), kept, sizeof(kept)) < 0 && strstr(why, ".bmp or .png"));
    CHECK(!exists(at("pic.jpg")));
    mkdir(at("folder.png"), 0777);
    CHECK(pf_save(p, at("folder.png"), why, sizeof(why), kept, sizeof(kept)) < 0 && strstr(why, "folder"));
    rmdir(at("folder.png"));
    CHECK(pf_save(p, at("none/x.png"), why, sizeof(why), kept, sizeof(kept)) < 0 && strstr(why, "No such file"));
    /* replaced through saveutil.h: the old file until the rename */
    make(f, old, sizeof(old));
    calm();
    CHECK(pf_save(p, f, why, sizeof(why), kept, sizeof(kept)) == 0 && file_size(f) > 100 && no_tmp(f));
    CHECK(wph_count[WPH_RENAME] == 1 && wph_count[WPH_FSYNC] == 1);
    writes = wph_count[WPH_WRITE];
    /* write failing at each of its calls: the old file as it was, no .tmp */
    for(n = 1; n <= writes + 1; n++) {
        make(f, old, sizeof(old));
        fail_at(WPH_WRITE, n, ENOSPC);
        r = pf_save(p, f, why, sizeof(why), kept, sizeof(kept));
        if(n <= writes) CHECK(r < 0 && strstr(why, "full") && same_file(f, old, sizeof(old)) && no_tmp(f) && !kept[0]);
        else CHECK(r == 0);
    }
    /* a file made with O_EXCL, nothing renamed; write failing at each of its calls: gone again */
    unlink(g);
    calm();
    CHECK(pf_save(p, g, why, sizeof(why), kept, sizeof(kept)) == 0 && exists(g));
    writes = wph_count[WPH_WRITE];
    CHECK(writes >= 1 && wph_count[WPH_RENAME] == 0 && wph_count[WPH_FSYNC] == 1);
    for(n = 1; n <= writes + 1; n++) {
        unlink(g);
        fail_at(WPH_WRITE, n, EIO);
        r = pf_save(p, g, why, sizeof(why), kept, sizeof(kept));
        if(n <= writes) CHECK(r < 0 && !exists(g) && strstr(why, "Input/output"));
        else CHECK(r == 0 && exists(g));
    }
    /* writes of a few bytes at a time, and a write of nothing */
    calm();
    wph_short = 7;
    unlink(g);
    CHECK(pf_save(p, g, why, sizeof(why), kept, sizeof(kept)) == 0 && file_size(g) == 54 + 64 * 3 * 48);
    CHECK(wph_count[WPH_WRITE] > 1000);               /* 9,270 bytes, 7 at a time */
    fail_at(WPH_WRITE, 1, 0);                           /* a write of nothing: an error, or it would never end */
    CHECK(pf_save(p, g, why, sizeof(why), kept, sizeof(kept)) < 0 && exists(g) && no_tmp(g) && strstr(why, "Input"));
    /* fsync, close: the same */
    make(f, old, sizeof(old));
    fail_at(WPH_FSYNC, 1, EIO);
    CHECK(pf_save(p, f, why, sizeof(why), kept, sizeof(kept)) < 0 && same_file(f, old, sizeof(old)) && no_tmp(f));
    fail_at(WPH_CLOSE, 1, EIO);
    CHECK(pf_save(p, f, why, sizeof(why), kept, sizeof(kept)) < 0 && same_file(f, old, sizeof(old)) && no_tmp(f));
    unlink(g);
    fail_at(WPH_FSYNC, 1, EROFS);
    CHECK(pf_save(p, g, why, sizeof(why), kept, sizeof(kept)) < 0 && !exists(g) && strstr(why, "read-only"));
    fail_at(WPH_CLOSE, 1, EIO);
    CHECK(pf_save(p, g, why, sizeof(why), kept, sizeof(kept)) < 0 && !exists(g));
    /* the file cannot be made at all */
    fail_at(WPH_OPEN, 1, EROFS);
    CHECK(pf_save(p, g, why, sizeof(why), kept, sizeof(kept)) < 0 && !exists(g) && strstr(why, "read-only"));
    fail_at(WPH_OPEN, 1, EACCES);
    CHECK(pf_save(p, f, why, sizeof(why), kept, sizeof(kept)) < 0 && same_file(f, old, sizeof(old)) && no_tmp(f));
    /* a rename that changed nothing: the new file goes, the old one stays */
    fail_at(WPH_RENAME, 1, ENOSPC);
    CHECK(pf_save(p, f, why, sizeof(why), kept, sizeof(kept)) < 0 && same_file(f, old, sizeof(old)) && no_tmp(f));
    CHECK(!kept[0] && strstr(why, "full"));
    /* one that may have half happened: the new file is kept, and named */
    fail_at(WPH_RENAME, 1, EIO);
    CHECK(pf_save(p, f, why, sizeof(why), kept, sizeof(kept)) == PF_KEPT && same_file(f, old, sizeof(old)));
    CHECK(!strcmp(kept, at("pic.png.tmp")) && exists(kept) && strstr(why, "pic.png.tmp") && strstr(why, "checked"));
    /* that pair is now the desktop's care: the next save over it is refused, and both are left alone */
    calm();
    {
        struct stat a;
        struct utimbuf u;
        size_t n2;
        unsigned char *b = slurp(kept, &n2);
        make(f, b, n2);                                 /* the twin: the same size, and the same time */
        free(b);
        stat(kept, &a);
        u.actime = a.st_atime;
        u.modtime = a.st_mtime;
        utime(f, &u);
    }
    CHECK(pf_save(p, f, why, sizeof(why), kept, sizeof(kept)) < 0 && strstr(why, "pic.png.tmp") && strstr(why, "fsck"));
    CHECK(exists(at("pic.png.tmp")) && wph_count[WPH_RENAME] == 0);
    unlink(at("pic.png.tmp"));
    /* no name free beside it: all ten are the user's */
    for(n = 0; n < 10; n++) {
        char t[700];
        snprintf(t, sizeof(t), n ? "%s.%d.tmp" : "%s.tmp", f, n);
        make(t, old, (size_t)n + 1);
    }
    make(f, old, sizeof(old));
    CHECK(pf_save(p, f, why, sizeof(why), kept, sizeof(kept)) < 0 && strstr(why, "No name is free") &&
          same_file(f, old, sizeof(old)));
    for(n = 0; n < 10; n++) {
        char t[700];
        snprintf(t, sizeof(t), n ? "%s.%d.tmp" : "%s.tmp", f, n);
        unlink(t);
    }
    /* a rename that changed nothing and a .tmp that will not go: kept, as saveutil.h says */
    fail_at(WPH_RENAME, 1, ENOSPC);
    wph_at[WPH_UNLINK] = 1;
    wph_errno[WPH_UNLINK] = EIO;
    CHECK(pf_save(p, f, why, sizeof(why), kept, sizeof(kept)) == PF_KEPT && exists(at("pic.png.tmp")));
    calm();
    unlink(at("pic.png.tmp"));
    unlink(f);
    unlink(g);
}

/**** opening ****/

/* the file at path, whose bytes are data, opened as the decoders read them: 1 for a picture, 0 refused */
static int as_decoded(const char *path, const unsigned char *data, size_t n, const unsigned *before) {
    struct wimg m;
    char err[WIMG_ERR_MAX];
    int r = wimg_decode(data, n, &m, err, sizeof(err)), ok;
    paint_free(p);
    picture(30, 20);
    if(r < 0) {
        CHECK(pf_open(p, path, why, sizeof(why)) < 0 && strstr(why, err) && p->w == 30 &&
              !memcmp(before, p->px, 30 * 20 * 4));
        return 0;
    }
    pf_flatten(m.px, (size_t)m.w * (size_t)m.h);
    ok = pf_open(p, path, why, sizeof(why)) == 0 && p->w == m.w && p->h == m.h &&
         !memcmp(p->px, m.px, (size_t)m.w * (size_t)m.h * 4);
    CHECK(ok);
    if(!ok) printf("  %s: %s\n", path, why);
    wimg_free(&m);
    return 1;
}

static void opening(void) {
    struct dirent *e;
    DIR *d;
    unsigned char *data;
    unsigned *before;
    size_t n;
    char path[600];
    int pictures = 0, refused = 0, ok, r;
    picture(30, 20);
    before = copy_canvas();
    /* not there, a folder, empty, cut short, unreadable, too large, not a picture: refused, the canvas as it was */
    CHECK(pf_open(p, at("missing.png"), why, sizeof(why)) < 0 && strstr(why, "No such file"));
    CHECK(pf_open(p, dir, why, sizeof(why)) < 0);
    make(at("empty.png"), "", 0);
    CHECK(pf_open(p, at("empty.png"), why, sizeof(why)) < 0 && strstr(why, "empty"));
    make(at("text.png"), "not a picture at all", 20);
    CHECK(pf_open(p, at("text.png"), why, sizeof(why)) < 0 && strstr(why, "Not read: not a picture"));
    fail_at(WPH_READ, 2, 0);
    {
        size_t len;
        unsigned char *b = slurp(FIXTURES "/rgb8.png", &len);
        make(at("short.png"), b, len);
        free(b);
    }
    wph_short = 0;
    CHECK(pf_open(p, at("short.png"), why, sizeof(why)) == 0);  /* read at once: the second call never came */
    paint_free(p);
    picture(30, 20);
    fail_at(WPH_READ, 1, 0);
    CHECK(pf_open(p, at("short.png"), why, sizeof(why)) < 0 && strstr(why, "to its end"));
    fail_at(WPH_READ, 1, EIO);
    CHECK(pf_open(p, at("short.png"), why, sizeof(why)) < 0 && strstr(why, "Input/output"));
    fail_at(WPH_OPEN, 1, EACCES);
    CHECK(pf_open(p, at("short.png"), why, sizeof(why)) < 0 && strstr(why, "ermission"));
    calm();
    make(at("big.bmp"), "BM", 2);
    CHECK(truncate(at("big.bmp"), (off_t)PF_FILE_MAX + 1) == 0);
    CHECK(pf_open(p, at("big.bmp"), why, sizeof(why)) < 0 && strstr(why, "72 MB"));
    CHECK(!memcmp(before, p->px, 30 * 20 * 4) && p->w == 30 && counted());
    unlink(at("big.bmp"));
    /* every picture of the decoders' fixtures, whole and cut in half: as they read it, on white; or refused as they
       refuse it, the canvas as it was */
    d = opendir(FIXTURES);
    CHECK(d != NULL);
    while(d && (e = readdir(d))) {
        if(e->d_name[0] == '.' || !strcmp(e->d_name, "manifest")) continue;
        snprintf(path, sizeof(path), "%s/%s", FIXTURES, e->d_name);
        data = slurp(path, &n);
        if(as_decoded(path, data, n, before)) pictures++;
        else refused++;
        snprintf(path, sizeof(path), "%s", at("cut"));
        make(path, data, n / 2);
        if(as_decoded(path, data, n / 2, before)) pictures++;
        else refused++;
        free(data);
    }
    if(d) closedir(d);
    CHECK(pictures >= 125 && refused >= 100);
    printf("test_wpaint_files: %d pictures opened, %d refused as the decoders refuse them\n", pictures, refused);
    /* an opened picture's transparency is on white, the history starts again, the clipboard stays */
    paint_free(p);
    picture(40, 40);
    paint_select_all(p);
    paint_copy(p);
    paint_set_tool(p, PAINT_PENCIL);
    paint_press(p, 1, 1, PAINT_LEFT, 0);
    paint_release(p, 1, 1, 0);
    CHECK(paint_can_undo(p) && p->clip);
    snprintf(path, sizeof(path), "%s/rgba8.png", FIXTURES);
    CHECK(pf_open(p, path, why, sizeof(why)) == 0 && !paint_can_undo(p) && !paint_modified(p) && p->clip);
    for(ok = 1, r = 0; r < p->w * p->h; r++) ok = ok && p->px[r] <= 0xFFFFFF;
    CHECK(ok && paint_paste(p) && p->sel.w == 40);
    free(before);
}

/**** no memory ****/

static void memory(void) {
    char path[600];
    long live, n;
    int r, opened = 0, saved = 0;
    unsigned *want;
    snprintf(path, sizeof(path), "%s/base420.jpg", FIXTURES);
    /* every allocation of an open failing in turn: refused, nothing held, until it opens */
    for(n = 0; n < 200 && !opened; n++) {
        paint_free(p);
        picture(30, 20);
        live = ut_alloc_live;
        ut_alloc_fail_after = n;
        r = pf_open(p, path, why, sizeof(why));
        ut_alloc_fail_after = -1;
        if(r == 0) opened = 1;
        else CHECK(ut_alloc_live == live && p->w == 30 && why[0]);
    }
    CHECK(opened && n > 3);
    /* the model refuses the picture: the decoder's pixels go again */
    paint_free(p);
    picture(30, 20);
    live = ut_alloc_live;
    p->mem_max = p->mem + 1000;
    CHECK(pf_open(p, path, why, sizeof(why)) < 0 && strstr(why, "Not enough memory") && ut_alloc_live == live);
    p->mem_max = PAINT_MEM_MAX;
    /* every allocation of a save failing in turn: the old file as it was, no .tmp, nothing held; then saved */
    picture(200, 100);
    want = copy_canvas();
    for(n = 0; n < 200 && !saved; n++) {
        make(at("mem.png"), "old", 3);
        live = ut_alloc_live;
        ut_alloc_fail_after = n;
        r = pf_save(p, at("mem.png"), why, sizeof(why), kept, sizeof(kept));
        ut_alloc_fail_after = -1;
        if(r == 0) saved = 1;
        else CHECK(ut_alloc_live == live && same_file(at("mem.png"), (const unsigned char *)"old", 3) &&
                   no_tmp(at("mem.png")) &&
                   strstr(why, "Not saved"));
    }
    CHECK(saved && n > 1);
    paint_new(p, 5, 5);
    CHECK(pf_open(p, at("mem.png"), why, sizeof(why)) == 0 && !memcmp(p->px, want, 200 * 100 * 4));
    free(want);
    unlink(at("mem.png"));
}

static void tidy(void) {
    struct dirent *e;
    DIR *d = opendir(dir);
    while(d && (e = readdir(d))) {
        if(e->d_name[0] == '.' && (!e->d_name[1] || (e->d_name[1] == '.' && !e->d_name[2]))) continue;
        unlink(at(e->d_name));
    }
    if(d) closedir(d);
    rmdir(dir);
}

int main(void) {
    const char *tmp = getenv("TMPDIR");
    snprintf(dir, sizeof(dir), "%s/wpaint.XXXXXX", tmp && tmp[0] ? tmp : "/tmp");
    if(!mkdtemp(dir)) {
        printf("test_wpaint_files: no folder of its own: %s\n", strerror(errno));
        return 1;
    }
    calm();
    names();
    flatten();
    round_trips();
    saving();
    opening();
    memory();
    paint_free(p);
    CHECK(p->mem == 0 && ut_alloc_live == 0);
    tidy();
    CHECK(!exists(dir));
    printf("test_wpaint_files: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
