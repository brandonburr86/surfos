/*
SurfOS windows for programs
--------------------
File: win.c     Date: 10/25/26
--------------------
SYS_winopen and SYS_winctl (surfos/win.h), and the desktop's side of the windows at the end of
surfos/win.h (shell/gui.c, shell/guisys_win.c, shell/guisys_term.c). Milestone M12, stage C3 of
docs/superpowers/plans/2026-10-25-app-windows.md.

A window is a descriptor, as a pseudo-terminal's master is (kernel/pty.c): a vnode of winfs, a
file system with no paths that is never mounted, opened as a file of the program's. read()
gives its events, poll() says what is ready, and the vnode's release, at the program's last
close of it or its reap after a kill, ends the window. A desktop registers itself the same way
(win_register()): a vnode of winfs that is a descriptor of the desktop's own task, so that its
release, when the desktop closes it or when its task is reaped after it died, lets every window
of that desktop go (the trick of shell/guisys_term.c's notes of 10/18/26). A window lives while
its program holds it or its desktop does (struct win's prog and held); the second to let go
frees it.

Which desktop a window goes to: the one of the caller's terminal, the console where gui
registered it or a pty that desktop's Terminal adopted (win_adopt(), by the pty's number); there is none for text
mode, another console, telnet's ptys or a desktop whose task is dead (-ENODEV). The arguments
are checked before, so a text console still sees -EFAULT and -EINVAL.

Pixels are copied, never shared: no frame of the kernel is mapped into a program. A window's
buffer is the kernel's, allocated and filled with the window's background before anything sees
it, so old contents of the heap never reach the screen. WIN_PRESENT checks the rectangle's
whole span in the program's memory (64-bit arithmetic; rows at most WIN_STRIDE_MAX pixels
apart) and copies it in during the program's own call, with interrupts on: a desktop frame drawn
meanwhile may show one torn frame, and the next mends it. WIN_SETSIZE allocates a new buffer;
the old one lives while the desktop still draws it. A buffer counts its holders (the window, the
desktop's view of it, a present copying into it), and the last one frees it.

Budgets: WIN_PER_DESK windows and WIN_DESK_BYTES of pixels per desktop, WIN_MAX windows and
WIN_ALL_BYTES in all, counted from the buffers alive, the desktop's views too; a buffer no larger
than its desktop's largest content area; and each window takes one of its program's descriptors.

Locking: the tables, the windows' fields and the buffers' counts are looked at and changed with
interrupts off, in short sections, and nothing else: no mutex. Memory is allocated, filled and
freed with interrupts on; pixels and events are copied to and from the program with interrupts
on. Nothing here calls the framebuffer, and the desktop never calls here from inside a
framebuffer_graphics_draw() callback. Whatever changes what a window or the desktop is ready for
calls poll_wake().

10/25/26 (the review of stages C1-C3): a pty's adoption ends with its hangup or its freeing
(win_pty_gone(), from kernel/pty.c), so a later session on the same pty (a telnet session's)
opens no window; a pointer move is merged only into a move, never into the press or release
before it; a window its desktop let go keeps no pixels (its size stays, for WIN_INFO); and a
window belongs to the program that opened it: its death takes the window away at once, also
when a child of it holds the descriptor.

10/26/26 (stage C12: the desktop's applications as programs): WIN_CLIP puts a window's text on
its desktop's clipboard, only while the window has the focus (asked before the text is copied
and again after): the text waits in the desktop's record (struct desk's clip, the latest
replacing the one before, with the handle of the window it came from) until the desktop takes
it (win_clip_take()), which keeps it only when that window has its focus still; text only, it
ends at a NUL, and empty text is refused. WIN_ALERT counts in the window's state
(win_state.alerts), once in WIN_ALERT_MS at most for its program, whichever of its windows asks
(struct alerter): the desktop shows the window in front of the others, without the focus.

11/05/26 (docs/superpowers/plans/2026-11-05-desktop-apps.md, track desk, stage 1: the ABI): WIN_THEME,
the desktop's theme and its colours (until a desktop gives its own, the ocean theme's below); and
the argument checks of WIN_MENU, WIN_PASTE, WIN_DIALOG and WIN_TAKE (surfos/win.h), each made on a
copy in the kernel's memory, never on the program's, which may change meanwhile.
11/05/26 (the review of the ABI): a menu's labels and shortcuts are UTF-8 by the console's own
rules (driver/cp437.c's utf8_feed(), as the desktop draws them) with no control character, and the
bar's cells are counted as the desktop draws them; no title passes for the desktop's own; menus of
14 entries at most, submenus one level deep and one Help merged with the desktop's; a chooser's
flags and types as surfos/win.h's win_types() takes them (16 types, whole names after '='); the
theme says how many of its slots it filled.

11/05/26 (stage 2): the ops themselves, and their desktop's side. A window's menus are the kernel's
copy of its last WIN_MENU (struct menus, held by a count), lent to the desktop as its pixels are:
win_update() gives the desktop the copy in force, which stays its until its next update or close,
so that nothing is copied with interrupts off. The clipboard is the desktop's (shell/gui.c's
state): the desktop gives this file a copy of it (win_paste_give()) while one of its windows has
the focus and the text changed. A window's WIN_CLIP that its desktop has not taken yet is newer:
that window's paste gets it, and the desktop's taking it (win_clip_take()) makes it the copy in
the same section, so that a paste never gets text older than the last copy. WIN_PASTE copies
during the program's call, holding the text by a count (struct paste), so that a new copy given
meanwhile frees the old one only after the call. A chooser asked for (WIN_DIALOG,
only after a key, a press or a menu's item came to the window since its last request) waits in
the window (ask) until the desktop takes it (win_dialog_take(), which numbers it) and answers it
by that number (win_dialog_answer()): the path chosen waits in the window (answer) for WIN_TAKE,
and the answer's WIN_EV_DIALOG, whose place in the queue is kept while the request waits, is
queued in the same section as the path is set. A desktop that lets the window go answers 0 for a
request that waits; a path chosen stays for WIN_TAKE until the program lets go. The theme is the
desktop's (win_theme_set()), kept in each window too, so that WIN_THEME says the last one after
the hang-up; a window that asked WIN_THEME gets WIN_EV_THEME at each change. The menus and a
request go when either side lets the window go: neither can use them after.
*/

#include <surfos/types.h>
#include <surfos/task.h>
#include <surfos/tty.h>
#include <surfos/wait.h>
#include <surfos/irq.h>
#include <surfos/timer.h>
#include <surfos/poll.h>
#include <surfos/win.h>
#include <surfos/sysimpl.h>
#include <mm/kalloc.h>
#include <fs/vfs.h>
#include <sys/cp437.h>                  /* 11/05/26: utf8_feed(), the console's UTF-8, which the desktop draws by */
#include <blibc_common.h>

#define SHOW_TICKS (250 * HZ / 1000)    /* a window that never presented appears after 250 ms */
#define DESK_PTYS 4                     /* ptys a desktop adopts (its Terminal's) */
#define USER_PAGE 4096

struct winbuf { u32 *px; int w, h; u_int refs; };
/* 11/05/26: text for WIN_PASTE, held by a count while a paste copies from it: the clipboard as the desktop gave it
   last, or a window's WIN_CLIP its desktop has not taken yet */
struct paste { u_int refs; u32 len; char text[]; };
/* 11/05/26: a window's menus (WIN_MENU), held by a count: the window's own, and the copy lent to its desktop */
struct menus { u_int refs; u32 count; struct win_menu_item it[]; };

struct desk {
    bool used;
    u_long owner;                       /* the desktop's task */
    struct tty *tty;                    /* its console */
    struct tty *ptys[DESK_PTYS];        /* its Terminal's */
    u_long shells[DESK_PTYS];           /* 10/25/26: the session shell on each (the Terminal's surfsh), 0: unknown */
    struct win_desk info;
    struct paste *clip;                 /* 10/26/26: text a window put on the clipboard, for the desktop to take */
    int clip_from;                      /* and the handle of that window */
    struct paste *paste;                /* 11/05/26: the desktop's clipboard, for WIN_PASTE; NULL: empty */
    bool themed;                        /* 11/05/26: the desktop said its theme: */
    u32 theme, colors[WIN_COLORS];
};

