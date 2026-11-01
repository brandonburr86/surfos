/*
SurfGPU diagnostics: the scenes
--------------------
File: gpuscenes.h Date: 10/18/26 (SurfGPU), 10/25/26 (GPU2)
--------------------
The pictures gpudiag draws and checks by FRAME_CRC (docs/GPU.md section 10). A scene draws only
through a gpu_sink, so the same code runs in three places: on SurfCPU (gpudiag.c: the gpu, video,
VRAM and text RAM registers), in the C model on the host (gpuscenes_expect.c: the expected
CRCs, at build time) and in the RTL tests. Scenes run in order, each on what the earlier ones
left, and each sets every video register its picture depends on. No scene blinks the cursor
(that would make the picture depend on the frame).
*/
#ifndef SURFGPU_GPUSCENES_H
#define SURFGPU_GPUSCENES_H

#include <stdint.h>

struct gpu_sink {
    void (*cmd)(uint32_t w);                        /* a word to gpu.CMD (waits for FIFO room) */
    void (*sync)(void);                             /* wait until the engine is idle (STATUS.BUSY 0) */
    void (*vram32)(uint32_t off, uint32_t v);       /* a word through the VRAM window */
    void (*vram8)(uint32_t off, uint32_t v);        /* a byte through the VRAM window */
    void (*video)(uint32_t off, uint32_t v);        /* a video register (SURF_VIDEO_*) */
    void (*text)(uint32_t cell, uint32_t v);        /* a text RAM cell */
};

struct gpu_scene {
    const char *name;
    int engine;                                     /* 1: needs the drawing engine */
    void (*draw)(const struct gpu_sink *s);
};

extern const struct gpu_scene gpu_scenes[];
extern const unsigned gpu_scene_count;

/* The checks of a complete gpudiag run: identity 5, the VRAM window 59, the video registers 4,
 * the engine's registers and errors 14, GPU2's commands 5, the scene count 1 and the FRAME pair 2
 * (with scene 1), the cube 6, then 3 for each scene. One of the video registers' (the GFX_ values a
 * reset leaves) is made only by the
 * first run after a reset: a rerun (Ctrl-R) cannot see reset values, says so, and makes one check
 * fewer. gpudiag fails a run that made any other number than its kind's (a section skipped or cut
 * short), and gpuscenes_expect.c writes both totals into gpuscenes_expect.h (GPUDIAG_CHECKS,
 * GPUDIAG_RERUN_CHECKS), which gpudiag_check.py wants in the PASS line. Change these with the
 * checks. */
#define GPUDIAG_SECTION_CHECKS (5u + 59u + 4u + 14u + 5u + 1u + 2u + 6u)
#define GPUDIAG_SCENE_CHECKS 3u
#define GPUDIAG_FIRST_RUN_CHECKS 1u

#endif
