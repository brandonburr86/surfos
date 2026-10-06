/*
SurfOS pseudo-terminals
--------------------
File: pty.c     Date: 10/6/26
--------------------
SYS_openpty, SYS_spawnpty and SYS_ptyctl (surfos/pty.h). There are PTY_MAX of them. Pty n
is ttys[NUM_CONSOLES + n] (kernel/tty.c), and its console, conArray[NUM_CONSOLES + n], is a
stream console (kernel/console.c): what its programs print is not drawn but handed to
pty_out(), which keeps it here, in an output buffer of PTY_OUT_SIZE bytes, for the master
side to read. What the master side writes arrives as typed keys, through the serial line's
rules (tty_term_input()).

The master side is a vnode of a file system of its own, ptyfs, which has no paths:
SYS_openpty makes an open file of it (vfs_open_vnode()), so a master is an ordinary
descriptor (read, write, poll, dup2, spawnfd, close). Its vnode lives as long as a
descriptor refers to it; its release is the hangup.

A pty's life: free; opened (its tty and console as new, 80x24); a session: SYS_spawnpty
starts a program on it, and every task whose terminal it is belongs to the session (what
those programs start, too); the session ends when no task has the pty as its terminal
any more (the master reads what is left, then 0); the master is closed: the hangup, every
task still on the pty gets the kill that waits for the end of its call, however long the
call takes (TI_HANGUP, surfos/task.h: a client going away never kills a kernel command
holding a lock), and what they print goes nowhere; free again once the master is closed and
no task is left on it. Only processes are ever on a pty: a kernel thread started from a
session is on F1 (task_alloc()).

Output: a writer that may wait (console.c says when) waits while the buffer is full, and
gives up when its call is cut short (a kill, the hangup, Ctrl+C); the bytes of one that
may not are dropped then, and counted. The last PTY_OUT_KEEP bytes of room are kept for
the pager's question and the console's own sequences (PTY_OUT_URGENT). Input: a write on
the master is never refused for want of room: when the tty's input queue (TTY_INPUT_SIZE
keys) is full, the write waits up to PTY_IN_WAIT_MS in all for the session to read, and
what still does not fit is dropped, as the keyboard and the serial port drop it (the tty
counts it).
*/

#include <surfos/types.h>
#include <surfos/task.h>
#include <surfos/tty.h>
#include <surfos/console.h>
#include <surfos/wait.h>
#include <surfos/irq.h>
#include <surfos/process.h>
#include <surfos/syscall.h>
#include <surfos/pty.h>
#include <surfos/poll.h>
#include <surfos/sysimpl.h>
#include <fs/vfs.h>
#include <blibc_common.h>

#define PTY_OUT_SIZE   8192             /* what the session printed and the master has not read */
#define PTY_OUT_KEEP   256              /* the last of it, for PTY_OUT_URGENT */
#define PTY_IN_WAIT_MS 250              /* a write on the master waits this long in all for the session to read */
#define PTY_COLS_MIN   8                /* PTY_SETSIZE's limits */
#define PTY_COLS_MAX   1024
#define PTY_ROWS_MIN   2
#define PTY_ROWS_MAX   1024

struct pty {
    u_char used;                /* opened, and not free again yet */
    u_char master;              /* the master side is open */
    u_char started;             /* a session began (SYS_spawnpty) */
    u_char ended;               /* ... and no task has the pty as its terminal any more */
    u_char in_esc;              /* tty_term_input()'s state for what the master writes */
    char out[PTY_OUT_SIZE];     /* what the session printed, for the master to read: a ring */
    u_int out_first, out_len;
    u_long lost;                /* output dropped: no room where the writer could not wait, or no master */
    wait_queue_t readers;       /* the master's readers, waiting for output or the end */
    wait_queue_t room;          /* writers waiting for room in out */
};

static struct pty ptys[PTY_MAX];        /* the wait queues start empty: all zero */

/* the masters' file system: no paths, nothing to sync, never mounted */
static const struct fs_type ptyfs_type = { "ptyfs", NULL, NULL, NULL };
static struct superblock ptyfs = { &ptyfs_type, NULL, NULL, NULL, NULL, false, MUTEX_INIT("ptyfs"), 0 };

static const struct vnode_ops master_ops;

static struct tty *pty_tty(struct pty *p) {
    return &ttys[NUM_CONSOLES + (p - ptys)];
}

static surf_console *pty_con(struct pty *p) {
    return &conArray[NUM_CONSOLES + (p - ptys)];
}

/* free again: nobody has it, nobody is on it. Interrupts off. */
static void pty_free(struct pty *p) {
    p->used = 0;
    p->master = 0;
    p->started = p->ended = 0;
    p->out_first = p->out_len = 0;
}

/**** the output: the stream console's bytes, for the master ****/

