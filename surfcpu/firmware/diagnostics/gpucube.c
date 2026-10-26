/*
SurfGPU diagnostics: the spinning cube
--------------------
File: gpucube.c Date: 10/25/26 (SurfGPU GPU2)
--------------------
gpucube.h's cube. The corners are (+-1, +-1, +-1) in Q12 (4096 = 1), turned about y by k + 7 and
about x by 2k + 11 steps of a 128-step turn (sines in Q14 from a quarter-wave table; the offsets
tilt it so that three faces show), seen from 4.5 units away with a focal length of 160 pixels,
into the 320x240 picture's 12.4 coordinates (at most 100 pixels from the centre); a corner's depth
maps its distance along z to 0 (near) .. 65535 (far), and its shade (11 .. 31 of a face's ramp of
32) grows as it comes nearer, so each face's colours change across it (Gouraud). Plain integer C
for RV32 and the host: only +, -, *, / and >> of int32_t values that never overflow (GCC's >> of
a negative number is arithmetic on both).
*/
#include "gpucube.h"
#include "surfcpu/gpu.h"
#include "surfcpu/soc.h"

#define CUBE_S   4096                  /* half a side, 1.0 in Q12 */
#define CUBE_R   7094                  /* sqrt(3) * 4096: the farthest a corner gets from the centre */
#define CUBE_D   18432                 /* the eye's distance from the centre, 4.5 */
#define CUBE_F16 2560                  /* 160 pixels of focal length, in sixteenths */

/* 16384 sin(2 pi i / 128), i = 0 .. 32 */
static const int16_t q_sin[33] = {0, 804, 1606, 2404, 3196, 3981, 4756, 5520, 6270, 7005, 7723, 8423, 9102,
                                  9760, 10394, 11003, 11585, 12140, 12665, 13160, 13623, 14053, 14449, 14811,
                                  15137, 15426, 15679, 15893, 16069, 16207, 16305, 16364, 16384};

static int32_t sin128(uint32_t a)
{
    a &= 127u;
    if (a <= 32u) return q_sin[a];
    if (a <= 64u) return q_sin[64u - a];
    if (a <= 96u) return -q_sin[a - 64u];
    return -q_sin[128u - a];
}

static int32_t cos128(uint32_t a) { return sin128(a + 32u); }

/* (a c - b s) / 16384, rounded to the nearest */
static int32_t turn(int32_t a, int32_t b, int32_t c, int32_t s) { return (a * c - b * s + 8192) >> 14; }

/* the faces as corners (bit 0 x, bit 1 y, bit 2 z set: +1), each cut along its first diagonal */
static const uint8_t faces[6][4] = {{0, 1, 3, 2}, {4, 6, 7, 5}, {0, 2, 6, 4}, {1, 5, 7, 3}, {0, 4, 5, 1}, {2, 3, 7, 6}};
/* each face's ramp colour: red, green, blue, yellow, cyan, magenta */
static const uint8_t hue[6][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {1, 1, 0}, {0, 1, 1}, {1, 0, 1}};
static const uint16_t cga[16] = {0x000, 0x00A, 0x0A0, 0x0AA, 0xA00, 0xA0A, 0xA50, 0xAAA,
                                 0x555, 0x55F, 0x5F5, 0x5FF, 0xF55, 0xF5F, 0xFF5, 0xFFF};

const char gpu_cube_bar[81] = " GPUDIAG: PASS   the SurfGPU cube: Gouraud triangles, a depth buffer, page flips";

void gpu_cube_display(const struct gpu_sink *s, int bar)
{
    for (uint32_t n = 0; n < 256u; n++) {
        uint32_t c = 0;
        if (n < 192u) {                                 /* ramp n / 32, shade n % 32 */
            const uint32_t f = n / 32u, lv = ((n % 32u) * 15u + 15u) / 31u;
            c = (hue[f][0] * lv) << 8 | (hue[f][1] * lv) << 4 | hue[f][2] * lv;
        } else if (n == GPU_CUBE_BG) {
            c = 0x113;                                  /* a dark blue */
        }
        s->video(SURF_VIDEO_GPAL + 4u * n, c);
    }
    for (uint32_t i = 0; i < 16u; i++)
        s->video(SURF_VIDEO_PALETTE + 4u * i, cga[i]);
    s->video(SURF_VIDEO_SCROLL, 0);
    s->video(SURF_VIDEO_CURSOR, 0);
    for (uint32_t i = 0; i < 2400u; i++)                /* blank: background 0 lets the picture through */
        s->text(i, (bar && i < 80u) ? (2u << 12 | 15u << 8 | (uint8_t)gpu_cube_bar[i]) : (uint32_t)' ');
    s->video(SURF_VIDEO_GFX_STRIDE, 320);
    s->video(SURF_VIDEO_GFX_SCALE, 2);
    s->video(SURF_VIDEO_CTRL, SURF_VIDEO_CTRL_ENABLE_MASK | ((bar ? 3u : 2u) << SURF_VIDEO_CTRL_MODE_LSB));
}

