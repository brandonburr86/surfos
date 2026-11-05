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
copy in the kernel's memory, never on the program's, which may change meanwhile. Until stage 2
implements them, a request that passes its checks is answered -ENOSYS.
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
#include <blibc_common.h>

#define SHOW_TICKS (250 * HZ / 1000)    /* a window that never presented appears after 250 ms */
#define DESK_PTYS 4                     /* ptys a desktop adopts (its Terminal's) */
#define USER_PAGE 4096

struct winbuf { u32 *px; int w, h; u_int refs; };

struct desk {
    bool used;
    u_long owner;                       /* the desktop's task */
    struct tty *tty;                    /* its console */
    struct tty *ptys[DESK_PTYS];        /* its Terminal's */
    u_long shells[DESK_PTYS];           /* 10/25/26: the session shell on each (the Terminal's surfsh), 0: unknown */
    struct win_desk info;
    char *clip;                         /* 10/26/26: text a window put on the clipboard, for the desktop to take */
    u32 clip_len;
    int clip_from;                      /* and the handle of that window */
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
    if(w->ev_count == WIN_EVENTS) {
        w->lost++;
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

/* the window let go of by its desktop (interrupts off): hung up, and its pixels go (10/25/26: a program that holds on
   to it keeps no memory with it; its size stays); free when its program let go too. The buffers to free are put in
   dead[0] and dead[1] */
static void hang_up(struct win *w, struct winbuf **dead) {
    dead[0] = buf_put(w->view);
    dead[1] = buf_put(w->buf);
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
    struct winbuf *dead[2] = { NULL, NULL };
    u_long flags;
    if(!w) return;                                      /* a vnode that never had its window */
    flags = irq_save();
    w->prog = false;
    if(!w->held) {
        dead[0] = buf_put(w->buf);
        dead[1] = buf_put(w->view);
        w->buf = w->view = NULL;
        w->desk = NULL;
        w->used = false;
    }
    irq_restore(flags);
    buf_free(dead[0]);
    buf_free(dead[1]);
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
    k.theme = WIN_THEME_OCEAN;
    memcpy(k.colors, ocean_colors, sizeof(k.colors));
    flags = irq_save();
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
    char *text, *old = NULL;
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
    text = (char *)kalloc(c.len);
    if(!text) return -ENOMEM;
    memcpy(text, c.text, c.len);
    c.len = (unsigned)strnlen(text, c.len);             /* text only: to a NUL that comes first */
    if(!c.len) { kfree(text); return -EINVAL; }
    flags = irq_save();
    r = clip_may(w);                                    /* the focus may have gone meanwhile */
    if(!r) {
        old = w->desk->clip;
        w->desk->clip = text;
        w->desk->clip_len = c.len;
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

/* n entries of a WIN_MENU, copied into the kernel, as menus (surfos/win.h): each label and shortcut made a string,
   then the rest checked: 0, or -EINVAL. A title's characters are counted as its bytes that are no UTF-8 continuation */
static int menu_check(struct win_menu_item *it, u32 n) {
    const u32 known = WIN_MENU_TITLE | WIN_MENU_SEPARATOR | WIN_MENU_DISABLED | WIN_MENU_CHECKED | WIN_MENU_RADIO;
    u32 i, k, menus = 0, items = 0, cells = 0;
    for(i = 0; i < n; i++) {
        struct win_menu_item *e = &it[i];
        if((e->flags & ~known) || ((e->flags & WIN_MENU_TITLE) && (e->flags & WIN_MENU_SEPARATOR))) return -EINVAL;
        menu_text(e->label, sizeof(e->label));
        menu_text(e->keys, sizeof(e->keys));
        if(e->flags & WIN_MENU_TITLE) {
            if((menus && !items) || ++menus > WIN_MENUS || !e->label[0]) return -EINVAL;   /* a menu with no items */
            items = 0;
            for(k = 0; e->label[k]; k++) if(((u8)e->label[k] & 0xC0) != 0x80) cells++;
            cells += 3;
        } else if(!menus || ++items > WIN_MENU_ITEMS || (!(e->flags & WIN_MENU_SEPARATOR) && !e->label[0]))
            return -EINVAL;                             /* before the first title; too many; an item with no label */
    }
    if(n && !items) return -EINVAL;                     /* the last menu has no items */
    return cells > WIN_MENU_BAR ? -EINVAL : 0;
}

/* WIN_MENU: the request copied in and checked (*out: the kernel's copy of its entries, NULL for none: count 0) */
static int menu_args(const struct win_menu *um, struct win_menu_item **out, u32 *count) {
    struct win_menu m;
    struct win_menu_item *it;
    int r;
    *out = NULL;
    *count = 0;
    if(!um || !user_buf(um, sizeof(m), false)) return -EFAULT;
    memcpy(&m, um, sizeof(m));
    if(m.count > WIN_MENU_ENTRIES) return -EINVAL;
    if(!m.count) return 0;
    if(!m.items || !user_buf(m.items, (u_long)m.count * sizeof(*it), false)) return -EFAULT;
    it = (struct win_menu_item *)kalloc(m.count * sizeof(*it));
    if(!it) return -ENOMEM;
    memcpy(it, m.items, m.count * sizeof(*it));
    r = menu_check(it, m.count);
    if(r) {
        kfree(it);
        return r;
    }
    *out = it;
    *count = m.count;
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

/* a chooser's file types: extensions of 1 to WIN_TYPE_LEN letters and digits, WIN_TYPES at most, separated by spaces
   (any number of them, around them too), the NUL within the array */
static bool types_ok(const char *s, u32 size) {
    u32 i, len = 0, n = 0;
    if(!memchr(s, 0, size)) return false;
    for(i = 0;; i++) {
        char c = s[i];
        if((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
            if(!len && ++n > WIN_TYPES) return false;
            if(++len > WIN_TYPE_LEN) return false;
        } else if(c == ' ') len = 0;
        else return !c;
    }
}

/* WIN_DIALOG: the request copied in (into d) and checked; its title made a string as a window's title is */
static int dialog_args(const struct win_dialog *ud, struct win_dialog *d) {
    if(!ud || !user_buf(ud, sizeof(*d), false)) return -EFAULT;
    memcpy(d, ud, sizeof(*d));
    if(d->kind < WIN_DLG_OPEN || d->kind > WIN_DLG_FOLDER) return -EINVAL;
    if(!plain_text(d->dir, sizeof(d->dir)) || (d->dir[0] && d->dir[0] != '/')) return -EINVAL;
    if(!plain_text(d->name, sizeof(d->name)) || strchr(d->name, '/')) return -EINVAL;
    if(!types_ok(d->types, sizeof(d->types))) return -EINVAL;
    d->title[title_cut(d->title, (u32)strnlen(d->title, sizeof(d->title)), WIN_TITLE_MAX - 1)] = 0;
    return 0;
}

/* 11/05/26, stage 1: the ABI's checks; what passes them is answered once stage 2 implements the ops */
static int set_menu(struct win *w, const struct win_menu *um) {
    struct win_menu_item *it;
    u32 n;
    int r = menu_args(um, &it, &n);
    (void)w;
    kfree(it);
    return r ? r : -ENOSYS;
}

static int paste(struct win *w, const struct win_buffer *ub) {
    struct win_buffer b;
    int r = buffer_args(ub, &b);
    (void)w;
    return r ? r : -ENOSYS;
}

static int ask_dialog(struct win *w, const struct win_dialog *ud) {
    struct win_dialog d;
    int r = dialog_args(ud, &d);
    (void)w;
    return r ? r : -ENOSYS;
}

static int take(struct win *w, const struct win_buffer *ub) {
    struct win_buffer b;
    int r = buffer_args(ub, &b);
    (void)w;
    return r ? r : -ENOSYS;
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
    struct winbuf *dead[2 * WIN_MAX];
    char *clip;
    u_long flags;
    int i, n = 0;
    if(!d) return;
    flags = irq_save();
    for(i = 0; i < WIN_MAX; i++) {
        if(!wins[i].used || wins[i].desk != d) continue;
        hang_up(&wins[i], dead + n);
        n += 2;
    }
    clip = d->clip;                                     /* 10/26/26: text nobody took */
    memset(d, 0, sizeof(*d));
    irq_restore(flags);
    for(i = 0; i < n; i++) buf_free(dead[i]);
    kfree(clip);
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
    u_long flags = irq_save();
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
        if(w->view != w->buf) {                         /* the desktop draws the new pixels from now on */
            dead = buf_put(w->view);
            w->view = w->buf;
            w->view->refs++;
        }
        view->pixels = w->view->px;
        view->w = w->view->w;
        view->h = w->view->h;
    }
    irq_restore(flags);
    buf_free(dead);
    return r;
}

int win_event(int h, const struct win_event *e) {
    struct win *w;
    u_long flags = irq_save();
    int r = -EINVAL;
    w = held_win(h);
    if(w) {
        if(e->type == WIN_EV_FOCUS) w->focused = e->value != 0;
        r = queue_event(w, e);
        if(r >= 0) wake_up_all(&w->readers);
    }
    irq_restore(flags);
    if(r >= 0) poll_wake();
    return r < 0 ? r : 0;
}

void win_close(int h) {
    struct win *w;
    struct winbuf *dead[2] = { NULL, NULL };
    u_long flags = irq_save();
    w = held_win(h);
    if(w) hang_up(w, dead);
    irq_restore(flags);
    buf_free(dead[0]);
    buf_free(dead[1]);
    if(w) poll_wake();
}

/* surfos/win.h, 10/26/26: the text waiting for the calling task's desktop's clipboard, taken (and freed): copied into
   out when it came from window h, the one with the desktop's focus, dropped when it came from another */
int win_clip_take(int h, char *out, u32 size) {
    struct desk *d;
    char *text = NULL;
    u32 len = 0;
    int from = -1;
    u_long flags = irq_save();
    d = own_desk();
    if(d) {
        text = d->clip;
        len = d->clip_len;
        from = d->clip_from;
        d->clip = NULL;
        d->clip_len = 0;
    }
    irq_restore(flags);
    if(!d) return -ENODEV;
    if(!text) return 0;
    if(h < 0 || from != h) len = 0;                     /* set as its window lost the focus: not the clipboard's */
    if(len > size) len = size;
    memcpy(out, text, len);
    kfree(text);
    return (int)len;
}
