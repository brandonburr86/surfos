/* Host tests for programs' windows on the desktop (shell/guiclient.c; milestone M12, stage C2 of
   docs/superpowers/plans/2026-10-25-app-windows.md), against the fake system of tests/host/gui_fake.c, whose windows
   wait for their program's first present and keep every event the desktop sends them. Built and run by
   `make unittest` (unittest-gui).
   10/25/26: a window appearing with its program's first present, its pixels at the content's origin and clipped
   at every edge; hover, leaving, a grab that goes on outside and ends when every button is up, the right and middle
   buttons, the wheel; one RESIZE per corner drag, maximizing and restoring, a program's limits; keys and the menus'
   two; focus; close, "End the program?" with Cancel first and End, a refused kill; a window whose program has gone;
   a ninth window waiting for a slot; leaving the desktop; and 8 programs' windows with every fixed window inside
   guard pixels at 640x400, 960x540 and 1280x800.
   10/25/26 (the review of C1-C3): a new window comes in front without the focus, which stays where it was (keys
   typed into Notepad stay Notepad's), its taskbar button marked until a click on it, on that button or Next window
   gives it the focus; gui_working() for the desktop's pace.
   10/25/26 (stage C6): Apps > Run program...: /initrd/bin's programs in a list, the line to run (a program picked,
   arguments typed), run through struct gui_sys's run; a refusal shown. The window of a program the launcher just
   started takes the focus when it appears, and so does that of the focused Terminal's foreground job.
   10/26/26 (stage C8): a turn of the wheel alone is the window's last event, no pointer event after it; one in a
   report that moves too is followed by where the pointer went.
   10/26/26 (stage C12): a window that asks to be seen comes to the front, shown again from the taskbar, without the
   focus, its taskbar button marked; a program's text for the clipboard is the desktop's clipboard. The review: it
   comes up after a pass's input (a press, or a whole click, queued in that pass lands where it was aimed), never
   while a button, a drag or a program's grab is held, and for GUI_GRACE_MS a press on it is nobody's; text from a
   window that no longer has the focus is dropped. An application that is a program now (the Clock): run from its
   tile, its window at the application's place, brought back when chosen again; Edit > Copy for a program's
   window. */
#include "gui_fake.h"

static struct gui_state g;
static unsigned held;                   /* the buttons the next pointer event says are down */

static void tick(void) { gui_tick(&g); gui_input_done(&g); }  /* a pass without input */

static void at(int x, int y) {
    struct pointer_event e;
    memset(&e, 0, sizeof(e));
    e.x = x;
    e.y = y;
    e.buttons = held;
    gui_pointer(&g, &e);
}
static void hold(unsigned b) { held = b; at(g.x, g.y); }
static void click(int x, int y) { held = 0; at(x, y); hold(POINTER_LEFT); hold(0); }
static void spin(int dz) {
    struct pointer_event e;
    memset(&e, 0, sizeof(e));
    e.x = g.x;
    e.y = g.y;
    e.buttons = held;
    e.wheel = dz;
    gui_pointer(&g, &e);
}

static void draw(int width, int height) {
    struct fb_surface s;
    fake_screen(&s, width, height);
    gui_render(&g, &s);
}

/* the desktop's window of fake window i, -1 */
static int win_id(int i) {
    int id;
    for(id = GUI_FIRST_CLIENT; id < GUI_SLOTS; id++)
        if(client_at(&g, id) && client_at(&g, id)->handle == fake_wins[i].handle) return id;
    return -1;
}

/* a window's content: its top left */
static int cx(int id) { return g.win[id].x + 1; }
static int cy(int id) { return g.win[id].y + TITLE_H; }

/* a fake window that has presented in color, on the desktop, and given the focus as a click on it would (10/25/26:
   a new window opens without it) */
static int shown(int w, int h, u32 flags, const char *title, int pid, u32 color) {
    int i = fake_win_open(w, h, flags, 0x203040, title, pid);
    fake_win_present(i, color);
    tick();
    gui_open(&g, win_id(i));
    return i;
}

/* s drawn at x, y in fg (its lit pixels) */
static bool shows(int width, int x, int y, const char *s, u32 fg) {
    int i, row, col;
    for(i = 0; s[i]; i++)
        for(row = 0; row < 16; row++)
            for(col = 0; col < 8; col++)
                if((font8x16[(u8)s[i]][row] & (0x80 >> col)) && fake_pixel(width, x + i * 8 + col, y + row) != fg)
                    return false;
    return true;
}

static const struct win_event *last(int i, int type) { return fake_win_last(i, type); }
static int count(int i, int type) { return fake_win_count(i, type); }

/* a user task the fake system's kill ends */
static void program(int pid, const char *name) {
    struct gui_task *t = &fake_tasks[fake_ntasks++];
    memset(t, 0, sizeof(*t));
    t->pid = pid;
    t->ppid = 5;
    t->state = GUI_TASK_SLEEPING;
    t->tty = 0;
    t->flags = GUI_TASK_USER;
    strlcpy(t->name, name, sizeof(t->name));
}

/**** appearing and drawing ****/

static void test_appear(void) {
    int a, b, id;
    fake_desktop(&g, 960, 540);
    a = fake_win_open(200, 100, 0, 0x102030, "Demo", 41);
    tick();
    CHECK(client_count(&g) == 0 && win_id(a) < 0);           /* nothing until its program presents */
    fake_win_present(a, 0xC0FFEE);
    tick();
    id = win_id(a);
    CHECK(client_count(&g) == 1 && id == GUI_FIRST_CLIENT);
    CHECK(g.win[id].open && !g.win[id].min && g.order[GUI_SLOTS - 1] == id);   /* in front, */
    CHECK(g.focus != id && client_waiting(&g, id));     /* 10/25/26: without the focus, its taskbar button marked */
    CHECK(g.win[id].w == 202 && g.win[id].h == 100 + TITLE_H + 1);
    draw(960, 540);
    CHECK(fake_guards(960, 540));
    CHECK(fake_pixel(960, cx(id), cy(id)) == 0xC0FFEE);
    CHECK(fake_pixel(960, cx(id) + 199, cy(id) + 99) == 0xC0FFEE);
    CHECK(fake_pixel(960, cx(id) + 200, cy(id) + 50) == INK);    /* the border just after its pixels */
    CHECK(shows(960, g.win[id].x + 30, g.win[id].y + 6, "Demo", PAPER));
    CHECK(shows(960, 6 + 1 * (156 + 4) + 24, 540 - TASK_H + 5, "Demo", FOAM));   /* the taskbar's second button */
    CHECK(fake_pixel(960, 6 + 1 * (156 + 4) + 80, 540 - 5) == CORAL);            /* and its mark */
    tick();
    CHECK(count(a, WIN_EV_FOCUS) == 0);
    click(g.win[id].x + 80, g.win[id].y + 14);          /* a click on its title bar: the focus */
    tick();
    CHECK(g.focus == id && !client_waiting(&g, id) && count(a, WIN_EV_FOCUS) == 1 && last(a, WIN_EV_FOCUS)->value == 1);
    draw(960, 540);
    CHECK(fake_pixel(960, 6 + 1 * (156 + 4) + 80, 540 - 5) != CORAL);
    /* a new present draws the desktop again */
    g.dirty = false;
    tick();
    CHECK(!g.dirty);
    fake_win_present(a, 0x00FF00);
    tick();
    CHECK(g.dirty);
    draw(960, 540);
    CHECK(fake_pixel(960, cx(id) + 100, cy(id) + 50) == 0x00FF00);
    /* a small one: the frame as wide as its title bar needs, its background beyond its pixels; not resizable, so
       its program is never asked for another size */
    b = shown(40, 20, 0, "Tiny", 42, 0xFF0000);
    id = win_id(b);
    CHECK(id == GUI_FIRST_CLIENT + 1 && g.win[id].w == 136 && g.win[id].h == TITLE_H + 24);
    CHECK(g.win[id].x != g.win[win_id(a)].x && g.focus == id);    /* a cascade, in front */
    draw(960, 540);
    CHECK(fake_pixel(960, cx(id), cy(id)) == 0xFF0000 && fake_pixel(960, cx(id) + 39, cy(id) + 19) == 0xFF0000);
    CHECK(fake_pixel(960, cx(id) + 40, cy(id)) == 0x203040 && fake_pixel(960, cx(id) + 120, cy(id) + 5) == 0x203040);
    CHECK(fake_pixel(960, cx(id), cy(id) + 20) == 0x203040);
    tick();
    tick();
    CHECK(count(b, WIN_EV_RESIZE) == 0);
    /* a program's window has no title of its own until it gives one */
    id = win_id(a);
    gui_open(&g, id);                                   /* in front of the small one, which covers its title bar */
    fake_wins[a].st.title[0] = 0;
    fake_wins[a].st.serial++;
    tick();
    draw(960, 540);
    CHECK(shows(960, g.win[id].x + 30, g.win[id].y + 6, "Program", PAPER));
}

