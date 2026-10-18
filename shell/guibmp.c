/*
SurfOS Desktop: BMP Pictures
--------------------
File: guibmp.c Date: 10/17/26
--------------------
Windows BMP files, for the Image Viewer (guiview.c) and for Sketch's Save as BMP and Open BMP
(Sketch's own code is in guicore.c). A file is treated as hostile: every field of its headers is
checked against the file's real length (gui_fs's read_at gives it) before it is used, and the
sizes are worked out in 64 bits. These are read: BITMAPINFOHEADER (40 bytes) and the versions
that extend it (52 and 56 bytes, V4's 108 and V5's 124: the 40 and what follows them), pixels of
1, 4 and 8 bits with a palette and of 24 and 32 bits (BI_RGB, and 32-bit BI_BITFIELDS with the
usual masks), rows bottom-up or top-down, up to GUI_VIEW_SIDE pixels each way. The rest is
refused, saying why: RLE compression, 16-bit pixels, OS/2 headers, a PNG or a JPEG inside. Alpha
is not used: a picture is drawn opaque.
Sketch saves its canvas as a 24-bit BMP through gui_fs's write, asking before it replaces a
file, as Notepad's Save as does (but not a file it wrote itself). Open BMP fits the picture into
the canvas: shrunk when it is larger (its proportions kept, nearest neighbour), centred, each
colour as the nearest of Sketch's six. The canvas changes only once the whole picture was read.
Open BMP reads in the dialog's answer, a row at a time (180 reads at most), so it takes files of
up to SKETCH_OPEN_MAX: each read of a big file would walk its clusters from the start, and the
desktop would wait. A larger one is refused, and the Image Viewer, which reads in the tick, named.
*/
#include "gui_media.h"
#include <string.h>
#include <stdio.h>

#define BI_RGB 0
#define BI_RLE8 1
#define BI_RLE4 2
#define BI_BITFIELDS 3
#define BI_JPEG 4
#define BI_PNG 5
#define SKETCH_OPEN_MAX (8 * 1024 * 1024)   /* the largest file Sketch's Open BMP reads (a 1920 x 1080 screenshot is 6 MB) */

