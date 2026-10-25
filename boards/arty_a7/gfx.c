/*
SurfOS board: SurfCPU on the Arty A7 -- the gfx command
--------------------
File: gfx.c     Date: 10/25/26 (SurfGPU G3)
--------------------
The shell's way to SurfGPU (boards/arty_a7/gpu.c, surfcpu/docs/GPU.md):

  gfx                          the GPU: ID, VRAM, mode, base/stride/scale, FIFO, fences, errors, frames
  gfx mode text|gfx|overlay [1|2]   the display mode and scale (1 = 640x480, 2 = 320x240 doubled)
  gfx clear [c]                the target filled with colour c (default 0)
  gfx fill x y w h c           FILL_RECT
  gfx rect x y w h c           an outline (four LINEs)
  gfx line x0 y0 x1 y1 c       LINE
  gfx copy sx sy dx dy w h [key]   COPY_RECT, key a colour that is not copied
  gfx text x y fg bg|- words...    EXPAND with the console's font (-: transparent)
  gfx pal rgb332|grey|ramps|<n> <rgb444>   a palette, or one entry
  gfx put x y c / gfx get x y  one pixel through the CPU's VRAM window (0x3000_0000)
  gfx crc                      FRAME_COUNT and FRAME_CRC of a frame begun after the last change
  gfx bench                    fills, copies, lines and text: pixels per second
  gfx demo [seconds]           an animation at 320x240, two pages flipped at vertical blanking
  gfx reset                    ABORT, sticky bits cleared, the target and clip a reset leaves

Coordinates are the target's (GPU.md section 5): x and y signed 16-bit, sizes unsigned 16-bit,
colours 0-255 (palette indexes); the clip cuts, nothing wraps. gfx mode gfx and overlay make the
target the picture shown (640x480 at VRAM 0, or 320x240 there at scale 2), so what is drawn is
seen; text mode keeps the target. Every argument is checked before anything is sent (usage:
CMD_USAGE), each command claims the driver for its run (two consoles never interleave), and a
drawing command returns once the engine has finished it and found no error. The demo and the
bench end on Ctrl+C with the engine idle, the demo with the display, the palette, the target and
the VRAM it drew on back as they were.

Pure code over the driver: the host test runs this file against the C model of the engine
(boards/arty_a7/tests/gpu/), and gfxscene.c there replays a list of these commands in the model
to know the FRAME_CRCs SurfOS must print on the RTL system (make test-gpuos).
*/

#include <surfos/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <surfcpu/soc.h>
#include <sys/framebuffer.h>            /* FONT_WIDTH, FONT_HEIGHT */
#include <sys/cp437.h>                  /* the console's UTF-8 decoder */
#include "gpu.h"
#include "../../shell/shell.h"

#define TEXT_MAX 240                    /* cells of gfx text (a shell line is 255 bytes) */

static const char *const usage_lines[] = {
    "gfx                          the GPU: ID, VRAM, mode, FIFO, fences, frames",
    "gfx mode text|gfx|overlay [1|2]   the display mode (2: 320x240 doubled)",
    "gfx clear [c]                the target filled with colour c (0)",
    "gfx fill x y w h c           FILL_RECT",
    "gfx rect x y w h c           an outline: four LINEs",
    "gfx line x0 y0 x1 y1 c       LINE",
    "gfx copy sx sy dx dy w h [key]   COPY_RECT (key: a colour not copied)",
    "gfx text x y fg bg|- words   EXPAND in the console's font (-: clear)",
    "gfx pal rgb332|grey|ramps    a palette",
    "gfx pal n rgb444             one palette entry",
    "gfx put x y c, gfx get x y   a pixel through the CPU's VRAM window",
    "gfx crc                      FRAME_CRC of a frame begun after a change",
    "gfx bench                    fills, copies, lines, text: pixels a second",
    "gfx demo [seconds]           an animation, two pages flipped (10 s)",
    "gfx reset                    ABORT, errors cleared, the reset target",
    NULL
};

static int usage(const char *line) {
    u_int i;
    if(line) {
        printf("    usage: %s\n", line);
        return CMD_USAGE;
    }
    printf("    usage:\n");
    for(i = 0; usage_lines[i]; i++) printf("    %s\n", usage_lines[i]);
    printf("    x, y -32768 to 32767, w, h 0 to 65535, colours 0 to 255 (palette indexes),\n"
           "    in the target's pixels; numbers in decimal or 0x hex\n");
    return CMD_USAGE;
}

/* a whole number in [lo, hi], decimal or 0x hex, with a sign */
static bool num(const char *s, long lo, long hi, long *out) {
    char *end;
    long v;
    if(!s || !*s || strlen(s) > 12) return false;
    v = strtol(s, &end, 0);
    if(*end || v < lo || v > hi) return false;
    *out = v;
    return true;
}