static void test_clip(void) {
    int a, id;
    fake_desktop(&g, 960, 540);
    a = shown(300, 200, 0, "Edge", 41, 0x00AA00);
    id = win_id(a);
    g.win[id].x = 960 - 40;                             /* off the right edge, and into the taskbar */
    g.win[id].y = 540 - TASK_H - 30;
    draw(960, 540);
    CHECK(fake_guards(960, 540));
    CHECK(fake_pixel(960, 959, cy(id)) == 0x00AA00);
    CHECK(fake_pixel(960, 959, 540 - TASK_H) != 0x00AA00);       /* never over the taskbar */
    g.win[id].x = -100;                                 /* off the left edge and the top */
    g.win[id].y = -50;
    draw(960, 540);
    CHECK(fake_guards(960, 540));
    CHECK(fake_pixel(960, 0, MENU_H + 5) == 0x00AA00);
    CHECK(fake_pixel(960, 0, 10) != 0x00AA00);                   /* never over the menu bar */
    CHECK(fake_pixel(960, 959, MENU_H + 5) != 0x00AA00);         /* nor further right than it reaches */
}

/**** the pointer ****/

static void test_hover(void) {
    int a, id, n;
    fake_desktop(&g, 960, 540);
    a = shown(200, 100, 0, "Demo", 41, 0xC0FFEE);
    id = win_id(a);
    fake_win_forget(a);
    held = 0;
    at(cx(id) + 10, cy(id) + 20);
    CHECK(count(a, WIN_EV_POINTER) == 1 && last(a, WIN_EV_POINTER)->x == 10 && last(a, WIN_EV_POINTER)->y == 20 &&
          last(a, WIN_EV_POINTER)->value == 0);
    at(cx(id) + 11, cy(id) + 21);
    CHECK(count(a, WIN_EV_POINTER) == 2 && last(a, WIN_EV_POINTER)->x == 11);
    at(cx(id) + 250, cy(id) + 20);                      /* off its right edge, over the desktop: where it went */
    CHECK(count(a, WIN_EV_POINTER) == 3 && last(a, WIN_EV_POINTER)->x == 250 && last(a, WIN_EV_POINTER)->y == 20);
    at(cx(id) + 300, cy(id) + 20);
    CHECK(count(a, WIN_EV_POINTER) == 3);               /* nothing while it is away */
    at(cx(id) + 10, g.win[id].y + 5);                   /* its title bar is the desktop's */
    CHECK(count(a, WIN_EV_POINTER) == 3);
    at(cx(id) + 5, cy(id) + 5);
    CHECK(count(a, WIN_EV_POINTER) == 4);
    at(cx(id) + 5, g.win[id].y + 5);                    /* into its title bar: it left */
    CHECK(count(a, WIN_EV_POINTER) == 5 && last(a, WIN_EV_POINTER)->y < 0);
    /* a window in front of part of it: over that part the pointer is the other window's */
    gui_open(&g, GUI_CALC);
    g.win[GUI_CALC].x = cx(id) + 100;
    g.win[GUI_CALC].y = cy(id) - 10;
    at(cx(id) + 50, cy(id) + 50);
    n = count(a, WIN_EV_POINTER);
    CHECK(last(a, WIN_EV_POINTER)->x == 50);
    at(cx(id) + 150, cy(id) + 50);                      /* under the Calculator: it left */
    CHECK(count(a, WIN_EV_POINTER) == n + 1 && last(a, WIN_EV_POINTER)->x == -1 && last(a, WIN_EV_POINTER)->y == -1);
    at(cx(id) + 160, cy(id) + 50);
    CHECK(count(a, WIN_EV_POINTER) == n + 1);
}

static void test_grab(void) {
    int a, b, c, id, n, ida;
    fake_desktop(&g, 960, 540);
    a = shown(200, 100, 0, "Demo", 41, 0xC0FFEE);
    id = win_id(a);
    held = 0;
    at(cx(id) + 5, cy(id) + 5);
    fake_win_forget(a);
    hold(POINTER_LEFT);
    CHECK(g.clients.grab == id && last(a, WIN_EV_POINTER)->value == POINTER_LEFT);
    at(900, 400);                                       /* far outside: still its own */
    CHECK(last(a, WIN_EV_POINTER)->x == 900 - cx(id) && last(a, WIN_EV_POINTER)->y == 400 - cy(id) &&
          last(a, WIN_EV_POINTER)->value == POINTER_LEFT);
    at(10, 535);                                        /* over the taskbar too, which does nothing */
    CHECK(last(a, WIN_EV_POINTER)->y == 535 - cy(id) && g.menu == 0);
    hold(0);                                            /* let go: the last event without buttons */
    CHECK(last(a, WIN_EV_POINTER)->value == 0 && g.clients.grab == -1);
    n = count(a, WIN_EV_POINTER);
    at(20, 500);
    CHECK(count(a, WIN_EV_POINTER) == n);               /* no longer its own */
    /* the right button over a window behind another: to the front, and its own */
    ida = id;
    b = shown(200, 100, 0, "Front", 42, 0x00FF00);
    CHECK(g.focus == win_id(b) && g.win[win_id(b)].x > cx(ida));
    held = 0;
    at(cx(ida) + 5, cy(ida) + 40);                      /* the part of it the other leaves */
    fake_win_forget(a);
    hold(POINTER_RIGHT);
    CHECK(g.focus == ida && g.order[GUI_SLOTS - 1] == ida && g.clients.grab == ida);
    CHECK(last(a, WIN_EV_POINTER)->value == POINTER_RIGHT && count(b, WIN_EV_POINTER) == 0);
    CHECK(!g.menu);                                     /* 10/26/26 (C8): no right-click menu over its content */
    hold(POINTER_RIGHT | POINTER_MIDDLE);
    CHECK(last(a, WIN_EV_POINTER)->value == (POINTER_RIGHT | POINTER_MIDDLE));
    hold(0);
    CHECK(last(a, WIN_EV_POINTER)->value == 0 && g.clients.grab == -1);
    /* 10/26/26 (stage C8): a right-click on its title bar opens the Window menu there, for it; the program gets no
       button (its last event: leaving its content, no button) */
    at(g.win[ida].x + 60, g.win[ida].y + 10);
    hold(POINTER_RIGHT);
    hold(0);
    CHECK(g.menu == 5 && g.menu_y && g.focus == ida && last(a, WIN_EV_POINTER)->value == 0);
    gui_key(&g, 27);
    CHECK(!g.menu);
    /* 10/26/26: a right-click menu is its window's: that window gone, it closes; a window coming up with the focus
       closes it too (a bar menu stays) */
    at(g.win[ida].x + 60, g.win[ida].y + 10);
    hold(POINTER_RIGHT);
    hold(0);
    CHECK(g.menu == 5 && g.menu_win == ida);
    gui_open(&g, GUI_NOTEPAD);
    at(g.win[GUI_NOTEPAD].x + 120, g.win[GUI_NOTEPAD].y + 150);
    hold(POINTER_RIGHT);
    hold(0);
    CHECK(g.menu == 3 && g.menu_win == GUI_NOTEPAD);
    client_launched(&g, 43);
    c = fake_win_open(80, 60, 0, 0x203040, "Launched", 43);
    fake_win_present(c, 0x123456);
    tick();
    CHECK(g.focus == win_id(c) && !g.menu);
    fake_win_end(c);                                    /* gone again: the windows below as they were */
    tick();
    gui_open(&g, ida);
    /* a press on the desktop or on a fixed window never grabs, and its drag over a program's window is not its own */
    at(900, 300);
    hold(POINTER_LEFT);
    CHECK(g.clients.grab == -1);
    n = count(a, WIN_EV_POINTER);
    at(cx(ida) + 5, cy(ida) + 30);
    at(cx(ida) + 6, cy(ida) + 31);
    CHECK(count(a, WIN_EV_POINTER) == n && g.clients.grab == -1);
    hold(0);                                            /* let go over it: it hovers there, no button */
    CHECK(count(a, WIN_EV_POINTER) == n + 1 && last(a, WIN_EV_POINTER)->value == 0 && g.clients.hover == ida);
    /* a window that appears while a menu is open leaves the menu open */
    gui_key(&g, 7);
    CHECK(g.menu == 1);
    fake_win_present(fake_win_open(80, 60, 0, 0x203040, "Late", 43), 0x123456);
    tick();
    CHECK(g.menu == 1 && client_count(&g) == 3);
    gui_key(&g, 27);
}

