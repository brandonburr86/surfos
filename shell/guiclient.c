/*
SurfOS Desktop: Programs' Windows
--------------------
File: guiclient.c Date: 10/25/26
--------------------
The windows of ordinary programs on the desktop (milestone M12,
docs/superpowers/plans/2026-10-25-app-windows.md), in the GUI_CLIENTS slots after the fixed
windows (sys/gui_client.h), through struct gui_sys's win_* calls: the kernel's side is
shell/guisys_win.c on kernel/win.c, the host tests' tests/host/gui_fake.c.

Every frame (client_tick(), from gui_tick()) each window is read again: its title, the size of
its pixels and the pixels themselves, lent until the next read (a new serial draws the desktop
again); one whose program has gone is let go and shut. Then windows that are ready (their
program presented, or 250 ms passed since it opened them) come into the free slots, in front,
opening in a cascade. 10/25/26 (the review): in front without the focus, which stays where it
was, so that a program in the background never takes keys typed into another window (the rest
of a password at a hidden prompt); its taskbar button is marked until a click on it, on its
taskbar button, or Next window gives it the focus. 10/25/26 (stage C6): the windows asked for
take it as they appear: the first of a program the launcher just started (client_launched()),
and that of the focused Terminal's foreground job (typed at its prompt without &). A window's frame follows its pixels: a pixel of border and the title bar
around them, never narrower than the title bar needs. A frame given another size, by its corner
or by maximizing, is the program's to answer: once the corner is let go its program is told the
new content size (one RESIZE, the Terminal's rule), and until it presents pixels of that size
the old ones are drawn on the window's background. FOCUS tells a program when its window comes
to the front or leaves it.

The pointer (client_pointer(), from gui_pointer()). Every event over a program's visible
content is its own: hover, and all three buttons (the desktop's own windows only see the left
one). A press there, with any button, brings the window to the front and holds the pointer for
it until every button is up: its program gets the events wherever the pointer goes meanwhile (a
grab). The corner grip of a resizable window stays the desktop's, and a button held since a
press elsewhere (a drag from the desktop) is not the program's either. When the pointer leaves
the content one event says where it went: a position outside the content. A dialog, a menu, the
window minimized or let go, or the buttons let go with no event (the screen came back) end a
grab with an event without buttons. The wheel over the content is its program's (rows,
positive down). Keys (client_key()): while a program's window is in front every key but Ctrl+G
and Ctrl+Space (the menus) is its program's, as in the Terminal.

Closing. The close button (or Close in the menus) sends CLOSE; the program closes its window,
or asks its own questions. Pressed again, the desktop asks "End the program?" (Cancel first),
and End kills it through struct gui_sys's kill, whose refusals stand, and lets the window go.
Leaving the desktop with programs' windows open asks first (guicore.c). When the desktop closes
(client_finish()), every window is let go: the system ends them for their programs, which are
not killed.

The model's rules hold here: no allocation and no waiting; the system is called from the
handlers and the tick only, never while drawing, and drawing reads only what the last update
lent.
*/
#include "guipriv.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* the keys and buttons reach a program with the desktop's numbers */
typedef char keys_are_the_desktops[WIN_KEY_UP == GUI_UP && WIN_KEY_PGDN == GUI_PGDN && WIN_KEY_INSERT == GUI_INSERT ? 1 : -1];
typedef char buttons_are_the_pointers[WIN_BUTTON_LEFT == POINTER_LEFT && WIN_BUTTON_RIGHT == POINTER_RIGHT &&
                                      WIN_BUTTON_MIDDLE == POINTER_MIDDLE ? 1 : -1];

#define FRAME_W 2                       /* a frame beyond its content: a pixel of border at each side, */
#define FRAME_H (TITLE_H + 1)           /* the title bar above and a pixel below */
#define MIN_W 136                       /* the narrowest frame: the icon, a few letters of the title, three buttons */
#define MIN_H (TITLE_H + 24)
#define GRIP 14                         /* a resizable window's corner, the desktop's own */
#define BUTTONS (POINTER_LEFT | POINTER_RIGHT | POINTER_MIDDLE)

