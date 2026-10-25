/*
SurfOS user program: wcalc
--------------------
File: wcalc.c   Date: 10/25/26
--------------------
The desktop's Calculator as a program with a window of its own (libgui, gui.h), looking and
counting as the desktop's does (the same arithmetic: surfos/calc.h, the kernel's shell/guicalc.c):
whole numbers, + - * /, within plus or minus 999,999,999. Click its buttons or type: digits, the
operators, Enter or = calculates, Backspace takes a digit back, C clears; Esc, Ctrl+W or the close
button end it. A fault in it would cost only its window. The first of the desktop's applications
moved out of the kernel (milestone M12, stage C6 of docs/superpowers/plans/2026-10-25-app-windows.md),
and the pattern for the others.
*/

#include <stdio.h>
#include <string.h>
#include <surf.h>
#include <gui.h>
#include <surfos/calc.h>

static const char keys[] = "789/456*123-C0=+";

/* the desktop's Calculator, its window's content: 246 x 271 (shell/guicore.c draws the same, 1 pixel and its title
   bar further from the window's corner) */
#define W 246
#define H 271
#define KEY_X 15
#define KEY_Y 76

static void draw(struct gw *win, const struct gui_calc *c) {
    char b[16];
    int i;
    gw_fill(win, GW_PAPER);
    gw_rect(win, 15, 16, 216, 44, GW_INK);
    snprintf(b, sizeof(b), "%d", c->value);
    gw_text(win, 27, 30, c->error ? "Error - press C" : b, GW_FOAM);
    if(c->op && !c->error) {
        b[0] = c->op;
        b[1] = 0;
        gw_text(win, 211, 30, b, GW_CORAL);
    }
    for(i = 0; i < 16; i++) {
        b[0] = keys[i];
        b[1] = 0;
        gw_button(win, KEY_X + (i % 4) * 54, KEY_Y + (i / 4) * 40, 48, 34, b,
                  i % 4 == 3 ? GW_TEAL : i == 14 ? GW_CORAL : GW_SHELL, GW_INK);
    }
    gw_text(win, 15, 250, "Integers | C clears", GW_MUTED);
}

/* the key of the button at x, y, 0 for none */
static int button_at(int x, int y) {
    int col = (x - KEY_X) / 54, row = (y - KEY_Y) / 40;
    if(x < KEY_X || y < KEY_Y || col > 3 || row > 3 || (x - KEY_X) % 54 >= 48 || (y - KEY_Y) % 40 >= 34) return 0;
    return keys[row * 4 + col];
}

int main(void) {
    struct gui_calc c;
    struct win_event ev;
    struct gw *win;
    unsigned buttons = 0;
    int key;
    memset(&c, 0, sizeof(c));
    win = gw_open("Calculator", W, H, 0, GW_PAPER);
    if(!win) {
        fprintf(stderr, "wcalc: no window: %s\n", strerror(errno));
        return 1;
    }
    draw(win, &c);
    gw_present_all(win);
    while(gw_next(win, &ev, -1) > 0) {
        key = 0;
        if(ev.type == WIN_EV_CLOSE) break;
        if(ev.type == WIN_EV_KEY) {
            if(ev.value == 27 || ev.value == 23) break;     /* Esc, Ctrl+W */
            key = ev.value < 0x100 ? (int)ev.value : 0;
        }
        if(ev.type == WIN_EV_POINTER) {                     /* a press of the left button on a button */
            if((ev.value & WIN_BUTTON_LEFT) && !(buttons & WIN_BUTTON_LEFT)) key = button_at(ev.x, ev.y);
            buttons = ev.value;
        }
        if(!key) continue;
        gui_calc_key(&c, key);
        draw(win, &c);
        gw_present_all(win);
    }
    gw_close(win);
    return 0;
}
