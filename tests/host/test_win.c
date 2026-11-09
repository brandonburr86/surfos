/* Host tests for the windows of programs (kernel/win.c; milestone M12, stage C3 of
   docs/superpowers/plans/2026-10-25-app-windows.md). kernel/win.c is built here as it is, with stand-ins for the
   rest of the kernel, as tests/host/guisys_host.c builds shell/guisys.c: tasks as plain structures with their
   descriptor tables and terminals, the VFS's vnodes, files and descriptors as small counted records (a task's reap
   closes its descriptors, as fd_close_all() does), an allocator that counts what is alive and can fail at the nth
   call, the program's memory as the test's own with a range it calls unreadable, and a clock the test moves.
   Built and run by `make unittest` (unittest-win).
   10/25/26: the pure core (events queued, merged and lost; rectangles, strides and spans that overflow 32 bits;
   sizes within the limits and the desktop's; budgets of windows and bytes, per desktop and in all; titles cut
   between UTF-8 characters; which desktop a terminal's programs reach), and the life cycle: a window appearing,
   events read and polled, both orders of ending, the desktop's exit and its death, the owner's death, out of memory
   at every allocation, a buffer the desktop still draws outliving a new size, stale handles. Every scenario ends
   with nothing allocated.
   10/25/26 (the review of C1-C3): a press or a release never takes a move's place; a pty's hangup ends its
   adoption (win_pty_gone()); windows let go keep no pixels, so the budgets count only windows that are shown or to
   be (WIN_ALL_BYTES stays a guard: four desktops of WIN_DESK_BYTES reach it and no more); a new size whose first
   allocation fails. test_present needs a 32-bit build (HOSTFLAGS' -m32, as every host test of kernel code has
   where the host can link one): present() takes the program's addresses to be below 4 GB, as they are in SurfOS,
   and the test presents from its own memory.
   10/25/26 (stage C6): whether a window's program is in the foreground job of a pty its desktop adopted.
   10/26/26 (stage C12): WIN_CLIP only while the window has the focus, the desktop taking the latest text once, and
   the text it never took going with it; WIN_ALERT counted in the window's state.
   11/05/26 (the desktop applications, track desk, stage 1: the ABI): WIN_THEME, the theme and its colours; the
   argument checks of WIN_MENU, WIN_PASTE, WIN_DIALOG and WIN_TAKE,
   each on the kernel's copy, with nothing left allocated; and (the review of the ABI) menu text as the desktop draws
   it, titles that would pass for the desktop's, submenus and a Help, a chooser's flags and types. 11/05/26 (stage 2):
   the calls themselves and their desktop's side: menus set, lent to the desktop and taken away; the clipboard given
   and pasted, only with the focus, whole or not at all, the window's own copy not yet taken first; a chooser asked
   for after input, taken, answered by its number (chosen, cancelled, without memory), its answer's place kept, its
   path taken once, after the hang-up too; the theme, to the windows that asked; and everything a window holds gone
   with either side's end. */
#include <stdio.h>
#include <string.h>
#include <surfos/types.h>
#define _SURFOS_IRQ_H                   /* the kernel's irq_save() is privileged: nothing to switch off here */
static u_long irq_save(void) { return 0; }
static void irq_restore(u_long flags) { (void)flags; }
#include <surfos/task.h>
#include <surfos/tty.h>
#include <surfos/wait.h>
#include <surfos/timer.h>
#include <surfos/poll.h>
#include <surfos/win.h>
#include <surfos/sysimpl.h>
#include <mm/kalloc.h>
#include <fs/vfs.h>

void setpos(surf_console *con) { (void)con; }      /* surfos/console.h declares it inline; kernel/console.c's */
void *malloc(size_t n);                 /* the host's, for the windows' pixels (megabytes of them) */
void free(void *p);

static int checks, failed;
#define CHECK(x) do { checks++; if(!(x)) { failed++; printf("FAIL %s line %d: %s\n", __FILE__, __LINE__, #x); } } while(0)

/**** stand-ins: tasks and terminals ****/

#define KTASKS 24
surf_task *curTask;
static surf_task ktask[KTASKS];
static int nktasks;
struct tty ttys[NUM_TTYS];              /* the consoles F1-F4, then the ptys (kernel/tty.c's) */
#define CONSOLE(n) (&ttys[(n) - 1])
#define PTY(n) (&ttys[NUM_CONSOLES + (n)])

surf_task *task_find(u_long pid) {
    int i;
    for(i = 0; i < nktasks; i++) if(ktask[i].pid == pid && ktask[i].state != (task_state)-1) return &ktask[i];
    return NULL;
}

static surf_task *task_new(u_long pid, struct tty *t) {
    surf_task *k = &ktask[nktasks++];
    memset(k, 0, sizeof(*k));
    k->pid = pid;
    k->tty = t;
    k->state = TS_BLOCKED;
    return k;
}

/**** stand-ins: memory ****/

static int live;                        /* allocations alive */
static int fail_in;                     /* the fail_in-th allocation from now fails (0: none) */
static int allocs;

void *kalloc(size_t n) {
    allocs++;
    if(fail_in && --fail_in == 0) return NULL;
    live++;
    return malloc(n ? n : 1);
}
void *kcalloc(size_t count, size_t size) {
    char *p = (char *)kalloc(count * size);
    if(p) memset(p, 0, count * size);
    return p;
}
void kfree(void *p) {
    if(!p) return;
    live--;
    free(p);
}

/* the program's memory: all of the test's, but [bad_lo, bad_hi) and the first page */
static const char *bad_lo, *bad_hi;
bool user_buf(const void *p, u_long len, bool write) {
    u_long a = (u_long)p, end = a + len;
    (void)write;
    if(a < 4096 || end < a) return false;
    if(bad_lo && a < (u_long)bad_hi && end > (u_long)bad_lo) return false;
    return true;
}

/**** stand-ins: waits, wakes, the clock ****/

static int wakes, woken, yields;
static void (*on_yield)(void);          /* what happens while a reader waits (NULL: its wait is cut short) */
void poll_wake(void) { wakes++; }
void wake_up_all(wait_queue_t *q) { (void)q; woken++; }
void wait_prepare_intr(wait_queue_t *q) { (void)q; }
void yield(void) {
    yields++;
    if(on_yield) on_yield();
    else curTask->intr = TI_KILL;
}
static u_long now;
u_long getticks(void) { return now; }

/**** stand-ins: vnodes, files, descriptors ****/

struct vnode *vnode_alloc(struct superblock *sb, const struct vnode_ops *ops, int type) {
    struct vnode *v = (struct vnode *)kcalloc(1, sizeof(*v));
    if(!v) return NULL;
    v->sb = sb;
    v->ops = ops;
    v->type = type;
    v->refs = 1;
    return v;
}
void vput(struct vnode *v) {
    if(!v || --v->refs) return;
    if(v->ops && v->ops->release) v->ops->release(v);
    kfree(v);
}
int vfs_open_vnode(struct vnode *v, int flags, struct file **out) {
    struct file *f = (struct file *)kcalloc(1, sizeof(*f));
    if(!f) return -ENOMEM;
    f->v = v;
    f->flags = flags;
    f->refs = 1;
    *out = f;
    return 0;
}
int vfs_close(struct file *f) {
    if(!f || --f->refs) return 0;
    vput(f->v);
    kfree(f);
    return 0;
}
int fd_install(struct file *f) {
    int i;
    for(i = 0; i < NR_OPEN; i++) if(!curTask->files[i]) { curTask->files[i] = f; return i; }
    return -EMFILE;
}
struct file *fd_get(int fd) { return fd >= 0 && fd < NR_OPEN ? curTask->files[fd] : NULL; }
int fd_close(int fd) {
    struct file *f = fd_get(fd);
    if(!f) return -EBADF;
    curTask->files[fd] = NULL;
    return vfs_close(f);
}
/* a task collected: its descriptors closed (fd_close_all()), and it is gone */
static void reap(surf_task *t) {
    int i;
    for(i = 0; i < NR_OPEN; i++) if(t->files[i]) { vfs_close(t->files[i]); t->files[i] = NULL; }
    t->state = (task_state)-1;
}

#include "../../kernel/win.c"

/**** the scenarios' setting: a desktop on F1, programs on F1, F2 and a pty ****/

static surf_task *desktop, *prog, *other, *ptyprog;
static int reg;                         /* the desktop's registration */
static const struct win_desk desk960 = { 960, 540, 958, 457, 2 };

static void as(surf_task *t) { curTask = t; }

static void begin(void) {
    memset(desks, 0, sizeof(desks));
    memset(wins, 0, sizeof(wins));
    memset(alerters, 0, sizeof(alerters));              /* 10/26/26 */
    nktasks = 0;
    now = 1000;
    fail_in = 0;
    bad_lo = bad_hi = NULL;
    on_yield = NULL;
    desktop = task_new(10, CONSOLE(1));
    prog = task_new(20, CONSOLE(1));
    other = task_new(30, CONSOLE(2));
    ptyprog = task_new(40, PTY(1));
    as(desktop);
    reg = win_register(&desk960);
    CHECK(reg >= 0);
}

/* every task collected (the programs, then the desktop): nothing is left */
static void end(void) {
    int i;
    for(i = nktasks - 1; i >= 0; i--) if(ktask[i].state != (task_state)-1) reap(&ktask[i]);
    for(i = 0; i < WIN_MAX; i++) CHECK(!wins[i].used);
    for(i = 0; i < WIN_DESKS; i++) CHECK(!desks[i].used);
    CHECK(live == 0);
    live = 0;
}

static void good(struct win_open *o, int w, int h) {
    memset(o, 0, sizeof(*o));
    o->version = WIN_VERSION;
    o->w = (unsigned short)w;
    o->h = (unsigned short)h;
    o->background = 0x102E3B;
    strlcpy(o->title, "test", sizeof(o->title));
}

static int open_as(surf_task *t, int w, int h) {
    struct win_open o;
    good(&o, w, h);
    as(t);
    return sys_winopen(&o);
}

static int present_all(surf_task *t, int fd, const unsigned *px, int w, int h) {
    struct win_present p;
    memset(&p, 0, sizeof(p));
    p.pixels = px;
    p.stride = (unsigned)w;
    p.w = (unsigned short)w;
    p.h = (unsigned short)h;
    as(t);
    return sys_winctl(fd, WIN_PRESENT, &p);
}

static struct win_info info_of(surf_task *t, int fd) {
    struct win_info k;
    memset(&k, 0, sizeof(k));
    as(t);
    CHECK(sys_winctl(fd, WIN_INFO, &k) == 0);
    return k;
}

static struct win *win_of_fd(surf_task *t, int fd) {
    return (struct win *)t->files[fd]->v->priv;
}

static int readev(surf_task *t, int fd, struct win_event *ev, u32 len) {
    as(t);
    return t->files[fd]->v->ops->read(t->files[fd]->v, 0, ev, len);
}

static int pollw(surf_task *t, int fd) {
    return t->files[fd]->v->ops->poll(t->files[fd]->v, t->files[fd]);
}

static struct win_event event(int type, int x, int y, unsigned value) {
    struct win_event e;
    memset(&e, 0, sizeof(e));
    e.type = (unsigned short)type;
    e.x = x;
    e.y = y;
    e.value = value;
    return e;
}

static int send_ev(int h, int type, int x, int y, unsigned value) {
    struct win_event e = event(type, x, y, value);
    as(desktop);
    return win_event(h, &e);
}

/* the desktop takes the window of fd once it presented */
static int shown(surf_task *t, int fd) {
    static const unsigned px[4] = { 1, 2, 3, 4 };
    struct win_present p;
    memset(&p, 0, sizeof(p));
    p.pixels = px;
    p.stride = 2;
    p.w = 2;
    p.h = 2;
    as(t);
    CHECK(sys_winctl(fd, WIN_PRESENT, &p) == 0);
    as(desktop);
    return win_accept();
}

/**** the pure core ****/

static void test_queue(void) {
    struct win w;
    struct win_event e;
    int i;
    memset(&w, 0, sizeof(w));
    e = event(WIN_EV_POINTER, 1, 2, 0);
    CHECK(queue_event(&w, &e) == 1);
    e = event(WIN_EV_POINTER, 3, 4, 0);                 /* a move with the same buttons takes its place */
    CHECK(queue_event(&w, &e) == 0 && w.ev_count == 1 && w.ev[0].x == 3 && w.ev[0].y == 4);
    e = event(WIN_EV_POINTER, 3, 4, WIN_BUTTON_LEFT);   /* other buttons: a press, kept */
    CHECK(queue_event(&w, &e) == 1 && w.ev_count == 2);
    e = event(WIN_EV_KEY, 0, 0, 'a');
    CHECK(queue_event(&w, &e) == 1);
    e = event(WIN_EV_POINTER, 5, 5, WIN_BUTTON_LEFT);   /* only the last event queued is merged with */
    CHECK(queue_event(&w, &e) == 1 && w.ev_count == 4);
    e = event(WIN_EV_RESIZE, 300, 200, 0);
    CHECK(queue_event(&w, &e) == 1);
    e = event(WIN_EV_RESIZE, 310, 220, 0);              /* and so does a new size */
    CHECK(queue_event(&w, &e) == 0 && w.ev_count == 5 && w.ev[4].x == 310 && w.ev[4].y == 220);
    e = event(WIN_EV_KEY, 0, 0, 'a');
    CHECK(queue_event(&w, &e) == 1);
    CHECK(queue_event(&w, &e) == 1 && w.ev_count == 7);     /* keys never merge */
    for(i = 7; i < WIN_EVENTS; i++) {
        e = event(WIN_EV_KEY, 0, 0, (unsigned)i);
        CHECK(queue_event(&w, &e) == 1);
    }
    e = event(WIN_EV_CLOSE, 0, 0, 0);
    CHECK(w.ev_count == WIN_EVENTS && queue_event(&w, &e) == -EAGAIN && w.lost == 1 && w.ev_count == WIN_EVENTS);
    e = event(WIN_EV_KEY, 0, 0, WIN_EVENTS - 1);
    CHECK(queue_event(&w, &e) == -EAGAIN && w.lost == 2);
    w.ev_first = WIN_EVENTS - 1;                        /* the ring wraps */
    w.ev_count = 1;
    e = event(WIN_EV_KEY, 0, 0, 'z');
    CHECK(queue_event(&w, &e) == 1 && w.ev[0].value == 'z');
    /* 10/25/26: a move after a press, or after a release, is queued: the press and the release keep their place */
    memset(&w, 0, sizeof(w));
    e = event(WIN_EV_POINTER, 10, 10, WIN_BUTTON_LEFT);
    CHECK(queue_event(&w, &e) == 1);
    e = event(WIN_EV_POINTER, 40, 12, WIN_BUTTON_LEFT); /* the same buttons, but the press is no move */
    CHECK(queue_event(&w, &e) == 1 && w.ev_count == 2 && w.ev[0].x == 10 && w.ev[0].y == 10);
    e = event(WIN_EV_POINTER, 50, 14, WIN_BUTTON_LEFT); /* a move after a move: merged */
    CHECK(queue_event(&w, &e) == 0 && w.ev_count == 2 && w.ev[1].x == 50 && w.ev[1].y == 14);
    e = event(WIN_EV_POINTER, 50, 14, 0);
    CHECK(queue_event(&w, &e) == 1 && w.ev_count == 3);
    e = event(WIN_EV_POINTER, 60, 20, 0);               /* the release keeps its place too */
    CHECK(queue_event(&w, &e) == 1 && w.ev_count == 4 && w.ev[2].x == 50 && w.ev[2].value == 0);
    e = event(WIN_EV_POINTER, 70, 25, 0);
    CHECK(queue_event(&w, &e) == 0 && w.ev_count == 4 && w.ev[3].x == 70);
    w.ev_first = 4;                                     /* all read: the next move is queued */
    w.ev_count = 0;
    e = event(WIN_EV_POINTER, 71, 25, 0);
    CHECK(queue_event(&w, &e) == 1 && w.ev_count == 1);
    e = event(WIN_EV_POINTER, 72, 25, 0);               /* and the one after it merged */
    CHECK(queue_event(&w, &e) == 0 && w.ev_count == 1 && w.ev[4].x == 72);
    e = event(WIN_EV_POINTER, 72, 25, WIN_BUTTON_LEFT); /* a press: queued */
    CHECK(queue_event(&w, &e) == 1 && w.ev_count == 2);
}