const struct gui_client *client_at(const struct gui_state *g, int id) {
    const struct gui_client *c;
    if(id < GUI_FIRST_CLIENT || id >= GUI_SLOTS) return NULL;
    c = &g->clients.slot[id - GUI_FIRST_CLIENT];
    return c->used ? c : NULL;
}

struct gui_client *client_of(struct gui_state *g, int id) {
    return (struct gui_client *)client_at(g, id);
}

/* 10/25/26: opened without the focus, and not given it yet: its taskbar button is marked */
bool client_waiting(const struct gui_state *g, int id) {
    const struct gui_client *c = client_at(g, id);
    return c && c->waiting && g->focus != id;
}

int client_count(const struct gui_state *g) {
    int k, n = 0;
    for(k = 0; k < GUI_CLIENTS; k++) if(g->clients.slot[k].used) n++;
    return n;
}

const char *client_title(const struct gui_state *g, int id) {
    const struct gui_client *c = client_at(g, id);
    return c && c->st.title[0] ? c->st.title : "Program";
}

bool client_resizable(const struct gui_state *g, int id) {
    const struct gui_client *c = client_at(g, id);
    return c && (c->st.flags & WIN_RESIZABLE);
}

int client_min_width(const struct gui_state *g, int id) {
    const struct gui_client *c = client_at(g, id);
    int w = c ? c->st.min_w + FRAME_W : 0;
    return w > MIN_W ? w : MIN_W;
}

int client_min_height(const struct gui_state *g, int id) {
    const struct gui_client *c = client_at(g, id);
    int h = c ? c->st.min_h + FRAME_H : 0;
    return h > MIN_H ? h : MIN_H;
}

/* a frame no larger than its program allows (max_w, max_h: 0 for no limit) */
void client_limit(const struct gui_state *g, int id, int *w, int *h) {
    const struct gui_client *c = client_at(g, id);
    if(!c) return;
    if(c->st.max_w && *w > c->st.max_w + FRAME_W) *w = c->st.max_w + FRAME_W;
    if(c->st.max_h && *h > c->st.max_h + FRAME_H) *h = c->st.max_h + FRAME_H;
    if(*w < client_min_width(g, id)) *w = client_min_width(g, id);
}

/* the window's content: its top left on the desktop and its size */
static void content(const struct gui_state *g, int id, int *x, int *y, int *w, int *h) {
    const struct gui_window *win = &g->win[id];
    *x = win->x + 1;
    *y = win->y + TITLE_H;
    *w = win->w - FRAME_W;
    *h = win->h - FRAME_H;
}

static void send(struct gui_state *g, struct gui_client *c, int type, int x, int y, unsigned value) {
    struct win_event e;
    if(!g->sys || !g->sys->win_event) return;
    memset(&e, 0, sizeof(e));
    e.type = (unsigned short)type;
    e.x = x;
    e.y = y;
    e.value = value;
    g->sys->win_event(g->sys->ctx, c->handle, &e);
}

/* the frame around pixels of w x h, kept on the screen */
static void fit(struct gui_state *g, int id, int w, int h) {
    struct gui_window *win = &g->win[id];
    win->w = w + FRAME_W > MIN_W ? w + FRAME_W : MIN_W;
    win->h = h + FRAME_H > MIN_H ? h + FRAME_H : MIN_H;
    gui_place(g, id);
}

/* the window let go: the system told, its slot free, its window shut (and the question about it gone) */
static void drop(struct gui_state *g, int id) {
    struct gui_client *c = client_of(g, id);
    if(!c) return;
    if(g->sys && g->sys->win_close) g->sys->win_close(g->sys->ctx, c->handle);
    c->used = false;
    if(g->clients.grab == id) g->clients.grab = -1;
    if(g->clients.hover == id) g->clients.hover = -1;
    if(g->drag == id) g->drag = -1;
    if(g->resize == id) g->resize = -1;
    if(g->dialog.kind && g->dialog.act == ACT_CLIENT_END + (id - GUI_FIRST_CLIENT)) dialog_close(g);
    g->win[id].max = false;
    gui_close_now(g, id);
}