/* argv[first..first + n) as numbers of these kinds: 'x' a coordinate, 'w' a size, 'c' a colour */
static bool nums(char **argv, int first, const char *kinds, long *out) {
    int i;
    for(i = 0; kinds[i]; i++) {
        long lo = kinds[i] == 'x' ? -32768 : 0, hi = kinds[i] == 'x' ? 32767 : kinds[i] == 'w' ? 65535 : 255;
        if(!num(argv[first + i], lo, hi, &out[i])) return false;
    }
    return true;
}

/* the driver's result as the command's status; GPU_EINTR is said here, the rest by the driver */
static int status(int r, const char *what) {
    if(r == GPU_OK) return CMD_OK;
    if(r == GPU_EINTR) printf("    %s: interrupted: the engine stopped (ABORT)\n", what);
    if(r == GPU_EARG) {
        printf("    %s: refused: a coordinate outside -32768..32767 or a size outside 0..65535\n", what);
        return CMD_USAGE;
    }
    return CMD_FAIL;
}

static int claim(struct gpu *g, const char *what) {
    if(gpu_claim(g, what) == GPU_OK) return CMD_OK;
    printf("    %s: gave up waiting for the GPU\n", what);
    return CMD_FAIL;
}

/* one drawing command's words, then the wait for it */
static int finish(struct gpu *g, int r, const char *what) {
    if(!r) r = gpu_sync(g, what);
    gpu_release(g);
    return status(r, what);
}

/**** gfx: the state ****/

static int show(struct gpu *g) {
    struct gpu_screen d;
    struct gpu_state s;
    char pal[64], irqs[64];
    if(claim(g, "gfx")) return CMD_FAIL;
    if(g->irq) snprintf(irqs, sizeof(irqs), "%lu interrupts (intc source 5, IE.FENCE)", g->irqs);
    else snprintf(irqs, sizeof(irqs), "waited for with sleeps (no interrupt)");
    gpu_screen_get(g, &d);
    gpu_state_read(g, &s);
    gpu_pal_describe(g, pal, sizeof(pal));
    gpu_release(g);
    printf("    SurfGPU %s (gpu.ID 0x%08lx), VRAM %lu KiB at 0x%08lx\n"
           "    engine %s, STATUS 0x%lx, FIFO %lu words, %lu free\n"
           "    display: mode %lu, %s\n"
           "    GFX_BASE %lu, GFX_STRIDE %lu, GFX_SCALE %lu (%s)\n"
           "    palette: %s\n",
           gpu_id_name(g->id), (u_long)g->id, (u_long)(g->vram_bytes / 1024), (u_long)SURF_VRAM_BASE,
           (s.status & SURF_GPU_STATUS_BUSY_MASK) ? "busy" : "idle", (u_long)s.status, (u_long)g->fifo_depth,
           (u_long)s.fifo_free, (u_long)d.mode, gpu_mode_name(d.mode), (u_long)d.base, (u_long)d.stride, (u_long)d.scale,
           d.scale == 2 ? "320x240 doubled" : "640x480", pal);
    printf("    target: VRAM %lu, stride %lu, %lux%lu; clip (%lu, %lu)-(%lu, %lu)\n"
           "    fences: %lu sent, FENCE %lu; %s\n"
           "    %lu words sent; errors %lu (overflow %lu, bad opcode %lu, bad parameters %lu)\n"
           "    timeouts %lu, ABORTs %lu; display changes %lu, %lu without a blanking\n"
           "    frames: FRAME_COUNT %lu, FRAME_CRC %08lx (that frame's)\n",
           (u_long)g->t_base, (u_long)g->t_stride, (u_long)g->t_w, (u_long)g->t_h, (u_long)g->c_x0, (u_long)g->c_y0,
           (u_long)g->c_x1, (u_long)g->c_y1, g->fences, (u_long)s.fence, irqs, g->words,
           g->errors, g->overflows, g->bad_opcodes, g->bad_params, g->timeouts, g->aborts, g->display_changes,
           g->latch_misses, (u_long)s.frame_count, (u_long)s.frame_crc);
    return CMD_OK;
}

/**** gfx mode ****/

