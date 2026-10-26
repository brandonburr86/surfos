/*
SurfOS host tool: gfx commands replayed in the C model
--------------------
File: gfxscene.c    Date: 10/25/26 (SurfGPU G3)
--------------------
  gfxscene [--ppm DIR] SCRIPT

Runs the gfx command lines of SCRIPT (one per line; empty lines and lines starting with # are
skipped) through the same gfx.c and gpu.c that SurfOS runs, on the stand-in of gpustub.c, whose
engine and display are sim/models/gpu_model.c, from the state SurfOS starts in (text mode,
GFX_BASE 0, GFX_STRIDE 640, GFX_SCALE 1, GPAL RGB332, the reset target). For each `gfx crc` it
prints

  expect <n> <crc> mode <m>

the CRC the C model computes for the picture shown (n counts the gfx crc lines from 1, m is the
display mode then): what SurfOS must print for the same commands on the RTL system, where the
picture is graphics alone (mode 2; in modes 1 and 3 the console's text is part of the frame,
which the model here has not got). With --ppm each of those frames is written as DIR/crc-<n>.ppm.
Since it runs the same gfx.c and gpu.c as SurfOS, test-gpuos takes its expectation from gfxref.c
(the pictures from GPU.md alone) and checks that gfxscene agrees.
Everything gfx says goes to the output too, after "> " and the line. Exit status 0 when every
command returned CMD_OK.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "surfcpu/soc.h"
#include "gpustub.h"
#include "../../../../shell/shell.h"

static uint16_t frame[GPU_SCREEN_W * GPU_SCREEN_H];

static int write_ppm(const char *path, const uint16_t *f) {
    FILE *o = fopen(path, "wb");
    int i;
    if(!o) return -1;
    fprintf(o, "P6\n%d %d\n255\n", GPU_SCREEN_W, GPU_SCREEN_H);
    for(i = 0; i < GPU_SCREEN_W * GPU_SCREEN_H; i++) {
        unsigned char rgb[3] = { (unsigned char)((f[i] >> 8 & 15) * 17), (unsigned char)((f[i] >> 4 & 15) * 17),
                                 (unsigned char)((f[i] & 15) * 17) };
        fwrite(rgb, 1, 3, o);
    }
    return fclose(o);
}

int main(int argc, char **argv) {
    static struct stub s;
    struct gpu g;
    const char *ppm = NULL, *script = NULL;
    char line[600], path[1024];
    FILE *in;
    int i, bad = 0, crcs = 0;
    for(i = 1; i < argc; i++) {
        if(!strcmp(argv[i], "--ppm") && i + 1 < argc) ppm = argv[++i];
        else script = argv[i];
    }
    if(!script || !(in = fopen(script, "r"))) {
        fprintf(stderr, "usage: gfxscene [--ppm DIR] SCRIPT\n");
        return 2;
    }
    stub_init(&s, 1);
    if(gpu_probe(&g, &s.port, SURF_SYSINFO_FEATURES_GPU_MASK | SURF_SYSINFO_FEATURES_VIDEO_MASK |
                 SURF_SYSINFO_FEATURES_IRQ_MASK) != GPU_OK) {
        fprintf(stderr, "gfxscene: the stand-in is no GPU?\n");
        return 1;
    }
    while(fgets(line, sizeof(line), in)) {
        char *e = line + strlen(line);
        int r;
        while(e > line && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ')) *--e = 0;
        if(!line[0] || line[0] == '#') continue;
        r = stub_gfx(&s, &g, line);
        printf("> %s\n%s", line, s.out);
        if(r != CMD_OK) {
            printf("gfxscene: '%s' returned %d\n", line, r);
            bad++;
        }
        if(!strcmp(line, "gfx crc")) {
            const char *p = strstr(s.out, "gfx crc: frame ");
            unsigned long crc = 0;
            if(p && (p = strstr(p, ", crc "))) crc = strtoul(p + 6, NULL, 16);
            crcs++;
            printf("expect %d %08lx mode %u\n", crcs, crc, (unsigned)s.reg.mode);
            if(ppm) {
                struct stub_disp d = stub_shown(&s, s.crc_frame);
                stub_render(&s, &d, frame);
                snprintf(path, sizeof(path), "%s/crc-%d.ppm", ppm, crcs);
                if(write_ppm(path, frame)) {
                    fprintf(stderr, "gfxscene: cannot write %s\n", path);
                    bad++;
                }
            }
        }
    }
    fclose(in);
    printf("gfxscene: %d gfx crc, %d commands failed\n", crcs, bad);
    stub_free(&s);
    return bad ? 1 : 0;
}
