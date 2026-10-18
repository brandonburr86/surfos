/*
SurfGPU diagnostics: the scenes
--------------------
File: gpuscenes.h Date: 10/18/26 (SurfGPU)
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
 * the engine's registers and errors 9 and the scene count 1, then 3 for each scene. gpudiag fails
 * a run that made any other number (a section skipped or cut short), and gpuscenes_expect.c writes
 * the total into gpuscenes_expect.h as GPUDIAG_CHECKS, which gpudiag_check.py wants in the PASS
 * line. Change these with the checks. */
#define GPUDIAG_SECTION_CHECKS (5u + 59u + 4u + 9u + 1u)
#define GPUDIAG_SCENE_CHECKS 3u

#endif