static void test_grab_ends(void) {
    int a, id;
    fake_desktop(&g, 960, 540);
    a = shown(200, 100, 0, "Demo", 41, 0xC0FFEE);
    id = win_id(a);
    held = 0;
    at(cx(id) + 5, cy(id) + 5);
    hold(POINTER_LEFT);
    fake_win_forget(a);
    dialog_message(&g, "A question", "in front of everything", "");    /* a dialog takes the pointer */
    tick();
    CHECK(g.clients.grab == -1 && count(a, WIN_EV_POINTER) == 1 && last(a, WIN_EV_POINTER)->value == 0);
    dialog_close(&g);
    hold(0);
    hold(POINTER_LEFT);
    CHECK(g.clients.grab == id);
    fake_win_forget(a);
    g.buttons = 0;                                      /* let go with no event: the screen came back (shell/gui.c) */
    gui_release(&g);
    tick();
    CHECK(g.clients.grab == -1 && last(a, WIN_EV_POINTER) && last(a, WIN_EV_POINTER)->value == 0);
    held = 0;
    at(cx(id) + 6, cy(id) + 6);
    hold(POINTER_LEFT);
    CHECK(g.clients.grab == id);
    fake_win_forget(a);
    gui_act(&g, ACT_MINIMIZE);                          /* minimized under the pointer */
    tick();
    CHECK(g.win[id].min && g.clients.grab == -1 && last(a, WIN_EV_POINTER)->value == 0);
    hold(0);
}

static void test_wheel(void) {
    int a, id;
    fake_desktop(&g, 960, 540);
    a = shown(200, 100, 0, "Demo", 41, 0xC0FFEE);
    id = win_id(a);
    held = 0;
    at(cx(id) + 7, cy(id) + 9);
    fake_win_forget(a);
    spin(1);                                            /* away from the user: up */
    CHECK(count(a, WIN_EV_WHEEL) == 1 && (int)last(a, WIN_EV_WHEEL)->value == -3 && last(a, WIN_EV_WHEEL)->x == 7 &&
          last(a, WIN_EV_WHEEL)->y == 9);
    CHECK(count(a, WIN_EV_POINTER) == 0);               /* 10/26/26 (C8): the wheel alone, its last event */
    spin(-2);
    CHECK(count(a, WIN_EV_WHEEL) == 2 && (int)last(a, WIN_EV_WHEEL)->value == 6);
    {                                                   /* a turn in a report that moves too: and where it went */
        struct pointer_event e;
        memset(&e, 0, sizeof(e));
        e.x = g.x + 3;
        e.y = g.y;
        e.wheel = 1;
        gui_pointer(&g, &e);
    }
    CHECK(count(a, WIN_EV_WHEEL) == 3 && count(a, WIN_EV_POINTER) == 1 && last(a, WIN_EV_POINTER)->x == 10);
    at(cx(id) + 7, g.win[id].y + 5);                    /* over its title bar: nothing */
    spin(1);
    CHECK(count(a, WIN_EV_WHEEL) == 3);
    /* 10/26/26 (C8): during its grab the wheel is the grabbing window's wherever the pointer is, not the window's
       under it; with a menu open nobody's */
    at(cx(id) + 20, cy(id) + 20);
    hold(POINTER_LEFT);
    CHECK(g.clients.grab == id);
    at(900, 300);                                       /* outside, over the desktop */
    spin(1);
    CHECK(count(a, WIN_EV_WHEEL) == 4 && (int)last(a, WIN_EV_WHEEL)->value == -3 &&
          last(a, WIN_EV_WHEEL)->x == 900 - cx(id) && last(a, WIN_EV_WHEEL)->y == 300 - cy(id));
    hold(0);
    at(cx(id) + 7, cy(id) + 9);
    gui_key(&g, 7);                                     /* Ctrl+G: the menus */
    CHECK(g.menu);
    spin(1);
    CHECK(count(a, WIN_EV_WHEEL) == 4);
    gui_key(&g, 27);
}

/**** sizes ****/

static void test_resize(void) {
    int a, id, gx, gy;
    fake_desktop(&g, 960, 540);
    a = shown(300, 200, WIN_RESIZABLE, "Grow", 41, 0x445566);
    id = win_id(a);
    tick();
    fake_win_forget(a);
    held = 0;
    gx = g.win[id].x + g.win[id].w - 5;
    gy = g.win[id].y + g.win[id].h - 5;
    at(gx, gy);
    CHECK(count(a, WIN_EV_POINTER) == 0);               /* the corner grip is the desktop's */
    hold(POINTER_LEFT);
    CHECK(g.resize == id && g.clients.grab == -1);
    at(gx + 40, gy + 30);
    tick();
    at(gx + 60, gy + 50);
    tick();
    CHECK(g.win[id].w == 362 && g.win[id].h == 250 + TITLE_H + 1);
    CHECK(count(a, WIN_EV_RESIZE) == 0);                /* not while the corner is held */
    draw(960, 540);                                     /* the old pixels, and the background beyond them */
    CHECK(fake_pixel(960, cx(id) + 299, cy(id) + 199) == 0x445566 && fake_pixel(960, cx(id) + 330, cy(id) + 10) == 0x203040);
    hold(0);
    tick();
    CHECK(count(a, WIN_EV_RESIZE) == 1 && last(a, WIN_EV_RESIZE)->x == 360 && last(a, WIN_EV_RESIZE)->y == 250);
    tick();
    tick();
    CHECK(count(a, WIN_EV_RESIZE) == 1);                /* once */
    fake_win_size(a, 360, 250, 0x778899);               /* the program answers */
    tick();
    CHECK(g.win[id].w == 362 && g.win[id].h == 250 + TITLE_H + 1 && count(a, WIN_EV_RESIZE) == 1);
    draw(960, 540);
    CHECK(fake_pixel(960, cx(id) + 359, cy(id) + 249) == 0x778899);
    fake_win_size(a, 320, 210, 0x778899);               /* a size of its own: the frame follows, nothing is asked */
    tick();
    CHECK(g.win[id].w == 322 && g.win[id].h == 210 + TITLE_H + 1 && count(a, WIN_EV_RESIZE) == 1);
    /* maximized: the whole room between the bars, asked for once; restored: its size before */
    click(g.win[id].x + g.win[id].w - 40, g.win[id].y + 14);
    tick();
    CHECK(g.win[id].max && g.win[id].w == 960 && g.win[id].h == 540 - MENU_H - TASK_H);
    CHECK(count(a, WIN_EV_RESIZE) == 2 && last(a, WIN_EV_RESIZE)->x == 958 && last(a, WIN_EV_RESIZE)->y == 457);
    fake_win_size(a, 958, 457, 0x778899);
    tick();
    CHECK(g.win[id].max && g.win[id].w == 960 && count(a, WIN_EV_RESIZE) == 2);
    click(g.win[id].x + g.win[id].w - 40, g.win[id].y + 14);
    tick();
    CHECK(!g.win[id].max && g.win[id].w == 322);
    CHECK(count(a, WIN_EV_RESIZE) == 3 && last(a, WIN_EV_RESIZE)->x == 320 && last(a, WIN_EV_RESIZE)->y == 210);
    /* a corner dragged to nothing: never smaller than the title bar needs */
    fake_win_size(a, 320, 210, 0x778899);
    tick();
    held = 0;
    gx = g.win[id].x + g.win[id].w - 5;
    gy = g.win[id].y + g.win[id].h - 5;
    at(gx, gy);
    hold(POINTER_LEFT);
    at(0, 0);
    hold(0);
    tick();
    CHECK(g.win[id].w == 136 && g.win[id].h == TITLE_H + 24);
    CHECK(last(a, WIN_EV_RESIZE)->x == 134 && last(a, WIN_EV_RESIZE)->y == 23);
}

