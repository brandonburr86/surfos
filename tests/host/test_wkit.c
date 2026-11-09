/* Host tests for wkit, the toolkit's demonstration (user/wkit/demo.c; the desktop applications, track kit), its window
   logic on the fake desktop (tests/host/gk_fake.c), driven as the screen checks drive it (tools/gui_apps_kit.py): the
   Controls page drawn; a name typed and the status bar following; Tab to the password field (dots); the check box;
   Greet's message answered by Enter; Copy only with the focus; Rename's line replacing the name; the Lists page, a
   row selected, Add and Remove; the Drawing page, a cell marked by a click and the cursor moved by the keys, each
   presenting no more than the cells that changed; the seconds' timer; the close button asking first; its menus in
   the desktop's bar (the Wi-Fi item checked as the check box is, a page chosen in the bar, a speed in View's
   submenu as the radio buttons have it, About in the desktop's Help), Open and Save as by
   their shortcuts through the desktop's choosers (chosen, cancelled) and through the line typed instead on a desktop
   without them. It is also the
   example of a program's window tested on the host: open, then each event as gk_next() gives it, then close. Built
   and run by `make unittest`. */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <gk.h>
#include "gk_fake.h"
#include "demo.h"

static int checks, failed;
#define CHECK(x) do { checks++; if(!(x)) { failed++; printf("FAIL %s line %d: %s\n", __FILE__, __LINE__, #x); } } while(0)

static struct demo d;

/* every event the window has, through the program; 0 once it asked to close */
static int run(void) {
    struct gk_event ev;
    int r;
    while((r = gk_next(d.ui, &ev, 0)) > 0) if(!demo_event(&d, &ev)) return 0;
    return 1;
}

static void click_on(struct gk_widget *w) {
    int x, y, width, height;
    gk_bounds(w, &x, &y, &width, &height);
    gkf_click(x + width / 2, y + height / 2);
}

static int status_says(const char *s) {
    return gkf_text(10, DEMO_H - GK_STATUS_H + 3, s, GW_INK);
}

static int boxed(int h) {                       /* a box over the window: its title bar */
    return gkf_pixel((DEMO_W - 470) / 2 + 2, (DEMO_H - h) / 2 + 2) == GW_TEAL;
}

