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
   10/25/26 (stage C6): whether a window's program is in the foreground job of a pty its desktop adopted. */
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
    printf("test_win: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
