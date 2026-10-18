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
static u32 big_reads, big_bytes;

static u8 big_byte(u32 off) {
    if(off >= NEEDLE_AT && off < NEEDLE_AT + 8) return (u8)needle[off - NEEDLE_AT];
    return (u8)((off * 2654435761u) >> 24) | 0x80;      /* never ASCII: the needle is only where it was put */
}

/* the huge picture: 54 bytes of headers, then 32-bit rows bottom-up; pixel x, y (from the top) is x/64, y/64, 0x40 */
static u8 huge_byte(u32 off) {
    static u8 head[54];
    u32 r, c, x, y;
    if(off < 54) {
        memset(head, 0, sizeof(head));
        head[0] = 'B'; head[1] = 'M';
        put32(head + 2, HUGE_SIZE); put32(head + 10, 54); put32(head + 14, 40);
        put32(head + 18, HUGE_SIDE); put32(head + 22, HUGE_SIDE); put16(head + 26, 1); put16(head + 28, 32);
        return head[off];
    }
    r = (off - 54) / 65536; c = (off - 54) % 65536;
    x = c / 4; y = HUGE_SIDE - 1 - r;
    switch(c % 4) {
    case 0: return 0x40;
    case 1: return (u8)(y / 64);
    case 2: return (u8)(x / 64);
    }
    return 0;
}

static bool made(const char *path, u32 *size, u8 (**byte)(u32)) {
    if(!strcmp(path, "/big.bin")) { *size = BIG_SIZE; *byte = big_byte; return true; }
    if(!strcmp(path, "/huge.bmp")) { *size = HUGE_SIZE; *byte = huge_byte; return true; }
    return false;
}

static int big_stat(void *ctx, const char *path, struct gui_entry *out, bool *device, bool *readonly) {
    u32 size;
    u8 (*byte)(u32);
    if(!made(path, &size, &byte)) return fake_fs.stat(ctx, path, out, device, readonly);
    memset(out, 0, sizeof(*out));
    strlcpy(out->name, gui_basename(path), sizeof(out->name));
    out->type = GUI_FILE;
    out->size = size;
    *device = false;
    *readonly = true;
    return 0;
}

static int big_read_at(void *ctx, const char *path, u32 off, char *buf, u32 n, u32 *len, u32 *size) {
    u8 (*byte)(u32);
    u32 i;
    if(!made(path, size, &byte)) return fake_fs.read_at(ctx, path, off, buf, n, len, size);
    *len = 0;
    big_reads++;
    if(off >= *size) return 0;
    if(n > *size - off) n = *size - off;
    for(i = 0; i < n; i++) buf[i] = (char)byte(off + i);
    *len = n;
    big_bytes += n;
    return 0;
}

static struct gui_fs big_fs;

static void desktop(int width, int height) {
    fake_desktop(&g, width, height);
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
    ticks(10);
    CHECK(!v->loading && !v->ready && g.dialog.kind == GUI_DLG_MESSAGE && strstr(g.dialog.text[1], "read to its end"));
    key(27);
    /* a picture of 16384 x 16384 x 32 bits (1 GB): kept at 1024 x 1024, a frame reading at most 256 KB (four rows) */
    viewer_show(&g, "/huge.bmp");
    CHECK(v->loading && v->dw == 1024 && v->dh == 1024);
    for(k = 0; k < 1000 && v->loading; k++) {
        big_bytes = 0;
        ticks(1);
        if(big_bytes > 256 * 1024 + 65536) break;
    }
    CHECK(v->ready && k == 255 && big_bytes <= 256 * 1024);
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
    gui_act(&g, ACT_FIND_NEXT); CHECK(!h->found && h->cursor == 703 && strstr(h->status, "Not found"));
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
    key('n'); CHECK(!h->found && strstr(h->status, "Not found"));
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
    CHECK(sketch_load(&g, "/huge.bmp") == 0 && strstr(s->status, "shrunk"));   /* 16384 x 16384: 180 rows of it read */
    CHECK(g.canvas[109] == 0 && g.canvas[110] == 1 && g.canvas[290] == 0);   /* 180 x 180 in the middle; dark blue: ink */
    /* the window's buttons: Open (212) and Save (272) */
    fake_click(&g, w->x + 230, w->y + 50); CHECK(g.dialog.kind == GUI_DLG_OPEN); key(27);
    fake_click(&g, w->x + 290, w->y + 50); CHECK(g.dialog.kind == GUI_DLG_SAVE); key(27);
    fake_click(&g, w->x + 350, w->y + 50); CHECK(g.canvas[0] == 0 && g.canvas[GUI_CANVAS_W * GUI_CANVAS_H - 1] == 0);   /* Clear */
    fake_click(&g, w->x + 20, w->y + 50); CHECK(g.brush == 0);   /* the swatches are where they were */
    fake_click(&g, w->x + 200, w->y + 50); CHECK(g.brush == 5);
    gui_render(&g, &screen); CHECK(fake_guards(960, 540));
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
    windows(640, 400);
    windows(960, 540);
    windows(1280, 800);
    return fake_done("test_guimedia");
}
