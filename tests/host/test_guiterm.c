/* Host tests for the Terminal (track TERM of docs/superpowers/plans/2026-10-17-gui-suite.md), against the
   in-memory file system and system of tests/host/gui_fake.c. Built and run by `make unittest` (unittest-gui). */
#include "gui_fake.h"

static struct gui_state g;
static const int ids[] = { GUI_TERMINAL };
#define IDS (int)(sizeof(ids) / sizeof(ids[0]))

/* each window opens, ticks, draws inside the screen (also maximized and hanging off the edge) and closes */
static void windows(int width, int height) {
    struct fb_surface screen;
    int i, k;
    fake_desktop(&g, width, height);
    fake_screen(&screen, width, height);
    for(i = 0; i < IDS; i++) {
        gui_open(&g, ids[i]);
        CHECK(g.win[ids[i]].open && g.focus == ids[i]);
        for(k = 0; k < 3; k++) { g.now += 1000; gui_tick(&g); }
        gui_render(&g, &screen);
        CHECK(fake_guards(width, height));
        gui_act(&g, ACT_MAXIMIZE);
        gui_render(&g, &screen);
        CHECK(fake_guards(width, height));
        g.win[ids[i]].x = width - 10;
        g.win[ids[i]].y = height - 10;
        gui_render(&g, &screen);
        CHECK(fake_guards(width, height));
    }
    gui_finish(&g);
    for(i = 0; i < IDS; i++) {
        gui_open(&g, ids[i]);
        gui_act(&g, ACT_CLOSE);
        CHECK(!g.win[ids[i]].open);
    }
}

int main(void) {
    windows(640, 400);
    windows(960, 540);
    windows(1280, 800);
    return fake_done("test_guiterm");
}