/* a grab ends when the buttons were let go with no event (the screen came back), or a dialog, a menu, the window
   minimized or shut takes the pointer: its program gets an event without buttons */
static void grab_check(struct gui_state *g) {
    int id = g->clients.grab;
    struct gui_client *c = client_of(g, id);
    if(id < 0) return;
    if(!c) { g->clients.grab = -1; return; }
    if((g->buttons & BUTTONS) && !g->dialog.kind && !g->menu && g->win[id].open && !g->win[id].min) return;
    g->clients.grab = -1;
    if(g->clients.hover == id) g->clients.hover = -1;   /* the next move over it is news again */
    c->buttons = 0;
    send(g, c, WIN_EV_POINTER, c->px, c->py, 0);
}

/* the window's state and pixels read again; the frame follows a new size of its pixels, a new frame size is told
   once its corner is let go, and the focus when it changes */
static void update(struct gui_state *g, int id) {
    struct gui_client *c = client_of(g, id);
    struct gui_window *win = &g->win[id];
    struct win_state st;
    struct win_view view;
    bool focus, resized;
    int cw, ch;
    if(g->sys->win_update(g->sys->ctx, c->handle, &st, &view) < 0) { drop(g, id); return; }
    resized = st.w != c->st.w || st.h != c->st.h;
    if(resized || st.serial != c->st.serial || view.pixels != c->view.pixels) g->dirty = true;
    c->st = st;
    c->view = view;
    if(resized) {                                       /* pixels of another size: the frame follows them */
        c->asked = false;
        if(g->resize != id && !win->max) fit(g, id, st.w, st.h);
    }
    cw = win->w - FRAME_W;
    ch = win->h - FRAME_H;
    if((st.flags & WIN_RESIZABLE) && g->resize != id && (cw != st.w || ch != st.h) &&
       !(c->asked && cw == c->sent_w && ch == c->sent_h)) {
        send(g, c, WIN_EV_RESIZE, cw, ch, 0);
        c->asked = true;
        c->sent_w = cw;
        c->sent_h = ch;
    }
    focus = g->focus == id && !win->min;
    if(focus) c->waiting = false;                       /* 10/25/26: it has it now */
    if(focus != c->focused) {
        c->focused = focus;
        send(g, c, WIN_EV_FOCUS, 0, 0, focus);
    }
}

/* 10/25/26 (stage C6): the launcher started a program: its first window takes the focus (the oldest record makes
   room) */
void client_launched(struct gui_state *g, int pid) {
    int k;
    for(k = GUI_LAUNCHED - 1; k > 0; k--) g->clients.launched[k] = g->clients.launched[k - 1];
    g->clients.launched[0] = pid;
}

/* a window that may take the focus as it appears: its program's first since the launcher started it, or the focused
   Terminal's foreground job's (the kernel's foreground, sys/win.h's win_state) */
static bool takes_focus(struct gui_state *g, const struct gui_client *c) {
    int k;
    for(k = 0; k < GUI_LAUNCHED; k++)
        if(c->st.pid > 0 && g->clients.launched[k] == c->st.pid) {
            g->clients.launched[k] = 0;
            return true;
        }
    return c->st.foreground && g->focus == GUI_TERMINAL && g->win[GUI_TERMINAL].open && !g->win[GUI_TERMINAL].min;
}

/* windows that are ready come into the free slots, in front, in a cascade from the top left; 10/25/26: without the
   focus (their taskbar buttons marked), but for the launcher's program and the focused Terminal's foreground job */