static void test_titles(void) {
    char s[80], out[WIN_TITLE_MAX];
    memset(s, 'a', sizeof(s));
    CHECK(title_cut(s, 70, 63) == 63);                  /* ASCII: cut where it reaches */
    CHECK(title_cut(s, 10, 63) == 10);
    s[62] = (char)0xC3;                                 /* é across the cut: it goes whole */
    s[63] = (char)0xA9;
    CHECK(title_cut(s, 70, 63) == 62);
    CHECK(title_cut(s, 64, 64) == 64);                  /* within it: kept */
    memset(s, 'a', sizeof(s));
    s[61] = (char)0xE2;                                 /* ≋ (E2 89 8B) at 61..63 */
    s[62] = (char)0x89;
    s[63] = (char)0x8B;
    CHECK(title_cut(s, 70, 63) == 61);
    CHECK(title_cut(s, 70, 64) == 64);
    memset(s, 'a', sizeof(s));
    s[60] = (char)0xF0;                                 /* a character of 4 bytes, 60..63 */
    s[61] = (char)0x9F;
    s[62] = (char)0x8C;
    s[63] = (char)0x8A;
    CHECK(title_cut(s, 70, 63) == 60);
    s[63] = 'b';                                        /* the cut right after a whole one */
    s[60] = s[61] = s[62] = 'a';
    CHECK(title_cut(s, 70, 63) == 63);
    title_set(out, "SurfOS", 6);
    CHECK(!strcmp(out, "SurfOS"));
    memset(s, 'x', sizeof(s));
    title_set(out, s, 80);
    CHECK(strlen(out) == WIN_TITLE_MAX - 1);
}

/**** opening: the arguments, which desktop, sizes, budgets ****/

static void test_open(void) {
    struct win_open o;
    struct win_info k;
    char guard[64];
    int fd, fd2;
    begin();
    as(other);                                          /* F2: no desktop there */
    CHECK(sys_winopen(NULL) == -EFAULT);
    bad_lo = guard;
    bad_hi = guard + sizeof(guard);
    CHECK(sys_winopen((const struct win_open *)(guard + 8)) == -EFAULT);
    bad_lo = bad_hi = NULL;
    good(&o, 200, 100);
    o.version = 2;
    CHECK(sys_winopen(&o) == -EINVAL);
    good(&o, 200, 100);
    o.flags = 0x80;
    CHECK(sys_winopen(&o) == -EINVAL);
    good(&o, 0, 100);
    CHECK(sys_winopen(&o) == -EINVAL);
    good(&o, 200, 0);
    CHECK(sys_winopen(&o) == -EINVAL);
    good(&o, 200, 100);
    o.min_w = 300;
    o.max_w = 200;
    CHECK(sys_winopen(&o) == -EINVAL);
    good(&o, 200, 100);
    o.min_h = 90;
    o.max_h = 80;
    CHECK(sys_winopen(&o) == -EINVAL);
    good(&o, 200, 100);
    o.background = 0xFF102E3B;
    CHECK(sys_winopen(&o) == -EINVAL);
    good(&o, 200, 100);
    CHECK(sys_winopen(&o) == -ENODEV);                  /* the arguments first, then the desktop */
    CHECK(live == 2);                                   /* the desktop's vnode and file, no more */
    other->tty = NULL;                                  /* no terminal at all */
    CHECK(sys_winopen(&o) == -ENODEV);
    other->tty = CONSOLE(2);
    as(prog);                                           /* F1: the desktop's console */
    fd = sys_winopen(&o);
    CHECK(fd >= 0 && prog->files[fd] && prog->files[fd]->v->ops == &win_ops);
    k = info_of(prog, fd);
    CHECK(k.version == WIN_VERSION && k.w == 200 && k.h == 100 && k.screen_w == 960 && k.screen_h == 540 &&
          k.scale == 2 && !k.focused && !k.lost);
    CHECK(!strcmp(win_of_fd(prog, fd)->title, "test") && win_of_fd(prog, fd)->desk == &desks[0]);
    /* a pty the desktop's Terminal adopted, and only while it is adopted */
    as(ptyprog);
    CHECK(sys_winopen(&o) == -ENODEV);
    as(other);
    CHECK(win_adopt(1) == -ENODEV);                     /* a task with no desktop adopts nothing */
    as(desktop);
    CHECK(win_adopt(-1) == -EINVAL && win_adopt(PTY_MAX) == -EINVAL);
    CHECK(win_adopt(1) == 0 && win_adopt(1) == 0);
    CHECK(win_adopt(2) == 0 && win_adopt(3) == 0 && win_adopt(4) == 0);
    CHECK(win_adopt(5) == -ENOSPC);                     /* DESK_PTYS of them */
    as(ptyprog);
    fd2 = sys_winopen(&o);
    CHECK(fd2 >= 0 && win_of_fd(ptyprog, fd2)->desk == &desks[0]);
    as(desktop);
    win_drop(1);
    as(ptyprog);
    CHECK(sys_winopen(&o) == -ENODEV);
    /* 10/25/26: a pty that hangs up, or is free again, is no desktop's: the next session on it (a telnet session's)
       opens no window; what its programs opened before stays theirs */
    as(desktop);
    CHECK(win_adopt(1) == 0);
    win_pty_gone(PTY(2));                               /* another pty: nothing changes */
    as(ptyprog);
    fd = sys_winopen(&o);
    CHECK(fd >= 0);
    win_pty_gone(PTY(1));
    CHECK(sys_winopen(&o) == -ENODEV && win_of_fd(ptyprog, fd)->desk == &desks[0]);
    CHECK(desks[0].ptys[0] == NULL);
    /* a desktop whose task is dead takes no window; one started on its console takes them */
    desktop->state = TS_DEAD;
    as(prog);
    CHECK(sys_winopen(&o) == -ENODEV);
    desktop->state = TS_BLOCKED;
    as(other);
    CHECK(win_accept() == -ENODEV);                     /* no desktop of its own */
    as(prog);
    CHECK(win_register(&desk960) == -EBUSY);            /* F1 has one */
    end();
}

static void test_dead_desktop_console(void) {
    surf_task *second;
    int fd, reg2;
    begin();
    desktop->state = TS_DEAD;                           /* dead, not collected yet: its console takes a new desktop */
    second = task_new(11, CONSOLE(1));
    as(second);
    reg2 = win_register(&desk960);
    CHECK(reg2 >= 0);
    fd = open_as(prog, 100, 100);
    CHECK(fd >= 0 && win_of_fd(prog, fd)->desk == &desks[1]);
    reap(desktop);                                      /* the dead one collected: the new one's windows stay */
    CHECK(desks[1].used && !win_of_fd(prog, fd)->hung);
    end();
}

static void test_sizes(void) {
    struct win_open o;
    struct win_info k;
    int fd, before;
    begin();
    fd = open_as(prog, 2000, 2000);                     /* within the desktop's largest content area */
    k = info_of(prog, fd);
    CHECK(fd >= 0 && k.w == 958 && k.h == 457);
    good(&o, 50, 50);
    o.min_w = 100;
    o.min_h = 60;
    o.max_w = 300;
    o.max_h = 70;
    as(prog);
    fd = sys_winopen(&o);
    k = info_of(prog, fd);
    CHECK(k.w == 100 && k.h == 60);                     /* up to its minimum */
    good(&o, 400, 400);
    o.max_w = 300;
    o.max_h = 70;
    as(prog);
    k = info_of(prog, sys_winopen(&o));
    CHECK(k.w == 300 && k.h == 70);                     /* down to its maximum */
    good(&o, 100, 100);
    o.min_w = 1000;                                     /* more than this desktop can show */
    as(prog);
    CHECK(sys_winopen(&o) == -EINVAL);
    /* new sizes */
    fd = open_as(prog, 200, 100);
    as(prog);
    CHECK(sys_winctl(fd, WIN_SETSIZE, (void *)(u_long)((959 << 16) | 100)) == -EINVAL);
    CHECK(sys_winctl(fd, WIN_SETSIZE, (void *)(u_long)((100 << 16) | 458)) == -EINVAL);
    CHECK(sys_winctl(fd, WIN_SETSIZE, (void *)(u_long)((0 << 16) | 100)) == -EINVAL);
    CHECK(sys_winctl(fd, WIN_SETSIZE, (void *)(u_long)((100 << 16) | 0)) == -EINVAL);
    before = live;
    CHECK(sys_winctl(fd, WIN_SETSIZE, (void *)(u_long)((200 << 16) | 100)) == 0 && live == before);   /* its size */
    CHECK(win_of_fd(prog, fd)->serial == 0);
    CHECK(sys_winctl(fd, WIN_SETSIZE, (void *)(u_long)((958 << 16) | 457)) == 0);
    k = info_of(prog, fd);
    CHECK(k.w == 958 && k.h == 457 && win_of_fd(prog, fd)->serial == 1 && live == before);   /* the old one freed */
    CHECK(win_of_fd(prog, fd)->buf->px[958 * 457 - 1] == 0x102E3B);   /* all background */
    good(&o, 200, 100);
    o.min_w = 150;
    o.max_h = 120;
    as(prog);
    fd = sys_winopen(&o);
    CHECK(sys_winctl(fd, WIN_SETSIZE, (void *)(u_long)((149 << 16) | 100)) == -EINVAL);   /* its own limits */
    CHECK(sys_winctl(fd, WIN_SETSIZE, (void *)(u_long)((150 << 16) | 121)) == -EINVAL);
    CHECK(sys_winctl(fd, WIN_SETSIZE, (void *)(u_long)((150 << 16) | 120)) == 0);
    end();
}

static void test_budgets(void) {
    surf_task *p[6];
    int i, k, fd;
    begin();
    for(i = 0; i < WIN_PER_DESK; i++) CHECK(open_as(prog, 10, 10) >= 0);
    CHECK(open_as(prog, 10, 10) == -EAGAIN);            /* the desktop's ninth */
    end();
    /* its bytes: four windows as large as can be (1.75 MB each), and a fifth does not fit; a small one does */
    begin();
    for(i = 0; i < 4; i++) CHECK(open_as(prog, 958, 457) >= 0);
    CHECK(open_as(prog, 958, 457) == -ENOMEM);
    CHECK(open_as(prog, 100, 100) >= 0);
    fd = open_as(prog, 100, 100);
    as(prog);                                           /* nor may a window grow past it */
    CHECK(sys_winctl(fd, WIN_SETSIZE, (void *)(u_long)((958 << 16) | 457)) == -ENOMEM);
    CHECK(info_of(prog, fd).w == 100);
    end();
    /* 10/25/26: windows let go by their desktop keep no pixels while their programs hold them, only their size: the
       next desktop has its whole budget */
    begin();
    for(k = 0; k < 4; k++) {
        p[k] = task_new(100 + k, CONSOLE(1));
        for(i = 0; i < 4; i++) CHECK(open_as(p[k], 958, 457) >= 0);
        as(desktop);
        win_unregister(reg);                            /* the desktop leaves; its windows are let go */
        CHECK(!desks[0].used && win_of_fd(p[k], 0)->hung && !win_of_fd(p[k], 0)->buf);
        CHECK(info_of(p[k], 0).w == 958 && info_of(p[k], 0).h == 457 && info_of(p[k], 0).screen_w == 0);
        as(desktop);
        reg = win_register(&desk960);                   /* and a new one starts on F1 */
        CHECK(reg >= 0);
    }
    CHECK(bytes_of(NULL) == 0);
    for(i = 0; i < 4; i++) CHECK(open_as(prog, 958, 457) >= 0);
    CHECK(open_as(prog, 958, 457) == -ENOMEM);          /* the desktop's own 8 MB, as for the first */
    CHECK(bytes_of(&desks[0]) == 4 * 958 * 457 * 4u && bytes_of(NULL) == bytes_of(&desks[0]));
    CHECK(open_as(prog, 100, 100) >= 0);
    end();
    /* in all: WIN_MAX windows */
    begin();
    for(k = 0; k < 4; k++) {
        p[k] = task_new(100 + k, CONSOLE(1));
        for(i = 0; i < WIN_PER_DESK; i++) CHECK(open_as(p[k], 10, 10) >= 0);
        as(desktop);
        win_unregister(reg);
        reg = win_register(&desk960);
    }
    CHECK(windows_of(NULL) == WIN_MAX && open_as(prog, 10, 10) == -EAGAIN);
    end();
    /* a descriptor each: none free, no window, and nothing allocated for it */
    begin();
    for(i = 0; i < NR_OPEN; i++) prog->files[i] = (struct file *)&ttys[0];
    k = live;
    CHECK(open_as(prog, 10, 10) == -EMFILE && live == k);
    for(i = 0; i < NR_OPEN; i++) prog->files[i] = NULL;
    end();
}

static void test_no_memory(void) {
    int i, fd, before;
    struct win_info k;
    begin();
    for(i = 1; i <= 4; i++) {                           /* each of the open's allocations: the pixels' record and */
        before = live;                                  /* their memory, the vnode, the file */
        fail_in = i;
        CHECK(open_as(prog, 50, 50) == -ENOMEM && live == before);
        fail_in = 0;
    }
    fd = open_as(prog, 50, 50);
    CHECK(fd >= 0);
    before = live;
    for(i = 1; i <= 2; i++) {                           /* a new size whose record (10/25/26) or pixels cannot be */
        fail_in = i;                                    /* had: the old ones stay */
        as(prog);
        CHECK(sys_winctl(fd, WIN_SETSIZE, (void *)(u_long)((60 << 16) | 60)) == -ENOMEM && live == before);
        fail_in = 0;
    }
    k = info_of(prog, fd);
    CHECK(k.w == 50 && k.h == 50);
    as(desktop);                                        /* a registration without memory: nothing left */
    win_unregister(reg);
    for(i = 1; i <= 2; i++) {
        before = live;
        fail_in = i;
        CHECK(win_register(&desk960) == -ENOMEM && live == before && !desks[0].used);
        fail_in = 0;
    }
    end();
}

/**** presents ****/