int main(void) {
    int x, y, w, h, n, i;
    setvbuf(stdout, NULL, _IOLBF, 0);
    gkf_reset(0, 0);
    gkf_refuse(ENODEV);
    CHECK(demo_open(&d) < 0 && errno == ENODEV);
    CHECK(demo_open(&d) == 0 && gkf_w() == DEMO_W && gkf_h() == DEMO_H && !strcmp(gkf_title(), "Toolkit"));
    gkf_focus(1);
    CHECK(run());
    /* the Controls page */
    CHECK(gkf_pixel(10, 6) == GW_TEAL && gkf_text(8 + 7, 7, "Controls", GW_PAPER) && gkf_text(8 + 78 + 4 + 7, 7, "Lists", GW_INK));
    CHECK(gkf_text(10, 40 + 3, "Name:", GW_INK) && gkf_text(10, 68 + 3, "Password:", GW_INK));
    CHECK(status_says("Ready") && gkf_text(DEMO_W - 24 - 24, DEMO_H - GK_STATUS_H + 3, "0 s", GW_MUTED));
    gk_bounds(d.name, &x, &y, &w, &h);
    CHECK(x == 10 + 88 + 8 && y == 40 && gkf_pixel(x, y) == GW_TEAL);   /* it has the keys */
    gkf_type("Surf");
    CHECK(run());
    CHECK(!strcmp(gk_text(d.name), "Surf") && status_says("Name: Surf") && gkf_text(x + 4, y + 3, "Sur", GW_INK));
    gkf_key(9, 0);
    gkf_type("pw");
    CHECK(run());
    CHECK(gk_focused(d.ui) == d.secret && status_says("Password: 2 bytes"));
    click_on(d.wifi);
    CHECK(run() && gk_value(d.wifi) == 0 && status_says("Wi-Fi off"));
    /* Greet: a message over the window, Enter answers it */
    gkf_advance(1000);
    gk_bounds(d.name, &x, &y, &w, &h);
    gkf_click(10 + 32, 159 + 12);                        /* Greet */
    CHECK(run() && gk_box_open(d.ui) && boxed(146));
    CHECK(gkf_text((DEMO_W - 470) / 2 + 16, (DEMO_H - 146) / 2 + 44, "Hello, Surf.", GW_INK));
    gkf_key('\r', 0);
    CHECK(run() && !gk_box_open(d.ui) && status_says("Answered 0"));
    /* Copy: the name on the desktop's clipboard while the window has the focus */
    gkf_click(238 + 32, 159 + 12);
    CHECK(run() && !strcmp(gkf_clip(), "Surf") && status_says("Copied the name, pasted back: Surf"));
    gkf_focus(0);
    gkf_advance(1000);
    gkf_click(238 + 32, 159 + 12);
    CHECK(run() && status_says("Not copied: the window must have the focus") && gkf_clips() == 1);
    gkf_focus(1);
    /* Rename: the name, chosen, replaced by what is typed */
    gkf_advance(1000);
    gkf_click(154 + 38, 159 + 12);
    CHECK(run() && boxed(160));
    gkf_type("Sea\n");
    CHECK(run() && !strcmp(gk_text(d.name), "Sea") && status_says("Renamed: Sea"));
    /* the menus, in the desktop's bar: File, View with its Speed submenu, and an item for the desktop's Help */
    CHECK(gkf_menu_count() == 18 && gk_menus_shown(d.ui));        /* View has a fourth page now */
    CHECK((gkf_menu_entry(gkf_menu_find("Speed"))->flags & WIN_MENU_SUBMENU) &&
          gkf_menu_entry(gkf_menu_find("Slow"))->flags == (WIN_MENU_CHILD | WIN_MENU_RADIO) &&
          gkf_menu_entry(gkf_menu_find("Help"))->flags == (WIN_MENU_TITLE | WIN_MENU_HELP));
    CHECK(gkf_menu_choose(MENU_SPEED + 2));             /* View > Speed > Fast: the radio buttons follow */
    CHECK(run() && gk_value(d.speed) == 2 && status_says("Speed: Fast") &&
          gkf_menu_entry(gkf_menu_find("Fast"))->flags == (WIN_MENU_CHILD | WIN_MENU_RADIO) &&
          gkf_menu_entry(gkf_menu_find("Slow"))->flags == WIN_MENU_CHILD);
    CHECK(gkf_menu_choose(MENU_ABOUT));                 /* Help > About the toolkit: a message */
    CHECK(run() && gk_box_open(d.ui) && gk_asking(d.ui) == BOX_ABOUT);
    gkf_key(27, 0);
    CHECK(run() && !gk_box_open(d.ui));
    CHECK(!strcmp(gkf_menu_entry(1)->label, "Open\xe2\x88\x99") && !strcmp(gkf_menu_entry(1)->keys, "Ctrl+O"));
    CHECK(gkf_menu_find("Lists") >= 0 && !strcmp(gkf_menu_entry(gkf_menu_find("Lists"))->keys, "F6"));
    CHECK(!(gkf_menu_entry(gkf_menu_find("Wi-Fi"))->flags & WIN_MENU_CHECKED));   /* the check box was clicked off */
    CHECK(gkf_menu_entry(gkf_menu_find("Controls"))->flags & WIN_MENU_RADIO);
    CHECK(gkf_menu_choose(MENU_WIFI));
    CHECK(run() && gk_value(d.wifi) == 1 && status_says("Wi-Fi on") && (gkf_menu_entry(gkf_menu_find("Wi-Fi"))->flags &
                                                                        WIN_MENU_CHECKED));
    gkf_key(15, WIN_MOD_CTRL);                          /* Ctrl+O: the desktop's Open chooser */
    CHECK(run() && gkf_dialog() && gkf_dialog()->kind == WIN_DLG_OPEN && gkf_dialog()->id == CHOOSE_OPEN);
    gkf_dialog_answer("/usb1p1/sea.png");
    CHECK(run() && status_says("Chose /usb1p1/sea.png"));
    gkf_key(19, WIN_MOD_CTRL);                          /* Ctrl+S: Save as, the name offered */
    CHECK(run() && gkf_dialog() && gkf_dialog()->kind == WIN_DLG_SAVE && !strcmp(gkf_dialog()->name, "notes.txt") &&
          !strcmp(gkf_dialog()->types, "txt"));
    gkf_dialog_answer(NULL);
    CHECK(run() && status_says("Not chosen"));
    /* Ctrl+Shift+O: busy three seconds, its events unread meanwhile, then Open: no room for the answer while
       WIN_EVENTS events wait (EAGAIN), and the toolkit asks again once it has read them */
    gkf_key(15, WIN_MOD_CTRL | WIN_MOD_SHIFT);
    for(i = 0; i < WIN_EVENTS + 16; i++) gkf_pointer(DEMO_W - 5, DEMO_H - 5, 0);
    CHECK(run() && gkf_dialog() && gkf_dialog()->id == CHOOSE_OPEN);
    gkf_dialog_answer(NULL);
    CHECK(run() && status_says("Not chosen; lost while busy: 0"));   /* (the fake's pipe loses nothing) */
    /* keys no widget takes, with their modifiers; a new theme */
    gkf_key('x', WIN_MOD_ALT);
    CHECK(run() && status_says("Key 0x78 mods 2"));
    gkf_key(WIN_KEY_ESC, WIN_MOD_CTRL);
    CHECK(run() && status_says("Key 0x10e mods 4") && !gk_box_open(d.ui));
    gkf_set_theme(WIN_THEME_DUSK, NULL);
    CHECK(run() && status_says("Theme: dusk"));
    gkf_set_theme(WIN_THEME_OCEAN, NULL);
    CHECK(run() && status_says("Theme: ocean"));
    gkf_lacks(1u << WIN_DIALOG, ENOSYS);                /* a desktop without choosers: a path typed instead */
    gkf_key(15, WIN_MOD_CTRL);
    CHECK(run() && gk_box_open(d.ui) && boxed(160));
    gkf_key(1, WIN_MOD_CTRL);
    gkf_type("/initrd/motd\n");
    CHECK(run() && status_says("Chose /initrd/motd"));
    gkf_key(15, WIN_MOD_CTRL);
    gkf_key(27, 0);
    CHECK(run() && status_says("Not chosen") && !gk_box_open(d.ui));
    gkf_lacks(0, 0);
    gkf_advance(1000);
    CHECK(gkf_menu_choose(MENU_PAGE + 1));              /* View > Lists, in the bar */
    CHECK(run() && gk_value(d.tabs) == 1 && status_says("Page: Lists") &&
          (gkf_menu_entry(gkf_menu_find("Lists"))->flags & WIN_MENU_RADIO) &&
          !(gkf_menu_entry(gkf_menu_find("Controls"))->flags & WIN_MENU_RADIO));
    gkf_key(WIN_KEY_F5, 0);                             /* F5: back to Controls, by the shortcut */
    CHECK(run() && gk_value(d.tabs) == 0);
    /* the Lists page */
    gkf_click(8 + 78 + 4 + 20, 12);
    CHECK(run() && gk_value(d.tabs) == 1 && status_says("Page: Lists"));
    gk_bounds(d.list, &x, &y, &w, &h);
    CHECK(gkf_text(x + 6, y + 2, "SurfSim", GW_INK) && gkf_text(x + 6 + 112, y + 2, "-40 dB", GW_INK) &&
          gkf_text(x + 6 + 184, y + 2, "open", GW_INK));
    gkf_advance(1000);
    gkf_click(x + 20, y + 1 + 18 + 8);
    CHECK(run() && gk_value(d.list) == 1 && status_says("Selected: SurfSimWPA") && !strcmp(gk_text(d.picked), "SurfSimWPA"));
    gkf_key(WIN_KEY_DOWN, 0);
    gkf_key('\r', 0);
    CHECK(run() && status_says("Opened: Harbour"));
    gkf_wheel(x + 20, y + 20, 3);                       /* more lines than it shows: the wheel scrolls it */
    CHECK(run() && gkf_text(x + 6, y + 2, "Lighthouse", GW_INK) && gk_value(d.list) == 2);
    n = gk_list_count(d.list);
    gkf_advance(1000);
    click_on(gk_focused(d.ui));                         /* the list still; then Add and Remove by the keys */
    gkf_key(9, 0);
    gkf_key(' ', 0);
    CHECK(run() && gk_list_count(d.list) == n + 1 && status_says("Added a line"));
    gkf_key(9, 0);
    gkf_key(' ', 0);
    CHECK(run() && gk_list_count(d.list) == n && status_says("Removed a line"));
    /* the Drawing page: a click marks a cell, and only the cells that change are presented */
    gkf_click(8 + 78 + 4 + 54 + 4 + 20, 12);
    CHECK(run() && gk_value(d.tabs) == 2);
    gk_bounds(d.area, &x, &y, &w, &h);
    CHECK(y == 30 && gkf_pixel(x + 5, y + 5) != GW_PAPER && gkf_pixel(x + w - 6, y + 20) == GW_MUTED);   /* its bars */
    gkf_presented(&i, &i, &i, &i);
    gkf_advance(1000);
    gkf_click(x + 3 * DEMO_CELL + 5, y + 2 * DEMO_CELL + 5);
    CHECK(run() && d.mark[2][3] && d.cx == 3 && d.cy == 2 && status_says("Cell 3, 2, marked"));
    CHECK(gkf_pixel(x + 3 * DEMO_CELL + 9, y + 2 * DEMO_CELL + 9) == GW_TEAL);
    n = gkf_presented(&i, &i, &w, &h);                  /* the cells, the status bar, the tab strip (the focus) */
    {
        int px, py, pw, ph, area = 0, cells = 0;
        for(i = 0; i < n; i++) {
            gkf_presented_rect(i, &px, &py, &pw, &ph);
            area += pw * ph;
            if(py > 30 && py < DEMO_H - GK_STATUS_H) cells += pw * ph;
        }
        CHECK(n >= 2 && cells <= 2 * DEMO_CELL * DEMO_CELL && area < DEMO_W * DEMO_H / 4);
    }
    gkf_key(WIN_KEY_RIGHT, 0);
    gkf_key(' ', 0);
    CHECK(run() && d.cx == 4 && d.mark[2][4] && status_says("Cell 4, 2, marked"));
    gkf_key(WIN_KEY_END, 0);                            /* the far corner, scrolled into view */
    CHECK(run() && d.cx == DEMO_CELLS - 1 && d.cy == DEMO_CELLS - 1);
    {
        struct gk_view v;
        gk_area_view(d.area, &v);
        CHECK(v.sx > 0 && v.sy > 0 && v.sx + v.w / DEMO_CELL > DEMO_CELLS - 1);
    }
    /* the seconds */
    gkf_advance(1000);
    CHECK(run() && d.seconds >= 1);
    /* More (stage 2): notes, a volume, a colour, a tool, the seconds of the minute */
    gkf_advance(1000);
    gkf_click(222 + 20, 12);
    CHECK(run() && gk_value(d.tabs) == 3 && status_says("Page: More"));
    gk_bounds(d.notes, &x, &y, &w, &h);
    CHECK(x == 10 && y == 68 && w == 370);
    gkf_click(x + 20, y + 20);
    gkf_type("tide");
    CHECK(run() && status_says("Notes: 4 bytes") && !strcmp(gk_text(d.notes), "tide") && gkf_text(x + 5, y + 2, "tid", GW_INK));
    gk_bounds(d.volume, &x, &y, &w, &h);
    gkf_click(x + w - 2, y + h / 2);
    CHECK(run() && gk_value(d.volume) == 100 && status_says("Volume: 100"));
    gk_bounds(d.colors, &x, &y, &w, &h);
    gkf_click(x + 1 + 3 * 20 + 9, y + 1 + 9);
    CHECK(run() && gk_value(d.colors) == 3 && status_says("Colour: #EE906D"));
    gk_bounds(d.tools, &x, &y, &w, &h);
    gkf_click(x + 1 + 26 + 12, y + 1 + 12);
    CHECK(run() && gk_value(d.tools) == 1 && status_says("Tool: Eraser"));
    gk_bounds(d.minute, &x, &y, &w, &h);
    CHECK(gkf_pixel(x + 2, y + 5) == GW_TEAL);
    /* the close button asks first; Cancel keeps it, Close ends it */
    gkf_close();
    CHECK(run() && gk_box_open(d.ui));
    gkf_key(27, 0);
    CHECK(run() && !gk_box_open(d.ui) && status_says("Not closed"));
    gkf_close();
    CHECK(run() && gk_box_open(d.ui));
    gkf_key('\r', 0);
    CHECK(!run());
    demo_close(&d);
    CHECK(!gkf_open());
    gkf_reset(0, 0);
    printf("test_wkit: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
