/*
SurfGPU command encodings
--------------------
File: gpu.h Date: 10/18/26 (SurfGPU)
--------------------
The gpu engine's command words (docs/GPU.md section 5) and VRAM's size, for C: the firmware,
the C reference model (sim/models/gpu_model.c) and the tests. The registers themselves are in
the generated surfcpu/soc.h (SURF_GPU_*, SURF_VIDEO_GFX_*, ...). rtl/peripherals/surf_gpu.v
uses the same values; sim/gpu/check_gpu_h.py compares the two.

A command is a header word, GPU_HDR(op, bits), then its parameter words, written in order to
gpu.CMD: coordinates are GPU_XY(x, y) (signed 16-bit each), sizes GPU_WH(w, h).
*/
#ifndef SURFCPU_GPU_H
#define SURFCPU_GPU_H

#define SURF_VRAM_BYTES 307200u     /* VRAM: 300 KiB of the 512 KiB vram window */

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

/* header bits */
#define GPU_COPY_KEY        (1u << 8)       /* COPY_RECT: source pixels equal to hdr 7:0 are not copied */
#define GPU_EXPAND_TRANSP   (1u << 16)      /* EXPAND: clear bits draw nothing (else hdr 15:8)          */
#define GPU_FENCE_IRQ       (1u << 0)       /* FENCE: also set STATUS.FENCE_EV                          */

#define GPU_HDR(op, bits)   (((unsigned)(op) << 24) | (unsigned)(bits))
#define GPU_XY(x, y)        (((unsigned)(x) & 0xFFFFu) | (((unsigned)(y) & 0xFFFFu) << 16))
#define GPU_WH(w, h)        (((unsigned)(w) & 0xFFFFu) | (((unsigned)(h) & 0xFFFFu) << 16))
#define GPU_ID_VALUE        0x31555047u     /* gpu.ID: "GPU1" */

#endif /* SURFCPU_GPU_H */