/* 10/25/26: a 32-bit build's (see the top): the image's address below 4 GB, as a program's is */
static void test_present(void) {
    static unsigned image[100 * 50];
    struct win_present p;
    struct win_state st;
    struct win_view view;
    struct win *w;
    int fd, h, i;
    begin();
    fd = open_as(prog, 100, 50);
    w = win_of_fd(prog, fd);
    for(i = 0; i < 100 * 50; i++) image[i] = 0xAB000000 | (u32)i;
    CHECK(present_all(prog, fd, image, 100, 50) == 0);
    CHECK(w->buf->px[0] == 0 && w->buf->px[4999] == 4999 && w->buf->px[257] == 257);   /* the top byte dropped */
    CHECK(w->presents == 1 && w->serial == 1);
    as(desktop);
    h = win_accept();
    CHECK(h >= 0 && win_update(h, &st, &view) == 0 && view.pixels == w->buf->px && view.w == 100 && view.h == 50);
    memset(&p, 0, sizeof(p));
    p.pixels = image;
    p.stride = 100;
    as(prog);
    p.x = 90; p.w = 20; p.h = 10;                       /* past the right edge */
    CHECK(sys_winctl(fd, WIN_PRESENT, &p) == -EINVAL);
    p.x = 0; p.w = 10; p.y = 45; p.h = 10;              /* past the bottom */
    CHECK(sys_winctl(fd, WIN_PRESENT, &p) == -EINVAL);
    p.y = 0; p.h = 10;
    p.stride = 0;
    CHECK(sys_winctl(fd, WIN_PRESENT, &p) == -EINVAL);
    p.stride = WIN_STRIDE_MAX + 1;
    CHECK(sys_winctl(fd, WIN_PRESENT, &p) == -EINVAL);
    p.stride = 8;                                       /* a row that runs into the next one */
    CHECK(sys_winctl(fd, WIN_PRESENT, &p) == -EINVAL);
    p.stride = 100;
    p.pixels = NULL;
    CHECK(sys_winctl(fd, WIN_PRESENT, &p) == -EFAULT);
    p.pixels = (const unsigned *)0xFFFFF000;            /* a span past 4 GB: refused before anything is read */
    p.stride = WIN_STRIDE_MAX;
    p.x = 0; p.y = 40; p.w = 10; p.h = 10;
    CHECK(sys_winctl(fd, WIN_PRESENT, &p) == -EFAULT);
    bad_lo = (const char *)(image + 100 * 49);          /* its last row not the program's to read */
    bad_hi = (const char *)(image + 100 * 50);
    p.pixels = image;
    p.stride = 100;
    p.x = 0; p.y = 0; p.w = 100; p.h = 50;
    image[0] = 0x777777;
    CHECK(sys_winctl(fd, WIN_PRESENT, &p) == -EFAULT && w->buf->px[0] == 0);       /* nothing copied */
    p.h = 49;
    CHECK(sys_winctl(fd, WIN_PRESENT, &p) == 0 && w->buf->px[0] == 0x777777);
    bad_lo = bad_hi = NULL;
    CHECK(w->presents == 2);
    p.pixels = NULL;                                    /* an empty one: nothing read, a present all the same */
    p.w = 0;
    CHECK(sys_winctl(fd, WIN_PRESENT, &p) == 0 && w->presents == 3);
    for(i = 0; i < 100 * 50; i++) image[i] = 0x00FF00;  /* a part of it: only that part */
    p.pixels = image;
    p.x = 10; p.y = 5; p.w = 20; p.h = 10;
    CHECK(sys_winctl(fd, WIN_PRESENT, &p) == 0);
    CHECK(w->buf->px[5 * 100 + 10] == 0x00FF00 && w->buf->px[14 * 100 + 29] == 0x00FF00);
    CHECK(w->buf->px[5 * 100 + 9] != 0x00FF00 && w->buf->px[15 * 100 + 10] != 0x00FF00 && w->buf->px[5 * 100 + 30] != 0x00FF00);
    as(prog);                                           /* no window: no such descriptor, or one of something else */
    CHECK(sys_winctl(9, WIN_PRESENT, &p) == -EBADF);
    as(desktop);
    CHECK(sys_winctl(reg, WIN_PRESENT, &p) == -EINVAL);
    as(prog);
    CHECK(sys_winctl(fd, 99, &p) == -EINVAL);
    CHECK(sys_winctl(fd, WIN_PRESENT, (void *)0x10) == -EFAULT);
    end();
}

/**** events, read and poll ****/

static int pending_h;
static void desktop_sends(void) {                       /* while the program waits: the desktop sends a key */
    surf_task *t = curTask;
    send_ev(pending_h, WIN_EV_KEY, 0, 0, 'k');
    curTask = t;
    on_yield = NULL;
}

static void test_events(void) {
    struct win_event ev[WIN_EVENTS + 4];
    struct win_info k;
    int fd, h, i, n;
    begin();
    fd = open_as(prog, 100, 100);
    as(desktop);
    CHECK(win_accept() == -EAGAIN);                     /* no present yet, and not 250 ms */
    h = shown(prog, fd);
    CHECK(h >= 0);
    CHECK(pollw(prog, fd) == POLLOUT);
    wakes = 0;
    CHECK(send_ev(h, WIN_EV_POINTER, 5, 6, 0) == 0 && wakes == 1);
    CHECK(send_ev(h, WIN_EV_POINTER, 7, 8, 0) == 0);    /* merged */
    CHECK(send_ev(h, WIN_EV_POINTER, 7, 8, WIN_BUTTON_LEFT) == 0);
    CHECK(send_ev(h, WIN_EV_KEY, 0, 0, WIN_KEY_UP) == 0);
    CHECK(pollw(prog, fd) == (POLLOUT | POLLIN));
    CHECK(readev(prog, fd, ev, 15) == -EINVAL);         /* less than one record */
    CHECK(readev(prog, fd, ev, 40) == 32);              /* whole records only */
    CHECK(ev[0].type == WIN_EV_POINTER && ev[0].x == 7 && ev[0].y == 8 && ev[0].value == 0);
    CHECK(ev[1].value == WIN_BUTTON_LEFT);
    CHECK(readev(prog, fd, ev, sizeof(ev)) == 16 && ev[0].type == WIN_EV_KEY && ev[0].value == WIN_KEY_UP);
    CHECK(pollw(prog, fd) == POLLOUT);
    /* an empty queue: the read waits; the desktop's event ends the wait */
    pending_h = h;
    on_yield = desktop_sends;
    yields = 0;
    CHECK(readev(prog, fd, ev, sizeof(ev)) == 16 && ev[0].value == 'k' && yields == 1);
    /* a wait cut short (a kill, Ctrl+C) */
    CHECK(readev(prog, fd, ev, sizeof(ev)) == -EINTR);
    prog->intr = 0;
    /* a full queue loses what comes, and says how much; focus is kept even then */
    for(i = 0; i < WIN_EVENTS + 3; i++) send_ev(h, WIN_EV_KEY, 0, 0, (unsigned)i);
    CHECK(send_ev(h, WIN_EV_FOCUS, 0, 0, 1) == -EAGAIN);
    k = info_of(prog, fd);
    CHECK(k.lost == 4 && k.focused == 1);
    n = readev(prog, fd, ev, sizeof(ev));
    CHECK(n == WIN_EVENTS * 16 && ev[0].value == 0 && ev[WIN_EVENTS - 1].value == WIN_EVENTS - 1);
    CHECK(send_ev(h, WIN_EV_FOCUS, 0, 0, 0) == 0 && info_of(prog, fd).focused == 0);
    end();
}

/**** the desktop's side: appearing, handles, updates ****/

static void test_accept(void) {
    struct win_state st;
    struct win_view view;
    int a, b, ha, hb, h;
    begin();
    a = open_as(prog, 100, 100);
    now += 1;
    b = open_as(prog, 120, 80);
    hb = shown(prog, b);                                /* the one that presented first */
    CHECK(hb >= 0 && win_of_fd(prog, b)->held && !win_of_fd(prog, a)->held);
    as(desktop);
    CHECK(win_accept() == -EAGAIN);
    now += SHOW_TICKS;                                  /* 250 ms: the other appears without a present */
    ha = win_accept();
    CHECK(ha >= 0 && ha != hb && win_accept() == -EAGAIN);
    CHECK(win_update(ha, &st, &view) == 0 && st.w == 100 && st.h == 100 && st.pid == 20 && st.serial == 0 &&
          st.background == 0x102E3B && !strcmp(st.title, "test") && view.pixels[0] == 0x102E3B);
    CHECK(win_update(hb, &st, &view) == 0 && st.w == 120 && st.serial == 1 && view.pixels[0] == 1);
    /* handles: only the desktop's own, and only while it holds the window */
    as(prog);
    CHECK(win_update(ha, &st, &view) == -EINVAL);
    as(desktop);
    CHECK(win_update(-1, &st, &view) == -EINVAL && win_update(ha + 1000 * WIN_MAX, &st, &view) == -EINVAL);
    win_close(ha);
    CHECK(win_update(ha, &st, &view) == -EINVAL && send_ev(ha, WIN_EV_KEY, 0, 0, 'a') == -EINVAL);
    as(prog);
    fd_close(a);                                        /* gone: its slot is taken again by the next */
    a = open_as(prog, 50, 50);
    h = shown(prog, a);
    CHECK(h >= 0 && h % WIN_MAX == ha % WIN_MAX && h != ha);
    as(desktop);
    CHECK(win_update(ha, &st, &view) == -EINVAL && win_update(h, &st, &view) == 0);
    end();
}

static void test_prog_first(void) {
    struct win_state st;
    struct win_view view;
    int fd, h, before;
    begin();
    before = live;
    fd = open_as(prog, 100, 100);
    h = shown(prog, fd);
    as(desktop);
    CHECK(win_update(h, &st, &view) == 0);
    as(prog);
    wakes = 0;
    fd_close(fd);                                       /* the program closes it: the desktop holds it still */
    CHECK(wakes == 1 && wins[h % WIN_MAX].used && live > before);
    as(desktop);
    CHECK(win_update(h, &st, &view) == -EPIPE);         /* ended */
    win_close(h);
    CHECK(!wins[h % WIN_MAX].used && live == before);
    end();
}

static void test_desktop_first(void) {
    struct win_present p;
    struct win_event ev[4];
    struct win_info k;
    unsigned px = 5;
    int fd, h, before;
    begin();
    before = live;
    fd = open_as(prog, 100, 100);
    h = shown(prog, fd);
    send_ev(h, WIN_EV_KEY, 0, 0, 'a');
    send_ev(h, WIN_EV_CLOSE, 0, 0, 0);
    as(desktop);
    woken = 0;
    win_close(h);                                       /* the desktop lets it go first (End, or its own end) */
    CHECK(woken == 1 && wins[h % WIN_MAX].used && win_of_fd(prog, fd)->hung);
    CHECK(!win_of_fd(prog, fd)->buf && !win_of_fd(prog, fd)->view && live == before + 2);   /* its pixels gone */
    CHECK(pollw(prog, fd) == (POLLOUT | POLLIN));
    CHECK(readev(prog, fd, ev, sizeof(ev)) == 32);      /* what was queued, then the end */
    CHECK(pollw(prog, fd) == (POLLOUT | POLLIN | POLLHUP));
    CHECK(readev(prog, fd, ev, sizeof(ev)) == 0);
    memset(&p, 0, sizeof(p));
    p.pixels = &px;
    p.stride = 1;
    p.w = p.h = 1;
    as(prog);
    CHECK(sys_winctl(fd, WIN_PRESENT, &p) == -EPIPE);
    CHECK(sys_winctl(fd, WIN_SETSIZE, (void *)(u_long)((50 << 16) | 50)) == -EPIPE);
    CHECK(sys_winctl(fd, WIN_TITLE, "late") == -EPIPE);
    k = info_of(prog, fd);
    CHECK(k.w == 100 && k.screen_w == 0 && k.scale == 0);
    fd_close(fd);                                       /* then the program: freed */
    CHECK(!wins[h % WIN_MAX].used && live == before);
    end();
}

static void test_desktop_end(void) {
    struct win_event ev[2];
    struct win_state st;
    struct win_view view;
    int a, b, ha, before;
    begin();
    before = live;
    a = open_as(prog, 100, 100);
    b = open_as(prog, 100, 100);                        /* never shown */
    ha = shown(prog, a);
    as(desktop);
    CHECK(win_update(ha, &st, &view) == 0);
    win_unregister(reg);                                /* the desktop leaves: every window let go */
    CHECK(!desks[0].used && win_of_fd(prog, a)->hung && win_of_fd(prog, b)->hung && !win_of_fd(prog, a)->view);
    CHECK(!win_of_fd(prog, a)->buf && !win_of_fd(prog, b)->buf);   /* 10/25/26: and their pixels */
    CHECK(readev(prog, a, ev, sizeof(ev)) == 0 && readev(prog, b, ev, sizeof(ev)) == 0);
    CHECK(open_as(prog, 10, 10) == -ENODEV);            /* no desktop on F1 now */
    as(prog);
    fd_close(a);
    fd_close(b);
    CHECK(live == before - 2);                          /* the registration's vnode and file went too */
    end();
    /* its death: the task collected, its descriptor with it */
    begin();
    a = open_as(prog, 100, 100);
    ha = shown(prog, a);
    as(desktop);
    CHECK(win_update(ha, &st, &view) == 0);
    desktop->state = TS_DEAD;
    reap(desktop);
    CHECK(!desks[0].used && win_of_fd(prog, a)->hung && readev(prog, a, ev, sizeof(ev)) == 0);
    end();
}

static void test_owner_death(void) {
    struct win_state st;
    struct win_view view;
    int fd, h;
    begin();
    fd = open_as(prog, 100, 100);
    h = shown(prog, fd);
    prog->state = TS_DEAD;                              /* killed, not collected yet */
    as(desktop);
    CHECK(win_update(h, &st, &view) == -EPIPE);         /* the desktop takes its window away at once */
    win_close(h);
    CHECK(wins[h % WIN_MAX].used && win_of_fd(prog, fd)->hung);
    reap(prog);                                         /* collected: freed */
    CHECK(!wins[h % WIN_MAX].used);
    end();
}

static void test_outlives_resize(void) {
    struct win_state st;
    struct win_view view, old;
    int fd, h, before;
    begin();
    fd = open_as(prog, 100, 100);
    h = shown(prog, fd);
    as(desktop);
    CHECK(win_update(h, &st, &view) == 0);
    old = view;
    before = live;
    as(prog);
    CHECK(sys_winctl(fd, WIN_SETSIZE, (void *)(u_long)((200 << 16) | 150)) == 0);
    CHECK(live == before + 2);                          /* the new pixels; the old ones live on, the desktop draws them */
    CHECK(old.pixels[0] == 1 && old.pixels[9999] == 0x102E3B);   /* still readable as they were */
    CHECK(sys_winctl(fd, WIN_SETSIZE, (void *)(u_long)((300 << 16) | 150)) == 0);
    CHECK(live == before + 2);                          /* the middle one nobody drew is freed at once */
    as(desktop);
    CHECK(win_update(h, &st, &view) == 0 && st.w == 300 && st.h == 150 && view.w == 300 && view.pixels != old.pixels);
    CHECK(live == before);                              /* the old ones freed once the desktop moved on */
    CHECK(bytes_of(&desks[0]) == 300 * 150 * 4u);
    end();
}