static void cmd(const struct gpu_sink *s, uint32_t w) { s->cmd(w); }

void gpu_cube_frame(const struct gpu_sink *s, uint32_t k, unsigned b, uint32_t fence)
{
    const uint32_t base = b ? GPU_CUBE_B : GPU_CUBE_A, a = k % GPU_CUBE_PERIOD;
    const int32_t cy = cos128(a + 7u), sy = sin128(a + 7u), cx = cos128(2u * a + 11u), sx = sin128(2u * a + 11u);
    int32_t vx[8], vy[8], vz[8], shade[8];
    for (unsigned i = 0; i < 8; i++) {
        const int32_t x = (i & 1u) ? CUBE_S : -CUBE_S, y = (i & 2u) ? CUBE_S : -CUBE_S, z = (i & 4u) ? CUBE_S : -CUBE_S;
        const int32_t x1 = turn(x, -z, cy, sy), z1 = turn(z, x, cy, sy);       /* about y */
        const int32_t y2 = turn(y, z1, cx, sx), z2 = turn(z1, -y, cx, sx);     /* then about x */
        vx[i] = 160 * 16 + CUBE_F16 * x1 / (z2 + CUBE_D);
        vy[i] = 120 * 16 + CUBE_F16 * y2 / (z2 + CUBE_D);
        int32_t d = (z2 + CUBE_R) * 65535 / (2 * CUBE_R), sh = 31 - (z2 + CUBE_R) * 20 / (2 * CUBE_R);
        vz[i] = d < 0 ? 0 : d > 65535 ? 65535 : d;
        shade[i] = sh < 11 ? 11 : sh > 31 ? 31 : sh;
    }
    cmd(s, GPU_HDR(GPU_OP_SET_TARGET, 0));                      /* the depth buffer's bytes */
    cmd(s, GPU_CUBE_Z);
    cmd(s, 640);
    cmd(s, GPU_WH(640, 240));
    cmd(s, GPU_HDR(GPU_OP_FILL_RECT, 0xFF));                    /* every depth 0xFFFF */
    cmd(s, GPU_XY(0, 0));
    cmd(s, GPU_WH(640, 240));
    cmd(s, GPU_HDR(GPU_OP_SET_TARGET, 0));                      /* the picture */
    cmd(s, base);
    cmd(s, 320);
    cmd(s, GPU_WH(320, 240));
    cmd(s, GPU_HDR(GPU_OP_FILL_RECT, GPU_CUBE_BG));
    cmd(s, GPU_XY(0, 0));
    cmd(s, GPU_WH(320, 240));
    cmd(s, GPU_HDR(GPU_OP_SET_ZBUF, 0));                        /* after the SET_TARGET */
    cmd(s, GPU_CUBE_Z);
    cmd(s, 640);
    for (unsigned f = 0; f < 6; f++)
        for (unsigned t = 0; t < 2; t++) {
            const unsigned c3[3] = {faces[f][0], faces[f][1 + t], faces[f][2 + t]};
            cmd(s, GPU_HDR(GPU_OP_TRIANGLE, GPU_TRI_GOURAUD | GPU_TRI_ZTEST | GPU_TRI_ZWRITE));
            for (unsigned i = 0; i < 3; i++) {
                cmd(s, GPU_XY(vx[c3[i]], vy[c3[i]]));
                cmd(s, GPU_ZC(vz[c3[i]], 32u * f + (uint32_t)shade[c3[i]]));
            }
        }
    cmd(s, GPU_HDR(GPU_OP_FLIP, 0));
    cmd(s, base);
    cmd(s, GPU_HDR(GPU_OP_FENCE, 0));
    cmd(s, fence);
}
