/*
SurfOS Graphics Commands
----------------------
File: graphics.c  Date: 10/10/26
----------------------
The basic 2D framebuffer driver, exercised from a local shell. No mode setting:
GRUB keeps the panel in the firmware's selected mode.
*/

#include <stdio.h>
#include <surfos/task.h>
#include <surfos/timer.h>
#include <sys/framebuffer.h>
#include "shell.h"

int cmd_gfxinfo(int argc, char **argv) {
    char description[200];
    if(argc != 1) return CMD_USAGE;
    printf("    graphics: software 2D (pixels, rectangles, lines, RGB images)\n");
    if(!framebuffer_active()) printf("    no framebuffer: boot the UEFI USB image for graphics\n");
    else printf("    %s\n", framebuffer_describe(description, sizeof(description)));
    return CMD_OK;
}

static void test_pattern(const struct fb_surface *s, void *arg) {
    static const u32 colors[] = { 0xFF0000, 0x00FF00, 0x0000FF, 0xFFFF00,
                                  0x00FFFF, 0xFF00FF, 0xFFFFFF, 0x000000 };
    static const u32 image[] = { 0xFF8000, 0xFFFFFF, 0x00FFFF, 0x8000FF };
    const struct fb_bitmap bitmap = { image, 2, 2, 2, sizeof(image) };
    const char *title = "SurfOS 2D graphics";
    const char *hint = "Ctrl+C or another console: return to text";
    u32 w = s->mode.width, h = s->mode.height, i;
    fb_rect_rgb(s, 0, 0, w, h, 0x102030);
    for(i = 0; title[i]; i++) fb_draw_glyph(s, 2 + i, 1, title[i], 0x0F, false);
    for(i = 0; hint[i]; i++) fb_draw_glyph(s, 2 + i, 3, hint[i], 0x07, false);
    for(i = 0; i < 8; i++)
        fb_rect_rgb(s, i * w / 8, h / 4, (i + 1) * w / 8 - i * w / 8, h / 4, colors[i]);
    fb_line_rgb(s, -50, h / 2 + 16, w + 50, h - 16, 0xFFFFFF);
    fb_line_rgb(s, -50, h - 16, w + 50, h / 2 + 16, 0x00FFFF);
    fb_rect_rgb(s, w / 2 - 32, 3 * h / 4 - 32, 64, 64, 0x204060);
    fb_blit_rgb(s, w / 2, 3 * h / 4, &bitmap);
    fb_pixel_rgb(s, w - 1, h - 1, 0xFFFFFF);
}

int cmd_gfxtest(int argc, char **argv) {
    u32 seconds = 5, start;
    int result = CMD_OK;
    if(argc > 2) goto usage;
    if(argc == 2) {
        const char *p = argv[1];
        if(!*p) goto usage;
        seconds = 0;
        while(*p) {
            if(*p < '0' || *p > '9') goto usage;
            seconds = seconds * 10 + (*p++ - '0');
            if(seconds > 60) goto usage;
        }
        if(seconds < 1) goto usage;
    }
    /* This may ask --More--. Do it while text is visible, before taking the
       screen; no printing or other potentially blocking I/O during graphics. */
    printf("    graphics test: %u seconds (Ctrl+C or switch consoles to end)\n", seconds);
    if(curTask->intr) return CMD_FAIL;
    if(framebuffer_graphics_begin() != 0) {
        printf("    graphics unavailable: use the active local framebuffer console; only one test at a time\n");
        return CMD_FAIL;
    }
    if(framebuffer_graphics_draw(test_pattern, NULL) != 0) result = CMD_FAIL;
    else {
        start = getticks();
        while(framebuffer_graphics_owned() && (u32)(getticks() - start) < seconds * HZ)
            if(sleep_intr(50)) break;
    }
    framebuffer_graphics_end();
    printf("    text console restored\n");
    return result;
usage:
    printf("    usage: gfxtest [seconds]   (1 to 60; default 5)\n");
    return CMD_USAGE;
}