static void test_title_calls(void) {
    static char page[2 * USER_PAGE] __attribute__((aligned(USER_PAGE)));
    struct win_open o;
    struct win_state st;
    struct win_view view;
    char t[100];
    int fd, h;
    begin();
    good(&o, 100, 100);
    memset(o.title, 'q', sizeof(o.title));              /* no NUL in its 64 bytes */
    o.title[62] = (char)0xC3;
    o.title[63] = (char)0xA9;
    as(prog);
    fd = sys_winopen(&o);
    CHECK(fd >= 0 && strlen(win_of_fd(prog, fd)->title) == 62);
    h = shown(prog, fd);
    memset(t, 'x', sizeof(t));
    t[99] = 0;
    as(prog);
    CHECK(sys_winctl(fd, WIN_TITLE, t) == 0 && strlen(win_of_fd(prog, fd)->title) == WIN_TITLE_MAX - 1);
    as(desktop);
    CHECK(win_update(h, &st, &view) == 0 && strlen(st.title) == WIN_TITLE_MAX - 1 && st.serial == 2);
    as(prog);
    CHECK(sys_winctl(fd, WIN_TITLE, "Calculator \xE2\x89\x8B") == 0 && !strcmp(win_of_fd(prog, fd)->title, "Calculator \xE2\x89\x8B"));
    CHECK(sys_winctl(fd, WIN_TITLE, NULL) == -EFAULT);
    memcpy(page + USER_PAGE - 5, "abcde", 5);           /* it runs on into a page that is not the program's */
    bad_lo = page + USER_PAGE;
    bad_hi = page + 2 * USER_PAGE;
    CHECK(sys_winctl(fd, WIN_TITLE, page + USER_PAGE - 5) == -EFAULT);
    CHECK(!strcmp(win_of_fd(prog, fd)->title, "Calculator \xE2\x89\x8B"));
    page[USER_PAGE - 1] = 0;                            /* one that ends before it */
    CHECK(sys_winctl(fd, WIN_TITLE, page + USER_PAGE - 5) == 0 && !strcmp(win_of_fd(prog, fd)->title, "abcd"));
    bad_lo = bad_hi = NULL;
    end();
}

/* 10/25/26 (stage C6): a window says whether its program is in the foreground job of a pty its desktop adopted (the
   Terminal's), a job that session's shell made, where the desktop gives its window the focus; a task in no job that
   makes a job of its own the foreground (sys_job lets it, CODE-AUDIT T14) gets no such window */
static void test_foreground(void) {
    struct win_state st;
    struct win_view view;
    surf_task *evil, *child;
    int fd, h;
    begin();
    as(desktop);
    CHECK(win_adopt(1) == 0);
    ptyprog->job = 40;                                  /* a job the session's shell (pid 50) made, in the foreground */
    ptyprog->job_shell = 50;
    PTY(1)->fg_job = 40;
    fd = open_as(ptyprog, 100, 100);
    h = shown(ptyprog, fd);
    CHECK(h >= 0 && win_update(h, &st, &view) == 0 && st.foreground == 0);     /* no shell known yet: none */
    as(desktop);
    CHECK(win_adopt_shell(1, 50) == 0 && desks[0].shells[0] == 50);
    CHECK(win_update(h, &st, &view) == 0 && st.foreground == 1);
    CHECK(win_adopt_shell(2, 50) == -EINVAL && win_adopt_shell(1, 0) == -EINVAL && win_adopt_shell(PTY_MAX, 50) == -EINVAL);
    as(other);
    CHECK(win_adopt_shell(1, 50) == -ENODEV);           /* a task with no desktop */
    as(desktop);
    PTY(1)->fg_job = 7;                                 /* another job in the foreground: this one is behind */
    CHECK(win_update(h, &st, &view) == 0 && st.foreground == 0);
    PTY(1)->fg_job = 40;
    ptyprog->job = 0;                                   /* in no job */
    CHECK(win_update(h, &st, &view) == 0 && st.foreground == 0);
    ptyprog->job = 40;
    /* a task in no job on that pty (detached from a job in the background) makes a job of its own the foreground */
    evil = task_new(60, PTY(1));
    child = task_new(61, PTY(1));
    child->job = 61;
    child->job_shell = 60;
    PTY(1)->fg_job = 61;
    fd = open_as(child, 50, 50);
    h = shown(child, fd);
    CHECK(h >= 0 && win_update(h, &st, &view) == 0 && st.foreground == 0);
    (void)evil;
    PTY(1)->fg_job = 40;
    as(desktop);
    win_drop(1);                                        /* a pty no longer adopted: its shell forgotten too */
    CHECK(!desks[0].ptys[0] && !desks[0].shells[0]);
    CHECK(win_adopt(1) == 0 && win_adopt_shell(1, 50) == 0);
    win_pty_gone(PTY(1));                               /* and so at its hangup */
    CHECK(!desks[0].ptys[0] && !desks[0].shells[0]);
    prog->job = 9;                                      /* the desktop's own console is no Terminal's pty */
    prog->job_shell = 50;
    CONSOLE(1)->fg_job = 9;
    fd = open_as(prog, 50, 50);
    h = shown(prog, fd);
    CHECK(h >= 0 && win_update(h, &st, &view) == 0 && st.foreground == 0);
    CONSOLE(1)->fg_job = PTY(1)->fg_job = 0;
    end();
}

/* 10/26/26 (stage C12); the review: nothing copied for a refusal, text kept only from the window with the desktop's
   focus at the take, empty text refused, exactly WIN_CLIP_MAX taken whole, one alert in WIN_ALERT_MS a program */
static void test_clip_alert(void) {
    static char big[WIN_CLIP_MAX], back[WIN_CLIP_MAX];
    struct win_clip c;
    struct win_state st;
    struct win_view view;
    surf_task *prog2;
    char out[64];
    int fd, fd2, fd3, h, h2, n;
    begin();
    prog2 = task_new(21, CONSOLE(1));
    fd = open_as(prog, 40, 30);
    h = shown(prog, fd);
    CHECK(fd >= 0 && h >= 0);
    c.text = "copied";
    c.len = 6;
    as(prog);
    n = allocs;
    CHECK(sys_winctl(fd, WIN_CLIP, &c) == -EACCES && allocs == n);  /* no focus: nothing planted, nor copied */
    as(desktop);
    CHECK(win_clip_take(h, out, sizeof(out)) == 0);
    send_ev(h, WIN_EV_FOCUS, 0, 0, 1);
    as(prog);
    CHECK(sys_winctl(fd, WIN_CLIP, &c) == 0);
    c.text = "second";
    CHECK(sys_winctl(fd, WIN_CLIP, &c) == 0);           /* the latest takes its place */
    as(desktop);
    CHECK(win_clip_take(h, out, 3) == 3 && !memcmp(out, "sec", 3));    /* as much as fits */
    CHECK(win_clip_take(h, out, sizeof(out)) == 0);     /* taken once */
    as(prog);                                           /* set before its FOCUS 0 was sent, taken when the desktop's */
    CHECK(sys_winctl(fd, WIN_CLIP, &c) == 0);           /* focus is elsewhere: dropped, not kept for later */
    as(desktop);
    CHECK(win_clip_take(-1, out, sizeof(out)) == 0 && win_clip_take(h, out, sizeof(out)) == 0);
    as(prog);
    CHECK(sys_winctl(fd, WIN_CLIP, &c) == 0);
    as(desktop);
    CHECK(win_clip_take(h + WIN_MAX, out, sizeof(out)) == 0);  /* another window's handle (another generation) */
    as(other);
    CHECK(win_clip_take(h, out, sizeof(out)) == -ENODEV);   /* a task with no desktop */
    as(prog);
    c.len = WIN_CLIP_MAX + 1;
    CHECK(sys_winctl(fd, WIN_CLIP, &c) == -EINVAL);
    memset(big, 'z', sizeof(big));                      /* exactly WIN_CLIP_MAX: all of it */
    c.text = big;
    c.len = WIN_CLIP_MAX;
    CHECK(sys_winctl(fd, WIN_CLIP, &c) == 0);
    as(desktop);
    CHECK(win_clip_take(h, back, sizeof(back)) == (int)WIN_CLIP_MAX && !memcmp(back, big, sizeof(big)));
    as(prog);
    c.len = 0;                                          /* no text: refused */
    CHECK(sys_winctl(fd, WIN_CLIP, &c) == -EINVAL);
    c.text = "\0ab";
    c.len = 3;
    CHECK(sys_winctl(fd, WIN_CLIP, &c) == -EINVAL);
    as(desktop);
    CHECK(win_clip_take(h, out, sizeof(out)) == 0);     /* nothing planted by them */
    as(prog);
    c.len = 6;
    c.text = (const char *)16;                          /* not the program's to read */
    CHECK(sys_winctl(fd, WIN_CLIP, &c) == -EFAULT);
    CHECK(sys_winctl(fd, WIN_CLIP, NULL) == -EFAULT);
    c.text = "x";
    c.len = 1;
    fail_in = 1;
    CHECK(sys_winctl(fd, WIN_CLIP, &c) == -ENOMEM);
    send_ev(h, WIN_EV_FOCUS, 0, 0, 0);
    as(prog);
    CHECK(sys_winctl(fd, WIN_CLIP, &c) == -EACCES);     /* the focus gone */
    as(desktop);
    CHECK(win_update(h, &st, &view) == 0 && st.alerts == 0);
    as(prog);
    CHECK(sys_winctl(fd, WIN_ALERT, NULL) == 0);
    CHECK(sys_winctl(fd, WIN_ALERT, NULL) == -EAGAIN);  /* once in WIN_ALERT_MS, */
    fd2 = open_as(prog, 40, 30);                        /* for its program, whichever window asks */
    h2 = shown(prog, fd2);
    as(prog);
    CHECK(h2 >= 0 && sys_winctl(fd2, WIN_ALERT, NULL) == -EAGAIN);
    fd_close(fd2);                                      /* and opened again */
    as(desktop);
    win_close(h2);
    fd2 = open_as(prog, 40, 30);
    CHECK(fd2 >= 0 && sys_winctl(fd2, WIN_ALERT, NULL) == -EAGAIN);
    fd3 = open_as(prog2, 40, 30);                       /* another program's is its own */
    CHECK(fd3 >= 0 && sys_winctl(fd3, WIN_ALERT, NULL) == 0 && sys_winctl(fd3, WIN_ALERT, NULL) == -EAGAIN);
    now += WIN_ALERT_MS * HZ / 1000 - 1;
    as(prog);
    CHECK(sys_winctl(fd, WIN_ALERT, NULL) == -EAGAIN);
    now += 1;
    CHECK(sys_winctl(fd2, WIN_ALERT, NULL) == 0 && sys_winctl(fd, WIN_ALERT, NULL) == -EAGAIN);
    as(desktop);
    CHECK(win_update(h, &st, &view) == 0 && st.alerts == 1);
    send_ev(h, WIN_EV_FOCUS, 0, 0, 1);                  /* text only: to a NUL that comes first */
    c.text = "ab\0cd";
    c.len = 5;
    as(prog);
    CHECK(sys_winctl(fd, WIN_CLIP, &c) == 0);
    as(desktop);
    CHECK(win_clip_take(h, out, sizeof(out)) == 2 && !memcmp(out, "ab", 2));
    as(prog);
    c.text = "x";
    c.len = 1;
    CHECK(sys_winctl(fd, WIN_CLIP, &c) == 0);           /* left for the desktop, which never takes it */
    as(desktop);
    win_close(h);
    as(prog);
    CHECK(sys_winctl(fd, WIN_ALERT, NULL) == -EPIPE && sys_winctl(fd, WIN_CLIP, &c) == -EPIPE);
    end();                                              /* the desktop's end frees the text it never took */
}

/**** 11/05/26 (track desk, stage 1): the new calls' arguments ****/

/* entries of a WIN_MENU */
static struct win_menu_item mi(unsigned id, unsigned flags, const char *label, const char *keys) {
    struct win_menu_item e;
    memset(&e, 0, sizeof(e));
    e.id = id;
    e.flags = flags;
    strlcpy(e.label, label, sizeof(e.label));
    strlcpy(e.keys, keys, sizeof(e.keys));
    return e;
}

static int menu_call(int fd, const struct win_menu_item *it, unsigned n) {
    struct win_menu m;
    m.items = it;
    m.count = n;
    as(prog);
    return sys_winctl(fd, WIN_MENU, &m);
}

static void test_theme(void) {
    struct win_info k;
    struct win_theme t;
    char guard[sizeof(struct win_theme)];
    int fd;
    begin();
    CHECK(sizeof(struct win_info) == 20 && sizeof(struct win_theme) == 136 && sizeof(struct win_menu_item) == 72 &&
          sizeof(struct win_menu) == 8 && sizeof(struct win_buffer) == 8 && sizeof(struct win_dialog) == 604);
    fd = open_as(prog, 200, 100);
    CHECK(fd >= 0);
    k = info_of(prog, fd);                              /* WIN_INFO as it was */
    CHECK(k.version == WIN_VERSION && k.w == 200 && k.h == 100 && k.screen_w == 960);
    CHECK(!win_of_fd(prog, fd)->themed);
    memset(&t, 0xA5, sizeof(t));                       /* 11/05/26 (the review): all of it written */
    CHECK(sys_winctl(fd, WIN_THEME, &t) == 0);          /* the theme, until a desktop says its own the ocean's */
    CHECK(t.theme == WIN_THEME_OCEAN && t.colors[WIN_COLOR_WINDOW] == 0xF4F1E6 && t.colors[WIN_COLOR_TEXT] == 0x153D49 &&
          t.colors[WIN_COLOR_ACCENT] == 0x197D83 && t.colors[WIN_COLOR_DESKTOP] == 0x245A67);
    CHECK(t.count == WIN_COLORS && t.colors[WIN_COLORS] == 0 && t.colors[WIN_THEME_SLOTS - 1] == 0);   /* room to grow */
    CHECK(win_of_fd(prog, fd)->themed);                 /* it hears of a new theme from now on */
    bad_lo = guard + 4;                                 /* not the program's to write */
    bad_hi = guard + sizeof(guard);
    CHECK(sys_winctl(fd, WIN_THEME, guard) == -EFAULT);
    bad_lo = bad_hi = NULL;
    CHECK(sys_winctl(fd, WIN_THEME, NULL) == -EFAULT);
    CHECK(sys_winctl(fd, 12, &t) == -EINVAL && sys_winctl(fd, 0, &t) == -EINVAL);  /* no such op */
    end();
}