struct win {
    bool used;
    bool prog;                          /* its program's file holds it */
    bool held;                          /* its desktop took it (win_accept()) and has not let it go */
    bool hung;                          /* its desktop let it go, or ended: reads end, presents fail */
    u_short gen;                        /* its handles' generation */
    struct desk *desk;                  /* NULL once it is hung up */
    u_long owner;                       /* the program that opened it */
    u_long opened, seq;                 /* when it opened (ticks), and in what order */
    struct winbuf *buf;                 /* its pixels */
    struct winbuf *view;                /* those its desktop draws, from its last update; NULL */
    int min_w, min_h, max_w, max_h;
    u32 flags, background, serial, presents;
    u32 alerts;                         /* 10/26/26: how many times it asked to be seen (WIN_ALERT) */
    bool themed;                        /* 11/05/26: its program asked WIN_THEME: it hears of a new theme */
    u32 theme, colors[WIN_COLORS];      /* 11/05/26: the theme it has, its desktop's (the last after the hang-up) */
    struct menus *menu;                 /* 11/05/26: its menus (WIN_MENU), the kernel's copy; NULL for none */
    struct menus *menu_view;            /* those its desktop has, from its last update; NULL */
    u32 menu_serial;                    /* how many times they were set */
    struct win_dialog *ask;             /* 11/05/26: the chooser it asked for, until the desktop takes it; NULL */
    bool asking;                        /* a request not answered yet: a place in the queue is kept for its answer */
    bool input;                         /* a key, a press or a menu's item came to it since its last request */
    u32 ask_id, ask_no;                 /* the request's id (WIN_EV_DIALOG's x), and its number (the answer names it) */
    char *answer;                       /* the path the chooser chose, until WIN_TAKE or the program's end; NULL */
    bool focused;
    int size_w, size_h;                 /* its size: buf's, and still once the buffers went at the hang-up */
    char title[WIN_TITLE_MAX];
    struct win_event ev[WIN_EVENTS];    /* a ring */
    u_int ev_first, ev_count, lost;
    u32 buttons, buttons_before;        /* the last pointer event queued: its buttons, and those before it */
    wait_queue_t readers;
};

/* 11/05/26: the theme WIN_THEME gives until a desktop says its own: the ocean's, the colours the desktop draws its
   windows in (include/sys/guidraw.h, by role: surfos/win.h's WIN_COLOR_*) */
static const u32 ocean_colors[WIN_COLORS] = {
    0x245A67, 0xF4F1E6, 0x153D49, 0x6C888C, 0x197D83, 0xF4F1E6, 0xE7E2D2, 0xDEE3D8,
    0xC9CFC4, 0xBFE3DD, 0xFFFDF5, 0x153D49, 0x102E3B, 0xB8E9D9, 0xEE906D, 0xA3B4B3
};

static struct desk desks[WIN_DESKS];
static struct win wins[WIN_MAX];
static u_long opened_so_far;

/* 10/26/26: the programs that asked to be seen lately (WIN_ALERT), and when: one record a program, the oldest making
   room for a new one */
struct alerter { u_long pid, at; };
static struct alerter alerters[WIN_MAX];

/* the windows' and the desktops' file system: no paths, nothing to sync, never mounted */
static const struct fs_type winfs_type = { "winfs", NULL, NULL, NULL };
static struct superblock winfs = { &winfs_type, NULL, NULL, NULL, NULL, false, MUTEX_INIT("winfs"), 0 };
static const struct vnode_ops win_ops, desk_ops;

/**** the pure core: buffers, budgets, events, titles, which desktop ****/

/* a holder of b lets go (interrupts off): b itself when that was the last, for the caller to free with them on */
static struct winbuf *buf_put(struct winbuf *b) {
    return b && --b->refs == 0 ? b : NULL;
}

static void buf_free(struct winbuf *b) {
    if(!b) return;
    kfree(b->px);
    kfree(b);
}

/* w x h pixels of background, held once (interrupts on); NULL without memory */
static struct winbuf *buf_new(int w, int h, u32 background) {
    struct winbuf *b = (struct winbuf *)kcalloc(1, sizeof(*b));
    u32 i, n = (u32)w * (u32)h;
    if(!b) return NULL;
    b->px = (u32 *)kalloc(n * 4);
    if(!b->px) {
        kfree(b);
        return NULL;
    }
    for(i = 0; i < n; i++) b->px[i] = background;
    b->w = w;
    b->h = h;
    b->refs = 1;
    return b;
}

static u_long buf_bytes(const struct winbuf *b) {
    return b ? (u_long)b->w * (u_long)b->h * 4 : 0;
}

/* the pixels alive of desk d's windows (NULL: of every window), the desktops' views too; interrupts off */
static u_long bytes_of(const struct desk *d) {
    u_long n = 0;
    int i;
    for(i = 0; i < WIN_MAX; i++) {
        const struct win *w = &wins[i];
        if(!w->used || (d && w->desk != d)) continue;
        n += buf_bytes(w->buf);
        if(w->view != w->buf) n += buf_bytes(w->view);
    }
    return n;
}

static int windows_of(const struct desk *d) {
    int i, n = 0;
    for(i = 0; i < WIN_MAX; i++) if(wins[i].used && (!d || wins[i].desk == d)) n++;
    return n;
}

/* room for bytes more of pixels on desk d (and in all): 0, or -ENOMEM; interrupts off */
static int room_for(const struct desk *d, u_long bytes) {
    if(bytes_of(d) + bytes > WIN_DESK_BYTES || bytes_of(NULL) + bytes > WIN_ALL_BYTES) return -ENOMEM;
    return 0;
}

/* e into w's queue: a pointer move with the same buttons as the event queued last takes its place when that one was a
   move too (10/25/26: never a press or a release, whose place would go), and a new size the last one's; a full queue
   drops it and counts it. 1 queued, 0 merged, -EAGAIN lost. Interrupts off */
static int queue_event(struct win *w, const struct win_event *e) {
    struct win_event *last = w->ev_count ? &w->ev[(w->ev_first + w->ev_count - 1) % WIN_EVENTS] : NULL;
    if(last && last->type == e->type &&
       ((e->type == WIN_EV_POINTER && last->value == e->value && w->buttons_before == last->value) ||
        e->type == WIN_EV_RESIZE)) {
        last->x = e->x;
        last->y = e->y;
        last->mods = e->mods;
        return 0;
    }
    if(e->type == WIN_EV_THEME) {                       /* 11/05/26 (the review of stage 2): one waits at most, */
        u_int k;                                        /* the newest (the program asks the colours anyway) */
        for(k = 0; k < w->ev_count; k++) {
            struct win_event *q = &w->ev[(w->ev_first + k) % WIN_EVENTS];
            if(q->type == WIN_EV_THEME) {
                q->value = e->value;
                return 0;
            }
        }
    }
    if(w->ev_count >= WIN_EVENTS - (w->asking && e->type != WIN_EV_DIALOG ? 1 : 0)) {
        w->lost++;                                      /* 11/05/26: the last place is a request's answer's */
        return -EAGAIN;
    }
    w->ev[(w->ev_first + w->ev_count++) % WIN_EVENTS] = *e;
    if(e->type == WIN_EV_POINTER) {
        w->buttons_before = w->buttons;
        w->buttons = e->value;
    }
    return 1;
}

/* how many of s's len bytes a title keeps, at most limit, never ending inside a character (UTF-8): a character the
   cut would split goes whole */
static u32 title_cut(const char *s, u32 len, u32 limit) {
    u32 k = len < limit ? len : limit;
    if(k < len) while(k > 0 && ((u8)s[k] & 0xC0) == 0x80) k--;
    return k;
}

static void title_set(char *out, const char *s, u32 len) {
    u32 k = title_cut(s, len, WIN_TITLE_MAX - 1);
    memcpy(out, s, k);
    out[k] = 0;
}

/* a desktop's task is alive (not dead waiting to be reaped); interrupts off */
static bool alive(u_long pid) {
    surf_task *t = task_find(pid);
    return t && t->state != TS_DEAD;
}

/* the desktop of terminal t: the console where it runs, or a pty its Terminal adopted; NULL for any other (text
   mode, another console, telnet's ptys) and for a desktop whose task is dead. Interrupts off */
static struct desk *desk_of(const struct tty *t) {
    int i, k;
    bool its;
    if(!t) return NULL;
    for(i = 0; i < WIN_DESKS; i++) {
        struct desk *d = &desks[i];
        if(!d->used) continue;
        its = d->tty == t;
        for(k = 0; k < DESK_PTYS; k++) if(d->ptys[k] == t) its = true;
        if(its && alive(d->owner)) return d;            /* a dead one's console may have a new desktop already */
    }
    return NULL;
}