static u32 le16(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8; }
static u32 le32(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
static void put16(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void put32(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }

/**** reading ****/

int bmp_parse(const u8 *h, u32 n, u32 size, struct gui_bmp *b, u32 *palette, char *why, size_t len) {
    u32 hsize, comp, planes, end, used, i;
    i32 width, height;
    memset(b, 0, sizeof(*b));
    memset(palette, 0, 256 * sizeof(*palette));
    if(n > size) n = size;
#define REFUSE(...) do { snprintf(why, len, __VA_ARGS__); return -EINVAL; } while(0)
    if(n >= 8 && !memcmp(h, "\x89PNG\r\n\x1a\n", 8)) REFUSE("A PNG picture: only BMP files are read.");
    if(n >= 3 && h[0] == 0xFF && h[1] == 0xD8 && h[2] == 0xFF) REFUSE("A JPEG picture: only BMP files are read.");
    if(n >= 4 && !memcmp(h, "GIF8", 4)) REFUSE("A GIF picture: only BMP files are read.");
    if(n >= 2 && (!memcmp(h, "BA", 2) || !memcmp(h, "CI", 2) || !memcmp(h, "CP", 2) || !memcmp(h, "IC", 2) ||
                  !memcmp(h, "PT", 2))) REFUSE("An OS/2 bitmap array or icon: not read.");
    if(n < 2 || h[0] != 'B' || h[1] != 'M') REFUSE("Not a BMP picture: it does not start with BM.");
    if(n < 18) REFUSE("It ends inside its headers.");
    hsize = le32(h + 14);
    if(hsize == 12) REFUSE("An OS/2 BMP (a 12-byte header): not read.");
    if(hsize == 64) REFUSE("An OS/2 2 BMP (a 64-byte header): not read.");
    if(hsize != 40 && hsize != 52 && hsize != 56 && hsize != 108 && hsize != 124)
        REFUSE("A header of %u bytes: not one of BMP's.", hsize);
    end = 14 + hsize;                   /* the headers' end; then the masks, the palette, and the pixels */
    if(n < end) REFUSE("It ends inside its headers.");
    width = (i32)le32(h + 18);
    height = (i32)le32(h + 22);
    planes = le16(h + 26);
    b->bpp = le16(h + 28);
    comp = le32(h + 30);
    if(planes != 1) REFUSE("Its header says %u planes; a BMP has 1.", planes);
    if(comp == BI_RLE8 || comp == BI_RLE4) REFUSE("RLE-compressed: only uncompressed BMPs are read.");
    if(comp == BI_JPEG) REFUSE("A JPEG inside: only uncompressed BMPs are read.");
    if(comp == BI_PNG) REFUSE("A PNG inside: only uncompressed BMPs are read.");
    if(b->bpp == 16) REFUSE("16-bit pixels: 1, 4, 8, 24 and 32 bits are read.");
    if(b->bpp != 1 && b->bpp != 4 && b->bpp != 8 && b->bpp != 24 && b->bpp != 32)
        REFUSE("%u-bit pixels: 1, 4, 8, 24 and 32 bits are read.", b->bpp);
    if(comp == BI_BITFIELDS) {          /* the masks are at 54 in every header: after a 40-byte one, or inside it */
        if(b->bpp != 32) REFUSE("Colour masks for %u-bit pixels: not read.", b->bpp);
        if(hsize == 40) end += 12;
        if(n < end || n < 66) REFUSE("It ends inside its colour masks.");
        if(le32(h + 54) != 0xFF0000 || le32(h + 58) != 0xFF00 || le32(h + 62) != 0xFF)
            REFUSE("Its colour masks are not the usual ones.");
    } else if(comp != BI_RGB) REFUSE("Compression %u: only uncompressed BMPs are read.", comp);
    if(width <= 0 || height == 0) REFUSE("%d x %d pixels: not the size of a picture.", (int)width, (int)height);
    if(width > GUI_VIEW_SIDE || height > GUI_VIEW_SIDE || height < -GUI_VIEW_SIDE)
        REFUSE("%u x %u pixels: %u x %u at most.", (u32)width, height < 0 ? 0u - (u32)height : (u32)height,
               GUI_VIEW_SIDE, GUI_VIEW_SIDE);
    b->width = (u32)width;
    b->topdown = height < 0;
    b->height = height < 0 ? (u32)-height : (u32)height;  /* -GUI_VIEW_SIDE at the least: no overflow */
    b->stride = (b->width * b->bpp + 31) / 32 * 4;          /* at most 16384 pixels of 32 bits: 64 KB */
    if(b->bpp <= 8) {                   /* the palette: the entries it says it has, of those the pixels can use */
        used = 1u << b->bpp;
        if(le32(h + 46) && le32(h + 46) < used) used = le32(h + 46);
        if((u64)end + used * 4 > size || end + used * 4 > n) REFUSE("It ends inside its palette.");
        for(i = 0; i < used; i++) palette[i] = le32(h + end + 4 * i) & 0xFFFFFF;   /* B, G, R, 0 */
        b->colors = used;
        end += used * 4;
    }
    b->offset = le32(h + 10);
    b->size = size;
    if(b->offset < end) REFUSE("Its pixels would start inside its headers.");
    if(b->offset > size || (u64)b->stride * b->height > (u64)(size - b->offset))
        REFUSE("It is cut short: it ends before its pixels do.");
    return 0;
#undef REFUSE
}

int bmp_open(struct gui_state *g, const char *path, struct gui_bmp *b, u32 *palette, u8 *buf, char *why, size_t len) {
    struct gui_entry e;
    bool device = false, readonly = false;
    u32 n = 0, size = 0;
    int r;
    if(!g->fs || !g->fs->read_at) { strlcpy(why, "There are no files to read here.", len); return -ENOSYS; }
    r = g->fs->stat(g->fs->ctx, path, &e, &device, &readonly);
    if(!r && e.type == GUI_DIR) r = -EISDIR;
    if(!r && device) { strlcpy(why, "It is a device, not a file.", len); return -EINVAL; }
    if(!r) r = g->fs->read_at(g->fs->ctx, path, 0, (char *)buf, BMP_HEAD, &n, &size);
    if(r) { strlcpy(why, gui_error(g, r), len); return r; }
    return bmp_parse(buf, n, size, b, palette, why, len);
}

u32 bmp_pixel(const struct gui_bmp *b, const u32 *palette, const u8 *row, u32 x) {
    const u8 *p;
    switch(b->bpp) {
    case 32: p = row + x * 4; return (u32)p[2] << 16 | (u32)p[1] << 8 | p[0];
    case 24: p = row + x * 3; return (u32)p[2] << 16 | (u32)p[1] << 8 | p[0];
    case 8: return palette[row[x]];
    case 4: return palette[(row[x >> 1] >> (x & 1 ? 0 : 4)) & 15];
    default: return palette[(row[x >> 3] >> (7 - (x & 7))) & 1];
    }
}

/* the tallest height whose row (as wide as the proportions make it) still leaves h * w within max */
void bmp_reduce(u32 w, u32 h, u32 max, u32 *dw, u32 *dh) {
    u32 lo = 1, hi = h, mid, x;
    if(w * h <= max) { *dw = w; *dh = h; return; }     /* GUI_VIEW_SIDE squared fits in 32 bits */
    while(lo < hi) {
        mid = lo + (hi - lo + 1) / 2;
        x = w * mid / h;
        if((x ? x : 1) * mid <= max) lo = mid;
        else hi = mid - 1;
    }
    *dh = lo;
    *dw = w * lo / h ? w * lo / h : 1;
}

/**** writing ****/

u32 bmp_header24(u8 *out, u32 w, u32 h) {
    u32 stride = (w * 3 + 3) / 4 * 4;
    memset(out, 0, 54);
    out[0] = 'B';
    out[1] = 'M';
    put32(out + 2, 54 + stride * h);    /* the file's length */
    put32(out + 10, 54);                /* where the pixels start */
    put32(out + 14, 40);                /* BITMAPINFOHEADER */
    put32(out + 18, w);
    put32(out + 22, h);                 /* positive: the bottom row first */
    put16(out + 26, 1);
    put16(out + 28, 24);                /* BI_RGB (0) at 30 */
    put32(out + 34, stride * h);
    put32(out + 38, 2835);              /* 72 dots per inch, in pixels per metre */
    put32(out + 42, 2835);
    return stride;
}

/**** Sketch's pictures ****/

bool sketch_can_open(const struct gui_state *g) {
    return g->fs && g->fs->read_at;
}

/* where Sketch's choosers open: its file's folder, else the last chooser's */
static void sketch_dir(const struct gui_state *g, char *out, size_t n) {
    const struct gui_sketchbmp *s = &g->viewer.sketch;
    if(s->path[0]) { strlcpy(out, s->path, n); gui_parent(out); }
    else strlcpy(out, g->dialog.dir[0] ? g->dialog.dir : "/", n);
}

void sketch_save_as(struct gui_state *g) {
    char dir[GUI_PATH_MAX];
    const struct gui_sketchbmp *s = &g->viewer.sketch;
    if(!g->fs) return;
    sketch_dir(g, dir, sizeof(dir));
    gui_open(g, GUI_SKETCH);
    dialog_chooser(g, GUI_DLG_SAVE, gui_app_act(GUI_VIEWER, VIEW_SKETCH_SAVE), dir,
                   s->path[0] ? gui_basename(s->path) : "sketch.bmp");
}

void sketch_open(struct gui_state *g) {
    char dir[GUI_PATH_MAX];
    if(!sketch_can_open(g)) return;
    sketch_dir(g, dir, sizeof(dir));
    gui_open(g, GUI_SKETCH);
    dialog_chooser(g, GUI_DLG_OPEN, gui_app_act(GUI_VIEWER, VIEW_SKETCH_OPEN), dir, "");
}

void sketch_answer(struct gui_state *g, int act, int button, const char *text, const char *target) {
    const struct gui_sketchbmp *s = &g->viewer.sketch;
    struct gui_entry e;
    bool device = false, readonly = false;
    char line[80];
    if(button != 0 || !g->fs) return;
    if(act == VIEW_SKETCH_OPEN) { sketch_load(g, text); return; }
    if(act == VIEW_SKETCH_REPLACE) { sketch_save(g, target); return; }
    if(act != VIEW_SKETCH_SAVE) return;
    /* a file that is there already: the question first, as Notepad's Save as asks it, unless Sketch wrote it */
    if((strcmp(text, s->path) || !s->saved) && g->fs->stat(g->fs->ctx, text, &e, &device, &readonly) == 0) {
        snprintf(line, sizeof(line), "%s is there already.", gui_basename(text));
        if(e.type == GUI_DIR || device) {
            dialog_message(g, "Not saved", line, "It is not a file that can be replaced.");
            return;
        }
        dialog_confirm(g, gui_app_act(GUI_VIEWER, VIEW_SKETCH_REPLACE), "Replace the file?", line,
                       "Saving replaces what it holds.", "Replace", NULL, "Cancel");
        strlcpy(g->dialog.target, text, sizeof(g->dialog.target));
        return;
    }
    sketch_save(g, text);
}

int sketch_save(struct gui_state *g, const char *path) {
    struct gui_sketchbmp *s = &g->viewer.sketch;
    char line[80], why[80], kept[GUI_PATH_MAX];
    u32 stride, x, y, c;
    u8 *p;
    int r;
    if(!g->fs) return -ENOSYS;
    stride = bmp_header24(s->file, GUI_CANVAS_W, GUI_CANVAS_H);
    for(y = 0; y < GUI_CANVAS_H; y++) {
        p = s->file + 54 + (GUI_CANVAS_H - 1 - y) * stride;
        for(x = 0; x < GUI_CANVAS_W; x++) {
            c = sketch_paints[g->canvas[y * GUI_CANVAS_W + x] % SKETCH_PAINTS];
            *p++ = (u8)c;
            *p++ = (u8)(c >> 8);
            *p++ = (u8)(c >> 16);
        }
        for(x = GUI_CANVAS_W * 3; x < stride; x++) *p++ = 0;
    }
    kept[0] = 0;
    r = g->fs->write(g->fs->ctx, path, (const char *)s->file, GUI_SKETCH_FILE, false, kept, sizeof(kept));
    if(r) {
        snprintf(line, sizeof(line), "%s: %s", gui_basename(path), gui_error(g, r));
        if(kept[0]) snprintf(why, sizeof(why), "The picture is in %s.", gui_basename(kept));
        else strlcpy(why, r == -EROFS ? "That volume is read-only: choose another folder." :
                          r == -ENOSPC ? "The volume is full." :
                          r == -EEXIST ? "Move the .tmp files beside it out of the way." : "", sizeof(why));
        dialog_message(g, "Not saved", line, why);
        return r;
    }
    strlcpy(s->path, path, sizeof(s->path));
    s->saved = true;
    snprintf(s->status, sizeof(s->status), "Saved %s (%u bytes)", gui_basename(path), (u32)GUI_SKETCH_FILE);
    if(g->win[GUI_FILES].open) files_refresh(g);
    g->dirty = true;
    return 0;
}

/* the one of Sketch's colours nearest to c */
static u8 nearest(u32 c) {
    u32 k, best = 0, d, least = 0xFFFFFFFF;
    int dr, dg, db;
    for(k = 0; k < SKETCH_PAINTS; k++) {
        dr = (int)(c >> 16 & 0xFF) - (int)(sketch_paints[k] >> 16 & 0xFF);
        dg = (int)(c >> 8 & 0xFF) - (int)(sketch_paints[k] >> 8 & 0xFF);
        db = (int)(c & 0xFF) - (int)(sketch_paints[k] & 0xFF);
        d = (u32)(dr * dr + dg * dg + db * db);
        if(d < least) { least = d; best = k; }
    }
    return (u8)best;
}

int sketch_load(struct gui_state *g, const char *path) {
    struct gui_sketchbmp *s = &g->viewer.sketch;
    struct gui_bmp b;
    u8 *row = s->file, *canvas = s->file + GUI_VIEW_CHUNK;   /* a row of the file (64 KB at most), the new canvas */
    char why[80], line[80];
    u32 pw, ph, ox, oy, x, y, sy, sx, len = 0, size = 0;
    int r = bmp_open(g, path, &b, s->palette, s->file, why, sizeof(why));
    if(r) {
        dialog_message(g, "Cannot open the picture", gui_basename(path), why);
        return r;
    }
    if(b.size > SKETCH_OPEN_MAX) {                      /* read here, in the answer: bounded */
        snprintf(line, sizeof(line), "%s has more than the 8 MB Sketch opens.", gui_basename(path));
        dialog_message(g, "Too large for Sketch", line, "The Image Viewer shows larger pictures.");
        return -EFBIG;
    }
    pw = b.width;
    ph = b.height;
    if(pw > GUI_CANVAS_W || ph > GUI_CANVAS_H) {        /* shrunk to fit, its proportions kept */
        if(pw * GUI_CANVAS_H >= ph * GUI_CANVAS_W) { ph = ph * GUI_CANVAS_W / pw; pw = GUI_CANVAS_W; }
        else { pw = pw * GUI_CANVAS_H / ph; ph = GUI_CANVAS_H; }
        if(!pw) pw = 1;
        if(!ph) ph = 1;
    }
    ox = (GUI_CANVAS_W - pw) / 2;
    oy = (GUI_CANVAS_H - ph) / 2;
    memset(canvas, 0, GUI_CANVAS_W * GUI_CANVAS_H);
    for(y = 0; y < ph; y++) {                           /* the row nearest to each of the canvas's: its middle */
        sy = ph == b.height ? y : (2 * y + 1) * b.height / (2 * ph);
        r = g->fs->read_at(g->fs->ctx, path, b.offset + (b.topdown ? sy : b.height - 1 - sy) * b.stride, (char *)row,
                           b.stride, &len, &size);
        if(!r && len < b.stride) r = -EIO;              /* it was cut short since */
        if(r) {
            dialog_message(g, "Cannot open the picture", gui_basename(path),
                           r == -EIO ? "It could not be read to its end." : gui_error(g, r));
            return r;
        }
        for(x = 0; x < pw; x++) {
            sx = pw == b.width ? x : (2 * x + 1) * b.width / (2 * pw);
            canvas[(oy + y) * GUI_CANVAS_W + ox + x] = nearest(bmp_pixel(&b, s->palette, row, sx));
        }
    }
    memcpy(g->canvas, canvas, sizeof(g->canvas));
    strlcpy(s->path, path, sizeof(s->path));
    s->saved = false;
    snprintf(s->status, sizeof(s->status), pw < b.width || ph < b.height ? "Opened %s, shrunk to fit" : "Opened %s",
             gui_basename(path));
    gui_open(g, GUI_SKETCH);
    g->dirty = true;
    return 0;
}