static void test_menu_args(void) {
    static struct win_menu_item it[WIN_MENU_ENTRIES + 1];
    struct win_menu m;
    struct win_menu_item kcopy[2];
    char guard[64];
    int fd, i, n, alive;
    begin();
    fd = open_as(prog, 200, 100);
    CHECK(fd >= 0);
    alive = live;
    CHECK(sys_winctl(fd, WIN_MENU, NULL) == -EFAULT);
    bad_lo = guard;
    bad_hi = guard + sizeof(guard);
    CHECK(sys_winctl(fd, WIN_MENU, guard) == -EFAULT);  /* the struct not the program's to read */
    it[0] = mi(1, WIN_MENU_TITLE, "File", "");
    it[1] = mi(2, 0, "Open", "Ctrl+O");
    m.items = (const struct win_menu_item *)guard;      /* nor the entries */
    m.count = 1;
    CHECK(sys_winctl(fd, WIN_MENU, &m) == -EFAULT);
    bad_lo = bad_hi = NULL;
    m.items = NULL;
    CHECK(sys_winctl(fd, WIN_MENU, &m) == -EFAULT);
    m.items = it;
    m.count = WIN_MENU_ENTRIES + 1;                     /* too many: before anything is read */
    CHECK(sys_winctl(fd, WIN_MENU, &m) == -EINVAL);
    CHECK(menu_call(fd, it, 2) == 0);                   /* a menu of one item: good */
    CHECK(menu_call(fd, it, 0) == 0);             /* none: good too (the desktop's File and Edit) */
    CHECK(menu_call(fd, it + 1, 1) == -EINVAL);         /* an item before any title */
    CHECK(menu_call(fd, it, 1) == -EINVAL);             /* a menu with no items */
    it[2] = mi(3, WIN_MENU_TITLE, "Edit", "");
    it[3] = mi(4, 0, "Copy", "Ctrl+C");
    CHECK(menu_call(fd, it, 4) == 0);
    it[1].flags = 0x100;                                /* a flag that is none */
    CHECK(menu_call(fd, it, 4) == -EINVAL);
    it[1].flags = WIN_MENU_TITLE | WIN_MENU_SEPARATOR;  /* both */
    CHECK(menu_call(fd, it, 4) == -EINVAL);
    it[1] = mi(2, WIN_MENU_TITLE, "View", "");          /* File then has no items */
    CHECK(menu_call(fd, it, 4) == -EINVAL);
    it[1] = mi(2, 0, "", "");                           /* an item with no label */
    CHECK(menu_call(fd, it, 4) == -EINVAL);
    it[1] = mi(2, WIN_MENU_SEPARATOR, "", "");          /* a separator needs none */
    CHECK(menu_call(fd, it, 4) == 0);
    it[1] = mi(2, WIN_MENU_CHECKED | WIN_MENU_RADIO | WIN_MENU_DISABLED, "Zoom 100%", "Ctrl+0");
    CHECK(menu_call(fd, it, 4) == 0);
    it[0] = mi(1, WIN_MENU_TITLE, "", "");              /* a menu with no name */
    CHECK(menu_call(fd, it, 4) == -EINVAL);
    it[0] = mi(1, WIN_MENU_TITLE | WIN_MENU_DISABLED, "File", "");
    CHECK(menu_call(fd, it, 4) == 0);
    /* WIN_MENUS menus and no more, WIN_MENU_ITEMS items of one and no more */
    for(n = 0, i = 0; i < WIN_MENUS; i++) {
        it[n++] = mi(100 + i, WIN_MENU_TITLE, "M", "");
        it[n++] = mi(200 + i, 0, "item", "");
    }
    CHECK(menu_call(fd, it, n) == 0);
    it[n] = mi(300, WIN_MENU_TITLE, "M", "");
    it[n + 1] = mi(301, 0, "item", "");
    CHECK(menu_call(fd, it, n + 2) == -EINVAL);
    it[0] = mi(1, WIN_MENU_TITLE, "Long", "");
    for(i = 1; i <= WIN_MENU_ITEMS; i++) it[i] = mi(i + 1, i % 4 ? 0 : WIN_MENU_SEPARATOR, "item", "F5");
    CHECK(menu_call(fd, it, WIN_MENU_ITEMS + 1) == 0);
    it[WIN_MENU_ITEMS + 1] = mi(99, 0, "one more", "");
    CHECK(menu_call(fd, it, WIN_MENU_ITEMS + 2) == -EINVAL);
    /* the bar: the titles' cells and 3 for each, WIN_MENU_BAR at most (UTF-8 counted as the console draws it) */
    it[0] = mi(1, WIN_MENU_TITLE, "Caf\xC3\xA9 \xE2\x80\x94 twenty-six cells!!!", "");  /* e acute, a dash */
    it[1] = mi(2, 0, "x", "");
    it[2] = mi(3, WIN_MENU_TITLE, "Twelve chars", "");  /* 12: 26 + 3 + 12 + 3 = 44 */
    it[3] = mi(4, 0, "y", "");
    CHECK(strlen(it[0].label) == 29 && menu_call(fd, it, 4) == 0);
    it[4] = mi(5, WIN_MENU_TITLE | WIN_MENU_HELP, "Help", "");  /* a Help menu is not in the bar */
    it[5] = mi(6, 0, "About this program", "");
    CHECK(menu_call(fd, it, 6) == 0);
    it[2] = mi(3, WIN_MENU_TITLE, "Thirteen char", "");     /* 45 */
    CHECK(menu_call(fd, it, 4) == -EINVAL);
    /* a label that fills its array is cut between characters, as a title is (the kernel's copy) */
    memset(kcopy, 0, sizeof(kcopy));
    kcopy[0].flags = WIN_MENU_TITLE;
    memcpy(kcopy[0].label, "T", 1);
    memset(kcopy[1].label, 'a', sizeof(kcopy[1].label));
    memcpy(kcopy[1].label + sizeof(kcopy[1].label) - 2, "\xC3\xA9", 2);   /* an e acute across the end */
    memset(kcopy[1].keys, 'k', sizeof(kcopy[1].keys));
    CHECK(menu_check(kcopy, 2) == 0 && strlen(kcopy[1].label) == sizeof(kcopy[1].label) - 2 &&
          strlen(kcopy[1].keys) == sizeof(kcopy[1].keys) - 1);
    /* memory: the copy that cannot be made, and none left after a refusal (the menus set last are the window's until
       it ends or they are taken away) */
    CHECK(menu_call(fd, it, 0) == 0 && live == alive);
    it[0] = mi(1, WIN_MENU_TITLE, "File", "");
    it[1] = mi(2, 0, "Open", "");
    fail_in = 1;
    CHECK(menu_call(fd, it, 2) == -ENOMEM);
    fail_in = 0;
    CHECK(live == alive);
    CHECK(sys_winctl(fd + 1, WIN_MENU, &m) == -EBADF);
    end();
}

/* 11/05/26 (the review of the ABI): labels and shortcuts drawn as they are, titles that pass for the desktop's */
static void test_menu_text(void) {
    static const char *const bad[] = {
        "a\tb", "a\x7F", "a\x1B[1m",                    /* C0 controls and DEL */
        "a\xC2\x85" "b", "\xC2\x9F",                     /* C1 (U+0085, U+009F) */
        "\xF7 SurfOS", "\xE9t\xE9",                         /* bytes that are no UTF-8: code page 437 ones */
        "\xC0\xAF", "\xE0\x80\xAF", "\xED\xA0\x80", "\xF4\x90\x80\x80",   /* overlong, surrogate, past U+10FFFF */
        "ab\xC3", "\xC3x", "\xE2\x89", "\x80", "\xBF\xBF",   /* sequences cut short, continuations alone */
    };
    static const char *const own[] = {
        "\xE2\x89\x8B SurfOS", "\xE2\x89\x8B", "My \xE2\x89\x88 menu",  /* the desktop's sign: U+224B, U+2248 */
        "SurfOS", "surfos", "Apps", " APPS ", "\xC2\xA0" "Apps\xC2\xA0", "Window", "wINDOW", "Help", " help",
    };
    struct win_menu_item it[4], k[2];
    unsigned i;
    int fd;
    begin();
    fd = open_as(prog, 200, 100);
    CHECK(fd >= 0);
    it[0] = mi(1, WIN_MENU_TITLE, "File", "");
    it[2] = mi(3, WIN_MENU_TITLE, "View", "");
    it[3] = mi(4, 0, "Zoom 100%", "Ctrl+0");
    for(i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        it[1] = mi(2, 0, bad[i], "");                   /* in a label */
        CHECK(menu_call(fd, it, 4) == -EINVAL);
        it[1] = mi(2, 0, "Open", bad[i]);               /* in a shortcut */
        CHECK(menu_call(fd, it, 4) == -EINVAL);
        it[1] = mi(2, 0, "Open", "");
        it[2] = mi(3, WIN_MENU_TITLE, bad[i], "");      /* in a title */
        CHECK(menu_call(fd, it, 4) == -EINVAL);
        it[2] = mi(3, WIN_MENU_TITLE, "View", "");
    }
    it[1] = mi(2, 0, "Caf\xC3\xA9 \xE2\x80\x9Cquoted\xE2\x80\x9D \xF0\x9F\x8C\x8A \xC2\xA0\xE2\x89\x88", "Ctrl+\xC3\xA9");
    CHECK(menu_call(fd, it, 4) == 0);             /* UTF-8, the sign among items too, a 4-byte one drawn '?' */
    for(i = 0; i < sizeof(own) / sizeof(own[0]); i++) {
        it[2] = mi(3, WIN_MENU_TITLE, own[i], "");
        CHECK(menu_call(fd, it, 4) == -EINVAL);
    }
    it[2] = mi(3, WIN_MENU_TITLE, "Applications", "");  /* names that only begin like the desktop's */
    CHECK(menu_call(fd, it, 4) == 0);
    it[2] = mi(3, WIN_MENU_TITLE, "Windows", "");
    CHECK(menu_call(fd, it, 4) == 0);
    it[2] = mi(3, WIN_MENU_TITLE, "Help me", "");
    CHECK(menu_call(fd, it, 4) == 0);
    it[2] = mi(3, WIN_MENU_TITLE | WIN_MENU_HELP, " Help ", "");    /* merged into the desktop's Help: it may say so */
    CHECK(menu_call(fd, it, 4) == 0);
    it[2] = mi(3, WIN_MENU_TITLE | WIN_MENU_HELP, "Apps", "");      /* but no other of the desktop's names */
    CHECK(menu_call(fd, it, 4) == -EINVAL);
    /* (the review of stage 2) no item of the Help menu reads as the desktop's own Help items, which follow it */
    it[2] = mi(3, WIN_MENU_TITLE | WIN_MENU_HELP, "Help", "");
    it[3] = mi(4, 0, "About SurfOS", "");
    CHECK(menu_call(fd, it, 4) == -EINVAL);
    it[3] = mi(4, 0, "\xC2\xA0" "about SURFOS ", "");
    CHECK(menu_call(fd, it, 4) == -EINVAL);
    it[3] = mi(4, 0, "Keyboard and Mouse", "F1");
    CHECK(menu_call(fd, it, 4) == -EINVAL);
    /* 11/09/26 (the second review, N4): blanks inside read as one, and punctuation at the end is left out */
    it[3] = mi(4, 0, "About  SurfOS.", "");
    CHECK(menu_call(fd, it, 4) == -EINVAL);
    it[3] = mi(4, 0, "About\xC2\xA0SurfOS!", "");        /* the no-break space is a blank */
    CHECK(menu_call(fd, it, 4) == -EINVAL);
    it[3] = mi(4, 0, "keyboard   AND mouse...", "");
    CHECK(menu_call(fd, it, 4) == -EINVAL);
    it[3] = mi(4, 0, "AboutSurfOS", "");                 /* no blank between: no name of the desktop's */
    CHECK(menu_call(fd, it, 4) == 0);
    it[3] = mi(4, 0, "About SurfOS 2", "");              /* a digit is no punctuation */
    CHECK(menu_call(fd, it, 4) == 0);
    it[2] = mi(3, WIN_MENU_TITLE, "Window:", "");        /* a title too */
    CHECK(menu_call(fd, it, 4) == -EINVAL);
    it[2] = mi(3, WIN_MENU_TITLE | WIN_MENU_HELP, "Help", "");
    it[3] = mi(4, 0, "About Zoomer", "");                /* its own: good */
    CHECK(menu_call(fd, it, 4) == 0);
    it[2] = mi(3, WIN_MENU_TITLE, "View", "");          /* in a menu of its own the name is no Help item's */
    it[3] = mi(4, 0, "About SurfOS", "");
    CHECK(menu_call(fd, it, 4) == 0);
    /* the kernel's copy is what is checked: a shortcut that fills its array is cut before a sequence it cuts */
    memset(k, 0, sizeof(k));
    k[0].flags = WIN_MENU_TITLE;
    k[0].label[0] = 'T';
    k[1].label[0] = 'b';
    memset(k[1].keys, 'k', sizeof(k[1].keys));
    memcpy(k[1].keys + sizeof(k[1].keys) - 2, "\xE2\x89", 2);
    CHECK(menu_check(k, 2) == 0 && strlen(k[1].keys) == sizeof(k[1].keys) - 2);
    end();
}

/* 11/05/26 (the review): submenus one level deep, and one Help merged into the desktop's */
static void test_menu_shape(void) {
    static struct win_menu_item it[40];
    int fd, i, n;
    begin();
    fd = open_as(prog, 200, 100);
    CHECK(fd >= 0);
#define SHAPE(...) do { const struct win_menu_item e[] = { __VA_ARGS__ }; n = (int)(sizeof(e) / sizeof(e[0])); \
                        memcpy(it, e, sizeof(e)); } while(0)
    SHAPE(mi(1, WIN_MENU_TITLE, "View", ""), mi(2, WIN_MENU_SUBMENU, "Zoom", ""), mi(3, WIN_MENU_CHILD | WIN_MENU_RADIO, "100%", "Ctrl+0"),
          mi(4, WIN_MENU_CHILD | WIN_MENU_SEPARATOR, "", ""), mi(5, WIN_MENU_CHILD, "Fit", ""), mi(6, 0, "Grid", "Ctrl+'"));
    CHECK(menu_call(fd, it, (unsigned)n) == 0);   /* a submenu with three children, an item after */
    CHECK(menu_call(fd, it, 5) == 0);             /* ending in its children */
    CHECK(menu_call(fd, it, 2) == -EINVAL);             /* a submenu with none, at the end */
    it[2] = mi(3, 0, "100%", "");                       /* ... or before the next item */
    CHECK(menu_call(fd, it, (unsigned)n) == -EINVAL);
    SHAPE(mi(1, WIN_MENU_TITLE, "View", ""), mi(2, WIN_MENU_SUBMENU, "Zoom", ""), mi(3, WIN_MENU_TITLE, "Format", ""), mi(4, 0, "Bold", ""));
    CHECK(menu_call(fd, it, (unsigned)n) == -EINVAL);   /* ... or before the next title */
    SHAPE(mi(1, WIN_MENU_TITLE, "View", ""), mi(2, 0, "Zoom", ""), mi(3, WIN_MENU_CHILD, "100%", ""));
    CHECK(menu_call(fd, it, (unsigned)n) == -EINVAL);   /* a child of an item with no submenu */
    SHAPE(mi(1, WIN_MENU_TITLE, "View", ""), mi(3, WIN_MENU_CHILD, "100%", ""));
    CHECK(menu_call(fd, it, (unsigned)n) == -EINVAL);   /* of a title */
    SHAPE(mi(1, WIN_MENU_TITLE, "View", ""), mi(2, WIN_MENU_SUBMENU, "Zoom", ""), mi(3, WIN_MENU_CHILD | WIN_MENU_SUBMENU, "Deeper", ""),
          mi(4, WIN_MENU_CHILD, "x", ""));
    CHECK(menu_call(fd, it, (unsigned)n) == -EINVAL);   /* one level only */
    SHAPE(mi(1, WIN_MENU_TITLE | WIN_MENU_SUBMENU, "View", ""), mi(2, 0, "Zoom", ""));
    CHECK(menu_call(fd, it, (unsigned)n) == -EINVAL);   /* SUBMENU, CHILD and HELP where they are none */
    it[0].flags = WIN_MENU_TITLE | WIN_MENU_CHILD;
    CHECK(menu_call(fd, it, (unsigned)n) == -EINVAL);
    it[0].flags = WIN_MENU_TITLE;
    it[1].flags = WIN_MENU_HELP;
    CHECK(menu_call(fd, it, (unsigned)n) == -EINVAL);
    it[1].flags = WIN_MENU_SUBMENU | WIN_MENU_SEPARATOR;
    CHECK(menu_call(fd, it, (unsigned)n) == -EINVAL);
    SHAPE(mi(1, WIN_MENU_TITLE, "View", ""), mi(2, WIN_MENU_SUBMENU, "Zoom", ""), mi(3, WIN_MENU_CHILD, "", ""));
    CHECK(menu_call(fd, it, (unsigned)n) == -EINVAL);   /* a child item with no label */
    /* a submenu of WIN_MENU_ITEMS children and no more, not counted in its menu's items */
    n = 0;
    it[n++] = mi(1, WIN_MENU_TITLE, "Insert", "");
    for(i = 0; i < WIN_MENU_ITEMS - 1; i++) it[n++] = mi(10 + i, 0, "item", "");
    it[n++] = mi(2, WIN_MENU_SUBMENU, "Symbol", "");
    for(i = 0; i < WIN_MENU_ITEMS; i++) it[n++] = mi(30 + i, WIN_MENU_CHILD, "child", "");
    CHECK(menu_call(fd, it, (unsigned)n) == 0);
    it[n] = mi(99, WIN_MENU_CHILD, "one more", "");
    CHECK(menu_call(fd, it, (unsigned)n + 1) == -EINVAL);
    /* one Help, of WIN_MENU_HELP_ITEMS items at most, anywhere among the menus */
    n = 0;
    it[n++] = mi(1, WIN_MENU_TITLE | WIN_MENU_HELP, "Help", "");
    for(i = 0; i < WIN_MENU_HELP_ITEMS; i++) it[n++] = mi(10 + i, 0, "topic", "");
    it[n++] = mi(2, WIN_MENU_TITLE, "File", "");
    it[n++] = mi(3, 0, "Open", "Ctrl+O");
    CHECK(menu_call(fd, it, (unsigned)n) == 0);
    it[n++] = mi(4, WIN_MENU_TITLE | WIN_MENU_HELP, "Help", "");     /* two */
    it[n++] = mi(5, 0, "More", "");
    CHECK(menu_call(fd, it, (unsigned)n) == -EINVAL);
    n = 0;
    it[n++] = mi(1, WIN_MENU_TITLE | WIN_MENU_HELP, "Help", "");
    for(i = 0; i <= WIN_MENU_HELP_ITEMS; i++) it[n++] = mi(10 + i, 0, "topic", "");
    CHECK(menu_call(fd, it, (unsigned)n) == -EINVAL);   /* one item too many for the desktop's Help */
    it[0] = mi(1, WIN_MENU_TITLE, "Topics", "");        /* in a menu of its own they fit */
    CHECK(menu_call(fd, it, (unsigned)n) == 0);