/* console.h: n bytes of stream console con for its master, all or none; 1, or 0 when dropped */
int pty_out(surf_console *con, const char *s, u_int n, int how) {
    int i = (int)(con - conArray) - NUM_CONSOLES;
    u_int limit = (how & PTY_OUT_URGENT) ? PTY_OUT_SIZE : PTY_OUT_SIZE - PTY_OUT_KEEP, k, at;
    struct pty *p;
    u_long flags;
    if(i < 0 || i >= PTY_MAX || !n) return 0;
    p = &ptys[i];
    for(;;) {
        flags = irq_save();
        if(!p->master) {                                /* hung up (or not open): nobody reads */
            p->lost += n;
            irq_restore(flags);
            return 0;
        }
        if(p->out_len + n <= limit) {
            at = (p->out_first + p->out_len) % PTY_OUT_SIZE;
            for(k = 0; k < n; k++) {
                p->out[at] = s[k];
                at = (at + 1) % PTY_OUT_SIZE;
            }
            p->out_len += n;
            irq_restore(flags);
            wake_up_all(&p->readers);
            poll_wake();
            return 1;
        }
        if(!(how & PTY_OUT_WAIT) || task_interrupted() || n > limit) {
            p->lost += n;                               /* it may not wait, or its call is cut short */
            irq_restore(flags);
            return 0;
        }
        wait_prepare_intr(&p->room);                    /* the master's next read makes room */
        irq_restore(flags);
        yield();
    }
}

/* tty.h: a task on a pty ended (tty_task_gone(), interrupts off). The last one ends the session;
   with the master closed as well, the pty is free. From trap_fatal() too, while the task lives:
   it is counted then, and nothing ends. */
void pty_task_gone(struct surf_task *task) {
    struct tty *t = task->tty;
    struct pty *p;
    int i;
    if(!t) return;
    i = (int)(t - ttys) - NUM_CONSOLES;
    if(i < 0 || i >= PTY_MAX) return;
    p = &ptys[i];
    if(!p->used || task_count_tty(t)) return;
    if(p->started && !p->ended) {
        p->ended = 1;
        wake_up_all(&p->readers);
        poll_wake();
    }
    if(!p->master) pty_free(p);
}

/**** the master side ****/

/* What the session printed, as it was printed: at least one byte (it waits for one), at most
   what is there; 0 once the session has ended and everything was read; -EINTR when the reader's
   call is cut short. A chunk at a time, copied out with interrupts on. */
static int master_read(struct vnode *v, u32 off, void *buf, u32 len) {
    struct pty *p = (struct pty *)v->priv;
    char chunk[1024];
    u_int n, k;
    u_long flags;
    (void)off;
    if(len > sizeof(chunk)) len = sizeof(chunk);
    for(;;) {
        flags = irq_save();
        if(p->out_len) {
            n = p->out_len < len ? p->out_len : len;
            for(k = 0; k < n; k++) chunk[k] = p->out[(p->out_first + k) % PTY_OUT_SIZE];
            p->out_first = (p->out_first + n) % PTY_OUT_SIZE;
            p->out_len -= n;
            irq_restore(flags);
            wake_up_all(&p->room);
            memcpy(buf, chunk, n);
            return (int)n;
        }
        if(p->ended) {
            irq_restore(flags);
            return 0;
        }
        if(task_interrupted()) {
            irq_restore(flags);
            return -EINTR;
        }
        wait_prepare_intr(&p->readers);
        irq_restore(flags);
        yield();
    }
}

/* The bytes as typed keys, through the serial line's rules (tty_term_input(): in line mode CR is
   Enter, DEL Backspace, the cursor keys' sequences those keys, Ctrl+C reaches the job; raw mode
   passes them). All of them are taken; see the top for a full input queue. */
static int master_write(struct vnode *v, u32 off, const void *buf, u32 len) {
    struct pty *p = (struct pty *)v->priv;
    struct tty *t = pty_tty(p);
    const u_char *s = (const u_char *)buf;
    u_int waited = 0;
    u32 i;
    (void)off;
    for(i = 0; i < len; i++) {
        u_char c = s[i];                                /* the caller's memory (checked), read with interrupts on */
        while(p->started && !p->ended && waited < PTY_IN_WAIT_MS && !tty_room(t)) {
            if(sleep_intr(10) < 0) return i ? (int)i : -EINTR;
            waited += 10;
        }
        tty_term_input(t, &p->in_esc, c);
    }
    return (int)len;
}

/* surfos/poll.h: POLLIN when there is output or the session has ended, POLLHUP once it has ended
   and everything was read; a write is always taken (POLLOUT) */
static int master_poll(struct vnode *v, struct file *f) {
    struct pty *p = (struct pty *)v->priv;
    int r = POLLOUT;
    u_long flags = irq_save();
    if(p->out_len || p->ended) r |= POLLIN;
    if(p->ended && !p->out_len) r |= POLLHUP;
    irq_restore(flags);
    (void)f;
    return r;
}