/* the calling task's desktop, NULL; interrupts off */
static struct desk *own_desk(void) {
    int i;
    for(i = 0; i < WIN_DESKS; i++) if(desks[i].used && curTask && desks[i].owner == curTask->pid) return &desks[i];
    return NULL;
}

/* a window's handle for its desktop, and the window of a handle that the calling task's desktop holds */
static int handle_of(const struct win *w) {
    return (int)((u_long)w->gen * WIN_MAX + (u_long)(w - wins));
}

static struct win *held_win(int h) {
    struct win *w;
    struct desk *d = own_desk();
    if(h < 0 || !d) return NULL;
    w = &wins[h % WIN_MAX];
    return w->used && w->held && w->desk == d && w->gen == (u_short)(h / WIN_MAX) ? w : NULL;
}

/* what goes when a window's side lets go, to be freed with interrupts on: its buffers, and (11/05/26) its menus, a
   request the desktop never took, a path never taken */
struct dead { struct winbuf *buf[2]; struct menus *menus[2]; void *mem[2]; };

/* 11/05/26: a holder of text for WIN_PASTE lets go (interrupts off): p itself when that was the last, to free with
   them on */
static struct paste *paste_put(struct paste *p) {
    return p && --p->refs == 0 ? p : NULL;
}

/* 11/05/26: a holder of menus lets go (interrupts off): m itself when that was the last, to free with them on */
static struct menus *menus_put(struct menus *m) {
    return m && --m->refs == 0 ? m : NULL;
}

static void dead_free(struct dead *d) {
    int i;
    for(i = 0; i < 2; i++) {
        buf_free(d->buf[i]);
        kfree(d->menus[i]);
        kfree(d->mem[i]);
    }
}

/* 11/05/26: the request that waits answered, WIN_EV_DIALOG (value: 1 a path was chosen, 0 cancelled) in the place
   kept for it (interrupts off) */
static void answer_queue(struct win *w, unsigned value) {
    struct win_event e;
    memset(&e, 0, sizeof(e));
    e.type = WIN_EV_DIALOG;
    e.value = value;
    e.x = (int)w->ask_id;
    w->asking = false;
    if(queue_event(w, &e) >= 0) wake_up_all(&w->readers);
}

/* the window let go of by its desktop (interrupts off): hung up, and its pixels go (10/25/26: a program that holds on
   to it keeps no memory with it; its size stays); free when its program let go too. What to free goes into dead.
   11/05/26: its menus go, and a request: one that waits is answered (cancelled); a path chosen stays for WIN_TAKE */
static void hang_up(struct win *w, struct dead *dead) {
    dead->buf[0] = buf_put(w->view);
    dead->buf[1] = buf_put(w->buf);
    dead->menus[0] = menus_put(w->menu);
    dead->menus[1] = menus_put(w->menu_view);
    dead->mem[0] = w->ask;
    w->menu = w->menu_view = NULL;
    w->ask = NULL;
    if(w->asking && w->prog) answer_queue(w, 0);
    w->asking = false;
    w->view = w->buf = NULL;
    w->desk = NULL;
    w->held = false;
    w->hung = true;
    wake_up_all(&w->readers);
    if(!w->prog) w->used = false;
}

/**** a program's side: the window's file ****/

/* whole events, as many as fit and are queued; waits for one; 0 once the window was let go and all was read */
static int win_read(struct vnode *v, u32 off, void *buf, u32 len) {
    struct win *w = (struct win *)v->priv;
    struct win_event chunk[WIN_EVENTS];
    u_int n, k;
    u_long flags;
    (void)off;
    if(len < sizeof(struct win_event)) return -EINVAL;
    n = len / sizeof(struct win_event);
    if(n > WIN_EVENTS) n = WIN_EVENTS;
    for(;;) {
        flags = irq_save();
        if(w->ev_count) {
            if(n > w->ev_count) n = w->ev_count;
            for(k = 0; k < n; k++) chunk[k] = w->ev[(w->ev_first + k) % WIN_EVENTS];
            w->ev_first = (w->ev_first + n) % WIN_EVENTS;
            w->ev_count -= n;
            irq_restore(flags);
            memcpy(buf, chunk, n * sizeof(chunk[0]));
            return (int)(n * sizeof(chunk[0]));
        }
        if(w->hung) {
            irq_restore(flags);
            return 0;
        }
        if(task_interrupted()) {
            irq_restore(flags);
            return -EINTR;
        }
        wait_prepare_intr(&w->readers);
        irq_restore(flags);
        yield();
    }
}

/* POLLIN with events or once let go, POLLHUP once let go and all was read, POLLOUT always */
static int win_poll(struct vnode *v, struct file *f) {
    struct win *w = (struct win *)v->priv;
    int r = POLLOUT;
    u_long flags = irq_save();
    if(w->ev_count || w->hung) r |= POLLIN;
    if(w->hung && !w->ev_count) r |= POLLHUP;
    irq_restore(flags);
    (void)f;
    return r;
}

/* the program's last close (or its reap): the window ends; freed now unless its desktop holds it, which then hears
   it ended at its next update */
static void win_release(struct vnode *v) {
    struct win *w = (struct win *)v->priv;
    struct dead dead;
    u_long flags;
    if(!w) return;                                      /* a vnode that never had its window */
    memset(&dead, 0, sizeof(dead));
    flags = irq_save();
    w->prog = false;
    dead.menus[0] = menus_put(w->menu);                 /* 11/05/26: nobody sets them again (its desktop keeps */
    dead.mem[0] = w->ask;                               /* the copy it has until its next update), nor asks, */
    dead.mem[1] = w->answer;                            /* nor takes a path */
    w->menu = NULL;
    w->ask = NULL;
    w->answer = NULL;
    w->asking = false;
    if(!w->held) {
        dead.buf[0] = buf_put(w->buf);
        dead.buf[1] = buf_put(w->view);
        dead.menus[1] = menus_put(w->menu_view);
        w->menu_view = NULL;
        w->buf = w->view = NULL;
        w->desk = NULL;
        w->used = false;
    }
    irq_restore(flags);
    dead_free(&dead);
    poll_wake();
}

static const struct vnode_ops win_ops = {
    NULL, NULL, win_read, NULL, NULL, NULL, NULL, win_release, NULL, win_poll
};

/* a free descriptor in the calling task's table */
static bool descriptor_free(void) {
    int i;
    for(i = 0; i < NR_OPEN; i++) if(!curTask->files[i]) return true;
    return false;
}

/* what o asks for is a window at all: -EINVAL otherwise (before any desktop is looked for) */
static int open_args(const struct win_open *o) {
    if(o->version != WIN_VERSION || (o->flags & ~(u32)WIN_RESIZABLE) || (o->background & 0xFF000000)) return -EINVAL;
    if(!o->w || !o->h) return -EINVAL;
    if((o->max_w && o->min_w > o->max_w) || (o->max_h && o->min_h > o->max_h)) return -EINVAL;
    return 0;
}

/* the size within the limits asked for and desktop d's: 0, or -EINVAL when they leave none; interrupts off */
static int open_size(const struct win_open *o, const struct desk *d, int *w, int *h) {
    int lo_w = o->min_w ? o->min_w : 1, lo_h = o->min_h ? o->min_h : 1;
    int hi_w = o->max_w && o->max_w < d->info.max_w ? o->max_w : d->info.max_w;
    int hi_h = o->max_h && o->max_h < d->info.max_h ? o->max_h : d->info.max_h;
    if(lo_w > hi_w || lo_h > hi_h) return -EINVAL;
    *w = o->w < lo_w ? lo_w : o->w > hi_w ? hi_w : o->w;
    *h = o->h < lo_h ? lo_h : o->h > hi_h ? hi_h : o->h;
    return 0;
}

/* desktop d may take a window of w x h now: 0, -EAGAIN, -ENOMEM; interrupts off */
static int open_room(const struct desk *d, int w, int h) {
    if(windows_of(d) >= WIN_PER_DESK || windows_of(NULL) >= WIN_MAX) return -EAGAIN;
    return room_for(d, (u_long)w * (u_long)h * 4);
}