static void accept(struct gui_state *g) {
    struct gui_client *c;
    struct gui_window *win;
    int k, h, id, step, menu;
    for(k = 0; k < GUI_CLIENTS; k++) {
        c = &g->clients.slot[k];
        if(c->used) continue;
        h = g->sys->win_accept(g->sys->ctx);
        if(h < 0) return;
        id = GUI_FIRST_CLIENT + k;
        memset(c, 0, sizeof(*c));
        c->used = true;
        c->handle = h;
        if(g->sys->win_update(g->sys->ctx, h, &c->st, &c->view) < 0) {    /* gone already */
            g->sys->win_close(g->sys->ctx, h);
            c->used = false;
            continue;
        }
        win = &g->win[id];
        memset(win, 0, sizeof(*win));
        step = g->clients.opened++ % 8;
        win->x = 72 + step * 24;
        win->y = MENU_H + 24 + step * 20;
        fit(g, id, c->st.w, c->st.h);
        win->open = true;                               /* gui_open() opens no closed slot of a program's */
        menu = g->menu;
        if(takes_focus(g, c)) gui_open(g, id);          /* 10/25/26 (C6): asked for: the focus */
        else {
            gui_raise(g, id);                           /* 10/25/26: the focus stays where it is */
            c->waiting = true;
        }
        g->menu = menu;                                 /* a menu open meanwhile stays open */
    }
}

void client_tick(struct gui_state *g) {
    int id;
    if(!g->sys || !g->sys->win_accept || !g->sys->win_update) return;
    grab_check(g);
    for(id = GUI_FIRST_CLIENT; id < GUI_SLOTS; id++) if(client_of(g, id)) update(g, id);
    accept(g);
}

void gui_client_room(const struct gui_state *g, int *w, int *h) {
    *w = g->width - FRAME_W;
    *h = g->height - MENU_H - TASK_H - FRAME_H;
}

void client_finish(struct gui_state *g) {
    int id;
    for(id = GUI_FIRST_CLIENT; id < GUI_SLOTS; id++) {
        struct gui_client *c = client_of(g, id);
        if(!c) continue;
        if(g->sys && g->sys->win_close) g->sys->win_close(g->sys->ctx, c->handle);
        c->used = false;
    }
}

/**** the pointer ****/

/* the window in front at the pointer (between the menu bar and the taskbar), -1 for none */
static int under(const struct gui_state *g) {
    int i, id;
    const struct gui_window *w;
    if(g->y < MENU_H || g->y >= g->height - TASK_H) return -1;
    for(i = GUI_SLOTS - 1; i >= 0; i--) {
        id = g->order[i];
        w = &g->win[id];
        if(w->open && !w->min && gui_inside(g->x, g->y, w->x, w->y, w->w, w->h)) return id;
    }
    return -1;
}

/* the pointer over the window's content, but for the corner grip a resizable window keeps: x, y from its top left */
static bool over(const struct gui_state *g, int id, int *x, int *y) {
    const struct gui_window *win = &g->win[id];
    int cx, cy, cw, ch;
    content(g, id, &cx, &cy, &cw, &ch);
    *x = g->x - cx;
    *y = g->y - cy;
    if(!gui_inside(g->x, g->y, cx, cy, cw, ch)) return false;
    return !(client_resizable(g, id) && !win->max && g->x >= win->x + win->w - GRIP && g->y >= win->y + win->h - GRIP);
}

/* the pointer left the content it was over: one event says where it went (a position outside the content: over the
   grip, which is inside, as -1, -1) */
static void leave(struct gui_state *g) {
    int id = g->clients.hover, cx, cy, cw, ch;
    struct gui_client *c = client_of(g, id);
    g->clients.hover = -1;
    if(!c) return;
    content(g, id, &cx, &cy, &cw, &ch);
    c->px = g->x - cx;
    c->py = g->y - cy;
    if(gui_inside(g->x, g->y, cx, cy, cw, ch)) c->px = c->py = -1;
    c->buttons = g->buttons & BUTTONS;
    send(g, c, WIN_EV_POINTER, c->px, c->py, c->buttons);
}

