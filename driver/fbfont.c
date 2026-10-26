/*
SurfOS Framebuffer Fonts
--------------------
File: fbfont.c  Date: 10/25/26 (track A, A7)
--------------------
Which font the framebuffer console draws in (include/sys/framebuffer.h, struct fb_font):
Spleen 8x16 as before; Spleen 12x24 on the laptop's screen, profile=laptop at 1920x1080 or
more, where it is drawn at its own size (160x45 cells); or the font font= on the kernel
command line names, 8x16, 12x24 or 16x32. A font is taken only where 80x25 of its cells fit
the mode, as the console starts on that many cells (driver/framebuffer.c); otherwise 8x16, and
the reason. Without profile=laptop and without font=, every boot is as it was. Pure code,
tested on the host (tests/host/test_framebuffer.c).
*/

#include <surfos/types.h>
#include <sys/framebuffer.h>
#include <blibc_common.h>

const struct fb_font *fb_font_named(const char *name) {
    static const struct fb_font *const fonts[] = { &fb_font_8x16, &fb_font_12x24, &fb_font_16x32 };
    u32 i;
    for(i = 0; i < sizeof(fonts) / sizeof(fonts[0]); i++)
        if(name && !strcmp(name, fonts[i]->name)) return fonts[i];
    return NULL;
}

const struct fb_font *fb_font_choose(const struct fb_mode *m, const char *word, bool laptop, const char **why) {
    const struct fb_font *f = &fb_font_8x16;
    if(why) *why = NULL;
    if(word) {
        f = fb_font_named(word);
        if(!f) {
            if(why) *why = "no such font (8x16, 12x24 or 16x32)";
            return &fb_font_8x16;
        }
    } else if(laptop && m->width >= 1920 && m->height >= 1080) {
        f = &fb_font_12x24;
    }
    if(m->width < 80 * f->width || m->height < 25 * f->height) {
        if(why) *why = "80x25 of its cells do not fit the screen";
        return &fb_font_8x16;
    }
    return f;
}