int sys_winopen(const struct win_open *uo) {
    struct win_open o;
    struct desk *d;
    struct winbuf *b;
    struct win *w = NULL;
    struct vnode *v;
    struct file *f;
    u_long flags, owner;
    int r, i, wd = 0, ht = 0;
    if(!uo || !user_buf(uo, sizeof(o), false)) return -EFAULT;
    memcpy(&o, uo, sizeof(o));
    r = open_args(&o);
    if(r) return r;
    flags = irq_save();
    d = desk_of(curTask->tty);
    r = !d ? -ENODEV : open_size(&o, d, &wd, &ht);
    if(!r) r = open_room(d, wd, ht);
    owner = d ? d->owner : 0;
    irq_restore(flags);
    if(r) return r;
    if(!descriptor_free()) return -EMFILE;              /* this task alone changes its table */
    b = buf_new(wd, ht, o.background);
    if(!b) return -ENOMEM;
    v = vnode_alloc(&winfs, &win_ops, VN_FILE);
    if(!v) {
        buf_free(b);
        return -ENOMEM;
    }
    flags = irq_save();                                 /* again, now: the desktop may have gone, others opened */
    d = desk_of(curTask->tty);
    r = !d || d->owner != owner ? -ENODEV : open_room(d, wd, ht);
    for(i = 0; !r && i < WIN_MAX && !w; i++) if(!wins[i].used) w = &wins[i];
    if(!r && w) {
        u_short gen = (u_short)(w->gen + 1);
        memset(w, 0, sizeof(*w));                       /* its wait queue is empty: nobody waits on a free window */
        w->used = w->prog = true;
        w->gen = gen ? gen : 1;
        w->desk = d;
        w->owner = curTask->pid;
        curTask->flags |= TF_WINDOW;                    /* 11/09/26: for its end, when the desktop started it */
        w->opened = getticks();
        w->seq = ++opened_so_far;
        w->buf = b;
        w->size_w = wd;
        w->size_h = ht;
        w->min_w = o.min_w;
        w->min_h = o.min_h;
        w->max_w = o.max_w;
        w->max_h = o.max_h;
        w->flags = o.flags;
        w->background = o.background;
        title_set(w->title, o.title, (u32)strnlen(o.title, WIN_TITLE_MAX));
        w->theme = d->themed ? d->theme : WIN_THEME_OCEAN;     /* 11/05/26 */
        memcpy(w->colors, d->themed ? d->colors : ocean_colors, sizeof(w->colors));
        v->priv = w;
    }
    irq_restore(flags);
    if(r || !w) {
        buf_free(b);
        vput(v);                                        /* no window yet: its release does nothing */
        return r ? r : -EAGAIN;
    }
    r = vfs_open_vnode(v, O_RDONLY, &f);
    if(r) {
        vput(v);                                        /* the window ends with its vnode */
        return r;
    }
    r = fd_install(f);
    if(r < 0) vfs_close(f);
    poll_wake();                                        /* the desktop looks for windows that are ready */
    return r;
}

/* WIN_PRESENT: the rectangle of the program's image copied in, during the call */
static int present(struct win *w, const struct win_present *up) {
    struct win_present p;
    struct winbuf *b, *dead;
    u64 first, span, start = 0;
    u_long flags;
    u32 i, j;
    int r = 0;
    if(!up || !user_buf(up, sizeof(p), false)) return -EFAULT;
    memcpy(&p, up, sizeof(p));
    if(!p.stride || p.stride > WIN_STRIDE_MAX) return -EINVAL;
    flags = irq_save();
    if(w->hung) {
        irq_restore(flags);
        return -EPIPE;
    }
    b = w->buf;
    b->refs++;                                          /* held while it is copied into: a new size cannot free it */
    irq_restore(flags);
    if(p.w && p.h) {
        if((u32)p.x + p.w > (u32)b->w || (u32)p.y + p.h > (u32)b->h || (u32)p.x + p.w > p.stride) r = -EINVAL;
        else {
            first = (u64)p.y * p.stride + p.x;          /* from the image's top left to the rectangle's */
            span = ((u64)(p.h - 1) * p.stride + p.w) * 4;
            start = (u64)(u_long)p.pixels + first * 4;
            if(!p.pixels || start + span > 0x100000000ULL || !user_buf((const void *)(u_long)start, (u_long)span, false))
                r = -EFAULT;
        }
        for(j = 0; !r && j < p.h; j++) {
            const u32 *from = (const u32 *)(u_long)start + (u_long)j * p.stride;
            u32 *to = b->px + ((u32)p.y + j) * (u32)b->w + p.x;
            for(i = 0; i < p.w; i++) to[i] = from[i] & 0xFFFFFF;
        }
    }
    flags = irq_save();
    if(!r) {
        w->serial++;
        w->presents++;
    }
    dead = buf_put(b);
    irq_restore(flags);
    buf_free(dead);
    if(!r) poll_wake();
    return r;
}

/* WIN_SETSIZE: new pixels, all background, within the window's limits and its desktop's */
static int set_size(struct win *w, u32 arg) {
    int nw = (int)(arg >> 16), nh = (int)(arg & 0xFFFF);
    struct winbuf *b, *dead;
    u_long flags;
    int r = 0;
    if(!nw || !nh || nw < w->min_w || nh < w->min_h || (w->max_w && nw > w->max_w) || (w->max_h && nh > w->max_h))
        return -EINVAL;
    flags = irq_save();
    if(w->hung) r = -EPIPE;
    else if(nw > w->desk->info.max_w || nh > w->desk->info.max_h) r = -EINVAL;
    else if(nw == w->buf->w && nh == w->buf->h) r = 1;  /* the size it has */
    else r = room_for(w->desk, (u_long)nw * (u_long)nh * 4);
    irq_restore(flags);
    if(r) return r > 0 ? 0 : r;
    b = buf_new(nw, nh, w->background);
    if(!b) return -ENOMEM;
    flags = irq_save();
    r = w->hung ? -EPIPE : room_for(w->desk, buf_bytes(b));
    dead = NULL;
    if(!r) {
        dead = buf_put(w->buf);
        w->buf = b;
        w->size_w = nw;
        w->size_h = nh;
        w->serial++;
    }
    irq_restore(flags);
    buf_free(r ? b : dead);
    if(!r) poll_wake();
    return r;
}

/* a title from the program: up to WIN_TITLE_MAX bytes read (each page checked as it is reached), of which the first
   WIN_TITLE_MAX - 1 at most are kept, cut between characters (the last one read says whether one goes on) */
static int user_title(const char *u, char *out) {
    char t[WIN_TITLE_MAX];
    u32 n;
    for(n = 0; n < WIN_TITLE_MAX; n++) {
        if((n == 0 || (((u_long)u + n) & (USER_PAGE - 1)) == 0) && !user_buf(u + n, 1, false)) return -EFAULT;
        t[n] = u[n];
        if(!t[n]) break;
    }
    title_set(out, t, n);
    return 0;
}

static int set_title(struct win *w, const char *u) {
    char t[WIN_TITLE_MAX];
    u_long flags;
    int r;
    if(!u) return -EFAULT;
    r = user_title(u, t);
    if(r) return r;
    flags = irq_save();
    if(w->hung) r = -EPIPE;
    else {
        memcpy(w->title, t, sizeof(t));
        w->serial++;
    }
    irq_restore(flags);
    if(!r) poll_wake();
    return r;
}

static int info(struct win *w, struct win_info *u) {
    struct win_info k;
    u_long flags;
    if(!u || !user_buf(u, sizeof(k), true)) return -EFAULT;
    memset(&k, 0, sizeof(k));
    k.version = WIN_VERSION;
    flags = irq_save();
    k.w = (unsigned short)w->size_w;                   /* buf's, or the last it had: none once hung up */
    k.h = (unsigned short)w->size_h;
    if(w->desk) {
        k.screen_w = (unsigned short)w->desk->info.width;
        k.screen_h = (unsigned short)w->desk->info.height;
        k.scale = (unsigned short)w->desk->info.scale;
    }
    k.focused = w->focused;
    k.lost = w->lost;
    irq_restore(flags);
    memcpy(u, &k, sizeof(k));
    return 0;
}

/* 11/05/26: WIN_THEME: the theme and its colours; from now on the window hears of a new one (WIN_EV_THEME) */
static int theme(struct win *w, struct win_theme *u) {
    struct win_theme k;
    u_long flags;
    if(!u || !user_buf(u, sizeof(k), true)) return -EFAULT;
    memset(&k, 0, sizeof(k));
    k.count = WIN_COLORS;
    flags = irq_save();
    k.theme = w->theme;
    memcpy(k.colors, w->colors, sizeof(w->colors));
    w->themed = true;
    irq_restore(flags);
    memcpy(u, &k, sizeof(k));
    return 0;
}

/* 10/26/26: whether the window may put text on its desktop's clipboard now (interrupts off): -EPIPE once it is let go,
   -EACCES without the focus */
static int clip_may(const struct win *w) {
    if(w->hung || !w->desk) return -EPIPE;
    return w->focused ? 0 : -EACCES;
}