static int mode(struct gpu *g, int argc, char **argv) {
    static const char *use = "gfx mode text|gfx|overlay [1|2]   (2: 320x240, each pixel doubled)";
    struct gpu_screen d;
    long scale = 0;
    u32 m;
    int r;
    if(argc < 2 || argc > 3) return usage(use);
    if(!strcmp(argv[1], "text")) m = GPU_MODE_TEXT;
    else if(!strcmp(argv[1], "gfx") || !strcmp(argv[1], "graphics")) m = GPU_MODE_GFX;
    else if(!strcmp(argv[1], "overlay")) m = GPU_MODE_OVERLAY;
    else return usage(use);
    if(argc == 3 && (m == GPU_MODE_TEXT || !num(argv[2], 1, 2, &scale))) return usage(use);
    if(claim(g, "gfx mode")) return CMD_FAIL;
    gpu_screen_get(g, &d);
    r = GPU_OK;
    if(m != GPU_MODE_TEXT) {
        if(!scale) scale = d.scale == 2 ? 2 : 1;
        d.base = 0;
        d.stride = 640 / (u32)scale;
        d.scale = (u32)scale;
        r = gpu_set_target(g, 0, d.stride, 640 / (u32)scale, 480 / (u32)scale, "gfx mode");
        if(!r) r = gpu_sync(g, "gfx mode");
    }
    d.mode = m;
    if(!r) r = gpu_screen_set(g, &d, "gfx mode");
    gpu_release(g);
    if(r) return status(r, "gfx mode");
    if(m == GPU_MODE_TEXT) printf("    display: mode 1, text (the picture stays in VRAM)\n");
    else printf("    display: mode %lu, %s, %s\n    the target is that picture: %lux%lu at VRAM 0, stride %lu\n",
                (u_long)m, gpu_mode_name(m), scale == 2 ? "320x240 doubled" : "640x480", (u_long)g->t_w,
                (u_long)g->t_h, (u_long)g->t_stride);
    if(m == GPU_MODE_GFX) printf("    the text console is hidden: gfx mode text or overlay shows it again\n");
    return CMD_OK;
}

/**** drawing ****/

static int clear(struct gpu *g, int argc, char **argv) {
    long c = 0;
    if(argc > 2 || (argc == 2 && !num(argv[1], 0, 255, &c))) return usage("gfx clear [c]   (c 0-255, 0 when not given)");
    if(claim(g, "gfx clear")) return CMD_FAIL;
    return finish(g, gpu_fill(g, 0, 0, g->t_w, g->t_h, (u8)c, "gfx clear"), "gfx clear");
}

static int fill(struct gpu *g, int argc, char **argv) {
    long v[5];
    if(argc != 6 || !nums(argv, 1, "xxwwc", v)) return usage("gfx fill x y w h c");
    if(claim(g, "gfx fill")) return CMD_FAIL;
    return finish(g, gpu_fill(g, v[0], v[1], v[2], v[3], (u8)v[4], "gfx fill"), "gfx fill");
}

static int rect(struct gpu *g, int argc, char **argv) {
    long v[5];
    if(argc != 6 || !nums(argv, 1, "xxwwc", v)) return usage("gfx rect x y w h c");
    if(v[0] + v[2] - 1 > 32767 || v[1] + v[3] - 1 > 32767)
        return usage("gfx rect x y w h c   (the far corner, x + w - 1 and y + h - 1, at most 32767)");
    if(claim(g, "gfx rect")) return CMD_FAIL;
    return finish(g, gpu_rect(g, v[0], v[1], v[2], v[3], (u8)v[4], "gfx rect"), "gfx rect");
}

static int line(struct gpu *g, int argc, char **argv) {
    long v[5];
    if(argc != 6 || !nums(argv, 1, "xxxxc", v)) return usage("gfx line x0 y0 x1 y1 c");
    if(claim(g, "gfx line")) return CMD_FAIL;
    return finish(g, gpu_line(g, v[0], v[1], v[2], v[3], (u8)v[4], "gfx line"), "gfx line");
}

static int copy(struct gpu *g, int argc, char **argv) {
    long v[7];
    if((argc != 7 && argc != 8) || !nums(argv, 1, "xxxxww", v) || (argc == 8 && !num(argv[7], 0, 255, &v[6])))
        return usage("gfx copy sx sy dx dy w h [key]   (key: a colour 0-255 that is not copied)");
    if(claim(g, "gfx copy")) return CMD_FAIL;
    return finish(g, gpu_copy(g, v[0], v[1], v[2], v[3], v[4], v[5], argc == 8 ? (int)v[6] : -1, "gfx copy"), "gfx copy");
}

/* the words after argv[first], joined by spaces, as code page 437 cells: UTF-8 decoded as the
   console decodes it (driver/cp437.c); bytes that are not UTF-8 are the cells they are */
static void cell_byte(struct utf8_dec *dec, u8 b, u8 *cells, u_int *n, u_int max) {
    u8 out[UTF8_OUT_MAX];
    int got = utf8_feed(dec, b, out), k;
    for(k = 0; k < got && *n < max; k++) cells[(*n)++] = out[k];
}

static u_int cells_of(int argc, char **argv, int first, u8 *cells, u_int max) {
    struct utf8_dec dec;
    const char *s;
    u_int n = 0, k;
    int i;
    memset(&dec, 0, sizeof(dec));
    for(i = first; i < argc; i++) {
        if(i > first) cell_byte(&dec, ' ', cells, &n, max);
        for(s = argv[i]; *s; s++) cell_byte(&dec, (u8)*s, cells, &n, max);
    }
    for(k = 0; k < dec.have && n < max; k++) cells[n++] = dec.b[k];    /* a sequence cut short at the end */
    return n;
}

