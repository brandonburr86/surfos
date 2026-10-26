/*
SurfGPU diagnostics: the scenes' expected CRCs
--------------------
File: gpuscenes_expect.c Date: 10/18/26 (SurfGPU), 10/25/26 (GPU2), 10/26/26 (the review of 3fea963)
--------------------
A host tool of the firmware build: runs gpudiag's scenes (firmware/diagnostics/gpuscenes.c) in
order through the C model (sim/models/gpu_model.c), as SurfCPU runs them through the hardware,
and writes the FRAME_CRC each must give as a C header for gpudiag (build/fw/gen/
gpuscenes_expect.h). The text modes need the font: driver/font8x16.c, which tools/fontrom.py
check keeps equal to the video block's font ROM. With --ppm DIR it also writes each scene's
frame as DIR/scene-NN.ppm (what the monitor should show).
GPU2: a FLIP the engine takes becomes the display's GFX_BASE, as surf_video takes it at the next
frame latch; and the cube's frames (firmware/diagnostics/gpucube.c) are drawn too: the 16 of
gpudiag's checks in MODE 2 (gpu_cube_expect_check) and all 128 of a turn under the result bar in
MODE 3 (gpu_cube_expect_run, the free run after a PASS), each frame's CRC as shown after its FLIP
(with --ppm also cube-NNN.ppm, the free run's). It fails, and writes no header, when two frames of
either table have the same CRC (gpudiag knows a frame by its CRC: the first cube's frames 9 and 73
were the same picture, the review of 3fea963's L1) or when a frame of the turn shows fewer than two
of the cube's faces (gpucube.c says why that would let two frames be the same picture).

    gpuscenes_expect --font driver/font8x16.c --out FILE [--ppm DIR] [--list]
*/
#include "gpu_model.h"
#include "gpucube.h"
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

static unsigned flips_seen;
static void m_cmd(uint32_t w)
{
    gpu_engine_word(&eng, w);
    if (eng.flips != flips_seen) {                      /* FLIP: GFX_BASE from the next frame on */
        flips_seen = eng.flips;
        disp.gfx_base = eng.flip_base;
    }
}
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

static void write_ppm(const char *dir, const char *kind, unsigned n)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s-%02u.ppm", dir, kind, n);
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
            write_ppm(ppm, "scene", n);
        if (list)
            printf("scene %2u %-36s crc %08x\n", n, gpu_scenes[n].name, crc[n]);
    }
    /* the cube: the checked frames (MODE 2), then a turn's frames under the bar (MODE 3), each
     * drawn into the buffer gpudiag draws it into and shown by its FLIP */
    uint32_t cube_check[GPU_CUBE_CHECK_FRAMES], cube_run[GPU_CUBE_PERIOD];
    unsigned faces_seen[4] = {0, 0, 0, 0};              /* frames of the turn showing 0 .. 3 faces */
    for (int pass = 0; pass < 2; pass++) {
        gpu_cube_display(&model_sink, pass);
        const uint32_t frames = pass ? GPU_CUBE_PERIOD : GPU_CUBE_CHECK_FRAMES;
        for (uint32_t k = 0; k < frames; k++) {
            gpu_cube_frame(&model_sink, k, k & 1u, k + 1);
            if (!gpu_engine_idle(&eng) || (eng.sticky & (GPU_ST_BAD_OPCODE | GPU_ST_BAD_PARAM)) ||
                eng.flip_base != ((k & 1u) ? GPU_CUBE_B : GPU_CUBE_A)) {
                fprintf(stderr, "gpuscenes_expect: cube frame %u leaves the engine wrong\n", k);
                problems++;
            }
            if (pass) {                                 /* the faces in the picture: ramp n / 32 of 0 .. 5 */
                const uint8_t *pic = vram + ((k & 1u) ? GPU_CUBE_B : GPU_CUBE_A);
                unsigned n[6] = {0, 0, 0, 0, 0, 0}, faces = 0;
                for (uint32_t i = 0; i < 320u * 240u; i++)
                    if (pic[i] < 192u)
                        n[pic[i] / 32u]++;
                for (unsigned f = 0; f < 6; f++)
                    faces += n[f] != 0;
                faces_seen[faces > 3 ? 3 : faces]++;
                if (faces < 2) {
                    fprintf(stderr, "gpuscenes_expect: cube frame %u shows %u of the cube's faces, not two or three\n",
                            k, faces);
                    problems++;
                }
            }
            gpu_display_render(&disp, vram, textram, (const uint8_t(*)[16])font, frame);
            (pass ? cube_run : cube_check)[k] = gpu_frame_crc(frame);
            if (ppm && pass && k % 16 == 0)
                write_ppm(ppm, "cube", k);
        }
        const uint32_t *crcs = pass ? cube_run : cube_check;
        for (uint32_t i = 0; i < frames; i++)
            for (uint32_t j = i + 1; j < frames; j++)
                if (crcs[i] == crcs[j]) {
                    fprintf(stderr, "gpuscenes_expect: cube frames %u and %u (%s) are the same picture, CRC %08x\n", i, j,
                            pass ? "under the bar" : "checked", crcs[i]);
                    problems++;
                }
        if (list)
            printf("cube %s: %u frames, the first %08x, the last %08x\n", pass ? "under the bar" : "checked", frames,
                   crcs[0], crcs[frames - 1]);
    }
    if (list)
        printf("cube: of the %u frames of a turn, %u show three faces, %u two, %u one or none\n", GPU_CUBE_PERIOD,
               faces_seen[3], faces_seen[2], faces_seen[0] + faces_seen[1]);
    if (eng.tri_clamped || eng.tri_wide) {
        fprintf(stderr, "gpuscenes_expect: the model's clamp changed %llu values, %llu were wide\n", eng.tri_clamped,
                eng.tri_wide);
        problems++;
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
        fprintf(f, "};\n/* the cube (gpucube.c): its checked frames in MODE 2, and a turn under the bar in MODE 3 */\n");
        fprintf(f, "static const uint32_t gpu_cube_expect_check[%u] = {\n", GPU_CUBE_CHECK_FRAMES);
        for (unsigned k = 0; k < GPU_CUBE_CHECK_FRAMES; k++)
            fprintf(f, "    0x%08xu,%s", cube_check[k], k % 6 == 5 ? "\n" : "");
        fprintf(f, "\n};\nstatic const uint32_t gpu_cube_expect_run[%u] = {\n", GPU_CUBE_PERIOD);
        for (unsigned k = 0; k < GPU_CUBE_PERIOD; k++)
            fprintf(f, "    0x%08xu,%s", cube_run[k], k % 6 == 5 ? "\n" : "");
        fprintf(f, "\n};\n/* the checks of a complete gpudiag run, a first one and a rerun (gpuscenes.h) */\n"
                   "#define GPUDIAG_CHECKS %uu\n#define GPUDIAG_RERUN_CHECKS %uu\n",
                GPUDIAG_SECTION_CHECKS + GPUDIAG_SCENE_CHECKS * gpu_scene_count,
                GPUDIAG_SECTION_CHECKS + GPUDIAG_SCENE_CHECKS * gpu_scene_count - GPUDIAG_FIRST_RUN_CHECKS);
        fclose(f);
    }
    return 0;
}