/* 10/26/26: WIN_CLIP: the program's text for its desktop's clipboard, read with interrupts on, kept for the desktop
   in place of what waited before, with the window it came from; only while the window has the focus, asked before
   the copy (nothing is copied for a refusal) and again after it. Text: empty, or a NUL first, it is refused */
static int set_clip(struct win *w, const struct win_clip *uc) {
    struct win_clip c;
    struct paste *text, *old = NULL;
    u_long flags;
    int r;
    if(!uc || !user_buf(uc, sizeof(c), false)) return -EFAULT;
    memcpy(&c, uc, sizeof(c));
    if(!c.len || c.len > WIN_CLIP_MAX) return -EINVAL;
    if(!c.text || !user_buf(c.text, c.len, false)) return -EFAULT;
    flags = irq_save();
    r = clip_may(w);
    irq_restore(flags);
    if(r) return r;
    text = (struct paste *)kalloc(sizeof(*text) + c.len);  /* 11/05/26: held by a count, as WIN_PASTE reads it */
    if(!text) return -ENOMEM;
    memcpy(text->text, c.text, c.len);
    text->refs = 1;
    text->len = (u32)strnlen(text->text, c.len);        /* text only: to a NUL that comes first */
    if(!text->len) { kfree(text); return -EINVAL; }
    flags = irq_save();
    r = clip_may(w);                                    /* the focus may have gone meanwhile */
    if(!r) {
        old = paste_put(w->desk->clip);
        w->desk->clip = text;
        w->desk->clip_from = handle_of(w);
        text = NULL;
    }
    irq_restore(flags);
    kfree(text);
    kfree(old);
    if(!r) poll_wake();                                 /* the desktop takes it */
    return r;
}

/* 10/26/26: WIN_ALERT: counted in the window's state, which its desktop reads at its next update; once in
   WIN_ALERT_MS for its program, whichever of its windows asks, opened again or not (its record in alerters[]; a
   program without one takes the oldest) */
static int alert(struct win *w) {
    u_long flags = irq_save(), now = getticks();
    int i, room = 0, r = 0;
    if(w->hung) r = -EPIPE;
    else {
        for(i = 0; i < WIN_MAX; i++) {
            if(alerters[i].pid == w->owner) { room = i; break; }
            if(now - alerters[i].at > now - alerters[room].at) room = i;
        }
        if(i < WIN_MAX && now - alerters[i].at < (u_long)WIN_ALERT_MS * HZ / 1000) r = -EAGAIN;
    }
    if(!r) {
        alerters[room].pid = w->owner;
        alerters[room].at = now;
        w->alerts++;
        w->serial++;
    }
    irq_restore(flags);
    if(!r) poll_wake();
    return r;
}

/**** 11/05/26: menus, the clipboard, the choosers (surfos/win.h): what a request may be ****/

/* a label's or a shortcut's array made a string: its NUL within the array, a string that fills it cut between
   characters as a title is; its length */
static u32 menu_text(char *s, u32 size) {
    u32 n = (u32)strnlen(s, size);
    if(n == size) n = title_cut(s, size, size - 1);
    s[n] = 0;
    return n;
}

/* 11/05/26 (the review): a label or a shortcut as the desktop draws it: UTF-8 by the console's rules (driver/cp437.c's
   utf8_feed(): whole sequences only, each one cell) with no control character (C0, DEL or C1): its cells, their code
   page 437 indexes in cell[] when it is not NULL (room for a cell a byte), or -1 for a string that is none (a byte the
   console would draw as a cell of its own, or a control) */
static int menu_cells(const char *s, u8 *cell) {
    struct utf8_dec d;
    u8 out[UTF8_OUT_MAX], had, c;
    int cells = 0, k;
    memset(&d, 0, sizeof(d));
    for(; *s; s++) {
        c = (u8)*s;
        if(c < 0x20 || c == 0x7F || (d.have == 1 && d.b[0] == 0xC2 && c < 0xA0)) return -1;   /* U+0080..U+009F */
        had = d.have;
        k = utf8_feed(&d, c, out);
        if(c < 0x80) {
            if(had) return -1;                          /* it cut a sequence short */
        } else if(!(!had && d.have == 1 && !k) && !(had && d.have == had + 1 && !k) && !(had && !d.have && k == 1))
            return -1;                                  /* neither a lead, a continuation nor a sequence's last */
        if(k && cell) cell[cells] = out[0];
        cells += k;
    }
    return d.have ? -1 : cells;
}

/* the cells drawn (n of them) read as one of names (lower case), in any case, with blank cells around them (0xFF:
   the no-break space) */
static bool reads_as(const u8 *cell, u32 n, const char *const *names, u32 count) {
    u32 i, k;
    while(n && (cell[0] == ' ' || cell[0] == 0xFF)) cell++, n--;
    while(n && (cell[n - 1] == ' ' || cell[n - 1] == 0xFF)) n--;
    for(i = 0; i < count; i++) {
        for(k = 0; k < n && names[i][k] && (cell[k] | 0x20) == names[i][k]; k++) ;
        if(k == n && !names[i][k]) return true;
    }
    return false;
}

/* a title that would pass for one of the desktop's own menus, by the cells it draws: ≋ in it (code page 437's 0xF7,
   which U+224B and U+2248 both draw), or SurfOS, Apps, Window or Help (help: a title merged into the desktop's Help,
   which may be called that) */
static bool desk_title(const u8 *cell, u32 n, bool help) {
    static const char *const own[] = { "surfos", "apps", "window", "help" };
    u32 i;
    for(i = 0; i < n; i++) if(cell[i] == 0xF7) return true;
    return reads_as(cell, n, own, help ? 3 : 4);
}

/* 11/05/26 (the review of stage 2): an item of a Help menu that would pass for one of the desktop's own Help items */
static bool desk_help_item(const u8 *cell, u32 n) {
    static const char *const own[] = { "keyboard and mouse", "about surfos" };
    return reads_as(cell, n, own, 2);
}

/* n entries of a WIN_MENU, copied into the kernel, as menus (surfos/win.h): each label and shortcut made a string,
   then the rest checked: 0, or -EINVAL. 11/05/26 (the review): labels and shortcuts the desktop draws as they are;
   14 entries a menu and a submenu; submenus one level deep, of an item, right after it; one Help title, not in the
   bar, so not counted in its cells, and (the review of stage 2) none of its items reads as the desktop's own */
static int menu_check(struct win_menu_item *it, u32 n) {
    const u32 known = WIN_MENU_TITLE | WIN_MENU_SEPARATOR | WIN_MENU_DISABLED | WIN_MENU_CHECKED | WIN_MENU_RADIO |
                      WIN_MENU_SUBMENU | WIN_MENU_CHILD | WIN_MENU_HELP;
    u32 i, menus = 0, items = 0, children = 0, cells = 0, helps = 0, most = WIN_MENU_ITEMS;
    bool sub = false;                                   /* the entry before was a SUBMENU item or one of its children */
    bool help = false;                                  /* the menu is the Help one */
    u8 cell[WIN_MENU_LABEL];
    int c;
    for(i = 0; i < n; i++) {
        struct win_menu_item *e = &it[i];
        u32 f = e->flags;
        bool title = (f & WIN_MENU_TITLE) != 0, child = (f & WIN_MENU_CHILD) != 0, sep = (f & WIN_MENU_SEPARATOR) != 0;
        if((f & ~known) || (title && (f & (WIN_MENU_SEPARATOR | WIN_MENU_SUBMENU | WIN_MENU_CHILD))) ||
           ((f & WIN_MENU_HELP) && !title) || (child && (f & WIN_MENU_SUBMENU)) || (sep && (f & WIN_MENU_SUBMENU)))
            return -EINVAL;
        menu_text(e->label, sizeof(e->label));
        menu_text(e->keys, sizeof(e->keys));
        if((c = menu_cells(e->label, cell)) < 0 || menu_cells(e->keys, NULL) < 0) return -EINVAL;
        if(title) {
            if((menus && !items) || (sub && !children) || ++menus > WIN_MENUS || !c) return -EINVAL;
            if(desk_title(cell, (u32)c, (f & WIN_MENU_HELP) != 0)) return -EINVAL;
            if((f & WIN_MENU_HELP) && ++helps > 1) return -EINVAL;
            if(!(f & WIN_MENU_HELP)) cells += (u32)c + 3;   /* a Help title is not in the bar */
            most = f & WIN_MENU_HELP ? WIN_MENU_HELP_ITEMS : WIN_MENU_ITEMS;
            help = (f & WIN_MENU_HELP) != 0;
            items = 0;
            sub = false;
        } else if(help && !sep && desk_help_item(cell, (u32)c)) {
            return -EINVAL;                             /* (an item or a child of the Help menu) */
        } else if(child) {
            if(!sub || ++children > WIN_MENU_ITEMS || (!sep && !c)) return -EINVAL;   /* after its item, 14 */
        } else {
            if(!menus || (sub && !children) || ++items > most || (!sep && !c)) return -EINVAL;
            sub = (f & WIN_MENU_SUBMENU) != 0;
            children = 0;
        }
    }
    if(n && (!items || (sub && !children))) return -EINVAL;     /* the last menu has no items; a submenu none */
    return cells > WIN_MENU_BAR ? -EINVAL : 0;
}