static void test_limits(void) {
    int a, id, gx, gy;
    fake_desktop(&g, 960, 540);
    a = fake_win_open(300, 200, WIN_RESIZABLE, 0x203040, "Limits", 41);
    fake_wins[a].st.min_w = 250;
    fake_wins[a].st.min_h = 150;
    fake_wins[a].st.max_w = 500;
    fake_wins[a].st.max_h = 300;
    fake_win_present(a, 0x111111);
    tick();
    id = win_id(a);
    held = 0;
    gx = g.win[id].x + g.win[id].w - 5;
    gy = g.win[id].y + g.win[id].h - 5;
    at(gx, gy);
    hold(POINTER_LEFT);
    at(gx - 200, gy - 200);                             /* smaller than it allows */
    CHECK(g.win[id].w == 252 && g.win[id].h == 150 + TITLE_H + 1);
    at(gx + 400, gy + 300);                             /* larger than it allows */
    CHECK(g.win[id].w == 502 && g.win[id].h == 300 + TITLE_H + 1);
    hold(0);
    tick();
    CHECK(last(a, WIN_EV_RESIZE)->x == 500 && last(a, WIN_EV_RESIZE)->y == 300);
    fake_win_size(a, 500, 300, 0x111111);
    tick();
    click(g.win[id].x + g.win[id].w - 40, g.win[id].y + 14);     /* maximized: as large as it allows */
    tick();
    CHECK(g.win[id].max && g.win[id].w == 502 && g.win[id].h == 300 + TITLE_H + 1);
}

/**** keys, focus, closing ****/

static void test_keys(void) {
    int a, id, n;
    fake_desktop(&g, 960, 540);
    a = shown(200, 100, 0, "Keys", 41, 0xC0FFEE);
    id = win_id(a);
    fake_win_forget(a);
    CHECK(gui_wants_arrows(&g));                        /* the arrows are its own, not the pointer's */
    fake_type(&g, "a\r");
    gui_key(&g, GUI_UP);
    gui_key(&g, 27);                                    /* Esc, Tab, Ctrl+W, Ctrl+Q and Ctrl+C are its own too */
    gui_key(&g, 9);
    gui_key(&g, 23);
    gui_key(&g, 17);
    gui_key(&g, 3);
    CHECK(count(a, WIN_EV_KEY) == 8);
    CHECK(fake_wins[a].ev[0].value == 'a' && fake_wins[a].ev[1].value == '\r' && fake_wins[a].ev[2].value == WIN_KEY_UP);
    CHECK(fake_wins[a].ev[3].value == 27 && fake_wins[a].ev[5].value == 23 && fake_wins[a].ev[6].value == 17);
    CHECK(g.win[id].open && !g.quit && g.focus == id && !g.dialog.kind);
    gui_key(&g, 7);                                     /* Ctrl+G: the menus, not the program */
    CHECK(g.menu == 1 && count(a, WIN_EV_KEY) == 8);
    gui_key(&g, 27);
    CHECK(g.menu == 0 && count(a, WIN_EV_KEY) == 8);
    gui_key(&g, 0);                                     /* Ctrl+Space, as in the Terminal */
    CHECK(g.menu == 1);
    gui_key(&g, 27);
    fake_type(&g, "b");
    CHECK(count(a, WIN_EV_KEY) == 9);
    /* 11/1/26: Shift+Tab is a key of its own; Enter with Shift is Enter, to a program that does not look at mods */
    gui_key(&g, GUI_BACKTAB);
    CHECK(count(a, WIN_EV_KEY) == 10 && last(a, WIN_EV_KEY)->value == WIN_KEY_BACKTAB && !last(a, WIN_EV_KEY)->mods);
    gui_key(&g, GUI_SHIFT_ENTER);
    CHECK(count(a, WIN_EV_KEY) == 11 && last(a, WIN_EV_KEY)->value == '\r' && last(a, WIN_EV_KEY)->mods == WIN_MOD_SHIFT);
    gui_key(&g, '\r');
    CHECK(count(a, WIN_EV_KEY) == 12 && last(a, WIN_EV_KEY)->value == '\r' && !last(a, WIN_EV_KEY)->mods);
    /* minimized: its keys are no longer its own */
    click(g.win[id].x + g.win[id].w - 40, g.win[id].y + 14);
    CHECK(g.win[id].min && g.focus != id);
    n = count(a, WIN_EV_KEY);
    fake_type(&g, "c");
    CHECK(count(a, WIN_EV_KEY) == n);
}

static void test_focus(void) {
    int a, id;
    fake_desktop(&g, 960, 540);
    a = shown(200, 100, 0, "Focus", 41, 0xC0FFEE);
    id = win_id(a);
    tick();
    CHECK(count(a, WIN_EV_FOCUS) == 1 && last(a, WIN_EV_FOCUS)->value == 1);
    gui_open(&g, GUI_CALC);
    tick();
    CHECK(count(a, WIN_EV_FOCUS) == 2 && last(a, WIN_EV_FOCUS)->value == 0);
    tick();
    CHECK(count(a, WIN_EV_FOCUS) == 2);
    g.win[GUI_CALC].x = 700;                            /* out of the way, then a click on its content */
    click(cx(id) + 10, cy(id) + 10);
    tick();
    CHECK(g.focus == id && count(a, WIN_EV_FOCUS) == 3 && last(a, WIN_EV_FOCUS)->value == 1);
}

/* 10/25/26 (the review, a security finding): a program's new window never takes the focus, so keys typed into
   another window (a password at a hidden prompt, in the Terminal's case) stay there; its taskbar button is marked,
   and a click on that button, or Next window, gives it the focus */
static int task_x(int id) {
    int ids[GUI_SLOTS], n = 0, i, w;
    for(i = 0; i < GUI_SLOTS; i++) if(g.win[i].open) ids[n++] = i;
    w = (g.width - 96) / n - 4;
    if(w > 156) w = 156;
    for(i = 0; i < n && ids[i] != id; i++) ;
    return 6 + i * (w + 4) + w / 2;
}