#undef SHAPE
    end();
}

static void test_paste_take_args(void) {
    struct win_buffer b;
    char buf[64], guard[16];
    int fd;
    begin();
    fd = open_as(prog, 200, 100);
    CHECK(fd >= 0);
    CHECK(sys_winctl(fd, WIN_PASTE, NULL) == -EFAULT && sys_winctl(fd, WIN_TAKE, NULL) == -EFAULT);
    bad_lo = guard;
    bad_hi = guard + sizeof(guard);
    CHECK(sys_winctl(fd, WIN_PASTE, guard) == -EFAULT && sys_winctl(fd, WIN_TAKE, guard) == -EFAULT);
    bad_lo = bad_hi = NULL;
    b.buf = NULL;                                       /* room with no buffer */
    b.size = 10;
    CHECK(sys_winctl(fd, WIN_PASTE, &b) == -EFAULT && sys_winctl(fd, WIN_TAKE, &b) == -EFAULT);
    b.size = 0;                                         /* a query: no buffer needed */
    CHECK(sys_winctl(fd, WIN_PASTE, &b) == -EACCES && sys_winctl(fd, WIN_TAKE, &b) == -ENOENT);   /* good: no focus, */
    b.buf = buf;                                                                                    /* nothing chosen */
    b.size = sizeof(buf);
    CHECK(sys_winctl(fd, WIN_PASTE, &b) == -EACCES && sys_winctl(fd, WIN_TAKE, &b) == -ENOENT);
    end();
}

static void test_dialog_args(void) {
    struct win_dialog d, k;
    char guard[64];
    int fd, i;
    begin();
    fd = open_as(prog, 200, 100);
    CHECK(fd >= 0);
#define FRESH() do { memset(&d, 0, sizeof(d)); d.kind = WIN_DLG_SAVE; d.id = 7; strlcpy(d.dir, "/usb1p1", sizeof(d.dir)); \
                     strlcpy(d.name, "cat.png", sizeof(d.name)); strlcpy(d.types, "png bmp", sizeof(d.types)); } while(0)
    CHECK(sys_winctl(fd, WIN_DIALOG, NULL) == -EFAULT);
    bad_lo = guard;
    bad_hi = guard + sizeof(guard);
    CHECK(sys_winctl(fd, WIN_DIALOG, guard) == -EFAULT);
    bad_lo = bad_hi = NULL;
    FRESH();
    CHECK(sys_winctl(fd, WIN_DIALOG, &d) == -EACCES);   /* good, from a window without the focus */
    for(i = WIN_DLG_OPEN; i <= WIN_DLG_FOLDER; i++) {
        d.kind = (unsigned)i;
        CHECK(sys_winctl(fd, WIN_DIALOG, &d) == -EACCES);
    }
    d.kind = 0;
    CHECK(sys_winctl(fd, WIN_DIALOG, &d) == -EINVAL);
    d.kind = WIN_DLG_FOLDER + 1;
    CHECK(sys_winctl(fd, WIN_DIALOG, &d) == -EINVAL);
    FRESH();
    d.dir[0] = 0;                                       /* the desktop's choice */
    d.name[0] = 0;
    d.types[0] = 0;
    CHECK(sys_winctl(fd, WIN_DIALOG, &d) == -EACCES);
    FRESH();
    strlcpy(d.dir, "usb1p1", sizeof(d.dir));            /* not absolute */
    CHECK(sys_winctl(fd, WIN_DIALOG, &d) == -EINVAL);
    FRESH();
    strlcpy(d.dir, "/usb1p1/a\tb", sizeof(d.dir));      /* a control character */
    CHECK(sys_winctl(fd, WIN_DIALOG, &d) == -EINVAL);
    FRESH();
    memset(d.dir, 'a', sizeof(d.dir));                  /* no NUL */
    d.dir[0] = '/';
    CHECK(sys_winctl(fd, WIN_DIALOG, &d) == -EINVAL);
    FRESH();
    strlcpy(d.name, "pics/cat.png", sizeof(d.name));    /* a name, not a path */
    CHECK(sys_winctl(fd, WIN_DIALOG, &d) == -EINVAL);
    FRESH();
    memset(d.name, 'n', sizeof(d.name));
    CHECK(sys_winctl(fd, WIN_DIALOG, &d) == -EINVAL);
    FRESH();
    d.flags = 1;                                        /* 11/05/26 (the review): room to grow, 0 now */
    CHECK(sys_winctl(fd, WIN_DIALOG, &d) == -EINVAL);
    d.flags = 0x80000000u;
    CHECK(sys_winctl(fd, WIN_DIALOG, &d) == -EINVAL);
    FRESH();
    strlcpy(d.types, "png,bmp", sizeof(d.types));
    CHECK(sys_winctl(fd, WIN_DIALOG, &d) == -EINVAL);
    strlcpy(d.types, ".png", sizeof(d.types));
    CHECK(sys_winctl(fd, WIN_DIALOG, &d) == -EINVAL);
    strlcpy(d.types, "abcdefghijklmnop", sizeof(d.types));      /* 16 letters */
    CHECK(sys_winctl(fd, WIN_DIALOG, &d) == -EINVAL);
    strlcpy(d.types, "abcdefghijklmno", sizeof(d.types));       /* 15 */
    CHECK(sys_winctl(fd, WIN_DIALOG, &d) == -EACCES);
    strlcpy(d.types, "a b c d e f g h i j k l m n o p q", sizeof(d.types));   /* 17 types */
    CHECK(sys_winctl(fd, WIN_DIALOG, &d) == -EINVAL);
    strlcpy(d.types, "  a b  c d e f g H i j k l m n o P  ", sizeof(d.types)); /* 16, spaces anywhere, any case */
    CHECK(sys_winctl(fd, WIN_DIALOG, &d) == -EACCES);
    strlcpy(d.types, "c h =Makefile =README.txt =.profile =a-b_c", sizeof(d.types));   /* whole names */
    CHECK(sys_winctl(fd, WIN_DIALOG, &d) == -EACCES);
    strlcpy(d.types, "=Makefile.in.backup1", sizeof(d.types));  /* 19 characters */
    CHECK(sys_winctl(fd, WIN_DIALOG, &d) == -EINVAL);
    strlcpy(d.types, "png =", sizeof(d.types));                 /* a name that is none */
    CHECK(sys_winctl(fd, WIN_DIALOG, &d) == -EINVAL);
    strlcpy(d.types, "=a/b", sizeof(d.types));
    CHECK(sys_winctl(fd, WIN_DIALOG, &d) == -EINVAL);
    strlcpy(d.types, "=a=b", sizeof(d.types));
    CHECK(sys_winctl(fd, WIN_DIALOG, &d) == -EINVAL);
    strlcpy(d.types, "tar.gz", sizeof(d.types));                /* an extension is letters and digits only */
    CHECK(sys_winctl(fd, WIN_DIALOG, &d) == -EINVAL);
    strlcpy(d.types, "png\tbmp", sizeof(d.types));
    CHECK(sys_winctl(fd, WIN_DIALOG, &d) == -EINVAL);
    memset(d.types, 'p', sizeof(d.types));
    CHECK(sys_winctl(fd, WIN_DIALOG, &d) == -EINVAL);
    memset(d.types, ' ', sizeof(d.types));              /* only spaces, no NUL */
    CHECK(sys_winctl(fd, WIN_DIALOG, &d) == -EINVAL);
    d.types[sizeof(d.types) - 1] = 0;                   /* the NUL in the last byte: an empty list */
    CHECK(sys_winctl(fd, WIN_DIALOG, &d) == -EACCES);
    memset(d.types, 0, sizeof(d.types));                /* 16 of 8 characters: 143 bytes and the NUL */
    memcpy(d.types, "abcdefgh", 8);
    for(i = 1; i < WIN_TYPES; i++) memcpy(d.types + 8 + 9 * (i - 1), " =abcdefg", 9);
    CHECK(strlen(d.types) == sizeof(d.types) - 1 && sys_winctl(fd, WIN_DIALOG, &d) == -EACCES);
    {                                                   /* the shared parser's list, as the desktop gets it */
        char out[WIN_TYPES][WIN_TYPE_LEN + 2];
        CHECK(win_types(" PNG =Makefile bmp ", 20, out) == 3 && !strcmp(out[0], "png") && !strcmp(out[1], "=makefile") &&
              !strcmp(out[2], "bmp"));
        CHECK(win_types("", 1, out) == 0 && win_types("png", 3, out) == -1 && win_types("png", 4, out) == 1);
    }
    FRESH();
    memset(d.title, 'T', sizeof(d.title));              /* a title with no NUL is cut, as a window's */
    CHECK(sys_winctl(fd, WIN_DIALOG, &d) == -EACCES);
    k = d;
    CHECK(dialog_args(&d, &k) == 0 && strlen(k.title) == WIN_TITLE_MAX - 1 && k.id == 7);
    CHECK(sys_winctl(fd + 1, WIN_DIALOG, &d) == -EBADF);
#undef FRESH
    end();
}

/**** 11/05/26 (track desk, stage 2): the calls and their desktop's side ****/

static void test_menus_set(void) {
    struct win_menu_item it[4];
    struct win_state st;
    struct win_view view;
    const struct win_menu_item *lent;
    int fd, h, alive;
    begin();
    fd = open_as(prog, 200, 100);
    h = shown(prog, fd);
    CHECK(fd >= 0 && h >= 0);
    as(desktop);
    CHECK(win_update(h, &st, &view) == 0 && !view.menu && !view.menus && st.menu_serial == 0);
    it[0] = mi(10, WIN_MENU_TITLE, "Game", "");
    it[1] = mi(11, 0, "New", "F2");
    it[2] = mi(12, WIN_MENU_SEPARATOR, "", "");
    it[3] = mi(13, WIN_MENU_CHECKED, "Large", "");
    CHECK(menu_call(fd, it, 4) == 0);
    as(desktop);
    CHECK(win_update(h, &st, &view) == 0 && view.menus == 4 && st.menu_serial == 1);   /* lent, as pixels are */
    CHECK(view.menu[0].id == 10 && !strcmp(view.menu[1].label, "New") && !strcmp(view.menu[1].keys, "F2") &&
          view.menu[3].flags == WIN_MENU_CHECKED);
    lent = view.menu;
    it[3].flags = 0;                                    /* sent again whole: the desktop keeps the copy it has */
    CHECK(menu_call(fd, it, 4) == 0);
    CHECK(lent[3].flags == WIN_MENU_CHECKED && win_of_fd(prog, fd)->menu_view->refs == 1);    /* (its alone) */
    as(desktop);
    alive = live;
    CHECK(win_update(h, &st, &view) == 0 && st.menu_serial == 2 && view.menus == 4 && !view.menu[3].flags &&
          live == alive - 1);                           /* until its next update: the copy before goes then */
    CHECK(menu_call(fd, it, 0) == 0);                   /* none: taken away */
    as(desktop);
    CHECK(win_update(h, &st, &view) == 0 && !view.menu && !view.menus && st.menu_serial == 3 && live == alive - 2);
    CHECK(menu_call(fd, it, 4) == 0);
    as(desktop);
    CHECK(win_update(h, &st, &view) == 0 && view.menus == 4);
    win_close(h);                                       /* the desktop's end of it: both copies go */
    CHECK(!win_of_fd(prog, fd)->menu && !win_of_fd(prog, fd)->menu_view && menu_call(fd, it, 4) == -EPIPE);
    fd = open_as(prog, 200, 100);                       /* the program's end: its copy goes, the desktop's */
    h = shown(prog, fd);                                /* stays until it lets the window go */
    CHECK(menu_call(fd, it, 4) == 0);
    as(desktop);
    CHECK(win_update(h, &st, &view) == 0 && view.menus == 4);
    as(prog);
    fd_close(fd);
    as(desktop);
    CHECK(view.menu[1].id == 11 && win_update(h, &st, &view) == -EPIPE);
    win_close(h);
    end();
}