/* WIN_MENU: the request copied in and checked (*out: the kernel's copy of its entries, NULL for none: count 0) */
static int menu_args(const struct win_menu *um, struct menus **out) {
    struct win_menu m;
    struct menus *k;
    int r;
    *out = NULL;
    if(!um || !user_buf(um, sizeof(m), false)) return -EFAULT;
    memcpy(&m, um, sizeof(m));
    if(m.count > WIN_MENU_ENTRIES) return -EINVAL;
    if(!m.count) return 0;
    if(!m.items || !user_buf(m.items, (u_long)m.count * sizeof(struct win_menu_item), false)) return -EFAULT;
    k = (struct menus *)kalloc(sizeof(*k) + m.count * sizeof(struct win_menu_item));
    if(!k) return -ENOMEM;
    memcpy(k->it, m.items, m.count * sizeof(struct win_menu_item));
    k->refs = 1;
    k->count = m.count;
    r = menu_check(k->it, m.count);
    if(r) {
        kfree(k);
        return r;
    }
    *out = k;
    return 0;
}

/* WIN_PASTE's and WIN_TAKE's buffer: the struct read (the buffer itself is checked when it is written) */
static int buffer_args(const struct win_buffer *ub, struct win_buffer *b) {
    if(!ub || !user_buf(ub, sizeof(*b), false)) return -EFAULT;
    memcpy(b, ub, sizeof(*b));
    return b->size && !b->buf ? -EFAULT : 0;
}

/* a path's or a name's array: its NUL within it, and no control character before it */
static bool plain_text(const char *s, u32 size) {
    u32 i;
    for(i = 0; i < size && s[i]; i++) if((u8)s[i] < 0x20 || s[i] == 0x7F) return false;
    return i < size;
}

/* WIN_DIALOG: the request copied in (into d) and checked; its title made a string as a window's title is */
static int dialog_args(const struct win_dialog *ud, struct win_dialog *d) {
    if(!ud || !user_buf(ud, sizeof(*d), false)) return -EFAULT;
    memcpy(d, ud, sizeof(*d));
    if(d->kind < WIN_DLG_OPEN || d->kind > WIN_DLG_FOLDER || d->flags) return -EINVAL;
    if(!plain_text(d->dir, sizeof(d->dir)) || (d->dir[0] && d->dir[0] != '/')) return -EINVAL;
    if(!plain_text(d->name, sizeof(d->name)) || strchr(d->name, '/')) return -EINVAL;
    if(win_types(d->types, sizeof(d->types), NULL) < 0) return -EINVAL;    /* surfos/win.h: the registry's too */
    d->title[title_cut(d->title, (u32)strnlen(d->title, sizeof(d->title)), WIN_TITLE_MAX - 1)] = 0;
    return 0;
}

/* 11/05/26: WIN_MENU: the window's menus replaced by the request's, which its desktop gets at its next update (the
   copy it has goes then) */
static int set_menu(struct win *w, const struct win_menu *um) {
    struct menus *m, *old = NULL;
    u_long flags;
    int r = menu_args(um, &m);
    if(r) return r;
    flags = irq_save();
    if(w->hung) r = -EPIPE;
    else {
        old = menus_put(w->menu);
        w->menu = m;
        w->menu_serial++;
        m = NULL;
    }
    irq_restore(flags);
    kfree(m);
    kfree(old);
    if(!r) poll_wake();                                 /* the desktop draws them */
    return r;
}

/* 11/05/26: WIN_PASTE: the desktop's clipboard, from the copy it gave (held meanwhile), into the program's buffer;
   only while the window has the focus. The window's own WIN_CLIP that its desktop has not taken yet is newer than
   that copy (the desktop makes it the clipboard while the window keeps the focus): that one is pasted */
static int paste(struct win *w, const struct win_buffer *ub) {
    struct win_buffer b;
    struct paste *p = NULL, *dead;
    u_long flags;
    u32 n;
    int r = buffer_args(ub, &b);
    if(r) return r;
    flags = irq_save();
    r = clip_may(w);                                    /* -EPIPE once let go, -EACCES without the focus */
    if(!r) {
        p = w->desk->clip && w->desk->clip_from == handle_of(w) ? w->desk->clip : w->desk->paste;
        if(p) p->refs++;
    }
    irq_restore(flags);
    if(r) return r;
    n = p ? p->len : 0;
    if(b.size && n > b.size) r = -ENOSPC;
    else if(b.size && !user_buf(b.buf, n + (b.size > n), true)) r = -EFAULT;
    else if(b.size) {
        if(n) memcpy(b.buf, p->text, n);
        if(b.size > n) b.buf[n] = 0;
    }
    flags = irq_save();
    dead = paste_put(p);
    irq_restore(flags);
    kfree(dead);
    return r ? r : (int)n;
}

/* 11/05/26: WIN_DIALOG: the request kept for the desktop, which shows its chooser while the window has the focus
   (only then may it ask: a chooser takes the keys), after something the user did in the window since its last request
   (so that a program that asks again after each answer cannot hold the keys), and while its queue has a place for the
   answer, which is kept for it from then on. 11/05/26 (the review of stage 2): not while a path chosen waits to be
   taken (-EBUSY), so that WIN_TAKE always gives the path of the answer the program read last */
static int ask_dialog(struct win *w, const struct win_dialog *ud) {
    static u32 numbers;                                 /* the requests' numbers (interrupts off) */
    struct win_dialog d, *copy;
    u_long flags;
    int r = dialog_args(ud, &d);
    if(r) return r;
    copy = (struct win_dialog *)kalloc(sizeof(*copy));
    if(!copy) return -ENOMEM;
    *copy = d;
    flags = irq_save();
    if(w->hung) r = -EPIPE;
    else if(!w->focused) r = -EACCES;
    else if(w->asking || w->answer) r = -EBUSY;          /* (the review of stage 2: a path chosen, not taken yet) */
    else if(!w->input) r = -EACCES;
    else if(w->ev_count >= WIN_EVENTS) r = -EAGAIN;    /* no place for the answer: the events first */
    else {
        w->ask = copy;
        w->asking = true;
        w->input = false;
        w->ask_id = d.id;
        if(!++numbers) numbers++;
        w->ask_no = numbers;
        copy = NULL;
    }
    irq_restore(flags);
    kfree(copy);
    if(!r) poll_wake();                                 /* the desktop shows it */
    return r;
}

/* 11/05/26: WIN_TAKE: the path the chooser chose, into the program's buffer (checked first, as far as any path
   reaches: a refusal keeps the path), and taken; after the hang-up too, as WIN_INFO works then */
static int take(struct win *w, const struct win_buffer *ub) {
    struct win_buffer b;
    char *a = NULL;
    u_long flags;
    u32 room, n = 0;
    int r = buffer_args(ub, &b);
    if(r) return r;
    room = b.size < WIN_PATH_MAX ? b.size : WIN_PATH_MAX;
    if(room && !user_buf(b.buf, room, true)) return -EFAULT;
    flags = irq_save();
    if(!w->answer) r = -ENOENT;
    else if((n = (u32)strlen(w->answer)) + 1 > room) r = -ENOSPC;   /* it stays, for another try */
    else {
        a = w->answer;
        w->answer = NULL;
    }
    irq_restore(flags);
    if(r) return r;
    memcpy(b.buf, a, n + 1);
    kfree(a);
    return (int)n;
}