static void test_focus_kept(void) {
    int a, b, ida, idb;
    fake_desktop(&g, 960, 540);
    gui_open(&g, GUI_NOTEPAD);
    fake_type(&g, "sec");
    a = fake_win_open(200, 100, 0, 0x102030, "Late", 41);
    fake_win_present(a, 0xC0FFEE);
    tick();
    ida = win_id(a);
    CHECK(ida >= 0 && g.order[GUI_SLOTS - 1] == ida && g.focus == GUI_NOTEPAD && client_waiting(&g, ida));
    fake_type(&g, "ret");                               /* the keys stay Notepad's */
    tick();
    CHECK(count(a, WIN_EV_KEY) == 0 && count(a, WIN_EV_FOCUS) == 0 && g.note.len == 6);
    CHECK(!client_keys(&g));
    click(task_x(ida), g.height - TASK_H / 2);          /* its taskbar button: the focus, and the mark goes */
    tick();
    CHECK(g.focus == ida && !client_waiting(&g, ida) && count(a, WIN_EV_FOCUS) == 1);
    fake_type(&g, "x");
    CHECK(count(a, WIN_EV_KEY) == 1 && g.note.len == 6);
    gui_open(&g, GUI_NOTEPAD);                          /* a second one: Next window gives it the focus first */
    b = fake_win_open(100, 50, 0, 0x102030, "Later", 42);
    fake_win_present(b, 0x00FF00);
    tick();
    idb = win_id(b);
    CHECK(g.focus == GUI_NOTEPAD && client_waiting(&g, idb) && !client_waiting(&g, ida));
    gui_act(&g, ACT_NEXT_WINDOW);
    tick();
    CHECK(g.focus == idb && !client_waiting(&g, idb) && count(b, WIN_EV_FOCUS) == 1);
    /* the desktop's pace: an application at work that draws nothing every pass */
    CHECK(!gui_working(&g));
    g.viewer.loading = true;
    CHECK(gui_working(&g));
    g.viewer.loading = false;
    g.hex.searching = true;
    CHECK(gui_working(&g));
    g.hex.searching = false;
    g.term.carry_len = 5;
    CHECK(gui_working(&g));
    g.term.carry_len = 0;
    g.term.pend_len = 2;
    CHECK(gui_working(&g));
    g.term.pend_len = 0;
    CHECK(!gui_working(&g));
}

static void test_close(void) {
    int a, b, id, n;
    fake_desktop(&g, 960, 540);
    program(41, "wdemo");
    a = shown(200, 100, 0, "Demo", 41, 0xC0FFEE);
    id = win_id(a);
    click(g.win[id].x + g.win[id].w - 15, g.win[id].y + 14);    /* x: CLOSE, the program decides */
    CHECK(count(a, WIN_EV_CLOSE) == 1 && g.win[id].open && !g.dialog.kind);
    click(g.win[id].x + g.win[id].w - 15, g.win[id].y + 14);    /* again: the desktop asks */
    CHECK(g.dialog.kind == GUI_DLG_CONFIRM && g.dialog.act == ACT_CLIENT_END + (id - GUI_FIRST_CLIENT));
    CHECK(!strcmp(g.dialog.title, "End the program?") && strstr(g.dialog.text[0], "Demo (pid 41)") && g.dialog.choice == 2);
    CHECK(count(a, WIN_EV_CLOSE) == 1);
    gui_key(&g, '\r');                                  /* Enter alone ends nothing */
    CHECK(!g.dialog.kind && fake_killed == 0 && g.win[id].open && client_at(&g, id));
    click(g.win[id].x + g.win[id].w - 15, g.win[id].y + 14);
    CHECK(g.dialog.kind == GUI_DLG_CONFIRM);
    gui_key(&g, GUI_RIGHT);                             /* End */
    gui_key(&g, '\r');
    CHECK(fake_killed == 41 && !client_at(&g, id) && !g.win[id].open && fake_wins[a].closed);
    CHECK(g.focus != id);
    /* a refused kill stands: the window stays, and the desktop says why */
    b = shown(200, 100, 0, "Init's", 1, 0x00FF00);
    id = win_id(b);
    gui_act(&g, ACT_CLOSE);                             /* Close in the menus is the close button */
    CHECK(count(b, WIN_EV_CLOSE) == 1);
    gui_act(&g, ACT_CLOSE);
    CHECK(g.dialog.kind == GUI_DLG_CONFIRM);
    g.dialog.choice = 0;
    gui_key(&g, '\r');
    CHECK(fake_killed == 1 && client_at(&g, id) && g.win[id].open);
    CHECK(g.dialog.kind == GUI_DLG_MESSAGE && !strcmp(g.dialog.title, "Not ended"));
    gui_key(&g, '\r');
    /* the program closes its window itself after CLOSE: the question about it goes with it */
    gui_act(&g, ACT_CLOSE);
    CHECK(g.dialog.kind == GUI_DLG_CONFIRM);
    n = client_count(&g);
    fake_win_end(b);
    tick();
    CHECK(!g.dialog.kind && client_count(&g) == n - 1 && !g.win[id].open && fake_wins[b].closed);
}

static void test_gone(void) {
    int a, b, c, ida, idb;
    fake_desktop(&g, 960, 540);
    a = shown(200, 100, 0, "First", 41, 0xC0FFEE);
    b = shown(200, 100, 0, "Second", 42, 0x00FF00);
    ida = win_id(a);
    idb = win_id(b);
    CHECK(g.focus == idb);
    held = 0;
    at(cx(idb) + 5, cy(idb) + 5);
    hold(POINTER_LEFT);                                 /* even in the middle of a grab */
    fake_win_end(b);
    tick();
    CHECK(!client_at(&g, idb) && !g.win[idb].open && fake_wins[b].closed && g.clients.grab == -1);
    CHECK(g.focus == ida && client_count(&g) == 1);
    hold(0);
    draw(960, 540);
    CHECK(fake_guards(960, 540) && fake_pixel(960, cx(idb) + 150, cy(idb) + 90) != 0x00FF00);
    /* one gone before it was ever shown never appears */
    c = fake_win_open(100, 100, 0, 0x203040, "Brief", 43);
    fake_win_present(c, 0x0000FF);
    fake_win_end(c);
    tick();
    CHECK(client_count(&g) == 1 && win_id(c) < 0);
}

static void test_ninth(void) {
    int w[9], k;
    fake_desktop(&g, 960, 540);
    for(k = 0; k < 9; k++) {
        w[k] = fake_win_open(100 + k * 10, 60, 0, 0x203040, "Many", 50 + k);
        fake_win_present(w[k], 0x010101 * (k + 1));
    }
    tick();
    CHECK(client_count(&g) == GUI_CLIENTS && win_id(w[8]) < 0);
    for(k = 0; k < 8; k++) CHECK(win_id(w[k]) == GUI_FIRST_CLIENT + k);
    fake_win_end(w[3]);                                 /* a slot comes free: the ninth takes it */
    tick();
    CHECK(client_count(&g) == GUI_CLIENTS && win_id(w[8]) == GUI_FIRST_CLIENT + 3);
    CHECK(g.order[GUI_SLOTS - 1] == win_id(w[8]) && client_waiting(&g, win_id(w[8])));     /* in front, no focus */
    CHECK(g.win[win_id(w[8])].w == 182);
}

static void test_menus(void) {
    int a, b, ida;
    struct gui_window keep;
    fake_desktop(&g, 960, 540);
    a = shown(200, 100, 0, "Alpha", 41, 0xC0FFEE);
    b = shown(200, 100, 0, "Beta", 42, 0x00FF00);
    ida = win_id(a);
    CHECK(gui_menu_on(&g, 5, ACT_WINDOW + ida) && gui_menu_on(&g, 5, ACT_WINDOW + win_id(b)));
    gui_act(&g, ACT_WINDOW + ida);                      /* the Window menu's item brings it to the front */
    CHECK(g.focus == ida);
    keep = g.win[ida];
    gui_act(&g, ACT_WINDOW + GUI_FIRST_CLIENT + 5);     /* an empty slot opens nothing */
    CHECK(!g.win[GUI_FIRST_CLIENT + 5].open && g.focus == ida && g.win[ida].x == keep.x);
    gui_open(&g, GUI_FIRST_CLIENT + 6);
    CHECK(!g.win[GUI_FIRST_CLIENT + 6].open);
    CHECK(!gui_menu_on(&g, 5, ACT_MAXIMIZE));           /* not resizable: no maximizing */
}