static void test_paste(void) {
    static char big[WIN_CLIP_MAX + 16], back[WIN_CLIP_MAX + 1];
    struct win_buffer b;
    struct win_clip c;
    char buf[16], guard[16], out[16];
    int fd, h, fd2, h2, n, alive;
    begin();
    fd = open_as(prog, 200, 100);
    h = shown(prog, fd);
    b.buf = buf;
    b.size = sizeof(buf);
    as(prog);
    CHECK(sys_winctl(fd, WIN_PASTE, &b) == -EACCES);    /* no focus: nothing read */
    send_ev(h, WIN_EV_FOCUS, 0, 0, 1);
    as(prog);
    memset(buf, 'x', sizeof(buf));
    CHECK(sys_winctl(fd, WIN_PASTE, &b) == 0 && buf[0] == 0);  /* the desktop gave nothing: empty */
    as(desktop);
    CHECK(win_paste_give("hello", 5) == 0);
    as(prog);
    memset(buf, 'x', sizeof(buf));
    CHECK(sys_winctl(fd, WIN_PASTE, &b) == 5 && !strcmp(buf, "hello"));     /* with its NUL */
    b.size = 0;                                         /* how long, nothing copied */
    b.buf = NULL;
    CHECK(sys_winctl(fd, WIN_PASTE, &b) == 5);
    b.buf = buf;
    b.size = 4;                                         /* too small: nothing copied */
    memset(buf, 'x', sizeof(buf));
    CHECK(sys_winctl(fd, WIN_PASTE, &b) == -ENOSPC && buf[0] == 'x');
    b.size = 5;                                         /* exactly: the bytes, no NUL */
    CHECK(sys_winctl(fd, WIN_PASTE, &b) == 5 && !memcmp(buf, "hello", 5) && buf[5] == 'x');
    bad_lo = guard;                                     /* not the program's to write */
    bad_hi = guard + sizeof(guard);
    b.buf = guard;
    b.size = sizeof(guard);
    CHECK(sys_winctl(fd, WIN_PASTE, &b) == -EFAULT);
    bad_lo = bad_hi = NULL;
    as(desktop);                                        /* text only: to a NUL; at most WIN_CLIP_MAX */
    CHECK(win_paste_give("ab\0cd", 5) == 0);
    as(prog);
    b.buf = buf;
    b.size = sizeof(buf);
    CHECK(sys_winctl(fd, WIN_PASTE, &b) == 2 && !strcmp(buf, "ab"));
    memset(big, 'q', sizeof(big));
    as(desktop);
    CHECK(win_paste_give(big, sizeof(big)) == 0);
    as(prog);
    b.buf = back;
    b.size = sizeof(back);
    n = sys_winctl(fd, WIN_PASTE, &b);
    CHECK(n == (int)WIN_CLIP_MAX && back[0] == 'q' && back[WIN_CLIP_MAX - 1] == 'q' && back[WIN_CLIP_MAX] == 0);
    b.size = WIN_CLIP_MAX;                              /* a buffer of WIN_CLIP_MAX always takes it */
    CHECK(sys_winctl(fd, WIN_PASTE, &b) == (int)WIN_CLIP_MAX);
    as(desktop);
    alive = live;
    fail_in = 1;                                        /* no memory for the new copy: the one before stays */
    CHECK(win_paste_give("new", 3) == -ENOMEM && live == alive);
    CHECK(win_paste_give("", 0) == 0 && live == alive - 1);   /* empty: no copy at all */
    as(prog);
    b.buf = buf;
    b.size = sizeof(buf);
    CHECK(sys_winctl(fd, WIN_PASTE, &b) == 0);
    as(other);
    CHECK(win_paste_give("x", 1) == -ENODEV);           /* a task without a desktop */
    /* a copy held by a paste under way outlives the copy that replaces it (struct paste's count) */
    as(desktop);
    CHECK(win_paste_give("first", 5) == 0);
    {
        struct paste *held = desks[0].paste;
        held->refs++;                                   /* as paste() holds it while it copies */
        CHECK(win_paste_give("second", 6) == 0 && live == alive + 1);  /* both alive */
        CHECK(desks[0].paste != held && !memcmp(desks[0].paste->text, "second", 6));
        kfree(paste_put(held));                         /* the paste's end frees it */
        CHECK(live == alive);
    }
    fd2 = open_as(prog, 40, 30);                        /* another window, not focused: refused */
    as(prog);
    CHECK(fd2 >= 0 && sys_winctl(fd2, WIN_PASTE, &b) == -EACCES);
    /* 11/05/26 (the review): the window's own Copy that the desktop has not taken yet is newer than the copy it gave:
       a paste gets that; the desktop's taking it makes it the copy at once; another window's is not the clipboard's
       (that window lost the focus: the desktop drops it) */
    as(desktop);
    CHECK(win_paste_give("given", 5) == 0);
    c.text = "mine";
    c.len = 4;
    as(prog);
    CHECK(sys_winctl(fd, WIN_CLIP, &c) == 0);
    CHECK(sys_winctl(fd, WIN_PASTE, &b) == 4 && !strcmp(buf, "mine"));
    as(desktop);
    CHECK(win_clip_take(h, out, sizeof(out)) == 4 && !memcmp(out, "mine", 4));
    as(prog);
    CHECK(sys_winctl(fd, WIN_PASTE, &b) == 4 && !strcmp(buf, "mine"));     /* before the desktop gives it again */
    as(desktop);
    CHECK(win_paste_give("mine", 4) == 0);
    h2 = shown(prog, fd2);
    as(prog);
    CHECK(sys_winctl(fd, WIN_CLIP, &c) == 0);           /* copied, and the focus goes to the other window */
    send_ev(h, WIN_EV_FOCUS, 0, 0, 0);
    send_ev(h2, WIN_EV_FOCUS, 0, 0, 1);
    as(desktop);
    CHECK(win_paste_give("theirs", 6) == 0);
    as(prog);
    CHECK(sys_winctl(fd2, WIN_PASTE, &b) == 6 && !strcmp(buf, "theirs"));
    as(desktop);
    CHECK(win_clip_take(h2, out, sizeof(out)) == 0);    /* dropped */
    as(prog);
    CHECK(sys_winctl(fd2, WIN_PASTE, &b) == 6 && !strcmp(buf, "theirs"));
    CHECK(sys_winctl(fd, WIN_PASTE, &b) == -EACCES);    /* the focus gone */
    as(desktop);
    win_close(h);
    win_close(h2);
    as(prog);
    CHECK(sys_winctl(fd, WIN_PASTE, &b) == -EPIPE);
    end();
}

static int ask(int fd, unsigned kind, unsigned id, const char *dir) {
    struct win_dialog d;
    memset(&d, 0, sizeof(d));
    d.kind = kind;
    d.id = id;
    strlcpy(d.dir, dir, sizeof(d.dir));
    strlcpy(d.types, "png", sizeof(d.types));
    strlcpy(d.title, "Open a picture", sizeof(d.title));
    as(prog);
    return sys_winctl(fd, WIN_DIALOG, &d);
}

static int take_path(int fd, char *buf, unsigned size) {
    struct win_buffer b;
    b.buf = buf;
    b.size = size;
    as(prog);
    return sys_winctl(fd, WIN_TAKE, &b);
}

static void test_dialogs(void) {
    struct win_dialog got;
    struct win_state st;
    struct win_view view;
    struct win_event ev[8];
    char path[WIN_PATH_MAX], small[8], guard[32], longest[WIN_PATH_MAX + 1];
    u32 no, no2;
    int fd, h, n, alive;
    begin();
    fd = open_as(prog, 200, 100);
    h = shown(prog, fd);
    send_ev(h, WIN_EV_KEY, 0, 0, 'o');
    CHECK(ask(fd, WIN_DLG_OPEN, 5, "/usb1p1") == -EACCES);     /* no focus: no chooser over another's keys */
    send_ev(h, WIN_EV_FOCUS, 0, 0, 1);
    readev(prog, fd, ev, sizeof(ev));                   /* (the events so far) */
    CHECK(ask(fd, WIN_DLG_OPEN, 5, "/usb1p1") == 0);    /* the key came since: it may ask */
    CHECK(ask(fd, WIN_DLG_SAVE, 6, "/usb1p1") == -EBUSY);      /* one at a time */
    as(desktop);
    CHECK(win_update(h, &st, &view) == 0 && st.dialog == 1);
    CHECK(win_dialog_take(h + WIN_MAX, &got, &no) == -EINVAL);
    memset(&got, 0, sizeof(got));
    CHECK(win_dialog_take(h, &got, &no) == 0 && no && got.kind == WIN_DLG_OPEN && got.id == 5 &&
          !strcmp(got.dir, "/usb1p1") && !strcmp(got.types, "png") && !strcmp(got.title, "Open a picture"));
    CHECK(win_update(h, &st, &view) == 0 && st.dialog == 0 && win_dialog_take(h, &got, &no2) == -ENOENT);
    CHECK(ask(fd, WIN_DLG_OPEN, 7, "") == -EBUSY);      /* taken, not answered: still one */
    CHECK(take_path(fd, path, sizeof(path)) == -ENOENT);
    as(desktop);
    CHECK(win_dialog_answer(h, no, "relative/x.png") == -EINVAL);  /* a path that is none */
    memset(longest, 'a', sizeof(longest));
    longest[0] = '/';
    longest[WIN_PATH_MAX] = 0;
    CHECK(win_dialog_answer(h, no, longest) == -EINVAL);
    CHECK(win_dialog_answer(h, no + 1, "/usb1p1/sea.png") == -EINVAL);    /* by its number only */
    CHECK(win_dialog_answer(h, no, "/usb1p1/sea.png") == 0);
    n = readev(prog, fd, ev, sizeof(ev));
    CHECK(n == (int)sizeof(ev[0]) && ev[0].type == WIN_EV_DIALOG && ev[0].value == 1 && ev[0].x == 5);
    as(desktop);
    CHECK(win_dialog_answer(h, no, "/usb1p1/sea.png") == -EINVAL);    /* answered already */
    CHECK(take_path(fd, small, sizeof(small)) == -ENOSPC);     /* too small: it stays */
    bad_lo = guard;
    bad_hi = guard + sizeof(guard);
    CHECK(take_path(fd, guard, sizeof(guard)) == -EFAULT);     /* not the program's to write: it stays */
    bad_lo = bad_hi = NULL;
    CHECK(take_path(fd, path, 16) == 15 && !strcmp(path, "/usb1p1/sea.png"));     /* the length and its NUL */
    CHECK(take_path(fd, path, sizeof(path)) == -ENOENT);       /* taken */
    /* the next request needs something the user did in the window since this one (11/05/26, the review): a program
       that asks again at each answer cannot hold the desktop's keys */
    CHECK(ask(fd, WIN_DLG_FOLDER, 8, "/") == -EACCES);
    send_ev(h, WIN_EV_POINTER, 10, 10, 0);              /* a move is nothing, nor the wheel */
    send_ev(h, WIN_EV_WHEEL, 10, 10, 1);
    send_ev(h, WIN_EV_FOCUS, 0, 0, 1);
    CHECK(ask(fd, WIN_DLG_FOLDER, 8, "/") == -EACCES);
    send_ev(h, WIN_EV_POINTER, 10, 10, WIN_BUTTON_LEFT);    /* a press is */
    readev(prog, fd, ev, sizeof(ev));
    CHECK(ask(fd, WIN_DLG_FOLDER, 8, "/") == 0);
    /* cancelled: no path */
    as(desktop);
    CHECK(win_dialog_take(h, &got, &no2) == 0 && no2 != no && win_dialog_answer(h, no2, NULL) == 0);
    n = readev(prog, fd, ev, sizeof(ev));
    CHECK(n == (int)sizeof(ev[0]) && ev[0].type == WIN_EV_DIALOG && ev[0].value == 0 && ev[0].x == 8);
    CHECK(take_path(fd, path, sizeof(path)) == -ENOENT && win_of_fd(prog, fd)->ask == NULL);
    /* (the review of stage 2) a path not taken keeps the next WIN_DIALOG at -EBUSY until it is taken; a menu's item
       chosen is the user's doing too */
    send_ev(h, WIN_EV_MENU, 1, 0, 2);
    readev(prog, fd, ev, sizeof(ev));
    CHECK(ask(fd, WIN_DLG_SAVE, 9, "/") == 0);
    as(desktop);
    CHECK(win_dialog_take(h, &got, &no) == 0 && win_dialog_answer(h, no, "/a.png") == 0);
    readev(prog, fd, ev, sizeof(ev));
    send_ev(h, WIN_EV_KEY, 0, 0, 's');
    readev(prog, fd, ev, sizeof(ev));
    CHECK(ask(fd, WIN_DLG_SAVE, 10, "/") == -EBUSY && win_of_fd(prog, fd)->input);      /* the input kept */
    CHECK(take_path(fd, path, sizeof(path)) == 6 && !strcmp(path, "/a.png"));
    alive = live;
    CHECK(ask(fd, WIN_DLG_SAVE, 10, "/") == 0 && live == alive + 1);   /* the request's copy */
    CHECK(take_path(fd, path, sizeof(path)) == -ENOENT);
    /* no memory for the path: answered as cancelled */
    as(desktop);
    CHECK(win_dialog_take(h, &got, &no) == 0);
    fail_in = 1;
    CHECK(win_dialog_answer(h, no, "/b.png") == -ENOMEM);
    n = readev(prog, fd, ev, sizeof(ev));
    CHECK(n == (int)sizeof(ev[0]) && ev[0].type == WIN_EV_DIALOG && ev[0].value == 0 && ev[0].x == 10);
    send_ev(h, WIN_EV_KEY, 0, 0, 'o');
    readev(prog, fd, ev, sizeof(ev));
    fail_in = 1;                                        /* nor for the request's copy: what the user did stays */
    CHECK(ask(fd, WIN_DLG_OPEN, 11, "/") == -ENOMEM && !win_of_fd(prog, fd)->asking && win_of_fd(prog, fd)->input);
    /* the desktop lets the window go: a request that waits is answered (cancelled), in its place */
    CHECK(ask(fd, WIN_DLG_OPEN, 12, "/") == 0);
    as(desktop);
    win_close(h);
    n = readev(prog, fd, ev, sizeof(ev));
    CHECK(n == (int)sizeof(ev[0]) && ev[0].type == WIN_EV_DIALOG && ev[0].value == 0 && ev[0].x == 12);
    CHECK(ask(fd, WIN_DLG_OPEN, 13, "/") == -EPIPE && take_path(fd, path, sizeof(path)) == -ENOENT);
    CHECK(win_dialog_answer(h, no, "/c.png") == -EINVAL);   /* let go: nobody to answer */
    /* a path chosen just before the hang-up is not lost: WIN_TAKE works after it, as WIN_INFO does */
    fd = open_as(prog, 200, 100);
    h = shown(prog, fd);
    send_ev(h, WIN_EV_FOCUS, 0, 0, 1);
    send_ev(h, WIN_EV_KEY, 0, 0, 's');
    CHECK(ask(fd, WIN_DLG_SAVE, 14, "/") == 0);
    as(desktop);
    CHECK(win_dialog_take(h, &got, &no) == 0 && win_dialog_answer(h, no, "/e.png") == 0);
    win_close(h);
    CHECK(take_path(fd, path, sizeof(path)) == 6 && !strcmp(path, "/e.png"));
    /* the program's end: a request and a path go with it */
    fd = open_as(prog, 200, 100);
    h = shown(prog, fd);
    send_ev(h, WIN_EV_FOCUS, 0, 0, 1);
    send_ev(h, WIN_EV_KEY, 0, 0, 'o');
    CHECK(ask(fd, WIN_DLG_OPEN, 15, "/") == 0);
    as(desktop);
    CHECK(win_dialog_take(h, &got, &no) == 0 && win_dialog_answer(h, no, "/f.png") == 0);
    send_ev(h, WIN_EV_KEY, 0, 0, 'o');
    CHECK(ask(fd, WIN_DLG_OPEN, 16, "/") == -EBUSY);    /* the path not taken: it goes with the window */
    as(prog);
    fd_close(fd);
    as(desktop);
    CHECK(win_dialog_take(h, &got, &no) == -ENOENT && win_dialog_answer(h, no, "/d.png") == -EINVAL);
    win_close(h);
    end();
}

/**** 11/05/26 (track desk): the review of stage 2's probes (K1-K5), and the close button's rule ****/

/* K1: a press is the user's doing; an event that says the pointer left (x, y -1) with a button held from a click
   elsewhere (the desktop's chooser over the window) is none, nor a button held on */