int sys_winctl(int fd, int op, void *arg) {
    struct file *f = fd_get(fd);
    struct win *w;
    if(!f) return -EBADF;
    if(f->v->ops != &win_ops) return -EINVAL;
    w = (struct win *)f->v->priv;
    switch(op) {
    case WIN_PRESENT: return present(w, (const struct win_present *)arg);
    case WIN_SETSIZE: return set_size(w, (u32)(u_long)arg);
    case WIN_TITLE:   return set_title(w, (const char *)arg);
    case WIN_INFO:    return info(w, (struct win_info *)arg);
    case WIN_CLIP:    return set_clip(w, (const struct win_clip *)arg);      /* 10/26/26 */
    case WIN_ALERT:   return alert(w);
    case WIN_MENU:    return set_menu(w, (const struct win_menu *)arg);      /* 11/05/26 */
    case WIN_PASTE:   return paste(w, (const struct win_buffer *)arg);
    case WIN_DIALOG:  return ask_dialog(w, (const struct win_dialog *)arg);
    case WIN_TAKE:    return take(w, (const struct win_buffer *)arg);
    case WIN_THEME:   return theme(w, (struct win_theme *)arg);
    }
    return -EINVAL;
}

/**** the desktop's side ****/

/* the desktop's end (its descriptor's last close, or its task's reap): every window of it let go */
static void desk_release(struct vnode *v) {
    struct desk *d = (struct desk *)v->priv;
    struct dead dead[WIN_MAX];
    struct paste *paste, *clip;
    u_long flags;
    int i, n = 0;
    if(!d) return;
    memset(dead, 0, sizeof(dead));
    flags = irq_save();
    for(i = 0; i < WIN_MAX; i++) {
        if(!wins[i].used || wins[i].desk != d) continue;
        hang_up(&wins[i], &dead[n++]);
    }
    clip = paste_put(d->clip);                          /* 10/26/26: text nobody took (11/05/26: unless a paste */
    paste = paste_put(d->paste);                        /* holds it), and the clipboard's copy */
    memset(d, 0, sizeof(*d));
    irq_restore(flags);
    for(i = 0; i < n; i++) dead_free(&dead[i]);
    kfree(clip);
    kfree(paste);
    poll_wake();
}

static const struct vnode_ops desk_ops = {
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, desk_release, NULL, NULL
};

int win_register(const struct win_desk *info) {
    struct desk *d = NULL;
    struct vnode *v;
    struct file *f;
    u_long flags;
    int i, r = 0;
    if(!curTask || !curTask->tty) return -ENODEV;
    if(info->max_w < 1 || info->max_h < 1 || info->max_w > info->width || info->max_h > info->height) return -EINVAL;
    v = vnode_alloc(&winfs, &desk_ops, VN_FILE);
    if(!v) return -ENOMEM;
    flags = irq_save();
    for(i = 0; i < WIN_DESKS; i++) if(desks[i].used && desks[i].tty == curTask->tty && alive(desks[i].owner)) r = -EBUSY;
    for(i = 0; !r && i < WIN_DESKS && !d; i++) if(!desks[i].used) d = &desks[i];
    if(!r && d) {
        memset(d, 0, sizeof(*d));
        d->used = true;
        d->owner = curTask->pid;
        d->tty = curTask->tty;
        d->info = *info;
        v->priv = d;
    }
    irq_restore(flags);
    if(r || !d) {
        vput(v);
        return r ? r : -EAGAIN;
    }
    r = vfs_open_vnode(v, O_RDONLY, &f);
    if(r) {
        vput(v);                                        /* the desktop's end at once */
        return r;
    }
    r = fd_install(f);
    if(r < 0) vfs_close(f);
    return r;
}

void win_unregister(int fd) {
    struct file *f = fd_get(fd);
    if(f && f->v->ops == &desk_ops) fd_close(fd);
}

/* pty number n's tty is ttys[NUM_CONSOLES + n] (kernel/pty.c) */
int win_adopt(int n) {
    struct tty *pty;
    struct desk *d;
    u_long flags;
    int k, r = -ENOSPC;
    if(n < 0 || n >= PTY_MAX) return -EINVAL;
    pty = &ttys[NUM_CONSOLES + n];
    flags = irq_save();
    d = own_desk();
    if(!d) r = -ENODEV;
    for(k = 0; d && k < DESK_PTYS; k++) if(d->ptys[k] == pty) r = 0;
    for(k = 0; d && r && k < DESK_PTYS; k++) {
        if(d->ptys[k]) continue;
        d->ptys[k] = pty;
        r = 0;
    }
    irq_restore(flags);
    return r;
}

/* 10/25/26 (kernel/pty.c): pty t hung up or is free again (interrupts off): no desktop has it adopted any more, so
   that a later session on the same pty, a telnet session's say, opens no window there */
void win_pty_gone(const struct tty *t) {
    int i, k;
    for(i = 0; i < WIN_DESKS; i++)
        for(k = 0; k < DESK_PTYS; k++)
            if(desks[i].ptys[k] == t) {
                desks[i].ptys[k] = NULL;
                desks[i].shells[k] = 0;
            }
}

/* 10/25/26 (stage C6): pty number n, adopted, runs a session whose shell is pid: only the jobs that shell makes may
   take the focus as the pty's foreground (in_foreground()); -ENODEV for a task without a desktop, -EINVAL for a pty
   it has not adopted */
int win_adopt_shell(int n, int pid) {
    struct tty *pty;
    struct desk *d;
    u_long flags;
    int k, r = -EINVAL;
    if(n < 0 || n >= PTY_MAX || pid <= 0) return -EINVAL;
    pty = &ttys[NUM_CONSOLES + n];
    flags = irq_save();
    d = own_desk();
    if(!d) r = -ENODEV;
    for(k = 0; d && k < DESK_PTYS; k++)
        if(d->ptys[k] == pty) {
            d->shells[k] = (u_long)pid;
            r = 0;
        }
    irq_restore(flags);
    return r;
}

void win_drop(int n) {
    struct tty *pty;
    struct desk *d;
    u_long flags;
    int k;
    if(n < 0 || n >= PTY_MAX) return;
    pty = &ttys[NUM_CONSOLES + n];
    flags = irq_save();
    d = own_desk();
    for(k = 0; d && k < DESK_PTYS; k++)
        if(d->ptys[k] == pty) {
            d->ptys[k] = NULL;
            d->shells[k] = 0;
        }
    irq_restore(flags);
}

/* the oldest window of the calling task's desktop that is ready: it presented, or SHOW_TICKS passed */
int win_accept(void) {
    struct desk *d;
    struct win *best = NULL;
    u_long flags = irq_save(), now = getticks();
    int i, h = -EAGAIN;
    d = own_desk();
    for(i = 0; d && i < WIN_MAX; i++) {
        struct win *w = &wins[i];
        if(!w->used || w->desk != d || w->held || !w->prog) continue;
        if(!w->presents && now - w->opened < SHOW_TICKS) continue;
        if(!best || w->seq < best->seq) best = w;
    }
    if(best) {
        best->held = true;
        h = handle_of(best);
    }
    irq_restore(flags);
    return d ? h : -ENODEV;
}

/* 10/25/26 (stage C6): the window's program is in the foreground job of a pty its desktop adopted (its Terminal's):
   typed at that Terminal's prompt without &, or started by such a program. The job must be one the session's shell
   made (win_adopt_shell()), not merely the terminal's foreground job: a surfsh started inside that shell loses the
   exception. 10/26/26: a task in no job that is no shell of the pty's may not set its foreground job any more
   (sys_job, CODE-AUDIT T14, closed); the rule stays as it was. Interrupts off */
static bool in_foreground(const struct win *w) {
    surf_task *t = w->desk ? task_find(w->owner) : NULL;
    int k;
    if(!t || !t->tty || !t->job || t->job != t->tty->fg_job) return false;
    for(k = 0; k < DESK_PTYS; k++)
        if(w->desk->ptys[k] == t->tty) return w->desk->shells[k] && t->job_shell == w->desk->shells[k];
    return false;
}

/* its state and pixels now, the pixels lent until the next update or close; -EPIPE once its program let it go or
   is gone (dead, not reaped yet: the desktop takes the window away at once; a window is the program's that opened it,
   so also when a child of it holds the descriptor) */
