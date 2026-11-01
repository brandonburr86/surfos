/*
SurfGPU command encodings
--------------------
File: gpu.h Date: 10/18/26 (SurfGPU), 10/25/26 (GPU2)
--------------------
The gpu engine's command words (docs/GPU.md sections 5 and 16), for C: the firmware,
the C reference model (sim/models/gpu_model.c) and the tests. The registers themselves are in
the generated surfcpu/soc.h (SURF_GPU_*, SURF_VIDEO_GFX_*, ...). rtl/peripherals/surf_gpu.v
uses the same values; sim/gpu/check_gpu_h.py compares the two.

A command is a header word, GPU_HDR(op, bits), then its parameter words, written in order to
gpu.CMD: coordinates are GPU_XY(x, y) (signed 16-bit each), sizes GPU_WH(w, h). TRIANGLE's
vertices are GPU_XY(x, y) too, with x and y in signed 12.4 fixed point (sixteenths of a pixel,
GPU_FX(pixels) for whole pixels), each followed by GPU_ZC(depth, colour).

GPU2 (0x32555047) is GPU1's command set (0x31555047) unchanged, plus SET_ZBUF, TRIANGLE and
FLIP (opcodes 0x0A to 0x0C); a GPU1 program runs on it as it did.
*/
#ifndef SURFCPU_GPU_H
#define SURFCPU_GPU_H

#include "surfcpu/soc.h"            /* SURF_VRAM_BYTES: 300 KiB of the 512 KiB vram window */

/* opcodes (header bits 31:24) and their parameter words */
#define GPU_OP_NOP          0x00u   /* -                                                  */
#define GPU_OP_SET_TARGET   0x01u   /* base; stride; GPU_WH(w, h)                         */
#define GPU_OP_SET_CLIP     0x02u   /* GPU_XY(x0, y0); GPU_XY(x1, y1) (unsigned)          */
#define GPU_OP_FILL_RECT    0x03u   /* GPU_XY(x, y); GPU_WH(w, h)          hdr 7:0 colour */
#define GPU_OP_COPY_RECT    0x04u   /* GPU_XY(sx, sy); GPU_XY(dx, dy); GPU_WH(w, h)       */
#define GPU_OP_LINE         0x05u   /* GPU_XY(x0, y0); GPU_XY(x1, y1)      hdr 7:0 colour */
#define GPU_OP_EXPAND       0x06u   /* GPU_XY(x, y); GPU_WH(w, h); ceil(w h / 32) data    */
#define GPU_OP_WRITE_RECT   0x07u   /* GPU_XY(x, y); GPU_WH(w, h); ceil(w h / 4) data     */
#define GPU_OP_FENCE        0x08u   /* value                                   hdr 0 IRQ  */
#define GPU_OP_WAIT_VBLANK  0x09u   /* -                                                  */
#define GPU_OP_SET_ZBUF     0x0Au   /* base; stride (GPU2: the depth buffer)              */
#define GPU_OP_TRIANGLE     0x0Bu   /* 3 x (GPU_XY(x, y) in 12.4; GPU_ZC(z, c))  GPU2     */
#define GPU_OP_FLIP         0x0Cu   /* base (GPU2: video GFX_BASE at the next blanking)   */

/* header bits */
#define GPU_COPY_KEY        (1u << 8)       /* COPY_RECT: source pixels equal to hdr 7:0 are not copied */
#define GPU_EXPAND_TRANSP   (1u << 16)      /* EXPAND: clear bits draw nothing (else hdr 15:8)          */
#define GPU_FENCE_IRQ       (1u << 0)       /* FENCE: also set STATUS.FENCE_EV                          */
/* TRIANGLE (hdr 7:0 the flat colour) */
#define GPU_TRI_GOURAUD     (1u << 8)       /* the vertices' colours interpolated (else hdr 7:0)        */
#define GPU_TRI_ZTEST       (1u << 9)       /* draw a pixel only where its depth passes the buffer's    */
#define GPU_TRI_ZFUNC_LE    (1u << 10)      /* ZTEST passes on less or equal (else on less)             */
#define GPU_TRI_ZWRITE      (1u << 11)      /* write the depth of every pixel drawn                     */
#define GPU_TRI_CULL_CW     (1u << 12)      /* draw nothing when clockwise on the screen (y down)       */
#define GPU_TRI_CULL_CCW    (1u << 13)      /* draw nothing when counter-clockwise on the screen        */

#define GPU_HDR(op, bits)   (((unsigned)(op) << 24) | (unsigned)(bits))
#define GPU_XY(x, y)        (((unsigned)(x) & 0xFFFFu) | (((unsigned)(y) & 0xFFFFu) << 16))
#define GPU_WH(w, h)        (((unsigned)(w) & 0xFFFFu) | (((unsigned)(h) & 0xFFFFu) << 16))
#define GPU_ZC(z, c)        (((unsigned)(z) & 0xFFFFu) | (((unsigned)(c) & 0xFFu) << 16))
#define GPU_FX(px)          ((int)(px) * 16)        /* whole pixels in 12.4 */
#define GPU_ID_VALUE        0x32555047u     /* gpu.ID: "GPU2", this command set */
#define GPU_ID_GPU1         0x31555047u     /* "GPU1": the G2 engine (no SET_ZBUF, TRIANGLE, FLIP) */

#endif /* SURFCPU_GPU_H */
