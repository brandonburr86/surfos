/*
SurfGPU diagnostics: the scenes' expected CRCs
--------------------
File: gpuscenes_expect.c Date: 10/18/26 (SurfGPU)
--------------------
A host tool of the firmware build: runs gpudiag's scenes (firmware/diagnostics/gpuscenes.c) in
order through the C model (sim/models/gpu_model.c), as SurfCPU runs them through the hardware,
and writes the FRAME_CRC each must give as a C header for gpudiag (build/fw/gen/
gpuscenes_expect.h). The text modes need the font: driver/font8x16.c, which tools/fontrom.py
check keeps equal to the video block's font ROM. With --ppm DIR it also writes each scene's
frame as DIR/scene-NN.ppm (what the monitor should show).

    gpuscenes_expect --font driver/font8x16.c --out FILE [--ppm DIR] [--list]
*/
#include "gpu_model.h"
#include "gpuscenes.h"
#include "surfcpu/soc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t vram[GPU_VRAM_BYTES];
static uint32_t textram[4096];
static uint8_t font[256][16];
static struct gpu_display disp;
static struct gpu_engine eng;
static uint16_t frame[GPU_SCREEN_W * GPU_SCREEN_H];
static int problems;

/* driver/font8x16.c: lines "[0xNN] = { 0x.., ... 16 bytes }" */
static int load_font(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    char line[512];
    int found = 0;
    while (fgets(line, sizeof line, f)) {
        const char *p = strstr(line, "[0x");
        if (!p)
            continue;
        unsigned idx, b[16];
        if (sscanf(p, "[0x%x] = { 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, "
                      "0x%x, 0x%x, 0x%x, 0x%x }", &idx, &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &b[6], &b[7],
                   &b[8], &b[9], &b[10], &b[11], &b[12], &b[13], &b[14], &b[15]) != 17 || idx > 255)
            continue;
        for (int r = 0; r < 16; r++)
            font[idx][r] = (uint8_t)b[r];
        found++;
    }
    fclose(f);
    return found == 256;
}

/* ------------------------------------------------------------------ the model as a sink */

static void m_cmd(uint32_t w) { gpu_engine_word(&eng, w); }
static void m_sync(void) {}

static void m_vram32(uint32_t off, uint32_t v)
{
    if (off >= GPU_VRAM_BYTES || (off & 3)) {
        fprintf(stderr, "gpuscenes_expect: a scene wrote VRAM word %u\n", off);
        problems++;
        return;
    }
    for (int k = 0; k < 4; k++)
        vram[off + k] = (uint8_t)(v >> (8 * k));
}

static void m_vram8(uint32_t off, uint32_t v)
{
    if (off >= GPU_VRAM_BYTES) {
        fprintf(stderr, "gpuscenes_expect: a scene wrote VRAM byte %u\n", off);
        problems++;
        return;
    }
    vram[off] = (uint8_t)v;
}

static void m_video(uint32_t off, uint32_t v)
{
    if (off == SURF_VIDEO_CTRL) {
        disp.enable = (v & SURF_VIDEO_CTRL_ENABLE_MASK) != 0;
        disp.mode = (int)((v & SURF_VIDEO_CTRL_MODE_MASK) >> SURF_VIDEO_CTRL_MODE_LSB);
        disp.cursor = (v & SURF_VIDEO_CTRL_CURSOR_EN_MASK) != 0;
        if (v & SURF_VIDEO_CTRL_CURSOR_BLINK_MASK) {
            fprintf(stderr, "gpuscenes_expect: a scene blinks the cursor (its picture would depend on the frame)\n");
            problems++;
        }
    } else if (off == SURF_VIDEO_CURSOR) {
        disp.cur_col = (int)((v & SURF_VIDEO_CURSOR_COL_MASK) >> SURF_VIDEO_CURSOR_COL_LSB);
        disp.cur_row = (int)((v & SURF_VIDEO_CURSOR_ROW_MASK) >> SURF_VIDEO_CURSOR_ROW_LSB);
    } else if (off == SURF_VIDEO_SCROLL) {
        disp.scroll = (int)(v & SURF_VIDEO_SCROLL_ROW_MASK);
    } else if (off >= SURF_VIDEO_PALETTE && off < SURF_VIDEO_PALETTE + 4 * SURF_VIDEO_PALETTE_COUNT) {
        disp.pal[(off - SURF_VIDEO_PALETTE) / 4] = (uint16_t)(v & 0xFFF);
    } else if (off == SURF_VIDEO_GFX_BASE) {
        disp.gfx_base = v & SURF_VIDEO_GFX_BASE_BASE_MASK & ~3u;
    } else if (off == SURF_VIDEO_GFX_STRIDE) {
        disp.gfx_stride = v & SURF_VIDEO_GFX_STRIDE_STRIDE_MASK & ~3u;
    } else if (off == SURF_VIDEO_GFX_SCALE) {
        disp.gfx_scale = v & SURF_VIDEO_GFX_SCALE_SCALE_MASK;
    } else if (off >= SURF_VIDEO_GPAL && off < SURF_VIDEO_GPAL + 4 * SURF_VIDEO_GPAL_COUNT) {
        disp.gpal[(off - SURF_VIDEO_GPAL) / 4] = (uint16_t)(v & 0xFFF);
    } else {
        fprintf(stderr, "gpuscenes_expect: a scene wrote video offset 0x%03x, which the model does not keep\n", off);
        problems++;
    }
}

