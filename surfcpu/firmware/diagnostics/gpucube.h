/*
SurfGPU diagnostics: the spinning cube
--------------------
File: gpucube.h Date: 10/25/26 (SurfGPU GPU2)
--------------------
gpudiag's cube (docs/GPU.md section 16.9): the CPU turns the eight corners of a cube in fixed
point (integers only: the same numbers on SurfCPU and on the host), and the engine draws its six
faces as twelve Gouraud triangles with depth into the back buffer of the 320x240 double-buffered
layout (section 16.6), then FLIPs to it. Everything goes through a gpu_sink (gpuscenes.h), so the
same code draws on the hardware (gpudiag.c) and in the C model (firmware/tests/gpuscenes_expect.c,
which computes every frame's FRAME_CRC at build time). A frame depends only on its number k modulo
GPU_CUBE_PERIOD: each one clears its buffer and the depth buffer first.
*/
#ifndef SURFGPU_GPUCUBE_H
#define SURFGPU_GPUCUBE_H

#include <stdint.h>

#include "gpuscenes.h"

#define GPU_CUBE_PERIOD       128u      /* frames in one turn */
#define GPU_CUBE_CHECK_FRAMES 16u       /* the frames gpudiag's checks draw at full rate */
#define GPU_CUBE_FRAME_WORDS  105u      /* CMD words of one frame */
#define GPU_CUBE_A            0u        /* picture A, picture B, the depth buffer (section 16.6) */
#define GPU_CUBE_B            76800u
#define GPU_CUBE_Z            153600u
#define GPU_CUBE_BG           255u      /* the background's GPAL entry */

/* The cube's display: GPAL (six ramps of 32 shades and the background), the text palette, SCROLL
 * and CURSOR 0, every text cell blank, GFX_STRIDE 320 and GFX_SCALE 2; with bar, the result bar of
 * a passing run on text row 0 (gpu_cube_bar) and MODE 3, else MODE 2. GFX_BASE is left to the
 * FLIPs. */
void gpu_cube_display(const struct gpu_sink *s, int bar);
extern const char gpu_cube_bar[81];

/* Frame k into picture A (b = 0) or B (b = 1): the depth buffer cleared, the picture cleared to
 * the background, the twelve triangles, FLIP to it, then a FENCE of fence: GPU_CUBE_FRAME_WORDS
 * words, ending without a sync. */
void gpu_cube_frame(const struct gpu_sink *s, uint32_t k, unsigned b, uint32_t fence);

#endif