static int text(struct gpu *g, int argc, char **argv) {
    static const char *use = "gfx text x y fg bg|- words...   (bg -: transparent; the console's 8x16 font)";
    long v[3], bg = -1;
    u8 cells[TEXT_MAX];
    u_int n;
    if(argc < 6 || !nums(argv, 1, "xxc", v) || (strcmp(argv[4], "-") && !num(argv[4], 0, 255, &bg))) return usage(use);
    n = cells_of(argc, argv, 5, cells, TEXT_MAX);
    if(claim(g, "gfx text")) return CMD_FAIL;
    return finish(g, gpu_text(g, v[0], v[1], (u8)v[2], (int)bg, cells, n, "gfx text"), "gfx text");
}

static int pal(struct gpu *g, int argc, char **argv) {
    static const char *use = "gfx pal rgb332|grey|ramps   or   gfx pal <n> <rgb444>   (n 0-255, rgb444 0-0xfff)";
    u16 p[256];
    char name[64];
    long n, rgb;
    if(argc == 2) {
        int which;
        if(!strcmp(argv[1], "rgb332")) which = GPU_PAL_RGB332;
        else if(!strcmp(argv[1], "grey") || !strcmp(argv[1], "gray")) which = GPU_PAL_GREY;
        else if(!strcmp(argv[1], "ramps")) which = GPU_PAL_RAMPS;
        else return usage(use);
        if(claim(g, "gfx pal")) return CMD_FAIL;
        gpu_pal_make(which, p);
        gpu_pal_write(g, p);
        gpu_pal_describe(g, name, sizeof(name));
        gpu_release(g);
        printf("    palette: %s\n", name);
        return CMD_OK;
    }
    if(argc != 3 || !num(argv[1], 0, 255, &n) || !num(argv[2], 0, 0xFFF, &rgb)) return usage(use);
    if(claim(g, "gfx pal")) return CMD_FAIL;
    gpu_pal_set(g, (u32)n, (u16)rgb);
    gpu_release(g);
    printf("    GPAL[%ld] = 0x%03lx (red %lu, green %lu, blue %lu of 15)\n", n, (u_long)rgb, (u_long)(rgb >> 8),
           (u_long)((rgb >> 4) & 15), (u_long)(rgb & 15));
    return CMD_OK;
}

static int put(struct gpu *g, int argc, char **argv) {
    long x, y, c;
    int r;
    if(argc != 4 || !num(argv[1], 0, 32767, &x) || !num(argv[2], 0, 32767, &y) || !num(argv[3], 0, 255, &c))
        return usage("gfx put x y c   (a pixel of the target)");
    if(claim(g, "gfx put")) return CMD_FAIL;
    if((u32)x >= g->t_w || (u32)y >= g->t_h) {
        printf("    gfx put: (%ld, %ld) is outside the target (%lux%lu); usage: gfx put x y c\n", x, y, (u_long)g->t_w,
               (u_long)g->t_h);
        gpu_release(g);
        return CMD_USAGE;
    }
    r = gpu_pixel_put(g, (u32)x, (u32)y, (u8)c, "gfx put");
    gpu_release(g);
    return status(r, "gfx put");
}

static int get(struct gpu *g, int argc, char **argv) {
    long x, y;
    u8 c = 0;
    int r;
    u32 off;
    if(argc != 3 || !num(argv[1], 0, 32767, &x) || !num(argv[2], 0, 32767, &y)) return usage("gfx get x y   (a pixel of the target)");
    if(claim(g, "gfx get")) return CMD_FAIL;
    if((u32)x >= g->t_w || (u32)y >= g->t_h) {
        printf("    gfx get: (%ld, %ld) is outside the target (%lux%lu); usage: gfx get x y\n", x, y, (u_long)g->t_w,
               (u_long)g->t_h);
        gpu_release(g);
        return CMD_USAGE;
    }
    off = g->t_base + (u32)y * g->t_stride + (u32)x;
    r = gpu_pixel_get(g, (u32)x, (u32)y, &c, "gfx get");
    gpu_release(g);
    if(r) return status(r, "gfx get");
    printf("    (%ld, %ld) = %u (0x%02x), VRAM byte %lu (0x%08lx)\n", x, y, c, c, (u_long)off, (u_long)(SURF_VRAM_BASE + off));
    return CMD_OK;
}

static int crc(struct gpu *g, int argc, char **argv) {
    u32 count = 0, value = 0;
    int r;
    (void)argv;
    if(argc != 1) return usage("gfx crc");
    if(claim(g, "gfx crc")) return CMD_FAIL;
    r = gpu_still_crc(g, &count, &value, "gfx crc");
    gpu_release(g);
    if(r) return status(r, "gfx crc");
    printf("    gfx crc: frame %lu, crc %08lx (a frame begun after the last change)\n",
           (u_long)count, (u_long)value);
    return CMD_OK;
}