static void test_dialog_press(void) {
    struct win_dialog got;
    struct win_event ev[8];
    u32 no;
    int fd, h;
    begin();
    fd = open_as(prog, 200, 100);
    h = shown(prog, fd);
    send_ev(h, WIN_EV_FOCUS, 0, 0, 1);
    send_ev(h, WIN_EV_KEY, 0, 0, 'o');                  /* the user's Ctrl+O */
    readev(prog, fd, ev, sizeof(ev));
    CHECK(ask(fd, WIN_DLG_OPEN, 1, "/") == 0);
    as(desktop);
    CHECK(win_dialog_take(h, &got, &no) == 0 && win_dialog_answer(h, no, NULL) == 0);    /* Cancel clicked */
    readev(prog, fd, ev, sizeof(ev));
    CHECK(ask(fd, WIN_DLG_OPEN, 2, "/") == -EACCES);
    send_ev(h, WIN_EV_POINTER, -1, -1, WIN_BUTTON_LEFT);    /* the button still down from the Cancel click */
    CHECK(ask(fd, WIN_DLG_OPEN, 3, "/") == -EACCES);
    send_ev(h, WIN_EV_POINTER, 5, 5, WIN_BUTTON_LEFT);  /* held on, now over it: no new button */
    CHECK(ask(fd, WIN_DLG_OPEN, 3, "/") == -EACCES);
    send_ev(h, WIN_EV_POINTER, 5, 5, 0);
    send_ev(h, WIN_EV_POINTER, 5, 5, WIN_BUTTON_RIGHT); /* a press: a button the event before did not have */
    readev(prog, fd, ev, sizeof(ev));
    CHECK(ask(fd, WIN_DLG_OPEN, 4, "/") == 0);
    as(desktop);
    win_close(h);
    end();
}

/* K2: WIN_TAKE gives the path of the answer read last: a second request waits until the first's path is taken; a
   cancelled answer leaves none and keeps nothing waiting */
static void test_take_bound(void) {
    struct win_dialog got;
    struct win_event ev[8];
    char path[WIN_PATH_MAX];
    u32 no;
    int fd, h, n;
    begin();
    fd = open_as(prog, 200, 100);
    h = shown(prog, fd);
    send_ev(h, WIN_EV_FOCUS, 0, 0, 1);
    send_ev(h, WIN_EV_KEY, 0, 0, 'o');
    readev(prog, fd, ev, sizeof(ev));
    CHECK(ask(fd, WIN_DLG_OPEN, 1, "/") == 0);          /* request 1: Open */
    as(desktop);
    CHECK(win_dialog_take(h, &got, &no) == 0 && win_dialog_answer(h, no, "/usb1p1/open-me.txt") == 0);
    send_ev(h, WIN_EV_KEY, 0, 0, 's');                  /* the user's Ctrl+S, after the answer */
    CHECK(ask(fd, WIN_DLG_SAVE, 2, "/") == -EBUSY);     /* asked before its events are read: not yet */
    n = readev(prog, fd, ev, sizeof(ev));
    CHECK(n == 2 * (int)sizeof(ev[0]) && ev[0].type == WIN_EV_DIALOG && ev[0].x == 1 && ev[0].value == 1);
    CHECK(take_path(fd, path, sizeof(path)) == 19 && !strcmp(path, "/usb1p1/open-me.txt"));    /* request 1's */
    CHECK(ask(fd, WIN_DLG_SAVE, 2, "/") == 0);          /* the Ctrl+S still counts */
    as(desktop);
    CHECK(win_dialog_take(h, &got, &no) == 0 && win_dialog_answer(h, no, NULL) == 0);    /* cancelled */
    send_ev(h, WIN_EV_KEY, 0, 0, 's');
    CHECK(ask(fd, WIN_DLG_SAVE, 3, "/") == 0);          /* no path: nothing keeps it waiting */
    as(desktop);
    CHECK(win_dialog_take(h, &got, &no) == 0 && win_dialog_answer(h, no, "/usb1p1/save-here.txt") == 0);
    readev(prog, fd, ev, sizeof(ev));
    CHECK(take_path(fd, path, sizeof(path)) == 21 && !strcmp(path, "/usb1p1/save-here.txt"));
    as(desktop);
    win_close(h);
    end();
}

/* K3: theme changes one after another: one WIN_EV_THEME waits at most, the newest */
static void test_theme_merged(void) {
    static const u32 c[WIN_COLORS];
    struct win_theme t;
    struct win_event ev[4];
    int fd, h, i, n;
    begin();
    fd = open_as(prog, 200, 100);
    h = shown(prog, fd);
    as(prog);
    CHECK(sys_winctl(fd, WIN_THEME, &t) == 0 && !win_of_fd(prog, fd)->ev_count);
    send_ev(h, WIN_EV_KEY, 0, 0, 'k');
    as(desktop);
    for(i = 0; i < 100; i++) win_theme_set((u32)(i & 1), c);
    send_ev(h, WIN_EV_KEY, 0, 0, 'l');
    CHECK(win_of_fd(prog, fd)->ev_count == 3 && win_of_fd(prog, fd)->lost == 0);
    n = readev(prog, fd, ev, sizeof(ev));
    CHECK(n == 3 * (int)sizeof(ev[0]) && ev[0].value == 'k' && ev[1].type == WIN_EV_THEME && ev[1].value == 1 &&
          ev[2].value == 'l');
    as(desktop);
    win_close(h);
    end();
}

/* K4: WIN_TAKE's refusals in their order: the buffer first, then whether a path waits, then its room */
static void test_take_order(void) {
    char guard[16], small[4];
    struct win_buffer b;
    int fd;
    begin();
    fd = open_as(prog, 200, 100);
    bad_lo = guard;
    bad_hi = guard + sizeof(guard);
    b.buf = guard;
    b.size = sizeof(guard);
    as(prog);
    CHECK(sys_winctl(fd, WIN_TAKE, &b) == -EFAULT);     /* no path waiting, and a buffer not its own: -EFAULT */
    bad_lo = bad_hi = NULL;
    CHECK(sys_winctl(fd, WIN_TAKE, &b) == -ENOENT);
    b.buf = small;
    b.size = sizeof(small);
    win_of_fd(prog, fd)->answer = (char *)kalloc(8);    /* a path waiting, longer than the room */
    strlcpy(win_of_fd(prog, fd)->answer, "/a.txt", 8);
    CHECK(sys_winctl(fd, WIN_TAKE, &b) == -ENOSPC && win_of_fd(prog, fd)->answer);
    end();                                              /* (the path goes with the window) */
}

/* K5: the answer's kept place on the hang-up paths: 63 events queued, a request waiting, then the desktop's close of
   the window, or its death: the cancel still comes, last */
static void test_hangup_full(int death) {
    struct win_event ev[WIN_EVENTS];
    int fd, h, i, n;
    begin();
    fd = open_as(prog, 200, 100);
    h = shown(prog, fd);
    send_ev(h, WIN_EV_FOCUS, 0, 0, 1);
    for(i = 0; i < WIN_EVENTS - 2; i++) send_ev(h, WIN_EV_KEY, 0, 0, 'a');     /* 63 with the focus */
    CHECK(ask(fd, WIN_DLG_OPEN, 9, "/") == 0);
    for(i = 0; i < 5; i++) send_ev(h, WIN_EV_KEY, 0, 0, 'b');                  /* lost: the last place is kept */
    CHECK(win_of_fd(prog, fd)->lost == 5);
    as(desktop);
    if(death) reap(desktop);
    else win_close(h);
    n = readev(prog, fd, ev, sizeof(ev));
    CHECK(n == (int)sizeof(ev) && ev[WIN_EVENTS - 1].type == WIN_EV_DIALOG && ev[WIN_EVENTS - 1].x == 9 &&
          ev[WIN_EVENTS - 1].value == 0 && ev[WIN_EVENTS - 2].value == 'a');
    CHECK(readev(prog, fd, ev, sizeof(ev)) == 0);       /* then the end */
    end();
}

/* the close button's rule (shell/guiclient.c): the desktop sees whether a WIN_EV_CLOSE waits unread */
static void test_close_waiting(void) {
    struct win_state st;
    struct win_view view;
    struct win_event ev[4];
    int fd, h;
    begin();
    fd = open_as(prog, 200, 100);
    h = shown(prog, fd);
    as(desktop);
    CHECK(win_update(h, &st, &view) == 0 && !st.close_waiting);
    send_ev(h, WIN_EV_CLOSE, 0, 0, 0);
    send_ev(h, WIN_EV_KEY, 0, 0, 'x');
    as(desktop);
    CHECK(win_update(h, &st, &view) == 0 && st.close_waiting);
    readev(prog, fd, ev, sizeof(ev[0]));                /* the CLOSE read, the key not yet */
    as(desktop);
    CHECK(win_update(h, &st, &view) == 0 && !st.close_waiting);
    win_close(h);
    end();
}

/* K6 (the second review, N1): a stuck program's full queue (typed at) refuses the CLOSE; win_update() says one waits
   all the same, until the program reads again, and the next CLOSE has a place then */
static void test_close_lost(void) {
    struct win_state st;
    struct win_view view;
    struct win_event ev[4];
    int fd, h, i, r;
    begin();
    fd = open_as(prog, 200, 100);
    h = shown(prog, fd);
    for(i = 0; i < WIN_EVENTS; i++) send_ev(h, WIN_EV_KEY, 0, 0, 'k');       /* a stuck program, typed at */
    r = send_ev(h, WIN_EV_CLOSE, 0, 0, 0);
    as(desktop);
    CHECK(r == -EAGAIN && win_of_fd(prog, fd)->lost == 1);
    CHECK(win_update(h, &st, &view) == 0 && st.close_waiting);
    CHECK(win_update(h, &st, &view) == 0 && st.close_waiting);         /* until it reads */
    readev(prog, fd, ev, sizeof(ev[0]));                /* it reads again: one key */
    as(desktop);
    CHECK(win_update(h, &st, &view) == 0 && !st.close_waiting);
    CHECK(send_ev(h, WIN_EV_CLOSE, 0, 0, 0) >= 0);      /* the next press: queued, and waits unread */
    as(desktop);
    CHECK(win_update(h, &st, &view) == 0 && st.close_waiting && win_of_fd(prog, fd)->lost == 1);
    win_close(h);
    end();
}

/* 11/05/26 (the review): an answer always comes: its place in the queue is kept while the request waits, and a request
   with no place for it is refused until the events are read */
static void test_dialog_queue(void) {
    struct win_dialog got;
    struct win_event ev[WIN_EVENTS];
    u32 no;
    int fd, h, i, n;
    begin();
    fd = open_as(prog, 200, 100);
    h = shown(prog, fd);
    send_ev(h, WIN_EV_FOCUS, 0, 0, 1);
    for(i = 0; i < WIN_EVENTS; i++) send_ev(h, WIN_EV_KEY, 0, 0, 'a');     /* full: the last one lost */
    CHECK(win_of_fd(prog, fd)->ev_count == WIN_EVENTS && win_of_fd(prog, fd)->lost == 1);
    CHECK(ask(fd, WIN_DLG_OPEN, 1, "/") == -EAGAIN);    /* no place for its answer */
    CHECK(readev(prog, fd, ev, 2 * sizeof(ev[0])) == 2 * (int)sizeof(ev[0]));
    CHECK(ask(fd, WIN_DLG_OPEN, 1, "/") == 0);
    CHECK(send_ev(h, WIN_EV_KEY, 0, 0, 'b') == 0);     /* one more fits; then the last place is the answer's */
    CHECK(send_ev(h, WIN_EV_KEY, 0, 0, 'c') == -EAGAIN && send_ev(h, WIN_EV_THEME, 0, 0, 1) == -EAGAIN);
    CHECK(win_of_fd(prog, fd)->ev_count == WIN_EVENTS - 1);
    as(desktop);
    CHECK(win_dialog_take(h, &got, &no) == 0 && win_dialog_answer(h, no, NULL) == 0);
    CHECK(win_of_fd(prog, fd)->ev_count == WIN_EVENTS);
    n = readev(prog, fd, ev, sizeof(ev));
    CHECK(n == (int)sizeof(ev) && ev[WIN_EVENTS - 2].value == 'b' && ev[WIN_EVENTS - 1].type == WIN_EV_DIALOG &&
          ev[WIN_EVENTS - 1].x == 1);
    CHECK(send_ev(h, WIN_EV_KEY, 0, 0, 'd') == 0);     /* answered: every place is the events' again */
    as(desktop);
    win_close(h);
    end();
}

static void test_themes(void) {
    static const u32 dusk[WIN_COLORS] = { 0x142638, 0xF4F1E6, 0x153D49, 0x6C888C, 0x197D83, 0xF4F1E6, 0xE7E2D2, 0xDEE3D8,
                                          0xC9CFC4, 0xBFE3DD, 0xFFFDF5, 0x153D49, 0x102E3B, 0xB8E9D9, 0xEE906D, 0xA3B4B3 };
    struct win_theme t;
    struct win_event ev[8];
    int fd, fd2, h, h2, n;
    begin();
    fd = open_as(prog, 200, 100);                       /* one that asks WIN_THEME, one that never does */
    fd2 = open_as(prog, 100, 50);
    h = shown(prog, fd);
    h2 = shown(prog, fd2);
    as(prog);
    CHECK(sys_winctl(fd, WIN_THEME, &t) == 0 && t.theme == WIN_THEME_OCEAN);
    readev(prog, fd, ev, sizeof(ev));
    readev(prog, fd2, ev, sizeof(ev));
    as(desktop);
    CHECK(win_theme_set(2, dusk) == -EINVAL);
    CHECK(win_theme_set(WIN_THEME_DUSK, dusk) == 0);
    n = readev(prog, fd, ev, sizeof(ev));
    CHECK(n == (int)sizeof(ev[0]) && ev[0].type == WIN_EV_THEME && ev[0].value == WIN_THEME_DUSK);
    CHECK(pollw(prog, fd2) == POLLOUT);                 /* nothing for the one that never asked */
    as(prog);
    CHECK(sys_winctl(fd2, WIN_THEME, &t) == 0 && t.theme == WIN_THEME_DUSK && t.colors[WIN_COLOR_DESKTOP] == 0x142638);
    fd_close(fd2);                                      /* a window opened now has the desktop's */
    as(desktop);
    win_close(h2);
    fd2 = open_as(prog, 100, 50);
    as(prog);
    CHECK(sys_winctl(fd2, WIN_THEME, &t) == 0 && t.theme == WIN_THEME_DUSK);
    as(desktop);
    win_close(h);                                       /* let go: the last it had */
    as(prog);
    CHECK(sys_winctl(fd, WIN_THEME, &t) == 0 && t.theme == WIN_THEME_DUSK && t.colors[WIN_COLOR_DESKTOP] == 0x142638);
    as(other);
    CHECK(win_theme_set(WIN_THEME_OCEAN, dusk) == -ENODEV);
    end();
}

int main(void) {
    test_queue();
    test_titles();
    test_open();
    test_dead_desktop_console();
    test_sizes();
    test_budgets();
    test_no_memory();
    test_present();
    test_events();
    test_accept();
    test_prog_first();
    test_desktop_first();
    test_desktop_end();
    test_owner_death();
    test_outlives_resize();
    test_title_calls();
    test_foreground();
    test_clip_alert();
    test_theme();                                       /* 11/05/26 (track desk, stage 1) */
    test_menu_args();
    test_menu_text();                                   /* 11/05/26 (the review of the ABI) */
    test_menu_shape();
    test_paste_take_args();
    test_dialog_args();
    test_menus_set();                                   /* 11/05/26 (track desk, stage 2) */
    test_paste();
    test_dialogs();
    test_dialog_queue();
    test_dialog_press();                                /* 11/05/26 (the review of stage 2) */
    test_take_bound();
    test_theme_merged();
    test_take_order();
    test_hangup_full(0);
    test_hangup_full(1);
    test_close_waiting();
    test_close_lost();                                  /* 11/09/26 (the second review) */
    test_themes();
    printf("test_win: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