static void test_leave(void) {
    int a;
    fake_desktop(&g, 960, 540);
    a = shown(200, 100, 0, "Stay", 41, 0xC0FFEE);
    gui_act(&g, ACT_QUIT);
    CHECK(g.dialog.kind == GUI_DLG_CONFIRM && !strcmp(g.dialog.title, "Leave the desktop?") &&
          !strcmp(g.dialog.text[0], "A program has a window open."));
    gui_key(&g, 27);                                    /* Esc: stay */
    CHECK(!g.dialog.kind && !g.quit);
    shown(100, 100, 0, "Two", 42, 0x00FF00);
    gui_act(&g, ACT_QUIT);
    CHECK(!strcmp(g.dialog.text[0], "2 programs have windows open."));
    g.dialog.choice = 0;
    gui_key(&g, '\r');                                  /* Leave */
    CHECK(g.quit && !fake_wins[a].closed);
    gui_finish(&g);                                     /* the desktop's end lets every window go */
    CHECK(fake_wins[a].closed && fake_wins[1].closed && client_count(&g) == 0);
}

static void test_no_system(void) {
    struct gui_sys old = fake_sys;
    int a;
    fake_desktop(&g, 960, 540);
    g.sys = NULL;
    a = fake_win_open(100, 100, 0, 0x203040, "None", 41);
    fake_win_present(a, 0x00FF00);
    tick();
    CHECK(client_count(&g) == 0);
    old.win_accept = NULL;                              /* a system of the suite's time, without the calls */
    old.win_update = NULL;
    old.win_event = NULL;
    old.win_close = NULL;
    old.run = NULL;
    g.sys = &old;
    tick();
    CHECK(client_count(&g) == 0);
    draw(960, 540);
    CHECK(fake_guards(960, 540));
}

/**** the launcher ****/

/* the launcher's programs: a folder first, then the names */
static void programs(void) {
    fake_dir("/initrd/bin");
    fake_file("/initrd/bin/wdemo", "\x7F" "ELF", 4);
    fake_file("/initrd/bin/wcalc", "\x7F" "ELF", 4);
    fake_file("/initrd/bin/ls", "\x7F" "ELF", 4);
    fake_dir("/initrd/bin/sub");
}

/* a click on button i of the open dialog (0 Run, 2 Cancel), or on its list's row */
static void dialog_button(int i) {
    int x, y, w, h;
    dialog_box(&g, &x, &y, &w, &h);
    fake_click(&g, i == 0 ? x + w - 200 + 44 : x + w - 16 - 44, y + h - 40 + 14);
}

static void dialog_row(int row) {
    int x, y, w, h;
    dialog_box(&g, &x, &y, &w, &h);
    fake_click(&g, x + 100, y + 64 + row * ROW_H + 9);
}

/* 10/26/26 (stage C12): an application that is a program now (the Clock): its tile and the Apps menu run it, its
   window takes the focus and stands at the application's place; chosen again, its window comes to the front; once it
   has ended, it runs again; a program that cannot start is said, and nothing kept. Edit > Copy for a program's window
   is its Ctrl+C */
static void test_app_program(void) {
    int a, b, id, x, y, k = GUI_CLOCK - GUI_FIRST_APP;
    fake_desktop(&g, 960, 540);
    x = g.win[GUI_CLOCK].x;
    y = g.win[GUI_CLOCK].y;
    CHECK(gui_app_of(GUI_CLOCK)->program && !strcmp(gui_app_of(GUI_CLOCK)->program, "/initrd/bin/wclock"));
    fake_run_result = 57;
    gui_act(&g, ACT_WINDOW + GUI_CLOCK);                /* its tile, or Apps > Clock */
    CHECK(fake_run_count == 1 && !strcmp(fake_run_line, "/initrd/bin/wclock") && !g.win[GUI_CLOCK].open &&
          g.app_pid[k] == 57);
    g.now += GUI_START_MS - 1;                          /* pressed again before its window came: a double-click */
    gui_act(&g, ACT_WINDOW + GUI_CLOCK);
    CHECK(fake_run_count == 1);
    a = fake_win_open(378, 271, 0, 0xF4F1E6, "Clock", 57);
    fake_win_present(a, 0xF4F1E6);
    tick();
    id = win_id(a);
    CHECK(id >= 0 && g.focus == id && g.win[id].x == x && g.win[id].y == y && g.win[id].w == 380 &&
          g.win[id].h == 300 && !client_waiting(&g, id));
    CHECK(gui_window_icon(&g, id) == ICON_CLOCK);       /* the Clock's icon, as its window had */
    b = fake_win_open(100, 80, 0, 0x203040, "Another", 99);
    fake_win_present(b, 0x203040);
    tick();
    CHECK(win_id(b) >= 0 && gui_window_icon(&g, win_id(b)) == ICON_PROGRAM);  /* any other program's: a program's */
    fake_win_end(b);
    tick();
    gui_open(&g, GUI_NOTEPAD);
    gui_act(&g, ACT_WINDOW + GUI_CLOCK);                /* again: its window to the front, nothing run */
    CHECK(fake_run_count == 1 && g.focus == id && g.order[GUI_SLOTS - 1] == id);
    CHECK(gui_menu_on(&g, 3, ACT_CLIENT_COPY));
    fake_win_forget(a);
    gui_act(&g, ACT_CLIENT_COPY);
    CHECK(count(a, WIN_EV_KEY) == 1 && last(a, WIN_EV_KEY)->value == 3);
    fake_win_end(a);
    tick();
    CHECK(win_id(a) < 0);
    gui_act(&g, ACT_WINDOW + GUI_CLOCK);                /* ended: it runs again */
    CHECK(fake_run_count == 2 && g.app_pid[k] == 57);
    fake_run_result = -2;
    g.now += GUI_START_MS;
    gui_act(&g, ACT_WINDOW + GUI_CLOCK);                /* 57 came with no window in that time: run, which fails */
    CHECK(fake_run_count == 3 && g.dialog.kind && !strcmp(g.dialog.title, "Not started") && g.app_pid[k] == 0);
    gui_key(&g, 27);
}

