/* Host tests for the Image Viewer, Hex and Sketch's BMP files (track MEDIA of docs/superpowers/plans/2026-10-17-gui-suite.md),
   against the in-memory file system and system of tests/host/gui_fake.c. Built and run by `make unittest` (unittest-gui).
   The BMP files are made here: each kind that is read, each that is refused, and hostile headers. A file system of
   its own (big_fs) adds files that are made as they are read: a 4,000,000,000-byte one for Hex and a 16384 x 16384
   32-bit picture (1 GB) for the Image Viewer. */
#include "gui_fake.h"
#include "../../shell/gui_media.h"

static struct gui_state g;
static struct fb_surface screen;
static u8 file[1200 * 1024];

static void key(int k) { gui_key(&g, k); }

/**** making BMP files ****/

#define TL 0xE04030                     /* the quadrants' colours: none is the desktop's */
#define TR 0x30C060
#define BL 0x3050E0
#define BR 0xF0D040
static u32 qw, qh;
static u32 quad(u32 x, u32 y) { return y < qh / 2 ? (x < qw / 2 ? TL : TR) : (x < qw / 2 ? BL : BR); }

static void put16(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void put32(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }
static u32 get32(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }

/* a BMP: a header of hsize bytes, w x h (h < 0: top-down), bpp bits, comp, colors in the palette (0: none);
   pix(x, y) (y from the top) is the colour, or the palette's index at 8 bits and less; its length */
static u32 bmp(u8 *out, u32 hsize, i32 w, i32 h, u32 bpp, u32 comp, u32 colors, const u32 *pal, u32 (*pix)(u32, u32)) {
    u32 ah = h < 0 ? (u32)-h : (u32)h, stride = ((u32)w * bpp + 31) / 32 * 4, masks = comp == 3 && hsize == 40 ? 12 : 0;
    u32 off = 14 + hsize + masks + colors * 4, r, x, y, c, i;
    u8 *row;
    memset(out, 0, off + stride * ah);
    out[0] = 'B'; out[1] = 'M';
    put32(out + 2, off + stride * ah);
    put32(out + 10, off);
    put32(out + 14, hsize);
    put32(out + 18, (u32)w);
    put32(out + 22, (u32)h);
    put16(out + 26, 1);
    put16(out + 28, bpp);
    put32(out + 30, comp);
    put32(out + 34, stride * ah);
    put32(out + 46, colors);
    if(comp == 3) { put32(out + 54, 0xFF0000); put32(out + 58, 0xFF00); put32(out + 62, 0xFF); }
    for(i = 0; i < colors; i++) put32(out + 14 + hsize + masks + i * 4, pal[i]);
    for(r = 0; r < ah; r++) {
        row = out + off + r * stride;
        y = h < 0 ? r : ah - 1 - r;
        for(x = 0; x < (u32)w; x++) {
            c = pix(x, y);
            switch(bpp) {
            case 32: row[x * 4] = (u8)c; row[x * 4 + 1] = (u8)(c >> 8); row[x * 4 + 2] = (u8)(c >> 16); row[x * 4 + 3] = 0xFF; break;
            case 24: row[x * 3] = (u8)c; row[x * 3 + 1] = (u8)(c >> 8); row[x * 3 + 2] = (u8)(c >> 16); break;
            case 8: row[x] = (u8)c; break;
            case 4: row[x / 2] |= (u8)(c << (x & 1 ? 0 : 4)); break;
            case 1: row[x / 8] |= (u8)(c << (7 - x % 8)); break;
            }
        }
    }
    return off + stride * ah;
}

static const u32 four[4] = { TL, TR, BL, BR };
static u32 quad_index(u32 x, u32 y) { return y < qh / 2 ? (x < qw / 2 ? 0 : 1) : (x < qw / 2 ? 2 : 3); }
static u32 bits(u32 x, u32 y) { return (x + y) & 1; }
static u32 gray256[256];
static u32 ramp(u32 x, u32 y) { return (x / 5 + y) & 255; }

/* the colour bmp_parse and bmp_pixel give for pixel x, y (from the top) of a file in memory */
static u32 pixel_of(const u8 *f, u32 len, u32 x, u32 y) {
    struct gui_bmp b;
    u32 pal[256];
    char why[80];
    if(bmp_parse(f, len, len, &b, pal, why, sizeof(why))) return 0xDEAD;
    return bmp_pixel(&b, pal, f + b.offset + (b.topdown ? y : b.height - 1 - y) * b.stride, x);
}

static bool refused(const u8 *f, u32 n, u32 size, const char *word) {
    struct gui_bmp b;
    u32 pal[256];
    char why[80] = "";
    bool ok = bmp_parse(f, n, size, &b, pal, why, sizeof(why)) == -EINVAL && strstr(why, word) != NULL;
    if(!ok) printf("  (refusal %s: \"%s\")\n", word, why);
    return ok;
}

/**** a file system with big files made as they are read ****/

#define BIG_SIZE 4000000000u
#define NEEDLE_AT 3000000u
static const char needle[] = "NEEDLE!!";
#define HUGE_SIDE 16384
#define HUGE_SIZE (54u + 65536u * HUGE_SIDE)
#define MAX_SIZE 0xFFFFFFFFu             /* the largest file there can be: offsets up to 0xFFFFFFFE */
#define SHOT_W 1920
#define SHOT_H 1080
#define SHOT_SIZE (54u + SHOT_W * 3u * SHOT_H)  /* 6,220,854 bytes: a screenshot, under Sketch's 8 MB */
static u32 big_reads, big_bytes;                /* read_at's calls and bytes, of every file */
/* FAT's walk of a made file's cluster chain (4 KB clusters), as the kernel pays it since gui/suite 3f77dfe: the file
   stays open through a frame's reads (walk_reset() is the frame's end), and a read goes on from where the last one left
   off in the chain when it lies further on, from the start otherwise: where the walk is, the clusters walked, the
   furthest reached */
#define CLUSTER 4096
static u32 walk_at, walk_steps, walk_far;
static void walk_reset(void) { walk_at = walk_steps = walk_far = 0; }
static int fail_after = -1;                     /* read_at fails (EIO) once this many more calls were made; -1: never */
static const char *device = "";                 /* a path that stat says is a device */

static u8 big_byte(u32 off) {
    if(off >= NEEDLE_AT && off < NEEDLE_AT + 8) return (u8)needle[off - NEEDLE_AT];
    return (u8)((off * 2654435761u) >> 24) | 0x80;      /* never ASCII: the needle is only where it was put */
}

/* a made picture's 54 bytes of headers: 40-byte BITMAPINFOHEADER, bottom-up */
static u8 head_byte(u32 off, u32 size, u32 w, u32 h, u32 bpp) {
    u8 head[54];
    memset(head, 0, sizeof(head));
    head[0] = 'B'; head[1] = 'M';
    put32(head + 2, size); put32(head + 10, 54); put32(head + 14, 40);
    put32(head + 18, w); put32(head + 22, h); put16(head + 26, 1); put16(head + 28, bpp);
    return head[off];
}

/* the huge picture: 32-bit rows bottom-up; pixel x, y (from the top) is x/64, y/64, 0x40 */
static u8 huge_byte(u32 off) {
    u32 r, c, x, y;
    if(off < 54) return head_byte(off, HUGE_SIZE, HUGE_SIDE, HUGE_SIDE, 32);
    r = (off - 54) / 65536; c = (off - 54) % 65536;
    x = c / 4; y = HUGE_SIDE - 1 - r;
    switch(c % 4) {
    case 0: return 0x40;
    case 1: return (u8)(y / 64);
    case 2: return (u8)(x / 64);
    }
    return 0;
}

/* a 1920 x 1080 24-bit screenshot, every pixel 0x404040 */
static u8 shot_byte(u32 off) {
    return off < 54 ? head_byte(off, SHOT_SIZE, SHOT_W, SHOT_H, 24) : 0x40;
}

/* a made picture: 54 bytes of headers, then its rows bottom-up, pixel x, y (from the top) pic(x, y) */
static u32 pic(u32 x, u32 y) { return ((x * 2654435761u) ^ (y * 40503u)) >> 8 & 0xFFFFFF; }
static u8 pic_byte(u32 off, u32 w, u32 h, u32 bpp) {
    u32 stride = (w * bpp + 31) / 32 * 4, r, c;
    if(off < 54) return head_byte(off, 54 + stride * h, w, h, bpp);
    r = (off - 54) / stride;
    c = (off - 54) % stride;
    return c / (bpp / 8) < w ? (u8)(pic(c / (bpp / 8), h - 1 - r) >> (8 * (c % (bpp / 8)))) : 0;
}
#define PHOTO_SIZE (54u + 12000u * 3000)        /* 4000 x 3000 x 24 */
#define NARROW_SIZE (54u + 2048u * 16384)       /* 682 x 16384 x 24: rows of 2,046 bytes and 2 of padding */
#define TALL_SIZE (54u + 4096u * 16384)         /* 1024 x 16384 x 32 */
static u8 photo_byte(u32 off) { return pic_byte(off, 4000, 3000, 24); }
static u8 tall_byte(u32 off) { return pic_byte(off, 1024, 16384, 32); }
static u8 narrow_byte(u32 off) { return pic_byte(off, 682, 16384, 24); }
static u32 edge_size;                           /* a 2048 x 1000 24-bit picture (6 MB), then bytes to make it edge_size */
static u8 edge_byte(u32 off) { return off < 54 ? head_byte(off, edge_size, 2048, 1000, 24) : 0x40; }

static bool made(const char *path, u32 *size, u8 (**byte)(u32)) {
    if(!strcmp(path, "/big.bin")) { *size = BIG_SIZE; *byte = big_byte; return true; }
    if(!strcmp(path, "/max.bin")) { *size = MAX_SIZE; *byte = big_byte; return true; }
    if(!strcmp(path, "/huge.bmp")) { *size = HUGE_SIZE; *byte = huge_byte; return true; }
    if(!strcmp(path, "/shot.bmp")) { *size = SHOT_SIZE; *byte = shot_byte; return true; }
    if(!strcmp(path, "/photo.bmp")) { *size = PHOTO_SIZE; *byte = photo_byte; return true; }
    if(!strcmp(path, "/tall.bmp")) { *size = TALL_SIZE; *byte = tall_byte; return true; }
    if(!strcmp(path, "/narrow.bmp")) { *size = NARROW_SIZE; *byte = narrow_byte; return true; }
    if(!strcmp(path, "/edge.bmp")) { *size = edge_size; *byte = edge_byte; return true; }
    return false;
}

static int big_stat(void *ctx, const char *path, struct gui_entry *out, bool *dev, bool *readonly) {
    u32 size;
    u8 (*byte)(u32);
    int r;
    if(!made(path, &size, &byte)) {
        r = fake_fs.stat(ctx, path, out, dev, readonly);
        if(!r && !strcmp(path, device)) *dev = true;
        return r;
    }
    memset(out, 0, sizeof(*out));
    strlcpy(out->name, gui_basename(path), sizeof(out->name));
    out->type = GUI_FILE;
    out->size = size;
    *dev = false;
    *readonly = true;
    return 0;
}

static int big_read_at(void *ctx, const char *path, u32 off, char *buf, u32 n, u32 *len, u32 *size) {
    u8 (*byte)(u32);
    u32 i;
    int r;
    big_reads++;
    if(fail_after == 0) { *len = *size = 0; return -EIO; }
    if(fail_after > 0) fail_after--;
    if(!made(path, size, &byte)) {
        r = fake_fs.read_at(ctx, path, off, buf, n, len, size);
        big_bytes += *len;
        return r;
    }
    *len = 0;
    if(off >= *size) return 0;
    if(n > *size - off) n = *size - off;
    for(i = 0; i < n; i++) buf[i] = (char)byte(off + i);
    *len = n;
    big_bytes += n;
    if(n) {                                             /* from the place remembered, or else from the start */
        walk_steps += (off + n - 1) / CLUSTER - (walk_at <= off / CLUSTER ? walk_at : 0);
        walk_at = (off + n - 1) / CLUSTER;
        if(walk_at > walk_far) walk_far = walk_at;
    }
    return 0;
}

static struct gui_fs big_fs;

static void desktop(int width, int height) {
    fake_desktop(&g, width, height);
    fail_after = -1;
    device = "";
    big_fs = fake_fs;
    big_fs.stat = big_stat;
    big_fs.read_at = big_read_at;
    g.fs = &big_fs;
    fake_screen(&screen, width, height);
}

/* the chooser in front: choose the entry called name (a folder goes into it, a file answers) */
static bool choose(const char *name) {
    int i;
    for(i = 0; i < g.dialog.count; i++) {
        if(strcmp(g.dialog.list[i].name, name)) continue;
        g.dialog.selected = i + (strcmp(g.dialog.dir, "/") != 0);
        g.dialog.list_focus = true;
        key('\r');
        return true;
    }
    printf("  (no %s in %s)\n", name, g.dialog.dir);
    return false;
}

static void ticks(int n) { while(n-- > 0) { g.now += 16; gui_tick(&g); } }

/**** BMP files ****/

static void bmp_reading(void) {
    struct gui_bmp b;
    u32 pal[256], n, i, dw, dh;
    char why[80];
    qw = 42; qh = 30;
    /* 24 bits, bottom-up, rows padded (42 * 3 = 126 bytes, 128 with the padding) */
    n = bmp(file, 40, 42, 30, 24, 0, 0, NULL, quad);
    CHECK(bmp_parse(file, n, n, &b, pal, why, sizeof(why)) == 0);
    CHECK(b.width == 42 && b.height == 30 && b.bpp == 24 && !b.topdown && b.stride == 128 && b.offset == 54);
    CHECK(pixel_of(file, n, 0, 0) == TL && pixel_of(file, n, 41, 0) == TR && pixel_of(file, n, 0, 29) == BL &&
          pixel_of(file, n, 41, 29) == BR && pixel_of(file, n, 20, 14) == TL && pixel_of(file, n, 21, 15) == BR);
    /* 32 bits top-down; 32-bit BI_BITFIELDS after a 40-byte header (masks at 54), in a V4 one, in a V5 one */
    n = bmp(file, 40, 42, -30, 32, 0, 0, NULL, quad);
    CHECK(bmp_parse(file, n, n, &b, pal, why, sizeof(why)) == 0 && b.topdown && b.height == 30 && b.stride == 168);
    CHECK(pixel_of(file, n, 0, 0) == TL && pixel_of(file, n, 41, 29) == BR && pixel_of(file, n, 0, 29) == BL);
    n = bmp(file, 40, 42, 30, 32, 3, 0, NULL, quad);
    CHECK(bmp_parse(file, n, n, &b, pal, why, sizeof(why)) == 0 && b.offset == 66 && pixel_of(file, n, 41, 0) == TR);
    n = bmp(file, 108, 42, 30, 32, 3, 0, NULL, quad);
    CHECK(bmp_parse(file, n, n, &b, pal, why, sizeof(why)) == 0 && b.offset == 122 && pixel_of(file, n, 0, 29) == BL);
    n = bmp(file, 124, 42, -30, 24, 0, 0, NULL, quad);
    CHECK(bmp_parse(file, n, n, &b, pal, why, sizeof(why)) == 0 && b.offset == 138 && pixel_of(file, n, 41, 29) == BR);
    n = bmp(file, 52, 42, 30, 24, 0, 0, NULL, quad);
    CHECK(bmp_parse(file, n, n, &b, pal, why, sizeof(why)) == 0 && pixel_of(file, n, 41, 0) == TR);
    n = bmp(file, 56, 42, 30, 32, 3, 0, NULL, quad);
    CHECK(bmp_parse(file, n, n, &b, pal, why, sizeof(why)) == 0 && pixel_of(file, n, 0, 0) == TL);
    /* palettes: 8 bits with 4 colours (and an index past them, which is black), 256 grays, 4 bits, 1 bit, odd widths */
    qw = 13; qh = 7;
    n = bmp(file, 40, 13, 7, 8, 0, 4, four, quad_index);
    CHECK(bmp_parse(file, n, n, &b, pal, why, sizeof(why)) == 0 && b.colors == 4 && b.offset == 54 + 16 && b.stride == 16);
    CHECK(pixel_of(file, n, 0, 0) == TL && pixel_of(file, n, 12, 0) == TR && pixel_of(file, n, 0, 6) == BL &&
          pixel_of(file, n, 12, 6) == BR);
    file[54 + 16] = 200;                                /* the bottom row's first pixel: no such colour */
    CHECK(pixel_of(file, n, 0, 6) == 0);
    for(i = 0; i < 256; i++) gray256[i] = i * 0x010101;
    n = bmp(file, 40, 300, -3, 8, 0, 256, gray256, ramp);
    put32(file + 46, 0);                                /* 0 colours: as many as 8 bits can use (all there) */
    CHECK(bmp_parse(file, n, n, &b, pal, why, sizeof(why)) == 0 && b.colors == 256);
    CHECK(pixel_of(file, n, 299, 2) == ((299 / 5 + 2) & 255) * 0x010101 && pixel_of(file, n, 7, 1) == 2 * 0x010101);
    n = bmp(file, 40, 13, 7, 4, 0, 4, four, quad_index);
    CHECK(bmp_parse(file, n, n, &b, pal, why, sizeof(why)) == 0 && b.stride == 8);
    CHECK(pixel_of(file, n, 0, 0) == TL && pixel_of(file, n, 12, 0) == TR && pixel_of(file, n, 5, 6) == BL &&
          pixel_of(file, n, 6, 6) == BR);
    n = bmp(file, 40, 13, -7, 1, 0, 2, four, bits);
    CHECK(bmp_parse(file, n, n, &b, pal, why, sizeof(why)) == 0 && b.stride == 4 && b.topdown);
    CHECK(pixel_of(file, n, 0, 0) == TL && pixel_of(file, n, 1, 0) == TR && pixel_of(file, n, 12, 6) == TL &&
          pixel_of(file, n, 11, 6) == TR);
    /* a palette that says it has more colours than its pixels can use: those they can */
    n = bmp(file, 40, 13, 7, 1, 0, 2, four, bits);
    put32(file + 46, 0xFFFFFFFF);
    CHECK(bmp_parse(file, n, n, &b, pal, why, sizeof(why)) == 0 && b.colors == 2);
    /* the size a picture is kept at */
    bmp_reduce(1000, 1000, GUI_VIEW_PIXELS, &dw, &dh); CHECK(dw == 1000 && dh == 1000);
    bmp_reduce(16384, 16384, GUI_VIEW_PIXELS, &dw, &dh); CHECK(dw == 1024 && dh == 1024);
    bmp_reduce(16384, 1, GUI_VIEW_PIXELS, &dw, &dh); CHECK(dw == 16384 && dh == 1);
    bmp_reduce(1, 16384, GUI_VIEW_PIXELS, &dw, &dh); CHECK(dw == 1 && dh == 16384);
    bmp_reduce(2048, 1024, GUI_VIEW_PIXELS, &dw, &dh); CHECK(dw * dh <= GUI_VIEW_PIXELS && dh == 724 && dw == 1448);
    bmp_reduce(16384, 100, GUI_VIEW_PIXELS, &dw, &dh);
    CHECK(dw * dh <= GUI_VIEW_PIXELS && 16384 * (dh + 1) / 100 * (dh + 1) > GUI_VIEW_PIXELS);
    bmp_reduce(1100, 1000, GUI_VIEW_PIXELS, &dw, &dh); CHECK(dw * dh <= GUI_VIEW_PIXELS && dw < 1100 && dh < 1000);
}

static void bmp_refusing(void) {
    u32 n;
    u8 head[64];
    qw = 42; qh = 30;
    memset(head, 0, sizeof(head));
    memcpy(head, "\x89PNG\r\n\x1a\n", 8); CHECK(refused(head, 64, 64, "PNG picture"));
    memcpy(head, "\xFF\xD8\xFF\xE0", 4); CHECK(refused(head, 64, 64, "JPEG picture"));
    memcpy(head, "GIF89a", 6); CHECK(refused(head, 64, 64, "GIF"));
    memcpy(head, "BA", 2); CHECK(refused(head, 64, 64, "OS/2"));
    memcpy(head, "XY", 2); CHECK(refused(head, 64, 64, "start with BM"));
    CHECK(refused(head, 0, 0, "start with BM"));         /* an empty file */
    n = bmp(file, 40, 42, 30, 24, 0, 0, NULL, quad);
    put32(file + 14, 12); CHECK(refused(file, n, n, "12-byte"));
    put32(file + 14, 64); CHECK(refused(file, n, n, "64-byte"));
    put32(file + 14, 100); CHECK(refused(file, n, n, "header of 100"));
    put32(file + 14, 40);
    CHECK(refused(file, 30, 30, "inside its headers"));  /* cut inside the header */
    CHECK(refused(file, 16, 16, "inside its headers"));
    put32(file + 30, 1); CHECK(refused(file, n, n, "RLE"));
    put32(file + 30, 2); CHECK(refused(file, n, n, "RLE"));
    put32(file + 30, 4); CHECK(refused(file, n, n, "JPEG inside"));
    put32(file + 30, 5); CHECK(refused(file, n, n, "PNG inside"));
    put32(file + 30, 7); CHECK(refused(file, n, n, "Compression 7"));
    put32(file + 30, 3); CHECK(refused(file, n, n, "masks for 24-bit"));
    put32(file + 30, 0);
    put16(file + 28, 16); CHECK(refused(file, n, n, "16-bit"));
    put16(file + 28, 2); CHECK(refused(file, n, n, "2-bit"));
    put16(file + 28, 24);
    put16(file + 26, 2); CHECK(refused(file, n, n, "planes"));
    put16(file + 26, 1);
    put32(file + 18, 0); CHECK(refused(file, n, n, "not the size"));
    put32(file + 18, (u32)-5); CHECK(refused(file, n, n, "not the size"));
    put32(file + 18, 42);
    put32(file + 22, 0); CHECK(refused(file, n, n, "not the size"));
    put32(file + 22, 30);
    n = bmp(file, 40, 42, 30, 32, 3, 0, NULL, quad);
    put32(file + 54, 0xFF000000); CHECK(refused(file, n, n, "not the usual"));
    /* hostile sizes and offsets: too big, beyond the end, inside the headers, the pixels cut short */
    n = bmp(file, 40, 42, 30, 24, 0, 0, NULL, quad);
    put32(file + 18, 16385); CHECK(refused(file, n, n, "at most"));
    put32(file + 18, 42);
    put32(file + 22, (u32)-16385); CHECK(refused(file, n, n, "at most"));
    put32(file + 22, 0x80000000); CHECK(refused(file, n, n, "at most"));   /* INT_MIN: no negation overflows */
    put32(file + 22, 30);
    put32(file + 18, 16384); put32(file + 22, 16384); put16(file + 28, 32);   /* 1 GB of pixels in a small file */
    CHECK(refused(file, n, n, "cut short"));
    put32(file + 18, 42); put32(file + 22, 30); put16(file + 28, 24);
    put32(file + 10, n); CHECK(refused(file, n, n, "cut short"));
    put32(file + 10, 0xFFFFFFF0); CHECK(refused(file, n, n, "cut short"));
    put32(file + 10, 10); CHECK(refused(file, n, n, "inside its headers"));
    put32(file + 10, 54);
    CHECK(refused(file, n - 1, n - 1, "cut short"));     /* the last row's padding missing */
    CHECK(refused(file, 54, n - 1, "cut short"));        /* only the headers read: the size is the file's */
    n = bmp(file, 40, 13, 7, 8, 0, 4, four, quad_index);
    CHECK(refused(file, 60, 60, "palette"));             /* the palette runs past the end */
    put32(file + 46, 0);                                 /* 256 colours, then: they are not there */
    CHECK(refused(file, n, n, "palette"));
}

/**** the Image Viewer ****/

static u32 at(int x, int y) { return fake_pixel(g.width, x, y); }

/* text s drawn at x, y in fg: each glyph's lit pixels fg, and none of the others */
static bool shows(int x, int y, const char *s, u32 fg) {
    int i, row, col;
    for(i = 0; s[i]; i++) for(row = 0; row < 16; row++) for(col = 0; col < 8; col++)
        if((((font8x16[(u8)s[i]][row] >> (7 - col)) & 1) != 0) != (at(x + i * 8 + col, y + row) == fg)) return false;
    return true;
}

static void viewer(void) {
    struct gui_viewer *v = &g.viewer;
    struct gui_window *w;
    int x, y, pw, ph, i, n, k;
    u32 sx, sy, c;
    struct pointer_event e;
    desktop(960, 540);
    qw = 42; qh = 30;
    n = (int)bmp(file, 40, 42, 30, 24, 0, 0, NULL, quad);
    fake_file("/home/q.bmp", (const char *)file, (u32)n);
    memcpy(file, "\x89PNG\r\n\x1a\n", 8);
    fake_file("/home/q.png", (const char *)file, 64);
    gui_act(&g, ACT_WINDOW + GUI_VIEWER);
    w = &g.win[GUI_VIEWER];
    CHECK(g.focus == GUI_VIEWER && !v->ready && w->x == 168 && w->y == 110 && w->w == 560 && w->h == 400);
    /* the chooser (O), then the picture: a small one is there at once */
    key('o');
    CHECK(g.dialog.kind == GUI_DLG_OPEN && g.dialog.act == gui_app_act(GUI_VIEWER, 1) && !strcmp(g.dialog.dir, "/"));
    CHECK(choose("home") && choose("q.bmp"));
    CHECK(!g.dialog.kind && v->ready && !v->loading && v->w == 42 && v->h == 30 && v->dw == 42 && v->dh == 30);
    CHECK(!strcmp(v->path, "/home/q.bmp"));
    /* Fit: enlarged by the largest whole number that fits the room (558 x 318): 10 times, centred */
    viewer_place(&g, &x, &y, &pw, &ph);
    CHECK(viewer_zoom(&g) == 10 * 65536 && pw == 420 && ph == 300 && x == 168 + 1 + 69 && y == 110 + 59 + 9);
    gui_render(&g, &screen);
    CHECK(fake_guards(960, 540));
    CHECK(at(x + 5, y + 5) == TL && at(x + pw - 5, y + 5) == TR && at(x + 5, y + ph - 5) == BL && at(x + pw - 5, y + ph - 5) == BR);
    CHECK(at(x + 209, y + 149) == TL && at(x + 210, y + 149) == TR && at(x + 209, y + 150) == BL && at(x + 210, y + 150) == BR);
    CHECK(at(x - 1, y + 10) == NAVY && at(x + 10, y - 1) == NAVY && at(x + pw, y + 10) == NAVY && at(x + 10, y + ph) == NAVY);
    /* the actual size (1), centred; zoom steps (+ and -) about the middle */
    key('1');
    viewer_place(&g, &x, &y, &pw, &ph);
    CHECK(!v->fit && viewer_zoom(&g) == 65536 && pw == 42 && ph == 30 && x == 169 + 258 && y == 169 + 144);
    gui_render(&g, &screen);
    CHECK(at(x, y) == TL && at(x + 41, y) == TR && at(x, y + 29) == BL && at(x + 41, y + 29) == BR && at(x - 1, y) == NAVY);
    CHECK(at(x + 20, y + 14) == TL && at(x + 21, y + 14) == TR && at(x + 21, y + 15) == BR);
    key('+'); CHECK(viewer_zoom(&g) == 65536 * 3 / 2);
    key('+'); CHECK(viewer_zoom(&g) == 2 * 65536);
    key('-'); CHECK(viewer_zoom(&g) == 65536 * 3 / 2);
    key('-'); key('-'); CHECK(viewer_zoom(&g) == 65536 * 2 / 3);
    for(i = 0; i < 30; i++) key('-');
    CHECK(viewer_zoom(&g) == 65536 / 64);
    viewer_place(&g, &x, &y, &pw, &ph); CHECK(pw == 1 && ph == 1);   /* 42 / 64: never less than a pixel */
    gui_render(&g, &screen); CHECK(fake_guards(960, 540));
    for(i = 0; i < 30; i++) key('+');
    CHECK(viewer_zoom(&g) == 16 * 65536);
    /* 16 times: 672 x 480 in a room of 558 x 318; the middle stays where it was */
    key('f'); CHECK(v->fit && viewer_zoom(&g) == 10 * 65536);
    key('+'); CHECK(viewer_zoom(&g) == 12 * 65536);        /* 504 x 360: taller than the room, centred across */
    CHECK(v->pan_x == 0 && v->pan_y == 21);
    key('+'); CHECK(viewer_zoom(&g) == 16 * 65536 && v->pan_x == 57 && v->pan_y == 81);
    gui_render(&g, &screen);
    viewer_place(&g, &x, &y, &pw, &ph);
    CHECK(pw == 672 && ph == 480 && x == 169 - 57 && y == 169 - 81);
    CHECK(at(169, 169) == TL && at(169 + 557, 169) == TR && at(169, 169 + 317) == BL && at(169 + 557, 169 + 317) == BR);
    CHECK(at(168, 200) != TL && fake_guards(960, 540));   /* the window's border, not the picture */
    /* moving it: the arrows, Home, End, Page Up and Down, the wheel, a drag */
    key(GUI_HOME); CHECK(v->pan_x == 0 && v->pan_y == 0);
    key(GUI_LEFT); key(GUI_UP); CHECK(v->pan_x == 0 && v->pan_y == 0);
    key(GUI_RIGHT); key(GUI_DOWN); CHECK(v->pan_x == 32 && v->pan_y == 32);
    key(GUI_END); CHECK(v->pan_x == 672 - 558 && v->pan_y == 480 - 318);
    key(GUI_RIGHT); CHECK(v->pan_x == 114);
    key(GUI_PGUP); CHECK(v->pan_y == 0);
    key(GUI_PGDN); CHECK(v->pan_y == 162);
    gui_render(&g, &screen);
    CHECK(at(169 + 557, 169 + 317) == BR && at(169, 169) == TL && fake_guards(960, 540));
    memset(&e, 0, sizeof(e));
    e.x = w->x + 300; e.y = w->y + 200; e.wheel = 1;     /* the wheel away from the user: up */
    gui_pointer(&g, &e); CHECK(v->pan_y == 162 - 48);
    e.wheel = 0; e.buttons = POINTER_LEFT; gui_pointer(&g, &e);
    e.x += 40; e.y += 30; gui_pointer(&g, &e); CHECK(v->pan_x == 114 - 40 && v->pan_y == 114 - 30);
    e.x += 1000; e.y += 1000; gui_pointer(&g, &e); CHECK(v->pan_x == 0 && v->pan_y == 0);
    e.buttons = 0; gui_pointer(&g, &e); CHECK(!v->dragging);
    /* a double-click switches between Fit and the actual size */
    g.now += 1000;
    fake_click(&g, w->x + 300, w->y + 200); g.now += 100; fake_click(&g, w->x + 301, w->y + 200);
    CHECK(v->fit);
    g.now += 1000;
    fake_click(&g, w->x + 300, w->y + 200); g.now += 100; fake_click(&g, w->x + 300, w->y + 202);
    CHECK(!v->fit && viewer_zoom(&g) == 65536);
    g.now += 1000;
    /* the toolbar: Open, Fit, Actual size, Zoom in, Zoom out (at 8, 58, 100, 206, 280 from the window's left) */
    fake_click(&g, w->x + 70, w->y + 40); CHECK(v->fit);
    fake_click(&g, w->x + 220, w->y + 40); CHECK(!v->fit && viewer_zoom(&g) == 12 * 65536);
    fake_click(&g, w->x + 300, w->y + 40); CHECK(viewer_zoom(&g) == 8 * 65536);
    fake_click(&g, w->x + 120, w->y + 40); CHECK(viewer_zoom(&g) == 65536);
    fake_click(&g, w->x + 20, w->y + 40); CHECK(g.dialog.kind == GUI_DLG_OPEN && !strcmp(g.dialog.dir, "/home"));
    key(27);
    /* Fit follows the window: maximized, the room is 958 x 404 */
    key('f');
    gui_act(&g, ACT_MAXIMIZE); CHECK(w->max && viewer_zoom(&g) == 13 * 65536);
    gui_render(&g, &screen); CHECK(fake_guards(960, 540));
    viewer_place(&g, &x, &y, &pw, &ph); CHECK(at(x + 3, y + 3) == TL && at(x + pw - 3, y + ph - 3) == BR);
    gui_act(&g, ACT_MAXIMIZE); CHECK(!w->max);
    CHECK(gui_app_viewer.title_of != NULL);
    {
        char t[64];
        gui_app_viewer.title_of(&g, t, sizeof(t));
        CHECK(!strcmp(t, "q.bmp - Image Viewer"));
    }
    /* a file that is not a BMP: said, and the picture shown stays */
    viewer_show(&g, "/home/q.png");
    CHECK(g.dialog.kind == GUI_DLG_MESSAGE && strstr(g.dialog.text[1], "PNG") && v->ready && !strcmp(v->path, "/home/q.bmp"));
    key(27);
    viewer_show(&g, "/home/none.bmp"); CHECK(g.dialog.kind == GUI_DLG_MESSAGE && v->ready); key(27);
    viewer_show(&g, "/home"); CHECK(g.dialog.kind == GUI_DLG_MESSAGE && v->ready); key(27);
    /* 1,100,000 pixels of 8 bits: kept at fewer, read a piece at a time in the tick */
    n = (int)bmp(file, 40, 1100, 1000, 8, 0, 256, gray256, ramp);
    fake_file("/home/big.bmp", (const char *)file, (u32)n);
    viewer_show(&g, "/home/big.bmp");
    CHECK(v->loading && !v->ready && strstr(v->status, "Reading big.bmp") && v->dw * v->dh <= GUI_VIEW_PIXELS);
    CHECK(v->dw < 1100 && v->dh < 1000 && v->next > 0 && v->next < v->dh);
    gui_render(&g, &screen); CHECK(fake_guards(960, 540));
    for(k = 0; k < 100 && v->loading; k++) ticks(1);
    CHECK(v->ready && k >= 3 && k < 10 && !v->status[0]);
    for(i = 0; i < 2000; i += 97) {                      /* each kept pixel is the one in the middle of what it stands for */
        u32 dx = (u32)i % v->dw, dy = (u32)i * 7 % v->dh;
        sx = (2 * dx + 1) * 1100 / (2 * v->dw);
        sy = (2 * dy + 1) * 1000 / (2 * v->dh);
        c = gray256[ramp(sx, sy)];
        CHECK(v->pixels[(dy * v->dw + dx) * 3] == (u8)(c >> 16) && v->pixels[(dy * v->dw + dx) * 3 + 2] == (u8)c);
    }
    gui_render(&g, &screen); CHECK(fake_guards(960, 540));
    /* Esc stops a reading; a file cut short while it is read is said */
    viewer_show(&g, "/home/big.bmp"); CHECK(v->loading);
    key(27); CHECK(!v->loading && !v->ready && !v->path[0] && strstr(v->status, "Stopped") && g.win[GUI_VIEWER].open);
    viewer_show(&g, "/home/big.bmp"); CHECK(v->loading);
    fake_file("/home/big.bmp", (const char *)file, (u32)n / 2);
    ticks(10);                                          /* the tick says so in the window, never in a dialog */
    CHECK(!v->loading && !v->ready && !g.dialog.kind && strstr(v->status, "big.bmp could not be read"));
    gui_render(&g, &screen);
    CHECK(shows(w->x + 17, w->y + 59 + 16, "big.bmp could not be read", CORAL) &&
          shows(w->x + 17, w->y + 59 + 38, "Open a BMP picture", FOAM));
    /* a picture of 16384 x 16384 x 32 bits (1 GB): kept at 1024 x 1024, a frame reading at most 256 KB (four rows),
       the opening handler one */
    viewer_show(&g, "/huge.bmp");
    CHECK(v->loading && v->dw == 1024 && v->dh == 1024);
    for(k = 0; k < 1000 && v->loading; k++) {
        big_bytes = 0;
        ticks(1);
        if(big_bytes > 256 * 1024 + 65536) break;
    }
    CHECK(v->ready && k == 256 && big_bytes <= 256 * 1024);
    for(i = 0; i < 1024; i += 61) {
        sx = (2 * (u32)i + 1) * 16384 / 2048;
        sy = (2 * (u32)(1023 - i) + 1) * 16384 / 2048;
        CHECK(v->pixels[((1023 - i) * 1024 + i) * 3] == sx / 64 && v->pixels[((1023 - i) * 1024 + i) * 3 + 1] == sy / 64 &&
              v->pixels[((1023 - i) * 1024 + i) * 3 + 2] == 0x40);
    }
    gui_render(&g, &screen); CHECK(fake_guards(960, 540));
    /* closing the window forgets the picture */
    gui_close(&g, GUI_VIEWER); CHECK(!v->ready && !v->path[0]);
}

/* no files at all: the windows say so, and nothing opens */
static void no_files(void) {
    gui_init(&g, 640, 400);
    fake_screen(&screen, 640, 400);
    gui_act(&g, ACT_WINDOW + GUI_VIEWER); key('o'); key('1'); key('+'); key(GUI_LEFT);
    CHECK(!g.dialog.kind && !g.viewer.ready);
    gui_render(&g, &screen); CHECK(fake_guards(640, 400));
    gui_act(&g, ACT_WINDOW + GUI_HEX); key('o'); key(GUI_DOWN); key(6); key(12); key(3);
    CHECK(!g.dialog.kind && !g.hex.path[0] && !g.quit);
    gui_render(&g, &screen); CHECK(fake_guards(640, 400));
    hex_show(&g, "/x"); viewer_show(&g, "/x.bmp");
    CHECK(!g.hex.path[0] && !g.viewer.ready && g.dialog.kind == GUI_DLG_MESSAGE);
    key(27);
    key(4); key(19); CHECK(g.focus == GUI_SKETCH && !g.dialog.kind);
    fake_click(&g, g.win[GUI_SKETCH].x + 230, g.win[GUI_SKETCH].y + 50); CHECK(!g.dialog.kind);
    fake_click(&g, g.win[GUI_SKETCH].x + 290, g.win[GUI_SKETCH].y + 50); CHECK(!g.dialog.kind);
    CHECK(!gui_menu_on(&g, 2, ACT_SKETCH_SAVE) && !gui_menu_on(&g, 2, ACT_SKETCH_OPEN));
    gui_render(&g, &screen); CHECK(fake_guards(640, 400));
}

/**** Hex ****/

static u8 data_byte(u32 i) { return (u8)(i * 7 + 3); }

static void hex_find(const char *what) {
    key(6);                                             /* Ctrl+F: the Edit menu's Find */
    CHECK(g.dialog.kind == GUI_DLG_INPUT && g.dialog.act == gui_app_act(GUI_HEX, 3));
    field_set(&g.dialog.field, what, false);
    key('\r');
}

static void hex_goto(const char *where) {
    key(12);                                            /* Ctrl+L */
    CHECK(g.dialog.kind == GUI_DLG_INPUT && g.dialog.act == gui_app_act(GUI_HEX, 2));
    field_set(&g.dialog.field, where, false);
    key('\r');
}

static void hex(void) {
    struct gui_hex *h = &g.hex;
    struct gui_window *w;
    struct pointer_event e;
    static u8 data[200000];
    u32 i, len;
    int k, x, y;
    const char *d;
    desktop(960, 540);
    for(i = 0; i < 1000; i++) data[i] = data_byte(i);
    memcpy(data + 300, "\xDE\xAD\xBE\xEF", 4);
    memcpy(data + 700, "\xDE\xAD\xBE\xEF", 4);
    memcpy(data + 500, "SurfOS", 6);
    memcpy(data + 600, "DE AD", 5);
    fake_file("/home/data.bin", (const char *)data, 1000);
    gui_act(&g, ACT_WINDOW + GUI_HEX);
    w = &g.win[GUI_HEX];
    CHECK(g.focus == GUI_HEX && w->x == 192 && w->y == 124 && w->w == 620 && w->h == 380);
    CHECK(gui_menu_on(&g, 3, ACT_FIND) == false);       /* no file yet */
    fake_click(&g, w->x + 20, w->y + 40);               /* the toolbar's Open */
    CHECK(g.dialog.kind == GUI_DLG_OPEN && g.dialog.act == gui_app_act(GUI_HEX, 1));
    CHECK(choose("home") && choose("data.bin"));
    CHECK(!strcmp(h->path, "/home/data.bin") && h->size == 1000 && h->page_ok && h->page_off == 0 && h->page_len == 1000);
    CHECK(gui_menu_on(&g, 3, ACT_FIND) && gui_menu_on(&g, 3, ACT_GOTO) && gui_menu_on(&g, 3, ACT_COPY) &&
          !gui_menu_on(&g, 3, ACT_FIND_NEXT) && !gui_menu_on(&g, 3, ACT_PASTE) && !gui_menu_on(&g, 3, ACT_UNDO));
    gui_render(&g, &screen); CHECK(fake_guards(960, 540));
    /* the first row on the screen: 00000000, then 03 0A 11 ... at a pitch of 22 (the window is 620 wide) */
    {
        const u8 *zero = font8x16['0'], *three = font8x16['3'];
        int row, col, okz = 1, ok3 = 1;
        for(row = 0; row < 16; row++) for(col = 0; col < 8; col++) {
            if(((zero[row] >> (7 - col)) & 1) != (at(w->x + 10 + col, w->y + 77 + row) != 0xFFFDF5)) okz = 0;
            if(((three[row] >> (7 - col)) & 1) != (at(w->x + 90 + 8 + col, w->y + 77 + row) != 0xFFFDF5 &&
                                                    at(w->x + 90 + 8 + col, w->y + 77 + row) != TEAL)) ok3 = 0;
        }
        CHECK(okz);
        CHECK(ok3 || at(w->x + 90 + 8, w->y + 77) == TEAL);   /* the cursor's byte is teal while Hex is in front */
    }
    /* moving: 17 rows are shown (the room under the columns' numbers is 280 pixels) */
    key(GUI_DOWN); CHECK(h->cursor == 16 && h->top == 0);
    key(GUI_RIGHT); CHECK(h->cursor == 17);
    key(GUI_LEFT); key(GUI_LEFT); key(GUI_LEFT); CHECK(h->cursor == 14);
    key(GUI_END); CHECK(h->cursor == 999 && h->top == (63 - 17) * 16);
    key(GUI_DOWN); key(GUI_RIGHT); CHECK(h->cursor == 999);
    key(GUI_HOME); CHECK(h->cursor == 0 && h->top == 0);
    key(GUI_UP); key(GUI_LEFT); CHECK(h->cursor == 0);
    key(GUI_PGDN); CHECK(h->cursor == 272 && h->top == 272);
    key(GUI_PGDN); key(GUI_PGDN); CHECK(h->top == (63 - 17) * 16 && h->cursor == 816);
    key(GUI_PGUP); CHECK(h->cursor == 544 && h->top == 464);
    /* Go to: hexadecimal with 0x, or decimal; not past the end */
    hex_goto("0x100"); CHECK(!g.dialog.kind && h->cursor == 256 && h->top == 256);
    hex_goto("300"); CHECK(h->cursor == 300);
    hex_goto("0x3E8"); CHECK(h->cursor == 300 && strstr(h->status, "past the end"));
    hex_goto("12ab"); CHECK(h->cursor == 300 && strstr(h->status, "Not an offset"));
    hex_goto("0x"); CHECK(h->cursor == 300 && strstr(h->status, "Not an offset"));
    hex_goto("99999999999"); CHECK(h->cursor == 300 && strstr(h->status, "Not an offset"));
    hex_goto(" 999 "); CHECK(h->cursor == 999);
    hex_goto("0"); CHECK(h->cursor == 0 && h->top == 0);
    /* Find: bytes in hexadecimal, from the cursor on; Find next; then the end */
    hex_find("DE AD BE EF");
    CHECK(!h->searching && h->found && h->found_at == 300 && h->anchor == 300 && h->cursor == 303 && strstr(h->status, "Found"));
    CHECK(gui_menu_on(&g, 3, ACT_FIND_NEXT));
    key(3); CHECK(!g.quit && g.clip_len == 11 && !memcmp(g.clip, "DE AD BE EF", 11));   /* Ctrl+C copies here */
    key('n'); CHECK(h->found_at == 700 && h->cursor == 703);
    gui_act(&g, ACT_FIND_NEXT); CHECK(h->anchor == 700 && h->cursor == 703 && strstr(h->status, "Not found from 0x000002BD"));
    key('n'); CHECK(h->anchor == 700 && h->cursor == 703 && strstr(h->status, "Not found from 0x000002BD"));   /* not 700 again */
    hex_goto("0"); hex_find("0xDEADBEEF"); CHECK(h->found_at == 300);
    hex_find("SurfOS"); CHECK(h->found_at == 500 && h->cursor == 505);
    hex_goto("0"); hex_find("\"DE AD\""); CHECK(h->found_at == 600);
    hex_goto("0"); hex_find("DE AD"); CHECK(h->found_at == 300);
    hex_find("0xABC"); CHECK(strstr(h->status, "pairs of digits") && h->plen == 0);
    {
        char longer[200];
        memset(longer, 'A', 129); longer[129] = 0;
        hex_find(longer); CHECK(strstr(h->status, "at most") && h->plen == 0);
    }
    /* the Edit menu's Select all, and copying too much */
    gui_act(&g, ACT_SELECT_ALL); CHECK(h->anchor == 0 && h->cursor == 999);
    key(3); CHECK(g.clip_len == 2999 && !memcmp(g.clip, "03 0A 11", 8));
    /* the toolbar's Copy offset (the button from 306 to 408) */
    key(GUI_HOME); key(GUI_DOWN);
    fake_click(&g, w->x + 320, w->y + 40); CHECK(g.clip_len == 10 && !memcmp(g.clip, "0x00000010", 10));
    /* the mouse: a click on a byte, a drag over several, the characters' column, the wheel, the scroll bar */
    g.now += 1000;
    x = w->x + 90 + 5 * 22 + 4; y = w->y + 77 + 2 * 16 + 8;
    memset(&e, 0, sizeof(e));
    e.x = x; e.y = y; e.buttons = POINTER_LEFT; gui_pointer(&g, &e);
    CHECK(h->cursor == 37 && h->anchor == 37 && h->selecting);
    e.x = w->x + 90 + 0 * 22 + 4; e.y = y + 16; gui_pointer(&g, &e);
    CHECK(h->cursor == 48 && h->anchor == 37);
    e.buttons = 0; gui_pointer(&g, &e); CHECK(!h->selecting);
    key(3); CHECK(g.clip_len == 12 * 3 - 1);
    fake_click(&g, w->x + 90 + 15 * 22 + 40 + 3 * 8 + 2, w->y + 77 + 16 + 4); CHECK(h->cursor == 19 && h->anchor == 19);
    e.x = w->x + 300; e.y = w->y + 200; e.wheel = -1; gui_pointer(&g, &e); CHECK(h->top == 48);   /* toward the user: down */
    e.wheel = 1; gui_pointer(&g, &e); gui_pointer(&g, &e); CHECK(h->top == 0);
    e.wheel = 0;
    fake_click(&g, w->x + 620 - 2 - 6, w->y + 300); CHECK(h->top == 17 * 16);   /* below the thumb: a page */
    gui_render(&g, &screen); CHECK(fake_guards(960, 540));
    /* a match across two pieces of 64 KB, a Knuth-Morris-Pratt one (AAAB in A's), and one at the end */
    for(i = 0; i < sizeof(data); i++) data[i] = 'A';
    data[65537] = 'B';
    memcpy(data + 131072 - 3, "PIECE!", 6);
    memcpy(data + sizeof(data) - 4, "TAIL", 4);
    fake_file("/home/pieces.bin", (const char *)data, sizeof(data));
    hex_show(&g, "/home/pieces.bin"); CHECK(h->size == sizeof(data) && h->cursor == 0);
    hex_find("AAAB"); CHECK(h->found_at == 65534);
    hex_find("PIECE!"); CHECK(h->found_at == 131069);
    hex_find("TAIL"); CHECK(h->found_at == sizeof(data) - 4 && h->cursor == sizeof(data) - 1);
    key('n'); CHECK(h->anchor == sizeof(data) - 4 && strstr(h->status, "Not found"));
    hex_find("ATAIL"); CHECK(!h->found);                 /* from the selection's start on: it began before */
    key(GUI_HOME); hex_find("ATAIL"); CHECK(h->found_at == sizeof(data) - 5);
    /* 4,000,000,000 bytes: the end, Go to past 2 GB, the scroll bar's ends, a search over many frames, Esc */
    hex_show(&g, "/big.bin");
    CHECK(h->size == BIG_SIZE && h->page_ok && h->page_len == GUI_HEX_PAGE);
    key(GUI_END); CHECK(h->cursor == BIG_SIZE - 1 && h->top == (250000000u - 17) * 16);
    gui_render(&g, &screen); CHECK(fake_guards(960, 540));
    hex_goto("0xEE6B27F0"); CHECK(h->cursor == 0xEE6B27F0u && h->top == (250000000u - 17) * 16);
    hex_goto("3000000007"); CHECK(h->cursor == 3000000007u && h->top == 3000000000u);
    hex_goto("4000000000"); CHECK(h->cursor == 3000000007u && strstr(h->status, "past the end"));
    hex_goto("0");
    e.x = w->x + 620 - 8; e.y = w->y + 77 + 2; e.buttons = POINTER_LEFT; gui_pointer(&g, &e);
    k = 0;
    if(h->thumb) {                                      /* the thumb, held: down to the bottom, then the top */
        e.y = w->y + 400; gui_pointer(&g, &e); k = h->top == (250000000u - 17) * 16;
        e.y = w->y - 200; gui_pointer(&g, &e); k = k && h->top == 0;
    }
    e.buttons = 0; gui_pointer(&g, &e);
    CHECK(k);
    hex_goto("0");
    big_reads = big_bytes = 0;
    hex_find(needle);
    CHECK(h->searching && strstr(h->status, "Searching") && big_bytes <= 256 * 1024 + GUI_HEX_PAGE);
    gui_render(&g, &screen); CHECK(fake_guards(960, 540));
    for(k = 0; k < 100 && h->searching; k++) { big_bytes = 0; ticks(1); CHECK(big_bytes <= 256 * 1024 + GUI_HEX_PAGE); }
    CHECK(!h->searching && h->found && h->found_at == NEEDLE_AT && k >= 10 && h->top <= NEEDLE_AT &&
          NEEDLE_AT < h->top + 17 * 16 && h->page_ok);
    key('n'); ticks(3); CHECK(h->searching);
    key(27); CHECK(!h->searching && strstr(h->status, "stopped") && g.win[GUI_HEX].open);
    key(27); CHECK(!g.win[GUI_HEX].open);               /* then Esc closes the window, which forgets the file */
    CHECK(!h->path[0]);
    d = fake_data("/home/data.bin", &len); CHECK(d && len == 1000);   /* read-only: nothing was written */
}

/**** Sketch's BMP files ****/

static u32 sketch_pixel(u32 x, u32 y) { return sketch_paints[(x / 67 + y / 30) % SKETCH_PAINTS]; }
static u32 paint_of(u32 x, u32 y) { return y < 180 ? (x < 400 ? sketch_paints[2] : sketch_paints[3]) :
                                                     (x < 400 ? sketch_paints[4] : sketch_paints[5]); }
static u32 ink_all(u32 x, u32 y) { (void)x; (void)y; return 0x153D49; }
static u32 red_white(u32 x, u32 y) { (void)y; return x < 10 ? 0xFF0000 : x < 20 ? 0xFFFFFF : 0x000000; }

static bool canvas_is(u32 (*pix)(u32, u32)) {
    u32 x, y;
    for(y = 0; y < GUI_CANVAS_H; y++) for(x = 0; x < GUI_CANVAS_W; x++)
        if(sketch_paints[g.canvas[y * GUI_CANVAS_W + x]] != pix(x, y)) return false;
    return true;
}

static void sketch(void) {
    struct gui_sketchbmp *s = &g.viewer.sketch;
    struct gui_window *w;
    const char *d;
    u32 len, x, y, n;
    bool same;
    desktop(960, 540);
    key(4); CHECK(g.focus == GUI_SKETCH);
    w = &g.win[GUI_SKETCH];
    for(y = 0; y < GUI_CANVAS_H; y++) for(x = 0; x < GUI_CANVAS_W; x++) g.canvas[y * GUI_CANVAS_W + x] = (u8)((x / 67 + y / 30) % 6);
    /* its File menu: Open BMP and Save as BMP (Ctrl+S), and not Notepad's Save */
    CHECK(gui_menu_on(&g, 2, ACT_SKETCH_SAVE) && gui_menu_on(&g, 2, ACT_SKETCH_OPEN) && !gui_menu_on(&g, 2, ACT_SAVE) &&
          !gui_menu_on(&g, 2, ACT_SAVE_AS) && gui_menu_on(&g, 2, ACT_NEW) && gui_menu_on(&g, 2, ACT_CLOSE));
    {
        int mx, my, mw, mh;
        gui_menu_box(&g, 2, &mx, &my, &mw, &mh); CHECK(my + mh <= 400 && mx + mw <= 640);
    }
    gui_key(&g, 7); while(g.menu != 2) key(GUI_RIGHT);
    gui_render(&g, &screen); CHECK(fake_guards(960, 540));
    key(27);
    gui_open(&g, GUI_NOTEPAD); CHECK(!gui_menu_on(&g, 2, ACT_SKETCH_SAVE) && gui_menu_on(&g, 2, ACT_SAVE));
    key(4);
    /* Save as BMP: the chooser, a folder, the name; read back, the same pixels */
    key(19);
    CHECK(g.dialog.kind == GUI_DLG_SAVE && g.dialog.act == gui_app_act(GUI_VIEWER, VIEW_SKETCH_SAVE) &&
          !strcmp(g.dialog.dir, "/") && !strcmp(g.dialog.field.text, "sketch.bmp"));
    CHECK(choose("home"));
    key('\r');
    CHECK(!g.dialog.kind && !strcmp(s->path, "/home/sketch.bmp") && s->saved && !strcmp(s->status, "Saved sketch.bmp (216054 bytes)"));
    d = fake_data("/home/sketch.bmp", &len);
    CHECK(d && len == GUI_SKETCH_FILE && len == 216054);
    if(d) {
        struct gui_bmp b;
        u32 pal[256];
        char why[80];
        CHECK(bmp_parse((const u8 *)d, len, len, &b, pal, why, sizeof(why)) == 0 && b.width == 400 && b.height == 180 &&
              b.bpp == 24 && !b.topdown && get32((const u8 *)d + 2) == len);
        same = true;
        for(y = 0; y < 180; y++) for(x = 0; x < 400; x++)
            if(pixel_of((const u8 *)d, len, x, y) != sketch_pixel(x, y)) same = false;
        CHECK(same);
    }
    gui_render(&g, &screen); CHECK(fake_guards(960, 540));
    /* again, to its own file: no question */
    key(19); CHECK(g.dialog.kind == GUI_DLG_SAVE && !strcmp(g.dialog.dir, "/home") && !strcmp(g.dialog.field.text, "sketch.bmp"));
    key('\r'); CHECK(!g.dialog.kind);
    /* onto another file: asked first, as Notepad's Save as asks; Cancel keeps it, Replace replaces it */
    fake_file("/home/other.bmp", "x", 1);
    key(19); field_set(&g.dialog.field, "other.bmp", false); key('\r');
    CHECK(g.dialog.kind == GUI_DLG_CONFIRM && g.dialog.act == gui_app_act(GUI_VIEWER, VIEW_SKETCH_REPLACE) &&
          !strcmp(g.dialog.title, "Replace the file?"));
    key(27); d = fake_data("/home/other.bmp", &len); CHECK(!g.dialog.kind && len == 1);
    key(19); field_set(&g.dialog.field, "other.bmp", false); key('\r'); key('\r');
    d = fake_data("/home/other.bmp", &len); CHECK(!g.dialog.kind && len == GUI_SKETCH_FILE && !strcmp(s->path, "/home/other.bmp"));
    /* a folder of that name is not replaced; the initrd is read-only; a failing disk */
    fake_dir("/home/pics");
    key(19); field_set(&g.dialog.field, "pics", false); key('\r');
    CHECK(g.dialog.kind == GUI_DLG_SAVE && !strcmp(g.dialog.dir, "/home/pics"));   /* the chooser goes into it */
    key(27);
    CHECK(sketch_save(&g, "/initrd/x.bmp") == -EROFS && g.dialog.kind == GUI_DLG_MESSAGE && strstr(g.dialog.text[1], "read-only"));
    key(27);
    fake_fail_write = -EIO;
    CHECK(sketch_save(&g, "/home/sketch.bmp") == -EIO && g.dialog.kind == GUI_DLG_MESSAGE && !strcmp(g.dialog.title, "Not saved"));
    key(27);
    fake_fail_write = 0;
    /* Open BMP: the drawing it saved comes back as it was */
    key(14); CHECK(g.canvas[1000] == 0 && g.canvas[GUI_CANVAS_W * GUI_CANVAS_H - 1] == 0);   /* Ctrl+N clears Sketch */
    gui_act(&g, ACT_SKETCH_OPEN);
    CHECK(g.dialog.kind == GUI_DLG_OPEN && g.dialog.act == gui_app_act(GUI_VIEWER, VIEW_SKETCH_OPEN) && !strcmp(g.dialog.dir, "/home"));
    CHECK(choose("sketch.bmp"));
    CHECK(!g.dialog.kind && canvas_is(sketch_pixel) && !s->saved && !strcmp(s->status, "Opened sketch.bmp"));
    /* a larger picture is shrunk to fit (800 x 360: by half), a smaller one centred, colours to the nearest */
    n = bmp(file, 40, 800, 360, 24, 0, 0, NULL, paint_of);
    fake_file("/home/large.bmp", (const char *)file, n);
    CHECK(sketch_load(&g, "/home/large.bmp") == 0 && strstr(s->status, "shrunk"));
    CHECK(g.canvas[0] == 2 && g.canvas[399] == 3 && g.canvas[179 * 400] == 4 && g.canvas[179 * 400 + 399] == 5 &&
          g.canvas[89 * 400 + 199] == 2 && g.canvas[90 * 400 + 200] == 5);
    n = bmp(file, 40, 40, 30, 32, 0, 0, NULL, ink_all);
    fake_file("/home/small.bmp", (const char *)file, n);
    CHECK(sketch_load(&g, "/home/small.bmp") == 0);
    CHECK(g.canvas[75 * 400 + 180] == 1 && g.canvas[104 * 400 + 219] == 1 && g.canvas[74 * 400 + 180] == 0 &&
          g.canvas[75 * 400 + 179] == 0 && g.canvas[105 * 400 + 219] == 0 && g.canvas[75 * 400 + 220] == 0);
    n = bmp(file, 40, 30, -10, 24, 0, 0, NULL, red_white);
    fake_file("/home/rgb.bmp", (const char *)file, n);
    CHECK(sketch_load(&g, "/home/rgb.bmp") == 0);
    CHECK(g.canvas[85 * 400 + 185] == 3 && g.canvas[85 * 400 + 195] == 0 && g.canvas[85 * 400 + 205] == 1);   /* coral, paper, ink */
    /* refused, or cut short while it is read: said, and the drawing stays */
    n = bmp(file, 40, 30, 10, 24, 0, 0, NULL, red_white);
    put32(file + 30, 1);
    fake_file("/home/rle.bmp", (const char *)file, n);
    CHECK(sketch_load(&g, "/home/rle.bmp") == -EINVAL && g.dialog.kind == GUI_DLG_MESSAGE && strstr(g.dialog.text[1], "RLE"));
    key(27);
    CHECK(g.canvas[85 * 400 + 185] == 3);
    /* 1 GB: refused (Open BMP reads in the dialog's answer), the Image Viewer named; 6 MB: read, a row at a time */
    CHECK(sketch_load(&g, "/huge.bmp") == -EFBIG && g.dialog.kind == GUI_DLG_MESSAGE &&
          !strcmp(g.dialog.title, "Too large for Sketch") &&
          !strcmp(g.dialog.text[0], "huge.bmp has more than the 8 MB Sketch opens.") &&
          strstr(g.dialog.text[1], "Image Viewer"));
    key(27);
    CHECK(g.canvas[85 * 400 + 185] == 3);
    big_reads = 0;
    CHECK(sketch_load(&g, "/shot.bmp") == 0 && strstr(s->status, "shrunk") && big_reads == 181);
    CHECK(g.canvas[39] == 0 && g.canvas[40] == 1 && g.canvas[179 * 400 + 359] == 1 && g.canvas[360] == 0);   /* 320 x 180, ink */
    edge_size = 8 * 1024 * 1024;                        /* 8 MB exactly: opened; a byte more: refused, saying "more than" */
    CHECK(sketch_load(&g, "/edge.bmp") == 0 && !g.dialog.kind && g.canvas[200] == 1);
    edge_size = 8 * 1024 * 1024 + 1;
    CHECK(sketch_load(&g, "/edge.bmp") == -EFBIG && g.dialog.kind == GUI_DLG_MESSAGE &&
          !strcmp(g.dialog.text[0], "edge.bmp has more than the 8 MB Sketch opens."));
    key(27);
    /* the window's buttons: Open (212) and Save (272) */
    fake_click(&g, w->x + 230, w->y + 50); CHECK(g.dialog.kind == GUI_DLG_OPEN); key(27);
    fake_click(&g, w->x + 290, w->y + 50); CHECK(g.dialog.kind == GUI_DLG_SAVE); key(27);
    fake_click(&g, w->x + 350, w->y + 50); CHECK(g.canvas[0] == 0 && g.canvas[GUI_CANVAS_W * GUI_CANVAS_H - 1] == 0);   /* Clear */
    fake_click(&g, w->x + 20, w->y + 50); CHECK(g.brush == 0);   /* the swatches are where they were */
    fake_click(&g, w->x + 200, w->y + 50); CHECK(g.brush == 5);
    gui_render(&g, &screen); CHECK(fake_guards(960, 540));
}

/**** what the first review found ****/

static u32 stripes(u32 x, u32 y) { return ((x >> 5) ^ (y >> 9)) & 1; }
static const u32 reds[2] = { 0xFF0000, 0xFF0000 };
static u32 zero(u32 x, u32 y) { (void)x; (void)y; return 0; }

/* the Image Viewer: a reading that fails in the tick never replaces a dialog in front (Notepad's question about
   unsaved changes, a chooser with a name typed in it); a frame's reads; 4- and 1-bit pictures; "kept at"; a file
   refused while another is read; dialogs' numbers that are not Sketch's */
static void review_viewer(void) {
    struct gui_viewer *v = &g.viewer;
    struct gui_window *w;
    int x, y, pw, ph;
    u32 n, i, k, most, frames, dx, dy, sx, sy;
    /* Notepad's question is in front when the reading fails: it stays, and so does what it waits for */
    desktop(960, 540);
    n = bmp(file, 40, 1100, 1000, 8, 0, 256, gray256, ramp);
    fake_file("/home/big.bmp", (const char *)file, n);
    fake_file("/home/n.txt", "hello\n", 6);
    viewer_show(&g, "/home/big.bmp");
    CHECK(v->loading && note_load(&g, "/home/n.txt") == 0);
    gui_open(&g, GUI_NOTEPAD);
    fake_type(&g, "x");
    key(17);
    CHECK(g.dialog.kind == GUI_DLG_CONFIRM && g.dialog.act == ACT_DLG_GUARD && g.pending == PEND_QUIT);
    fake_file("/home/big.bmp", (const char *)file, n / 2);
    ticks(10);
    CHECK(!v->loading && !v->ready && strstr(v->status, "big.bmp could not be read"));
    CHECK(g.dialog.kind == GUI_DLG_CONFIRM && g.dialog.act == ACT_DLG_GUARD && g.pending == PEND_QUIT);
    key(27);                                            /* Cancel: nothing waits any more */
    CHECK(!g.dialog.kind && g.pending == PEND_NONE && !g.quit);
    key(19);                                            /* a later Ctrl+S in Notepad saves, and the desktop stays */
    CHECK(!g.quit && !g.note.modified && !g.dialog.kind);
    gui_open(&g, GUI_VIEWER);
    w = &g.win[GUI_VIEWER];
    gui_render(&g, &screen);
    CHECK(shows(w->x + 17, w->y + 59 + 16, "big.bmp could not be read", CORAL));
    CHECK(shows(w->x + 10, w->y + 377 + 3, "big.bmp could not be read", INK));   /* and its status bar */
    /* Sketch's Save chooser with a name typed in it: it stays, and saves */
    desktop(960, 540);
    fake_file("/home/big.bmp", (const char *)file, n);
    viewer_show(&g, "/home/big.bmp");
    key(4); key(19);
    field_set(&g.dialog.field, "my-drawing.bmp", false);
    fake_file("/home/big.bmp", (const char *)file, n / 2);
    ticks(10);
    CHECK(!v->loading && g.dialog.kind == GUI_DLG_SAVE && !strcmp(g.dialog.field.text, "my-drawing.bmp"));
    key('\r');
    CHECK(!g.dialog.kind && fake_data("/my-drawing.bmp", &k) && k == GUI_SKETCH_FILE);
    /* the first piece, read in the chooser's answer, and each frame: 16 reads at most, 256 KB and a chunk at most;
       short rows that a reduced picture takes every other of are read as spans of the file, not a read each */
    desktop(960, 540);
    n = bmp(file, 40, 256, 16384, 1, 0, 2, four, stripes);
    fake_file("/home/thin.bmp", (const char *)file, n);
    big_reads = big_bytes = 0;
    viewer_show(&g, "/home/thin.bmp");
    CHECK(v->dw == 128 && v->dh == 8192 && big_reads >= 2 && big_reads <= 1 + 16 && big_bytes <= 2048 + 256 * 1024 + GUI_VIEW_CHUNK);
    for(most = frames = 0; v->loading && frames < 100; frames++) {
        big_reads = big_bytes = 0;
        ticks(1);
        if(big_reads > most) most = big_reads;
        CHECK(big_bytes <= 256 * 1024 + GUI_VIEW_CHUNK);
    }
    CHECK(v->ready && most <= 16 && frames < 10);
    for(i = 0; i < 64; i++) {                           /* each kept pixel the one in the middle of what it stands for */
        dx = i * 37 % v->dw; dy = i * 977 % v->dh;
        sx = (2 * dx + 1) * 256 / (2 * v->dw); sy = (2 * dy + 1) * 16384 / (2 * v->dh);
        CHECK(v->pixels[(dy * v->dw + dx) * 3] == (u8)(four[stripes(sx, sy)] >> 16) &&
              v->pixels[(dy * v->dw + dx) * 3 + 2] == (u8)four[stripes(sx, sy)]);
    }
    n = bmp(file, 40, 1024, 1100, 8, 0, 256, gray256, ramp);
    fake_file("/home/wide.bmp", (const char *)file, n);
    big_reads = 0;
    viewer_show(&g, "/home/wide.bmp");
    CHECK(big_reads <= 1 + 16);
    for(most = frames = 0; v->loading && frames < 100; frames++) { big_reads = 0; ticks(1); if(big_reads > most) most = big_reads; }
    CHECK(v->ready && most <= 16);
    /* 4-bit and 1-bit pictures through the viewer, on the screen: 13 x 7, enlarged 16 times */
    qw = 13; qh = 7;
    n = bmp(file, 40, 13, 7, 4, 0, 4, four, quad_index);
    fake_file("/home/q4.bmp", (const char *)file, n);
    viewer_show(&g, "/home/q4.bmp");
    viewer_place(&g, &x, &y, &pw, &ph);
    gui_render(&g, &screen);
    CHECK(v->ready && v->bmp.bpp == 4 && pw == 208 && ph == 112);
    CHECK(at(x + 95, y + 47) == TL && at(x + 96, y + 47) == TR && at(x + 95, y + 48) == BL && at(x + 96, y + 48) == BR &&
          at(x + pw - 1, y + ph - 1) == BR && at(x, y) == TL);
    n = bmp(file, 40, 13, -7, 1, 0, 2, four, bits);
    fake_file("/home/b1.bmp", (const char *)file, n);
    viewer_show(&g, "/home/b1.bmp");
    viewer_place(&g, &x, &y, &pw, &ph);
    gui_render(&g, &screen);
    CHECK(v->ready && v->bmp.bpp == 1 && v->bmp.topdown && pw == 208);
    CHECK(at(x + 8, y + 8) == TL && at(x + 24, y + 8) == TR && at(x + 8, y + 24) == TR && at(x + 12 * 16 + 8, y + 6 * 16 + 8) == TL);
    /* a picture of more than 1,048,576 pixels: the status bar says what it was kept at */
    desktop(960, 540);                                  /* (the fake's pool of 2 MB is full) */
    n = bmp(file, 40, 1100, 1000, 8, 0, 256, gray256, ramp);
    fake_file("/home/big.bmp", (const char *)file, n);
    viewer_show(&g, "/home/big.bmp");
    ticks(10);
    gui_render(&g, &screen);
    w = &g.win[GUI_VIEWER];
    CHECK(v->ready && shows(w->x + 10, w->y + 377 + 3, "big.bmp  1100 x 1000, 8-bit, kept at 1073 x 976", INK));
    /* a file refused while another is read (its palette checked aside): that reading goes on, its colours its own */
    viewer_show(&g, "/home/big.bmp");
    CHECK(v->loading);
    n = bmp(file, 40, 10, 10, 8, 0, 2, reds, zero);
    fake_file("/home/cut.bmp", (const char *)file, n - 20);
    viewer_show(&g, "/home/cut.bmp");
    CHECK(v->loading && g.dialog.kind == GUI_DLG_MESSAGE && strstr(g.dialog.text[1], "cut short"));
    key(27);
    ticks(10);
    CHECK(v->ready && !strcmp(v->path, "/home/big.bmp") && v->pixels[0] == v->pixels[1] && v->pixels[1] == v->pixels[2]);
    for(i = 0, k = 0; i < v->dw * v->dh * 3; i += 3) if(v->pixels[i] != v->pixels[i + 2]) k++;
    CHECK(k == 0);                                      /* grays all of it: no red from the refused file's palette */
    /* only Sketch's three numbers go to Sketch */
    gui_answer(&g, gui_app_act(GUI_VIEWER, 23), 0, "/home/zz.bmp", "");
    gui_answer(&g, gui_app_act(GUI_VIEWER, 19), 0, "/home/zz.bmp", "");
    CHECK(!g.dialog.kind && !fake_data("/home/zz.bmp", &k) && v->ready);
}

/* Hex: a Find that is no pattern, a copy that fails part-way or is too big, Find next after "Not found", read errors,
   devices, and the view at the end of a 4 GB file */
static void review_hex(void) {
    struct gui_hex *h = &g.hex;
    static u8 data[200000];
    static const char *const bad[] = { "0xZZ", "", "0xABC", "a pattern of 129 bytes" };
    char text[200];
    u32 i;
    int b;
    /* a Find that is no pattern stops the search that runs: never "Found" a match of nothing */
    memset(text, 'A', 129); text[129] = 0;
    for(b = 0; b < 4; b++) {
        desktop(960, 540);
        hex_show(&g, "/big.bin");
        hex_find(needle);
        CHECK(h->searching && h->plen == 8);
        hex_find(b == 3 ? text : bad[b]);
        CHECK(!h->searching && h->plen == 0 && !strstr(h->status, "Searching"));
        ticks(3);
        CHECK(!h->searching && !h->found && h->anchor == 0 && h->cursor == 0 && !strstr(h->status, "Found"));
        CHECK(!gui_menu_on(&g, 3, ACT_FIND_NEXT));
        key('n'); ticks(2); CHECK(!h->searching);
    }
    /* a copy that fails part-way leaves the clipboard as it was */
    for(i = 0; i < 1000; i++) data[i] = (u8)(i * 7 + 3);
    fake_file("/home/d.bin", (const char *)data, 1000);
    hex_show(&g, "/home/d.bin");
    memcpy(g.clip, "Hello, clipboard", 16);
    g.clip_len = 16;
    gui_act(&g, ACT_SELECT_ALL);
    fake_file("/home/d.bin", (const char *)data, 500); /* shorter now than the selection */
    key(3);
    CHECK(strstr(h->status, "Not copied") && g.clip_len == 16 && !memcmp(g.clip, "Hello, clipboard", 16));
    fake_file("/home/d.bin", (const char *)data, 1000);
    fail_after = 0;
    key(3);
    CHECK(strstr(h->status, "Not copied: I/O error") && g.clip_len == 16 && !memcmp(g.clip, "Hello, clipboard", 16));
    fail_after = -1;
    /* Find next after "Not found": "Not found" again, not the last match again */
    memcpy(data + 300, "\xDE\xAD\xBE\xEF", 4);
    memcpy(data + 700, "\xDE\xAD\xBE\xEF", 4);
    fake_file("/home/d.bin", (const char *)data, 1000);
    hex_show(&g, "/home/d.bin");
    hex_find("DE AD BE EF"); CHECK(h->found_at == 300 && strstr(h->status, "Found at 0x0000012C"));
    key('n'); CHECK(h->found_at == 700 && strstr(h->status, "Found at 0x000002BC"));
    key('n'); CHECK(h->anchor == 700 && h->cursor == 703 && strstr(h->status, "Not found from 0x000002BD"));
    key('n'); CHECK(h->anchor == 700 && h->cursor == 703 && strstr(h->status, "Not found from 0x000002BD"));
    key(GUI_HOME); key('n'); CHECK(h->found_at == 300 && h->anchor == 300);   /* elsewhere: from the cursor */
    /* the copy's limit: 43,690 bytes (3 n - 1 = 131,069 of the clipboard's 131,072), one more is refused */
    for(i = 0; i < sizeof(data); i++) data[i] = (u8)i;
    fake_file("/home/l.bin", (const char *)data, sizeof(data));
    hex_show(&g, "/home/l.bin");
    memset(g.note.path, 0x5A, 8);                       /* what follows the clipboard in gui_state */
    h->anchor = 10;
    h->cursor = 10 + 43690 - 1;
    key(3);
    CHECK(g.clip_len == 131069 && !memcmp(g.clip, "0A 0B 0C", 8) && !memcmp(g.clip + 131069 - 5, "B2 B3", 5) &&
          !memcmp(g.note.path, "\x5A\x5A\x5A\x5A\x5A\x5A\x5A\x5A", 8) && strstr(h->status, "Copied 43690 bytes"));
    h->cursor = 10 + 43691 - 1;
    key(3);
    CHECK(strstr(h->status, "Too much to copy: 43690 bytes at most") && g.clip_len == 131069);
    /* a page that cannot be read: said, and not read again every frame; a search whose read fails */
    hex_show(&g, "/home/l.bin");
    fail_after = 0;
    for(i = 0; i < 16; i++) key(GUI_PGDN);
    CHECK(!h->page_ok && h->page_bad && strstr(h->status, "Cannot read at 0x"));
    big_reads = 0;
    ticks(5);
    CHECK(big_reads == 0);
    gui_render(&g, &screen); CHECK(fake_guards(960, 540));
    fail_after = -1;
    key(GUI_HOME); CHECK(h->page_ok && !h->page_bad);
    fail_after = 2;
    hex_find("not in the file at all");
    CHECK(!h->searching && strstr(h->status, "Find stopped at 0x00020000: I/O error"));
    fail_after = -1;
    /* devices: refused by Hex, the Image Viewer and Sketch's Open BMP */
    fake_file("/home/dev0", "BMxxxxxxxxxxxxxxxxx", 20);
    device = "/home/dev0";
    hex_show(&g, "/home/dev0");
    CHECK(!strcmp(h->path, "/home/l.bin") && g.dialog.kind == GUI_DLG_MESSAGE && strstr(g.dialog.text[0], "dev0 is a device"));
    key(27);
    viewer_show(&g, "/home/dev0");
    CHECK(!g.viewer.ready && g.dialog.kind == GUI_DLG_MESSAGE && strstr(g.dialog.text[1], "device"));
    key(27);
    CHECK(sketch_load(&g, "/home/dev0") < 0 && g.dialog.kind == GUI_DLG_MESSAGE && strstr(g.dialog.text[1], "device"));
    key(27);
    device = "";
    /* a file of 0xFFFFFFFF bytes: the view ends at 4 GB, where top + rows * 16 would wrap */
    gui_open(&g, GUI_HEX);
    hex_show(&g, "/max.bin");
    key(GUI_END); CHECK(h->cursor == 0xFFFFFFFEu && h->top == 0xFFFFFEF0u);
    key(GUI_UP); CHECK(h->cursor == 0xFFFFFFEEu && h->top == 0xFFFFFEF0u);
    hex_goto("0xFFFFFF00"); CHECK(h->cursor == 0xFFFFFF00u && h->top == 0xFFFFFEF0u);
    key(GUI_DOWN); key(GUI_DOWN); CHECK(h->cursor == 0xFFFFFF20u && h->top == 0xFFFFFEF0u);
    gui_render(&g, &screen); CHECK(fake_guards(960, 540));
}

/* Sketch: the line under the canvas until the drawing changes; "shrunk to fit" for a picture taller than the canvas */
static void review_sketch(void) {
    struct gui_sketchbmp *s = &g.viewer.sketch;
    struct gui_window *w;
    u32 n;
    desktop(960, 540);
    key(4);
    w = &g.win[GUI_SKETCH];
    CHECK(sketch_save(&g, "/home/s.bmp") == 0 && strstr(s->status, "Saved s.bmp"));
    g.now += 1000;
    fake_click(&g, w->x + 16 + 50, w->y + 70 + 50);    /* a stroke */
    CHECK(g.canvas[50 * GUI_CANVAS_W + 50] == g.brush && !s->status[0]);
    CHECK(sketch_save(&g, "/home/s.bmp") == 0 && s->status[0]);
    fake_click(&g, w->x + 350, w->y + 50);             /* Clear */
    CHECK(!s->status[0] && !g.canvas[50 * GUI_CANVAS_W + 50]);
    CHECK(sketch_save(&g, "/home/s.bmp") == 0 && s->status[0]);
    key(14);                                            /* Ctrl+N clears Sketch */
    CHECK(!s->status[0]);
    gui_render(&g, &screen);
    CHECK(shows(w->x + 16, w->y + 266, "Drag/Space draws.", INK));
    n = bmp(file, 40, 1, 1000, 24, 0, 0, NULL, ink_all);
    fake_file("/home/tall.bmp", (const char *)file, n);
    CHECK(sketch_load(&g, "/home/tall.bmp") == 0 && !strcmp(s->status, "Opened tall.bmp, shrunk to fit"));
    CHECK(g.canvas[199] == 1 && g.canvas[179 * GUI_CANVAS_W + 199] == 1 && !g.canvas[198] && !g.canvas[200]);
}

/* the frames a big picture takes (the second review): a 12-megapixel photo and a tall, narrow picture, both reduced.
   0c245c0 took 40 and 63 frames, reading only the rows they need; 2a75187 took 81 and 204, reading the rows they skip
   too. They must take one frame more at most (the opening handler reads one piece, no more), read the rows they
   need and no others, and keep a frame to 128 reads and 256 KB and a chunk, with every pixel right where looked at */
static void review_frames(void) {
    static const struct { const char *path; u32 w, h, frames, bytes; } pics[] = {
        { "/photo.bmp", 4000, 3000, 41, 2048 + 887 * 12000 },
        { "/tall.bmp", 1024, 16384, 64, 2048 + 4096 * 4096 },
    };
    struct gui_viewer *v = &g.viewer;
    u32 i, j, frames, total, most_reads, most_bytes, dx, dy, c;
    for(i = 0; i < 2; i++) {
        desktop(960, 540);
        big_reads = big_bytes = 0;
        viewer_show(&g, pics[i].path);
        CHECK(v->loading && big_reads == 2 && big_bytes <= 2048 + GUI_VIEW_CHUNK);   /* the headers, then one read */
        total = big_bytes;
        most_reads = most_bytes = 0;
        for(frames = 0; v->loading && frames < 1000; frames++) {
            big_reads = big_bytes = 0;
            walk_reset();
            ticks(1);
            total += big_bytes;
            if(big_reads > most_reads) most_reads = big_reads;
            if(big_bytes > most_bytes) most_bytes = big_bytes;
            CHECK(walk_steps == walk_far);              /* one walk a frame, to its furthest read: the reads go forward */
        }
        CHECK(v->ready && v->w == pics[i].w && v->h == pics[i].h);
        CHECK(frames <= pics[i].frames && total == pics[i].bytes && most_reads <= 128 && most_bytes <= 256 * 1024 + GUI_VIEW_CHUNK);
        if(frames > pics[i].frames || total != pics[i].bytes)
            printf("  (%s: %u frames, %u bytes read)\n", pics[i].path, frames, total);
        for(j = 0; j < 97; j++) {
            dx = j * 389 % v->dw;
            dy = j * 1291 % v->dh;
            c = pic((2 * dx + 1) * v->w / (2 * v->dw), (2 * dy + 1) * v->h / (2 * v->dh));
            CHECK(v->pixels[(dy * v->dw + dx) * 3] == (u8)(c >> 16) && v->pixels[(dy * v->dw + dx) * 3 + 1] == (u8)(c >> 8) &&
                  v->pixels[(dy * v->dw + dx) * 3 + 2] == (u8)c);
        }
    }
}

/* the third review: a tall picture with short rows, 682 x 16384 x 24 (rows of 2,048 bytes, which a read counts for at
   least anyway), kept at a third or so of its rows: each row it needs is a read of its own (none is next to another),
   so every frame makes 128 reads exactly, but the last; the bytes are those rows' and no others; the reads of a frame
   go forward, so that on the kernel, which keeps the file open through a frame since gui/suite 3f77dfe, a frame walks
   the cluster chain once, to its furthest read (8,193 clusters of 4 KB at most here, where a walk from the start for
   each read cost up to a million); and the pixels are right where looked at */
static void review_cap(void) {
    struct gui_viewer *v = &g.viewer;
    u32 frames, rows, total, worst = 0, j, dx, dy, c;
    desktop(960, 540);
    big_reads = big_bytes = 0;
    walk_reset();
    viewer_show(&g, "/narrow.bmp");
    CHECK(v->loading && v->dh > 4096 && v->dh < 8192 && big_reads == 2 && big_bytes == 2048 + 2048 && walk_steps == walk_far);
    total = big_bytes;
    rows = v->dh - 1;                                   /* the rows still to read, each a read */
    for(frames = 0; v->loading && frames < 1000; frames++) {
        big_reads = big_bytes = 0;
        walk_reset();
        ticks(1);
        total += big_bytes;
        CHECK(big_reads == (rows < 128 ? rows : 128) && big_bytes == big_reads * 2048 && walk_steps == walk_far &&
              walk_far <= NARROW_SIZE / CLUSTER);
        rows -= big_reads <= rows ? big_reads : rows;
        if(walk_steps > worst) worst = walk_steps;
    }
    CHECK(v->ready && !rows && frames == (v->dh - 1 + 127) / 128 && total == 2048 + v->dh * 2048 && worst > 4096);
    for(j = 0; j < 97; j++) {
        dx = j * 61 % v->dw;
        dy = j * 1291 % v->dh;
        c = pic((2 * dx + 1) * v->w / (2 * v->dw), (2 * dy + 1) * v->h / (2 * v->dh));
        CHECK(v->pixels[(dy * v->dw + dx) * 3] == (u8)(c >> 16) && v->pixels[(dy * v->dw + dx) * 3 + 1] == (u8)(c >> 8) &&
              v->pixels[(dy * v->dw + dx) * 3 + 2] == (u8)c);
    }
}

/**** every window inside the screen ****/

static void windows(int width, int height) {
    static const int ids[] = { GUI_VIEWER, GUI_HEX, GUI_SKETCH };
    int i, k, n;
    struct gui_window *w;
    desktop(width, height);
    qw = 42; qh = 30;
    n = (int)bmp(file, 40, 42, 30, 24, 0, 0, NULL, quad);
    fake_file("/home/q.bmp", (const char *)file, (u32)n);
    for(i = 0; i < 3; i++) {
        gui_open(&g, ids[i]);
        w = &g.win[ids[i]];
        CHECK(g.win[ids[i]].open && g.focus == ids[i] && w->x >= 0 && w->x + w->w <= width && w->y + w->h <= height - TASK_H);
        if(ids[i] == GUI_VIEWER) { viewer_show(&g, "/home/q.bmp"); key('+'); key('+'); key('+'); key(GUI_END); }
        if(ids[i] == GUI_HEX) { hex_show(&g, "/big.bin"); key(GUI_END); g.hex.anchor = g.hex.cursor - 40; }
        if(ids[i] == GUI_SKETCH) strlcpy(g.viewer.sketch.status, "Saved a name that is far too long to fit in the window.bmp", GUI_STATUS);
        for(k = 0; k < 3; k++) ticks(1);
        gui_render(&g, &screen); CHECK(fake_guards(width, height));
        gui_act(&g, ACT_MAXIMIZE);
        gui_render(&g, &screen); CHECK(fake_guards(width, height));
        CHECK(w->x >= 0 && w->x + w->w <= width && w->y + w->h <= height - TASK_H);
        gui_act(&g, ACT_MAXIMIZE);
        w->w = ids[i] == GUI_SKETCH ? w->w : gui_app_of(ids[i])->min_width(); w->h = ids[i] == GUI_SKETCH ? w->h : 200;
        gui_render(&g, &screen); CHECK(fake_guards(width, height));
        w->x = width - 10;
        w->y = height - 10;
        gui_render(&g, &screen); CHECK(fake_guards(width, height));
        gui_place(&g, ids[i]);
    }
    CHECK(gui_app_viewer.min_width() <= 640 - 48 && gui_app_hex.min_width() <= 640 - 48);
    /* the dialogs over them */
    gui_open(&g, GUI_HEX); key(6); gui_render(&g, &screen); CHECK(fake_guards(width, height)); key(27);
    gui_open(&g, GUI_SKETCH); key(19); gui_render(&g, &screen); CHECK(fake_guards(width, height)); key(27);
    viewer_show(&g, "/huge.bmp"); gui_render(&g, &screen); CHECK(fake_guards(width, height)); key(27);
    gui_finish(&g);
    for(i = 0; i < 2; i++) {
        gui_open(&g, ids[i]);
        gui_act(&g, ACT_CLOSE);
        CHECK(!g.win[ids[i]].open);
    }
    /* Help's lines fit its window at 640 x 400: 62 cells */
    for(i = 0; i < gui_app_viewer.help_lines; i++) CHECK(strlen(gui_app_viewer.help[i]) <= 60);
    for(i = 0; i < gui_app_hex.help_lines; i++) CHECK(strlen(gui_app_hex.help[i]) <= 60);
}

int main(void) {
    bmp_reading();
    bmp_refusing();
    viewer();
    no_files();
    hex();
    sketch();
    review_viewer();
    review_hex();
    review_sketch();
    review_frames();
    review_cap();
    windows(640, 400);
    windows(960, 540);
    windows(1280, 800);
    return fake_done("test_guimedia");
}