static int reset(struct gpu *g, int argc, char **argv) {
    int r;
    (void)argv;
    if(argc != 1) return usage("gfx reset");
    if(claim(g, "gfx reset")) return CMD_FAIL;
    r = gpu_reset(g, "gfx reset");
    gpu_release(g);
    if(r) return status(r, "gfx reset");
    printf("    gfx reset: ABORT, errors cleared; target 640x480 at VRAM 0, stride 640\n");
    return CMD_OK;
}

/**** gfx bench ****/

/* a little generator of its own, so a bench draws the same on every run */
static u32 next(u32 *s) {
    *s = *s * 1664525u + 1013904223u;
    return *s >> 8;
}

static i32 absdiff(i32 a, i32 b) { return a > b ? a - b : b - a; }

static void rate(const char *name, const char *what, u64 px, u64 us) {
    u64 tenths = us ? px * 10 / us : 0;
    printf("    %-5s %s: %llu pixels in %llu.%03llu ms, %llu.%llu million/s\n", name, what, (unsigned long long)px,
           (unsigned long long)(us / 1000), (unsigned long long)(us % 1000), (unsigned long long)(tenths / 10),
           (unsigned long long)(tenths % 10));
}

static int bench(struct gpu *g, int argc, char **argv) {
    const char *what = "gfx bench";
    char desc[64];
    u8 cells[32];
    i16 ends[256][4];
    u32 w, h, seed = 2026, i, spin;
    u64 t0, px;
    int r = GPU_OK;
    (void)argv;
    if(argc != 1) return usage("gfx bench");
    if(claim(g, what)) return CMD_FAIL;
    w = g->t_w;
    h = g->t_h;
    if(w < 64 || h < 32) {
        printf("    gfx bench: the target (%lux%lu) is too small; gfx mode gfx makes it the picture\n", (u_long)w, (u_long)h);
        gpu_release(g);
        return CMD_FAIL;
    }
    printf("    gfx bench on the target (%lux%lu at VRAM %lu), first word to fence:\n", (u_long)w, (u_long)h,
           (u_long)g->t_base);
    spin = g->spin_us;                          /* the fences polled: the engine's time, not the scheduler's */
    g->spin_us = GPU_TIMEOUT_US;
    r = gpu_sync(g, what);
    /* fills: the whole target, 8 times */
    t0 = gpu_now_us(g);
    for(i = 0; i < 8 && !r; i++) r = gpu_fill(g, 0, 0, w, h, (u8)(0x21 * i + 3), what);
    if(!r) r = gpu_sync(g, what);
    if(!r) {
        snprintf(desc, sizeof(desc), "8 x %lux%lu", (u_long)w, (u_long)h);
        rate("fill", desc, 8ull * w * h, gpu_now_us(g) - t0);
    }
    /* copies: the left half onto the right half, 8 times */
    t0 = gpu_now_us(g);
    for(i = 0; i < 8 && !r && !gpu_interrupted(g); i++) r = gpu_copy(g, 0, 0, (i32)(w / 2), 0, w / 2, h, -1, what);
    if(!r) r = gpu_sync(g, what);
    if(!r) {
        snprintf(desc, sizeof(desc), "8 x %lux%lu", (u_long)(w / 2), (u_long)h);
        rate("copy", desc, 8ull * (w / 2) * h, gpu_now_us(g) - t0);
    }
    /* lines between points of the target, chosen before the clock starts: every pixel drawn */
    px = 0;
    for(i = 0; i < 256; i++) {
        i32 dx, dy;
        ends[i][0] = (i16)(next(&seed) % w);
        ends[i][1] = (i16)(next(&seed) % h);
        ends[i][2] = (i16)(next(&seed) % w);
        ends[i][3] = (i16)(next(&seed) % h);
        dx = absdiff(ends[i][0], ends[i][2]);
        dy = absdiff(ends[i][1], ends[i][3]);
        px += (u64)(dx > dy ? dx : dy) + 1;
    }
    t0 = gpu_now_us(g);
    for(i = 0; i < 256 && !r && !gpu_interrupted(g); i++)
        r = gpu_line(g, ends[i][0], ends[i][1], ends[i][2], ends[i][3], (u8)(i * 37 + 1), what);
    if(!r) r = gpu_sync(g, what);
    if(!r) rate("line", "256 lines", px, gpu_now_us(g) - t0);
    /* text: 64 lines of 32 cells */
    for(i = 0; i < 32; i++) cells[i] = (u8)(0x41 + i % 26);
    t0 = gpu_now_us(g);
    for(i = 0; i < 64 && !r && !gpu_interrupted(g); i++)
        r = gpu_text(g, (i32)(next(&seed) % (w - 32)), (i32)(next(&seed) % (h - 16)), (u8)next(&seed), (int)(next(&seed) & 0xFF),
                     cells, 32, what);
    if(!r) r = gpu_sync(g, what);
    if(!r) rate("text", "64 x 32 cells", 64ull * 32 * FONT_WIDTH * FONT_HEIGHT, gpu_now_us(g) - t0);
    if(!r && gpu_interrupted(g)) r = GPU_EINTR;
    g->spin_us = spin;
    gpu_release(g);
    return status(r, what);
}