int win_update(int h, struct win_state *st, struct win_view *view) {
    struct win *w;
    struct winbuf *dead = NULL;
    struct menus *gone = NULL;
    u_long flags = irq_save();
    u_int k;
    int r = 0;
    w = held_win(h);
    if(!w) r = -EINVAL;
    else if(!w->prog || !alive(w->owner)) r = -EPIPE;
    else {
        memcpy(st->title, w->title, sizeof(st->title));
        st->w = w->buf->w;
        st->h = w->buf->h;
        st->min_w = w->min_w;
        st->min_h = w->min_h;
        st->max_w = w->max_w;
        st->max_h = w->max_h;
        st->flags = w->flags;
        st->background = w->background;
        st->serial = w->serial;
        st->pid = (int)w->owner;
        st->foreground = in_foreground(w);
        st->alerts = w->alerts;                         /* 10/26/26 */
        st->menu_serial = w->menu_serial;               /* 11/05/26 */
        st->dialog = w->ask != NULL;
        st->close_waiting = 0;                          /* (the review of stage 2: the close button's rule) */
        for(k = 0; k < w->ev_count; k++)
            if(w->ev[(w->ev_first + k) % WIN_EVENTS].type == WIN_EV_CLOSE) st->close_waiting = 1;
        if(w->view != w->buf) {                         /* the desktop draws the new pixels from now on */
            dead = buf_put(w->view);
            w->view = w->buf;
            w->view->refs++;
        }
        if(w->menu_view != w->menu) {                   /* 11/05/26: and has the menus in force */
            gone = menus_put(w->menu_view);
            w->menu_view = w->menu;
            if(w->menu_view) w->menu_view->refs++;
        }
        view->pixels = w->view->px;
        view->w = w->view->w;
        view->h = w->view->h;
        view->menu = w->menu_view ? w->menu_view->it : NULL;
        view->menus = w->menu_view ? (int)w->menu_view->count : 0;
    }
    irq_restore(flags);
    buf_free(dead);
    kfree(gone);
    return r;
}

int win_event(int h, const struct win_event *e) {
    struct win *w;
    u_long flags = irq_save();
    int r = -EINVAL;
    w = held_win(h);
    if(w) {
        u32 before = w->buttons;                        /* the buttons of the pointer event queued last */
        if(e->type == WIN_EV_FOCUS) w->focused = e->value != 0;
        r = queue_event(w, e);
        if(r >= 0) wake_up_all(&w->readers);
        /* 11/05/26: the user did something there, so it may ask for a chooser: a key, an item chosen, or (the review
           of stage 2) a press in it: a pointer event over its content with a button the one before did not have,
           never one that says the pointer left (x, y -1) or a button held from a press elsewhere */
        if(r >= 0 && (e->type == WIN_EV_KEY || e->type == WIN_EV_MENU ||
                      (e->type == WIN_EV_POINTER && e->x >= 0 && e->y >= 0 && (e->value & ~before))))
            w->input = true;
    }
    irq_restore(flags);
    if(r >= 0) poll_wake();
    return r < 0 ? r : 0;
}

void win_close(int h) {
    struct win *w;
    struct dead dead;
    u_long flags;
    memset(&dead, 0, sizeof(dead));
    flags = irq_save();
    w = held_win(h);
    if(w) hang_up(w, &dead);
    irq_restore(flags);
    dead_free(&dead);
    if(w) poll_wake();
}

/* surfos/win.h, 10/26/26: the text waiting for the calling task's desktop's clipboard, taken (and freed): copied into
   out when it came from window h, the one with the desktop's focus, dropped when it came from another */
int win_clip_take(int h, char *out, u32 size) {
    struct desk *d;
    struct paste *text = NULL, *old = NULL;
    u32 len = 0;
    int from = -1;
    u_long flags = irq_save();
    d = own_desk();
    if(d) {
        text = d->clip;                                 /* 11/05/26: the slot's count is the desktop's now */
        from = d->clip_from;
        d->clip = NULL;
        if(text && h >= 0 && from == h) {               /* the clipboard's from now: WIN_PASTE's too, at once */
            old = paste_put(d->paste);
            d->paste = text;
            text->refs++;
        }
    }
    irq_restore(flags);
    kfree(old);
    if(!d) return -ENODEV;
    if(!text) return 0;
    len = text->len;
    if(h < 0 || from != h) len = 0;                     /* set as its window lost the focus: not the clipboard's */
    if(len > size) len = size;
    memcpy(out, text->text, len);
    flags = irq_save();
    text = paste_put(text);                             /* 11/05/26: unless a paste still copies from it */
    irq_restore(flags);
    kfree(text);
    return (int)len;
}

/**** 11/05/26 (track desk, stage 2): the desktop's side of the menus, the clipboard, the choosers and the theme ****/

/* the calling task's desktop's clipboard, len bytes of text (to a NUL if one comes first, at most WIN_CLIP_MAX; 0:
   empty), copied for WIN_PASTE in place of the copy before (freed once no paste holds it): 0, -ENODEV for a task
   without a desktop, -ENOMEM */
int win_paste_give(const char *text, u32 len) {
    struct paste *p = NULL, *old;
    struct desk *d;
    u_long flags;
    if(len > WIN_CLIP_MAX) len = WIN_CLIP_MAX;
    len = (u32)strnlen(text, len);
    if(len) {
        p = (struct paste *)kalloc(sizeof(*p) + len);
        if(!p) return -ENOMEM;
        p->refs = 1;
        p->len = len;
        memcpy(p->text, text, len);
    }
    flags = irq_save();
    d = own_desk();
    if(d) {
        old = paste_put(d->paste);
        d->paste = p;
        p = old;
    }
    irq_restore(flags);
    kfree(p);                                           /* the copy before, or this one when there is no desktop */
    return d ? 0 : -ENODEV;
}

/* the chooser window h asked for (WIN_DIALOG), taken by its desktop into out, and its number into no, which its answer
   names: 0; -ENOENT when none waits; -EINVAL for a handle that is not one of the calling task's desktop's. It waits
   for its answer from then on */
int win_dialog_take(int h, struct win_dialog *out, u32 *no) {
    struct win_dialog *a = NULL;
    struct win *w;
    u_long flags = irq_save();
    int r = 0;
    w = held_win(h);
    if(!w) r = -EINVAL;
    else if(!w->ask) r = -ENOENT;
    else {
        a = w->ask;
        w->ask = NULL;
        *no = w->ask_no;
    }
    irq_restore(flags);
    if(a) {
        *out = *a;
        kfree(a);
    }
    return r;
}

/* the answer to window h's request number no: path (absolute, shorter than WIN_PATH_MAX) when one was chosen, NULL
   when it was cancelled; with it, in the same section, WIN_EV_DIALOG for its program, in the place kept for it. 0;
   -EINVAL for a handle that is not one of the calling task's desktop's, a request that is not the one waiting (an
   answer given already, or to one before), or a path that is none; -ENOMEM (the request is then answered as
   cancelled) */
int win_dialog_answer(int h, u32 no, const char *path) {
    struct win_dialog *ask = NULL;
    struct win *w;
    char *copy = NULL, *old = NULL;
    u_long flags;
    u32 n = 0;
    int r = 0;
    if(path) {
        n = (u32)strnlen(path, WIN_PATH_MAX);
        if(n == WIN_PATH_MAX || path[0] != '/') return -EINVAL;
        copy = (char *)kalloc(n + 1);
        if(copy) memcpy(copy, path, n + 1);
        else r = -ENOMEM;
    }
    flags = irq_save();
    w = held_win(h);
    if(!w || !w->asking || !w->prog || w->ask_no != no) {
        if(!r) r = -EINVAL;
    } else {
        ask = w->ask;                                   /* never taken: answered as it is */
        w->ask = NULL;
        old = w->answer;
        w->answer = copy;
        copy = NULL;
        answer_queue(w, w->answer ? 1 : 0);
    }
    irq_restore(flags);
    kfree(ask);
    kfree(old);
    kfree(copy);
    if(w) poll_wake();
    return r;
}

/* the calling task's desktop's theme now: theme (WIN_THEME_*) and colors (WIN_COLORS of them), kept for its windows
   to come and given to those it has; each that asked WIN_THEME gets WIN_EV_THEME. 0, -ENODEV for a task without a
   desktop, -EINVAL for a theme that is none */
int win_theme_set(u32 theme, const u32 *colors) {
    struct win_event e;
    struct desk *d;
    u_long flags;
    int i;
    if(theme > WIN_THEME_DUSK) return -EINVAL;
    memset(&e, 0, sizeof(e));
    e.type = WIN_EV_THEME;
    e.value = theme;
    flags = irq_save();
    d = own_desk();
    if(d) {
        d->themed = true;
        d->theme = theme;
        memcpy(d->colors, colors, sizeof(d->colors));
        for(i = 0; i < WIN_MAX; i++) {
            struct win *w = &wins[i];
            if(!w->used || w->desk != d) continue;
            w->theme = theme;
            memcpy(w->colors, colors, sizeof(w->colors));
            if(w->themed && w->prog && queue_event(w, &e) >= 0) wake_up_all(&w->readers);
        }
    }
    irq_restore(flags);
    if(d) poll_wake();
    return d ? 0 : -ENODEV;
}