bool client_pointer(struct gui_state *g, unsigned old) {
    struct gui_clients *cs = &g->clients;
    struct gui_client *c;
    unsigned pressed = g->buttons & ~old & BUTTONS;
    int id, x, y, cx, cy, cw, ch;
    if(cs->grab >= 0) {                                 /* its own until every button is up, wherever it goes */
        id = cs->grab;
        c = client_of(g, id);
        if(!c) { cs->grab = -1; return false; }
        content(g, id, &cx, &cy, &cw, &ch);
        c->px = g->x - cx;
        c->py = g->y - cy;
        c->buttons = g->buttons & BUTTONS;
        send(g, c, WIN_EV_POINTER, c->px, c->py, c->buttons);
        if(!c->buttons) {                               /* let go: over its content, it hovers on; outside, the */
            cs->grab = -1;                              /* event just sent said where it went */
            cs->hover = under(g) == id && over(g, id, &x, &y) ? id : -1;
            if(cs->hover < 0 && gui_inside(g->x, g->y, cx, cy, cw, ch)) {   /* over the grip, or another window */
                c->px = c->py = -1;
                send(g, c, WIN_EV_POINTER, -1, -1, 0);
            }
        }
        return true;
    }
    if(g->menu || g->drag >= 0 || g->resize >= 0 || g->scroll >= 0 || g->note.selecting || g->app_drag >= 0 ||
       g->drawing) {
        leave(g);
        return false;
    }
    id = under(g);
    c = client_of(g, id);
    if(!c || !over(g, id, &x, &y) || ((g->buttons & BUTTONS) && !pressed)) {   /* a button held since a press elsewhere */
        leave(g);                                       /* (a drag from the desktop): not its own */
        return false;
    }
    if(cs->hover != id) leave(g);
    cs->hover = id;
    if(pressed) {                                       /* any button: to the front, and the pointer is its own */
        gui_open(g, id);
        cs->grab = id;
    }
    c->px = x;
    c->py = y;
    c->buttons = g->buttons & BUTTONS;
    send(g, c, WIN_EV_POINTER, x, y, c->buttons);
    return true;
}

void client_wheel(struct gui_state *g, int id, int rows) {
    struct gui_client *c = client_of(g, id);
    int x, y;
    if(c && over(g, id, &x, &y)) send(g, c, WIN_EV_WHEEL, x, y, (unsigned)rows);
}

/**** keys and closing ****/

bool client_keys(const struct gui_state *g) {
    return client_at(g, g->focus) && !g->win[g->focus].min;
}

void client_key(struct gui_state *g, int key) {
    struct gui_client *c = client_of(g, g->focus);
    if(c) send(g, c, WIN_EV_KEY, 0, 0, (unsigned)key);
}

void client_close(struct gui_state *g, int id) {
    struct gui_client *c = client_of(g, id);
    char name[48], line[80];
    if(!c) return;
    if(!c->closing) {
        c->closing = true;
        send(g, c, WIN_EV_CLOSE, 0, 0, 0);
        return;
    }
    gui_middle(client_title(g, id), name, sizeof(name), 24);
    snprintf(line, sizeof(line), "%s (pid %d) has not closed its window.", name, c->st.pid);
    dialog_confirm(g, ACT_CLIENT_END + (id - GUI_FIRST_CLIENT), "End the program?", line,
                   "What it has not saved is lost.", "End", NULL, "Cancel");
    snprintf(g->dialog.target, sizeof(g->dialog.target), "%d", c->st.pid);
    g->dialog.choice = 2;                               /* Enter alone ends nothing */
}

/* "End the program?" answered: End kills it (the system's refusal is shown) and lets its window go */
void client_answer(struct gui_state *g, int slot, int button, const char *target) {
    int id = GUI_FIRST_CLIENT + slot;
    struct gui_client *c = client_of(g, id);
    char msg[80];
    if(button != 0 || !c || c->st.pid != atoi(target)) return;     /* the slot may hold another window by now */
    msg[0] = 0;
    if(g->sys && g->sys->kill && g->sys->kill(g->sys->ctx, c->st.pid, msg, sizeof(msg)) < 0) {
        dialog_message(g, "Not ended", msg, "");
        return;
    }
    drop(g, id);
}

/**** drawing ****/

void client_render(const struct gui_state *g, const struct gui_draw *d, int id) {
    const struct gui_client *c = client_at(g, id);
    int x, y, w, h;
    if(!c) return;
    content(g, id, &x, &y, &w, &h);
    gd_rect(d, x, y, w, h, c->st.background);
    if(c->view.pixels) gd_blit(d, x, y, c->view.pixels, c->view.w, c->view.h, c->view.w);
}