/**** gfx demo ****/

/* Two pages of 320x240 (A at VRAM 0, B after it) and a row of sprites below them, all in one
   target 320 wide; each frame is drawn into the page not shown, with the clip on its rows, and
   shown by writing GFX_BASE once the engine has finished it. The engine draws the next frame
   only after a WAIT_VBLANK: by then the page just left is no longer on the screen (the frame that
   showed it ended its last line), so no frame ever shows a page being drawn. */
#define DW      320u
#define DH      240u
#define DPAGE   (DW * DH)                       /* 76,800 bytes a page */
#define DSHEET  (2u * DH)                       /* the sprites' row */
#define DROWS   (2u * DH + 16u)
#define DBYTES  (DW * DROWS)                    /* VRAM the demo draws on: 158,720 bytes */
#define DKEY    0xE3u                           /* the sprites' transparent colour */

static const u16 ball[16] = { 0x07E0, 0x1FF8, 0x3FFC, 0x7FFE, 0x7FFE, 0xFFFF, 0xFFFF, 0xFFFF,
                              0xFFFF, 0xFFFF, 0xFFFF, 0x7FFE, 0x7FFE, 0x3FFC, 0x1FF8, 0x07E0 };
static const u8 bands[8] = { 0x01, 0x02, 0x22, 0x42, 0x43, 0x23, 0x03, 0x02 };
static const u8 fan[8] = { 0xE0, 0xF4, 0xFC, 0x9C, 0x1C, 0x1F, 0x5F, 0xE7 };
static const u16 quarter[17] = { 0, 25, 50, 74, 98, 121, 142, 162, 181, 198, 213, 226, 237, 245, 251, 255, 256 };

/* sine and cosine of a 64th of a turn, times 256 */
static i32 dsin(u32 a) {
    a &= 63;
    if(a < 16) return quarter[a];
    if(a < 32) return quarter[32 - a];
    if(a < 48) return -(i32)quarter[a - 32];
    return -(i32)quarter[64 - a];
}

static i32 dcos(u32 a) { return dsin(a + 16); }

struct demo_box {
    i32 x, y, dx, dy;
    u32 w, h;
    u8 c;
};

struct demo {
    struct demo_box box[5];
    u32 frame, fps;
};

static void demo_start(struct demo *s) {
    static const struct demo_box boxes[5] = {
        { 20, 30, 2, 1, 40, 30, 0xE0 }, { 200, 40, -1, 2, 56, 24, 0x1C }, { 100, 150, 3, -2, 32, 48, 0x13 },
        { 240, 160, -2, -1, 64, 40, 0xF0 }, { 150, 90, 1, 3, 24, 24, 0x8F } };
    memcpy(s->box, boxes, sizeof(boxes));
    s->frame = 0;
    s->fps = 0;
}

static void demo_move(struct demo *s) {
    u_int k;
    for(k = 0; k < 5; k++) {
        struct demo_box *b = &s->box[k];
        b->x += b->dx;
        b->y += b->dy;
        if(b->x < 0) { b->x = -b->x; b->dx = -b->dx; }
        if(b->x + (i32)b->w > (i32)DW) { b->x = 2 * ((i32)DW - (i32)b->w) - b->x; b->dx = -b->dx; }
        if(b->y < 0) { b->y = -b->y; b->dy = -b->dy; }
        if(b->y + (i32)b->h > (i32)DH) { b->y = 2 * ((i32)DH - (i32)b->h) - b->y; b->dy = -b->dy; }
    }
    s->frame++;
}

/* the sprites: a ball (16x16) and the font's smiling face (8x16), on the key colour */
static int demo_sheet(struct gpu *g, const char *what) {
    static const u8 face = 0x02;                /* ☻ */
    u32 bits[8];
    u_int i;
    int r;
    for(i = 0; i < 8; i++) bits[i] = (u32)ball[2 * i] << 16 | ball[2 * i + 1];
    r = gpu_fill(g, 0, DSHEET, DW, 16, DKEY, what);
    if(!r) r = gpu_expand(g, 0, DSHEET, 16, 16, 0xFC, DKEY, bits, what);
    if(!r) r = gpu_fill(g, 4, DSHEET + 3, 4, 3, 0xFF, what);      /* a highlight */
    if(!r) r = gpu_text(g, 16, DSHEET, 0xFF, DKEY, &face, 1, what);
    return r;
}