static void m_text(uint32_t cell, uint32_t v)
{
    if (cell < 4096)
        textram[cell] = v;
}

static const struct gpu_sink model_sink = {m_cmd, m_sync, m_vram32, m_vram8, m_video, m_text};

static void write_ppm(const char *dir, unsigned n)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/scene-%02u.ppm", dir, n);
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "gpuscenes_expect: cannot write %s\n", path);
        problems++;
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", GPU_SCREEN_W, GPU_SCREEN_H);
    for (int i = 0; i < GPU_SCREEN_W * GPU_SCREEN_H; i++) {
        uint8_t px[3] = {(uint8_t)(((frame[i] >> 8) & 15) * 17), (uint8_t)(((frame[i] >> 4) & 15) * 17),
                         (uint8_t)((frame[i] & 15) * 17)};
        fwrite(px, 1, 3, f);
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    const char *font_path = NULL, *out = NULL, *ppm = NULL;
    int list = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--font") && i + 1 < argc) font_path = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--ppm") && i + 1 < argc) ppm = argv[++i];
        else if (!strcmp(argv[i], "--list")) list = 1;
        else {
            fprintf(stderr, "usage: %s --font driver/font8x16.c --out FILE [--ppm DIR] [--list]\n", argv[0]);
            return 2;
        }
    }
    if (!font_path || !load_font(font_path)) {
        fprintf(stderr, "gpuscenes_expect: no font (--font driver/font8x16.c)\n");
        return 2;
    }
    gpu_display_reset(&disp, 1);
    gpu_engine_init(&eng, vram);
    uint32_t crc[64];
    if (gpu_scene_count > 64)
        return 2;
    for (unsigned n = 0; n < gpu_scene_count; n++) {
        gpu_scenes[n].draw(&model_sink);
        if (!gpu_engine_idle(&eng) || (eng.sticky & (GPU_ST_BAD_OPCODE | GPU_ST_BAD_PARAM))) {
            fprintf(stderr, "gpuscenes_expect: scene %u (%s) leaves the engine %s\n", n, gpu_scenes[n].name,
                    gpu_engine_idle(&eng) ? "with an error" : "inside a command");
            problems++;
        }
        gpu_display_render(&disp, vram, textram, (const uint8_t(*)[16])font, frame);
        crc[n] = gpu_frame_crc(frame);
        if (ppm)
            write_ppm(ppm, n);
        if (list)
            printf("scene %2u %-36s crc %08x\n", n, gpu_scenes[n].name, crc[n]);
    }
    if (problems)
        return 1;
    if (out) {
        FILE *f = fopen(out, "w");
        if (!f)
            return 1;
        fprintf(f, "/* generated by firmware/tests/gpuscenes_expect.c from gpuscenes.c and the C model -- do not "
                   "edit */\n#define GPU_SCENE_COUNT %u\nstatic const uint32_t gpu_scene_expect[GPU_SCENE_COUNT] = {\n",
                gpu_scene_count);
        for (unsigned n = 0; n < gpu_scene_count; n++)
            fprintf(f, "    0x%08xu,   /* %2u %s */\n", crc[n], n, gpu_scenes[n].name);
        fprintf(f, "};\n/* the checks of a complete gpudiag run (gpuscenes.h) */\n#define GPUDIAG_CHECKS %uu\n",
                GPUDIAG_SECTION_CHECKS + GPUDIAG_SCENE_CHECKS * gpu_scene_count);
        fclose(f);
    }
    return 0;
}