static void test_launcher(void) {
    fake_desktop(&g, 960, 540);
    programs();
    CHECK(gui_menu_on(&g, 4, ACT_RUN));                 /* Apps > Run program... */
    gui_act(&g, ACT_RUN);                               /* /initrd/bin's programs, and wcalc offered, selected */
    CHECK(g.dialog.kind == GUI_DLG_RUN && !strcmp(g.dialog.dir, "/initrd/bin") && g.dialog.count == 4 &&
          !strcmp(g.dialog.list[1].name, "ls") && !strcmp(g.dialog.field.text, "wcalc") && g.dialog.field.all &&
          g.dialog.selected == 2 && !g.dialog.list_focus && !strcmp(g.dialog.label[0], "Run") &&
          !strcmp(g.dialog.title, "Run a program"));
    draw(960, 540);
    CHECK(fake_guards(960, 540));
    fake_type(&g, "wdemo hang");                        /* typing replaces what was offered */
    gui_key(&g, '\r');
    CHECK(fake_run_count == 1 && !strcmp(fake_run_line, "wdemo hang") && !g.dialog.kind);

    gui_act(&g, ACT_RUN);                               /* a program picked in the list is the line */
    gui_key(&g, GUI_UP);
    CHECK(g.dialog.selected == 1 && !strcmp(g.dialog.field.text, "ls"));
    gui_key(&g, GUI_DOWN);
    gui_key(&g, GUI_DOWN);
    gui_key(&g, '\r');
    CHECK(fake_run_count == 2 && !strcmp(fake_run_line, "wdemo") && !g.dialog.kind);
    gui_act(&g, ACT_RUN);                               /* and arguments typed after it */
    gui_key(&g, GUI_UP);
    fake_type(&g, " -l");
    CHECK(!g.dialog.list_focus);
    gui_key(&g, '\r');
    CHECK(fake_run_count == 3 && !strcmp(fake_run_line, "ls -l"));

    gui_act(&g, ACT_RUN);                               /* the launcher never leaves its folder */
    gui_key(&g, GUI_UP);
    gui_key(&g, GUI_UP);                                /* sub: the line stays */
    gui_key(&g, '\r');
    CHECK(g.dialog.kind == GUI_DLG_RUN && !strcmp(g.dialog.dir, "/initrd/bin") && fake_run_count == 3 &&
          !strcmp(g.dialog.field.text, "ls"));
    gui_key(&g, 8);                                     /* Backspace edits the line */
    CHECK(!strcmp(g.dialog.dir, "/initrd/bin") && !strcmp(g.dialog.field.text, "l") && !g.dialog.list_focus);
    gui_key(&g, 27);

    gui_act(&g, ACT_RUN);                               /* by the pointer: a row, then Run */
    dialog_row(3);
    CHECK(g.dialog.selected == 3 && !strcmp(g.dialog.field.text, "wdemo"));
    dialog_button(0);
    CHECK(fake_run_count == 4 && !strcmp(fake_run_line, "wdemo") && !g.dialog.kind);
    gui_act(&g, ACT_RUN);                               /* Run with the line offered */
    dialog_button(0);
    CHECK(fake_run_count == 5 && !strcmp(fake_run_line, "wcalc"));
    gui_act(&g, ACT_RUN);                               /* Cancel, and Esc, run nothing */
    dialog_button(2);
    CHECK(fake_run_count == 5 && !g.dialog.kind);
    gui_act(&g, ACT_RUN);
    gui_key(&g, 27);
    CHECK(fake_run_count == 5 && !g.dialog.kind);
    gui_act(&g, ACT_RUN);                               /* nothing to run */
    gui_key(&g, 8);
    gui_key(&g, '\r');
    CHECK(fake_run_count == 5 && g.dialog.kind == GUI_DLG_RUN);
    gui_key(&g, 27);

    fake_run_result = -ENOENT;                          /* a program that cannot start: the desktop says why */
    gui_act(&g, ACT_RUN);
    fake_type(&g, "nosuch");
    gui_key(&g, '\r');
    CHECK(fake_run_count == 6 && g.dialog.kind == GUI_DLG_MESSAGE && !strcmp(g.dialog.title, "Not started") &&
          strstr(g.dialog.text[0], "nosuch"));
    gui_key(&g, '\r');

    fake_files_reset();                                 /* no /initrd/bin: an empty list that says so, the line still runs */
    fake_run_result = 77;
    gui_act(&g, ACT_RUN);
    CHECK(g.dialog.kind == GUI_DLG_RUN && !strcmp(g.dialog.dir, "/initrd/bin") && !g.dialog.count &&
          g.dialog.text[0][0]);
    gui_key(&g, '\r');
    CHECK(fake_run_count == 7 && !strcmp(fake_run_line, "wcalc"));

    fake_desktop(&g, 640, 400);                         /* the smallest desktop: the menu and the dialog fit */
    programs();
    CHECK(gui_menu_on(&g, 4, ACT_RUN));
    gui_act(&g, ACT_RUN);
    draw(640, 400);
    CHECK(g.dialog.kind == GUI_DLG_RUN && fake_guards(640, 400));
    gui_key(&g, 27);
    g.sys = NULL;                                       /* no system: nothing to run with */
    CHECK(!gui_menu_on(&g, 4, ACT_RUN));
    gui_act(&g, ACT_RUN);
    CHECK(!g.dialog.kind);
}

/* 10/25/26 (stage C6): the windows asked for take the focus as they appear: the first of a program the launcher just
   started, and the focused Terminal's foreground job's (the kernel says which, win_state.foreground); others wait */
static void test_asked_focus(void) {
    int a, b, c, t;
    fake_desktop(&g, 960, 540);
    programs();
    gui_open(&g, GUI_NOTEPAD);
    gui_act(&g, ACT_RUN);
    gui_key(&g, '\r');                                  /* wcalc, pid 77 */
    CHECK(fake_run_count == 1 && g.focus == GUI_NOTEPAD && g.clients.launched[0] == 77);
    a = fake_win_open(100, 80, 0, 0x203040, "Calculator", 77);
    fake_win_present(a, 0x00FF00);
    tick();
    CHECK(win_id(a) >= 0 && g.focus == win_id(a) && !client_waiting(&g, win_id(a)) && !g.clients.launched[0]);
    b = fake_win_open(100, 80, 0, 0x203040, "Second", 77);  /* its second window waits */
    fake_win_present(b, 0x0000FF);
    tick();
    CHECK(win_id(b) >= 0 && g.focus == win_id(a) && client_waiting(&g, win_id(b)));
    fake_run_result = -ENOENT;                          /* a refusal records nothing */
    gui_act(&g, ACT_RUN);
    gui_key(&g, '\r');
    CHECK(!g.clients.launched[0]);
    gui_key(&g, '\r');
    fake_run_result = 77;
    gui_open(&g, GUI_TERMINAL);                         /* the focused Terminal's foreground job */
    c = fake_win_open(100, 80, 0, 0x203040, "Fg", 88);
    fake_wins[c].st.foreground = 1;
    fake_win_present(c, 0x111111);
    tick();
    CHECK(win_id(c) >= 0 && g.focus == win_id(c));
    gui_open(&g, GUI_NOTEPAD);                          /* the same with Notepad in front of the Terminal: it waits */
    t = fake_win_open(100, 80, 0, 0x203040, "Fg2", 89);
    fake_wins[t].st.foreground = 1;
    fake_win_present(t, 0x222222);
    tick();
    CHECK(win_id(t) >= 0 && g.focus == GUI_NOTEPAD && client_waiting(&g, win_id(t)));
}

/**** every window at once ****/

static void test_guards(int width, int height) {
    static const u32 colors[] = { 0xFF0000, 0x00FF00, 0x0000FF, 0xFFFF00, 0x00FFFF, 0xFF00FF, 0x808080, 0xFFFFFF };
    int k, id, i;
    fake_desktop(&g, width, height);
    i = fake_win_open(width - 2, height - MENU_H - TASK_H - TITLE_H - 1, WIN_RESIZABLE, 0x203040, "As large as can be",
                      40);
    fake_win_present(i, colors[0]);
    for(k = 1; k < GUI_CLIENTS; k++) {
        i = fake_win_open(60 + k * 37, 40 + k * 29, k % 2 ? WIN_RESIZABLE : 0, 0x203040,
                          "A program's window with a title too long for its frame", 40 + k);
        fake_win_present(i, colors[k]);
    }
    tick();
    CHECK(client_count(&g) == GUI_CLIENTS);
    gui_act(&g, ACT_WINDOW + GUI_FIRST_CLIENT + 1);
    gui_act(&g, ACT_MAXIMIZE);                          /* one maximized */
    for(id = 0; id < GUI_WINDOWS; id++) if(!gui_app_of(id) || !gui_app_of(id)->program) gui_open(&g, id);
    tick();
    draw(width, height);
    CHECK(fake_guards(width, height));
    for(id = 0; id < GUI_SLOTS; id++) {
        const struct gui_window *w = &g.win[id];
        if(gui_app_of(id) && gui_app_of(id)->program) continue;     /* 10/26/26: a program now (the Clock) */
        CHECK(w->open);
        CHECK(w->x >= 0 && w->y >= MENU_H && w->x + w->w <= width && w->y + w->h <= height - TASK_H);
    }
    gui_finish(&g);
}

