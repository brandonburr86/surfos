/*
SurfOS Desktop: the Terminal's Side of struct gui_sys
--------------------
File: guisys_term.c Date: 10/17/26
--------------------
A shell session on a pseudo-terminal for the Terminal (kernel/pty.c); track TERM of
docs/superpowers/plans/2026-10-17-gui-suite.md.
10/17/26: the sessions. A session is a pty's master, held here as an open file of its own (no
descriptor: pty_open() and its friends, surfos/pty.h), and /initrd/bin/surfsh -l started on it,
as telnetd starts one for a connection; the shell is a child of the task that runs the desktop
(gui is a kernel command, run in the shell that typed it), and starts in that task's directory.
A handle is the session's place in a small table, and each call checks that the session is the
calling task's: every desktop has its own. Nothing here waits: a read takes what the session
printed so far, a write what the terminal's input queue takes now.

The end. When the shell exits (and whatever else ran on the pty has ended), the master reads what
is left, then -EPIPE. The shell is collected on the reads (task_wait_opts(WNOHANG)) once it has
ended, before the session has, if a program it left running holds the pty. Closing a session
hangs the pty up (the last reference to its master: kernel/pty.c kills every task on it, each at
the end of its call) and collects the shell if it has gone already. A shell that needs a moment to
end (it was inside a call) is handed to init (task_detach()), which collects it then; whatever it
leaves behind is init's when it ends, as for any orphan. So the desktop never waits for a session,
and leaves no zombie of its own nor a task without a parent that collects it, at a window's close
or at the desktop's end (gui_finish()) alike, as telnetd's sessions end when telnetd does.

The kernel log gets a line when a session starts and when it ends; nothing typed or printed in one
is logged anywhere.
*/
#include <stdio.h>
#include <surfos/types.h>
#include <surfos/task.h>
#include <surfos/irq.h>
#include <surfos/klog.h>
#include <surfos/pty.h>
#include <fs/vfs.h>
#include "guisys.h"

#define SESSIONS PTY_MAX
#define SHELL_PATH "/initrd/bin/surfsh"

struct session {
    bool used;                          /* claimed (a session, or one being started) */
    struct file *master;                /* its pty's master */
    u_long owner;                       /* the desktop's task, whose child the shell is */
    int pid;                            /* the shell; 0 once collected or handed to init */
    int pty;
};
static struct session sessions[SESSIONS];

/* the calling task's session h, NULL when it is none */
static struct session *own(int h) {
    if(h < 0 || h >= SESSIONS || !curTask) return NULL;
    if(!sessions[h].used || !sessions[h].master || sessions[h].owner != curTask->pid) return NULL;
    return &sessions[h];
}

/* the shell, collected if it has ended; never waiting */
static void collect(struct session *s) {
    int status;
    if(s->pid > 0 && task_wait_opts((u_long)s->pid, &status, WNOHANG) != 0) s->pid = 0;  /* its pid, or -1: none */
}

int guisys_term_open(void *ctx, int cols, int rows) {
    char *argv[] = { "surfsh", "-l", NULL };
    char line[96];
    struct session *s = NULL;
    struct file *f;
    u_long flags;
    int i, r;
    (void)ctx;
    if(!curTask) return -EINVAL;
    flags = irq_save();
    for(i = 0; i < SESSIONS && !s; i++) {
        if(sessions[i].used) continue;
        s = &sessions[i];
        s->used = true;
        s->master = NULL;
        s->owner = curTask->pid;
        s->pid = 0;
    }
    irq_restore(flags);
    if(!s) return -EAGAIN;
    r = pty_open(&f);
    if(!r) pty_resize(f, (u32)cols, (u32)rows);         /* before the shell starts: it sees its size at once */
    if(!r && (r = pty_spawn(f, SHELL_PATH, 2, argv)) < 0) pty_close(f);       /* a pty nobody used: free again */
    if(r < 0) {
        s->used = false;
        return r;
    }
    s->pid = r;
    s->pty = pty_number(f);
    s->master = f;
    snprintf(line, sizeof(line), "gui: Terminal: surfsh (pid %d) on pty%d\n", s->pid, s->pty);
    klog_write(line);
    return (int)(s - sessions);
}

int guisys_term_read(void *ctx, int h, char *buf, u32 n) {
    struct session *s = own(h);
    (void)ctx;
    if(!s) return -EINVAL;
    collect(s);
    return pty_read(s->master, buf, n);
}

int guisys_term_write(void *ctx, int h, const char *buf, u32 n) {
    struct session *s = own(h);
    int r;
    (void)ctx;
    if(!s) return -EINVAL;
    r = pty_write(s->master, buf, n);
    return r == -EAGAIN ? 0 : r;                        /* nothing fits now */
}

int guisys_term_resize(void *ctx, int h, int cols, int rows) {
    struct session *s = own(h);
    (void)ctx;
    if(!s) return -EINVAL;
    return pty_resize(s->master, (u32)cols, (u32)rows);
}

void guisys_term_close(void *ctx, int h) {
    struct session *s = own(h);
    surf_task *t;
    char line[96];
    u_long flags;
    (void)ctx;
    if(!s) return;
    pty_close(s->master);                               /* the hangup: what runs on the pty is killed */
    s->master = NULL;
    collect(s);
    snprintf(line, sizeof(line), "gui: Terminal: the session on pty%d ended%s\n", s->pty,
             s->pid > 0 ? "; init collects its shell" : "");
    if(s->pid > 0) {                                    /* still ending, inside a call: init collects it */
        flags = irq_save();
        t = task_find((u_long)s->pid);
        if(t && t->parent == curTask) task_detach(t);
        irq_restore(flags);
        s->pid = 0;
    }
    klog_write(line);
    flags = irq_save();
    s->owner = 0;
    s->used = false;
    irq_restore(flags);
}