/* The last descriptor of the master is closed: the hangup. The tasks on the pty get the kill that
   waits for the end of their call, however long (task_kill_tty(); their waits for keys, for room, at
   the pager end at once); what they print from now on is dropped, and reads of the pty end; the pty
   is free once the last of them is gone (pty_task_gone()), or now when none is left. */
static void master_release(struct vnode *v) {
    struct pty *p = (struct pty *)v->priv;
    struct tty *t = pty_tty(p);
    u_long flags = irq_save();
    p->master = 0;
    p->lost += p->out_len;
    p->out_len = 0;
    t->hungup = 1;                                      /* no key comes: reads end (tty.c), and a process */
    wake_up_all(&t->readers);                           /* started on it from now on is killed (task.c) */
    wake_up_all(&p->room);
    if(!task_kill_tty(t)) pty_free(p);
    irq_restore(flags);
}

static const struct vnode_ops master_ops = {
    NULL, NULL, master_read, master_write, NULL, NULL, NULL, master_release, NULL, master_poll
};

/* the master of a free pty, its tty and console as new; -EAGAIN when every pty is in use */
static int open_master(struct vnode **out) {
    struct pty *p = NULL;
    struct vnode *v;
    u_long flags;
    int i;
    flags = irq_save();
    for(i = 0; i < PTY_MAX && !p; i++) {
        if(ptys[i].used) continue;
        p = &ptys[i];
        p->used = 1;
        p->master = 1;
        p->started = p->ended = 0;
        p->in_esc = 0;
        p->out_first = p->out_len = 0;
        p->lost = 0;
    }
    irq_restore(flags);
    if(!p) return -EAGAIN;
    tty_reset(pty_tty(p));                              /* nobody else has it: no task is on it */
    console_stream_reset(pty_con(p));
    v = vnode_alloc(&ptyfs, &master_ops, VN_FILE);
    if(!v) {
        flags = irq_save();
        pty_free(p);
        irq_restore(flags);
        return -ENOMEM;
    }
    v->priv = p;
    v->ino = 0x100 + (u32)(p - ptys);
    *out = v;
    return 0;
}

/**** the calls ****/

/* the pty whose master descriptor fd is */
static int master_of(int fd, struct pty **pp) {
    struct file *f = fd_get(fd);
    if(!f) return -EBADF;
    if(f->v->ops != &master_ops) return -EINVAL;        /* not a pty's master */
    *pp = (struct pty *)f->v->priv;
    return 0;
}

int sys_openpty(void) {
    struct vnode *v;
    struct file *f;
    int r = open_master(&v);
    if(r) return r;
    r = vfs_open_vnode(v, O_RDWR, &f);
    if(r) {
        vput(v);                                        /* the hangup of a pty nobody used: free again */
        return r;
    }
    r = fd_install(f);
    if(r < 0) vfs_close(f);                             /* the same */
    return r;
}

/* the program's terminal is the pty: its console and tty, so its descriptors 0, 1 and 2
   (/dev/console, which devfs resolves through the task) and the kernel's commands it runs */
int sys_spawnpty(const char *path, char *const argv[], int masterfd) {
    char *kargv[SPAWN_MAX_ARGS];
    struct pty *p;
    u_long flags;
    int argc, r;
    r = master_of(masterfd, &p);
    if(r) return r;
    if(!user_str(path)) return -EFAULT;
    argc = user_argv(path, argv, kargv);
    if(argc < 0) return argc;
    flags = irq_save();
    p->started = 1;                                     /* before it can run, and end */
    p->ended = 0;
    irq_restore(flags);
    r = process_spawn_files(path, argc, kargv, pty_con(p), 0, NULL);
    if(r < 0) {
        flags = irq_save();
        if(!task_count_tty(pty_tty(p)) && !p->ended) {  /* nothing runs on it: an empty session, over */
            p->ended = 1;
            wake_up_all(&p->readers);
            poll_wake();
        }
        irq_restore(flags);
    }
    return r;
}

/* PTY_SETSIZE: columns 8-1024, rows 2-1024. The cursor stays in the row; a question of the pager
   that the new size has room for is called off, and its asker goes on counting rows at that size.
   PTY_INDEX: the pty's number. */
int sys_ptyctl(int masterfd, int op, u32 arg) {
    struct pty *p;
    u_int cols = arg >> 16, rows = arg & 0xFFFF;
    int r = master_of(masterfd, &p);
    if(r) return r;
    if(op == PTY_INDEX) return (int)(p - ptys);
    if(op != PTY_SETSIZE) return -EINVAL;
    if(cols < PTY_COLS_MIN || cols > PTY_COLS_MAX || rows < PTY_ROWS_MIN || rows > PTY_ROWS_MAX) return -EINVAL;
    console_stream_resize(pty_con(p), cols, rows);
    return 0;
}