/* 10/26/26 (stage C12) */
static void test_alert_clip(void) {
    int a, b, ida, idb, n, nb, x, y;
    fake_desktop(&g, 960, 540);
    a = shown(200, 100, 0, "Asker", 41, 0x123456);
    b = shown(200, 100, 0, "Other", 42, 0x654321);
    ida = win_id(a);
    idb = win_id(b);
    gui_open(&g, ida);
    gui_act(&g, ACT_MINIMIZE);
    gui_open(&g, idb);
    tick();
    CHECK(g.win[ida].min && g.focus == idb);
    n = count(b, WIN_EV_FOCUS);
    fake_wins[a].st.alerts++;
    tick();
    CHECK(!g.win[ida].min && g.order[GUI_SLOTS - 1] == ida && g.focus == idb && client_waiting(&g, ida));
    CHECK(count(b, WIN_EV_FOCUS) == n && last(b, WIN_EV_FOCUS)->value == 1);   /* the focus stayed where it was */
    gui_open(&g, ida);
    tick();
    CHECK(g.focus == ida && !client_waiting(&g, ida));
    fake_wins[a].st.alerts++;                           /* asked while in front with the focus: nothing to do */
    tick();
    CHECK(g.focus == ida && !client_waiting(&g, ida) && g.order[GUI_SLOTS - 1] == ida);
    /* held while a button is down: no window comes up under a click under way; once it is let go, it does */
    gui_open(&g, idb);
    gui_open(&g, ida);
    gui_act(&g, ACT_MINIMIZE);
    gui_open(&g, idb);
    tick();
    held = 0;
    at(900, 300);
    hold(POINTER_LEFT);
    fake_wins[a].st.alerts++;
    tick();
    CHECK(g.win[ida].min && g.order[GUI_SLOTS - 1] == idb);
    hold(0);
    tick();
    CHECK(!g.win[ida].min && g.order[GUI_SLOTS - 1] == ida && g.focus == idb && client_waiting(&g, ida));
    /* after the pass's input: a press queued in the pass that sees the alert lands where it was aimed, on Other in
       front (x, y is on both windows); the window comes up once it is let go */
    gui_open(&g, idb);
    tick();
    x = cx(idb) + 20;
    y = cy(idb) + 10;
    CHECK(gui_inside(x, y, g.win[ida].x, g.win[ida].y, g.win[ida].w, g.win[ida].h));
    at(x, y);
    fake_wins[a].st.alerts++;
    gui_tick(&g);
    hold(POINTER_LEFT);
    gui_input_done(&g);
    CHECK(g.order[GUI_SLOTS - 1] == idb && g.clients.grab == idb && last(b, WIN_EV_POINTER)->value == POINTER_LEFT);
    hold(0);
    tick();
    CHECK(g.order[GUI_SLOTS - 1] == ida && g.focus == idb && client_waiting(&g, ida));
    /* a whole click queued in that pass: Other's, and the window comes up after it */
    gui_open(&g, idb);
    tick();
    n = count(a, WIN_EV_POINTER);
    nb = count(b, WIN_EV_POINTER);
    fake_wins[a].st.alerts++;
    g.now = 10000;
    gui_tick(&g);
    hold(POINTER_LEFT);
    hold(0);
    gui_input_done(&g);
    CHECK(g.order[GUI_SLOTS - 1] == ida && g.focus == idb && count(a, WIN_EV_POINTER) == n &&
          count(b, WIN_EV_POINTER) == nb + 2);
    /* for GUI_GRACE_MS a press on it, anywhere on it, is nobody's: no focus, no grab, no drag, nothing for its
       program, nor the moves while it is held; the first after that is the window's */
    g.now += GUI_GRACE_MS - 1;
    hold(POINTER_LEFT);
    at(x + 6, y + 4);
    CHECK(g.focus == idb && g.clients.grab < 0 && count(a, WIN_EV_POINTER) == n);
    hold(0);
    CHECK(g.focus == idb && last(a, WIN_EV_POINTER)->value == 0);      /* a move over it, without buttons */
    at(g.win[ida].x + 60, g.win[ida].y + 8);            /* its title bar */
    hold(POINTER_LEFT);
    at(g.x + 30, g.y + 20);
    CHECK(g.drag < 0 && g.focus == idb && g.win[ida].x + 60 == g.x - 30);
    hold(0);
    g.now += 1;
    at(x, y);
    hold(POINTER_LEFT);
    CHECK(g.focus == ida && g.clients.grab == ida && last(a, WIN_EV_POINTER)->value == POINTER_LEFT);
    hold(0);
    /* given the focus otherwise within that time (its taskbar button, Next window), it is the user's: a press is its */
    gui_act(&g, ACT_MINIMIZE);
    gui_open(&g, idb);
    tick();
    fake_wins[a].st.alerts++;
    tick();
    CHECK(g.order[GUI_SLOTS - 1] == ida && g.focus == idb);
    gui_open(&g, ida);
    tick();
    hold(POINTER_LEFT);
    CHECK(g.focus == ida && g.clients.grab == ida && last(a, WIN_EV_POINTER)->value == POINTER_LEFT);
    hold(0);
    /* held during a drag of the desktop's (Other's title bar) and during a program's grab, until each ends */
    gui_act(&g, ACT_MINIMIZE);
    gui_open(&g, idb);
    tick();
    at(g.win[idb].x + 60, g.win[idb].y + 8);
    hold(POINTER_LEFT);
    at(g.x + 30, g.y + 20);
    CHECK(g.drag == idb);
    fake_wins[a].st.alerts++;
    tick();
    CHECK(g.win[ida].min);
    hold(0);
    tick();
    CHECK(!g.win[ida].min && g.order[GUI_SLOTS - 1] == ida && g.focus == idb);
    gui_open(&g, ida);
    gui_act(&g, ACT_MINIMIZE);
    gui_open(&g, idb);
    tick();
    at(cx(idb) + 30, cy(idb) + 30);
    hold(POINTER_LEFT);
    CHECK(g.clients.grab == idb);
    fake_wins[a].st.alerts++;
    tick();
    at(g.x + 300, g.y);                                 /* the grab goes on outside its window */
    tick();
    CHECK(g.win[ida].min && g.clients.grab == idb);
    hold(0);
    tick();
    CHECK(!g.win[ida].min && g.order[GUI_SLOTS - 1] == ida && g.focus == idb);
    /* a program's text is the clipboard's only from the window with the focus when the desktop takes it */
    gui_open(&g, idb);
    tick();
    strlcpy(fake_clip, "from a program", sizeof(fake_clip));
    fake_clip_from = fake_wins[b].handle;
    tick();
    CHECK(g.clip_len == 14 && !memcmp(g.clip, "from a program", 14));
    tick();
    CHECK(g.clip_len == 14);                            /* nothing new: the clipboard stays */
    strlcpy(fake_clip, "too late", sizeof(fake_clip));  /* set as Other lost the focus, its FOCUS 0 not sent yet */
    gui_open(&g, ida);
    tick();
    CHECK(g.clip_len == 14 && !memcmp(g.clip, "from a program", 14) && !fake_clip[0]);
    strlcpy(fake_clip, "from Asker", sizeof(fake_clip));
    fake_clip_from = fake_wins[a].handle;
    gui_open(&g, GUI_NOTEPAD);                          /* the focus on a window of the desktop's: nobody's text */
    tick();
    CHECK(g.clip_len == 14 && !fake_clip[0]);
    strlcpy(fake_clip, "from Asker", sizeof(fake_clip));
    gui_open(&g, ida);
    tick();
    CHECK(g.clip_len == 10 && !memcmp(g.clip, "from Asker", 10));
}

int main(void) {
    test_appear();
    test_clip();
    test_hover();
    test_grab();
    test_grab_ends();
    test_wheel();
    test_resize();
    test_limits();
    test_keys();
    test_focus();
    test_focus_kept();
    test_close();
    test_gone();
    test_ninth();
    test_menus();
    test_leave();
    test_no_system();
    test_launcher();
    test_app_program();
    test_asked_focus();
    test_alert_clip();
    test_guards(640, 400);
    test_guards(960, 540);
    test_guards(1280, 800);
    return fake_done("test_guiclient");
}