static int demo_frame(struct gpu *g, struct demo *s, u32 page, const char *what) {
    char line[48];
    u8 cells[48];
    i32 py = (i32)(page * DH);
    u32 f = s->frame, a;
    u_int k, n;
    int r = gpu_set_clip(g, 0, (u32)py, DW, (u32)py + DH, what);
    for(k = 0; k < 8 && !r; k++) r = gpu_fill(g, 0, py + (i32)k * 30, DW, 30, bands[(k + f / 8) & 7], what);
    for(k = 0; k < 16 && !r; k++) {                     /* a turning fan of lines (the clip cuts it) */
        a = k * 4 + f;
        r = gpu_line(g, 160, py + 120, 160 + dcos(a) * 150 / 256, py + 120 + dsin(a) * 150 / 256, fan[k & 7], what);
    }
    for(k = 0; k < 5 && !r; k++) {                      /* the boxes, filled and outlined */
        struct demo_box *b = &s->box[k];
        r = gpu_fill(g, b->x, py + b->y, b->w, b->h, b->c, what);
        if(!r) r = gpu_rect(g, b->x, py + b->y, b->w, b->h, 0xFF, what);
    }
    for(k = 0; k < 6 && !r; k++) {                      /* balls: copies from the sheet, the key left out */
        a = f + k * 11;
        r = gpu_copy(g, 0, DSHEET, 152 + dsin(a) * 140 / 256, py + 112 + dsin(a * 2 + 16) * 100 / 256, 16, 16,
                     DKEY, what);
    }
    for(k = 0; k < 2 && !r; k++) {
        a = f * 2 + k * 32;
        r = gpu_copy(g, 16, DSHEET, 156 + dcos(a) * 60 / 256, py + 112 + dsin(a) * 60 / 256, 8, 16, DKEY, what);
    }
    n = (u_int)snprintf(line, sizeof(line), "SurfGPU  frame %lu  %lu a second", (u_long)f, (u_long)s->fps);
    for(k = 0; k < n && k < sizeof(cells); k++) cells[k] = (u8)line[k];
    if(!r) r = gpu_text(g, 8, py + 216, 0xFF, -1, cells, k, what);
    return r;
}

static int demo(struct gpu *g, int argc, char **argv) {
    const char *what = "gfx demo";
    struct gpu_screen before, shown;
    struct demo s;
    u16 pal_before[256], rgb[256];
    u32 *kept = NULL, t_base, t_stride, t_w, t_h, c_x0, c_y0, c_x1, c_y1, page = 1, frames = 0, last = 0, v, i, spin;
    u64 t0, tick, now, secs;
    long seconds = 10;
    bool pal_changed = false, interrupted = false;
    int r, r2;
    if(argc > 2 || (argc == 2 && !num(argv[1], 1, 3600, &seconds))) return usage("gfx demo [seconds]   (1-3600, 10 when not given)");
    if(claim(g, what)) return CMD_FAIL;
    /* what is put back afterwards */
    gpu_screen_get(g, &before);
    gpu_pal_read(g, pal_before);
    t_base = g->t_base; t_stride = g->t_stride; t_w = g->t_w; t_h = g->t_h;
    c_x0 = g->c_x0; c_y0 = g->c_y0; c_x1 = g->c_x1; c_y1 = g->c_y1;
    r = gpu_sync(g, what);
    if(!r) {
        kept = (u32 *)gpu_alloc(g, DBYTES);
        if(kept) for(i = 0; i < DBYTES / 4; i++) kept[i] = gpu_vram_rd(g, 4 * i);
        else printf("    gfx demo: no memory to keep the %lu bytes of VRAM it draws on;\n"
                    "    they stay as the demo leaves them\n", (u_long)DBYTES);
    }
    /* RGB332, the sprites, both pages, the first frame on page A, then the display */
    gpu_pal_make(GPU_PAL_RGB332, rgb);
    if(memcmp(rgb, pal_before, sizeof(rgb))) {
        gpu_pal_write(g, rgb);
        pal_changed = true;
    }
    demo_start(&s);
    if(!r) r = gpu_set_target(g, 0, DW, DW, DROWS, what);
    if(!r) r = demo_sheet(g, what);
    if(!r) r = gpu_fill(g, 0, 0, DW, 2 * DH, 0, what);
    if(!r) r = demo_frame(g, &s, 0, what);
    if(!r) r = gpu_sync(g, what);
    shown.mode = GPU_MODE_GFX;
    shown.base = 0;
    shown.stride = DW;
    shown.scale = 2;
    if(!r) r = gpu_screen_set(g, &shown, what);
    secs = (u64)seconds * 1000000u;
    spin = g->spin_us;                          /* a frame's fence polled: the flip comes at once */
    g->spin_us = 20000u;
    if(!r) printf("    gfx demo: %ld s at 320x240, two pages flipped at blanking (Ctrl+C ends)\n", seconds);
    t0 = tick = gpu_now_us(g);
    while(!r) {
        demo_move(&s);
        r = gpu_wait_vblank(g, what);                   /* the page last shown is off the screen by then */
        if(!r) r = demo_frame(g, &s, page, what);
        if(!r) r = gpu_fence_send(g, what, &v);
        if(!r) r = gpu_fence_wait(g, v, what);
        if(!r) r = gpu_check(g, what);
        if(r) break;
        gpu_flip(g, page * DPAGE);                      /* the next frame latch shows it, whole */
        page ^= 1;
        frames++;
        now = gpu_now_us(g);
        if(now - tick >= 1000000u) {
            s.fps = frames - last;
            last = frames;
            tick += 1000000u;
            printf("    %lu s: %lu frames, %lu in the last second\n", (u_long)((now - t0) / 1000000u), (u_long)frames,
                   (u_long)s.fps);
        }
        if(now - t0 >= secs) break;
        if(gpu_interrupted(g)) {
            interrupted = true;
            break;
        }
    }
    if(r == GPU_EINTR) interrupted = true;
    now = gpu_now_us(g);
    g->spin_us = spin;
    /* the display back as it was, also after Ctrl+C */
    g->keep_going = true;
    if(interrupted) gpu_abort(g, what);
    r2 = gpu_sync(g, what);
    if(!r2) r2 = gpu_screen_set(g, &before, what);
    if(kept) {
        for(i = 0; i < DBYTES / 4; i++) gpu_vram_wr(g, 4 * i, kept[i]);
        gpu_free(g, kept);
    }
    if(pal_changed) gpu_pal_write(g, pal_before);
    if(!r2) r2 = gpu_set_target(g, t_base, t_stride, t_w, t_h, what);
    if(!r2 && (c_x0 || c_y0 || c_x1 != t_w || c_y1 != t_h)) r2 = gpu_set_clip(g, c_x0, c_y0, c_x1, c_y1, what);
    if(!r2) r2 = gpu_sync(g, what);
    g->keep_going = false;
    gpu_release(g);
    if(frames) {
        u64 us = now - t0, rate10 = us ? (u64)frames * 10000000u / us : 0;
        printf("    gfx demo: %lu frames in %lu.%02lu s, %lu.%lu a second; %s\n", (u_long)frames, (u_long)(us / 1000000u),
               (u_long)(us % 1000000u / 10000u), (u_long)(rate10 / 10), (u_long)(rate10 % 10),
               r2 ? "display NOT put back" : "display back as it was");
    }
    if(interrupted) {
        printf("    gfx demo: ended by Ctrl+C\n");
        return CMD_FAIL;
    }
    return r ? status(r, what) : r2 ? CMD_FAIL : CMD_OK;
}

/**** the command ****/

static const struct {
    const char *name;
    int (*fn)(struct gpu *g, int argc, char **argv);
} subs[] = {
    { "mode", mode }, { "clear", clear }, { "fill", fill }, { "rect", rect }, { "line", line }, { "copy", copy },
    { "text", text }, { "pal", pal }, { "put", put }, { "get", get }, { "crc", crc }, { "bench", bench },
    { "demo", demo }, { "reset", reset },
};

int gfx_command(struct gpu *g, int argc, char **argv) {
    u_int i;
    /* The kernel shell hands a command SHELL_MAX_ARGS words at most and drops the rest without a
       word (arch/riscv32/kshell.c's split_args(), as shell/main.c's): a line that filled them may
       have lost some, so it is refused rather than drawn short. */
    if(argc >= SHELL_MAX_ARGS) {
        printf("    usage: a gfx line holds %d words at most, gfx text 9 of text: the shell\n"
               "    passes a command %d and drops the rest\n", SHELL_MAX_ARGS - 1, SHELL_MAX_ARGS);
        return CMD_USAGE;
    }
    if(argc >= 2 && (!strcmp(argv[1], "help") || !strcmp(argv[1], "-h"))) {
        usage(NULL);
        return CMD_OK;
    }
    if(!g->present) {
        if(g->why && g->id) printf("    gfx: no GPU: %s (it reads 0x%08lx)\n", g->why, (u_long)g->id);
        else printf("    gfx: no GPU: %s\n", g->why ? g->why : "none found");
        return CMD_FAIL;
    }
    if(argc < 2) return show(g);
    for(i = 0; i < sizeof(subs) / sizeof(subs[0]); i++)
        if(!strcmp(argv[1], subs[i].name)) return subs[i].fn(g, argc - 1, argv + 1);
    printf("    gfx: no such subcommand: %s\n", argv[1]);
    return usage(NULL);
}
