/*
SurfOS user programs: top's model
----------------------
File: topmodel.h  Date: 11/01/26
----------------------
The tasks of two readings made into what top shows: each one's share of the processor between
them, the order it is asked for, and the text of every row, fitted to the terminal. No system
call is made here (top.c makes them), so tests/host/test_top.c runs this as it is.

The kernel counts a timer tick to whichever task it finds running, so a share is a sample: over
two seconds at 100 ticks a second, a program that ran half the time shows 50.0 give or take a
tick or two, and a share cannot be finer than that.
*/

#ifndef _TOPMODEL_H
#define _TOPMODEL_H

#include <surfos/proc.h>
#include "termview.h"

#define TOP_MAX_TASKS 4096              /* procinfo() is asked for no more of them than this */
#define TOP_ROW_MAX   (TV_MAX_COLS + 64)    /* a row's bytes: columns of text, and the colours around it */

enum { TOP_BY_CPU, TOP_BY_MEM, TOP_BY_PID };

struct top_task {
    struct procinfo pi;
    unsigned int tenths;                /* its share since the last reading, in tenths of a percent */
};

struct top_list {
    struct top_task *t;
    int n, cap;
};

/* 0, or -1 without memory */
static TV_UNUSED int top_add(struct top_list *l, const struct procinfo *pi) {
    if(l->n == l->cap) {
        int cap = l->cap ? l->cap * 2 : 64;
        struct top_task *p = realloc(l->t, (size_t)cap * sizeof(*p));
        if(!p) return -1;
        l->t = p;
        l->cap = cap;
    }
    l->t[l->n].pi = *pi;
    l->t[l->n].tenths = 0;
    l->n++;
    return 0;
}

static TV_UNUSED void top_free(struct top_list *l) {
    free(l->t);
    l->t = NULL;
    l->n = l->cap = 0;
}

static int top_cmp_pid(const void *a, const void *b) {
    const struct top_task *x = a, *y = b;
    return x->pi.pid < y->pi.pid ? -1 : x->pi.pid > y->pi.pid;
}

static int top_cmp_cpu(const void *a, const void *b) {
    const struct top_task *x = a, *y = b;
    if(x->tenths != y->tenths) return x->tenths > y->tenths ? -1 : 1;
    return top_cmp_pid(a, b);
}

static int top_cmp_mem(const void *a, const void *b) {
    const struct top_task *x = a, *y = b;
    if(x->pi.mem_pages != y->pi.mem_pages) return x->pi.mem_pages > y->pi.mem_pages ? -1 : 1;
    return top_cmp_pid(a, b);
}

static TV_UNUSED void top_order(struct top_list *l, int by) {
    if(l->n > 1) qsort(l->t, (size_t)l->n, sizeof(l->t[0]), by == TOP_BY_CPU ? top_cmp_cpu : by == TOP_BY_MEM ? top_cmp_mem :
                       top_cmp_pid);
}

/* a fresh reading in pid order, each pid once: procinfo() is by position in a list that may change between two calls,
   so a task can be read twice (a task before it was collected) or missed. A missed one shows at the next reading. */
static TV_UNUSED void top_tidy(struct top_list *l) {
    int i, kept = 0;
    top_order(l, TOP_BY_PID);
    for(i = 0; i < l->n; i++)
        if(!kept || l->t[kept - 1].pi.pid != l->t[i].pi.pid) l->t[kept++] = l->t[i];
    l->n = kept;
}

/* ticks of elapsed in tenths of a percent, 1000 at most: the two readings are not one instant, and no task
   runs more than the time that passed. No 64-bit division: libsurf has none. */
static TV_UNUSED unsigned int top_share(unsigned int ticks, unsigned int elapsed) {
    if(!elapsed) return 0;
    if(ticks >= elapsed) return 1000;
    return ticks <= 4294967u ? ticks * 1000 / elapsed : ticks / (elapsed / 1000);
}

/* each task's share of the elapsed ticks since the reading before, which is in pid order; with none (the first
   report), of the time since boot. A task not in the reading before has run only since it. */
static TV_UNUSED void top_shares(struct top_list *now, const struct top_list *before, unsigned int elapsed) {
    int i;
    for(i = 0; i < now->n; i++) {
        struct top_task *t = &now->t[i];
        const struct top_task *was = before && before->n ?
            bsearch(t, before->t, (size_t)before->n, sizeof(before->t[0]), top_cmp_pid) : NULL;
        t->tenths = top_share(was ? t->pi.cpu_ticks - was->pi.cpu_ticks : t->pi.cpu_ticks, elapsed);
    }
}

static TV_UNUSED char top_state(int state) {
    switch(state) {
    case PROC_READY: return 'r';
    case PROC_RUNNING: return 'R';
    case PROC_BLOCKED: return 'B';
    case PROC_SLEEPING: return 'S';
    case PROC_STOPPED: return 'T';
    case PROC_DEAD: return 'Z';
    }
    return '?';
}

static TV_UNUSED void top_hms(unsigned int seconds, char *out, size_t size) {
    snprintf(out, size, "%u:%02u:%02u", seconds / 3600, seconds / 60 % 60, seconds % 60);
}

/* the terminal, as the desktop's System Monitor names them */
static TV_UNUSED void top_tty(int tty, char *out, size_t size) {
    if(tty >= PROC_TTY_PTY) snprintf(out, size, "pty%d", tty - PROC_TTY_PTY);
    else if(tty >= 0) snprintf(out, size, "F%d", tty + 1);
    else snprintf(out, size, "-");
}

/* The two rows about the whole machine. The processor's share is what the idle task (pid 0) did not get. */
static TV_UNUSED size_t top_summary(const struct top_list *l, const struct sysinfo *si, char *out, size_t size, int width) {
    char up[24];
    int i, running = 0, stopped = 0;
    unsigned int idle = 0, hz = si->hz ? si->hz : 1;
    for(i = 0; i < l->n; i++) {
        const struct top_task *t = &l->t[i];
        if(t->pi.pid == 0) idle = t->tenths;
        if(t->pi.state == PROC_RUNNING || t->pi.state == PROC_READY) running++;
        else if(t->pi.state == PROC_STOPPED) stopped++;
    }
    idle = idle > 1000 ? 0 : 1000 - idle;
    top_hms(si->ticks / hz, up, sizeof(up));
    snprintf(out, size, "top: up %s, %d tasks, %d running, %d stopped, processor %u.%u%% busy", up, l->n, running, stopped,
             idle / 10, idle % 10);
    return tv_cut(out, width);
}

static TV_UNUSED size_t top_memory(const struct sysinfo *si, char *out, size_t size, int width) {
    unsigned int kb = si->page_size / 1024;
    size_t n = (size_t)snprintf(out, size, "Memory: %u KB, %u free, %u programs, %u kernel heap", si->total_pages * kb,
                                si->free_pages * kb, si->program_pages * kb, si->heap_used / 1024);
    if(si->dying_pages && n < size) snprintf(out + n, size - n, ", %u being freed", si->dying_pages * kb);
    return tv_cut(out, width);
}

#define TOP_HEADING "  PID  PPID S TTY    %CPU      TIME MEM (KB) NAME"

/* a task's row. A kernel thread's name is in brackets: it holds no memory of a program's, and a process does from the
   moment it is made, before it first runs and is marked as a program. Names can be anything a file's name can, so
   they go through tv_clean(). */
static TV_UNUSED size_t top_row(const struct top_task *t, const struct sysinfo *si, int utf8, char *out, size_t size, int width) {
    const struct procinfo *pi = &t->pi;
    char name[sizeof(pi->name) * 4 + 8], tty[16], ppid[16], when[24], mem[16];
    unsigned int hz = si->hz ? si->hz : 1;
    int user = pi->mem_pages != 0;
    tv_clean(pi->name, sizeof(pi->name), name, sizeof(name), utf8);
    top_tty(pi->tty, tty, sizeof(tty));
    if(pi->ppid < 0) snprintf(ppid, sizeof(ppid), "-");
    else snprintf(ppid, sizeof(ppid), "%d", pi->ppid);
    top_hms(pi->cpu_ticks / hz, when, sizeof(when));
    if(pi->mem_pages) snprintf(mem, sizeof(mem), "%u", pi->mem_pages * (si->page_size / 1024));
    else snprintf(mem, sizeof(mem), "-");
    snprintf(out, size, "%5d %5s %c %-5s %3u.%u %9s %8s %s%s%s", pi->pid, ppid, top_state(pi->state), tty, t->tenths / 10,
             t->tenths % 10, when, mem, user ? "" : "[", name, user ? "" : "]");
    return tv_cut(out, width);
}

/* the bottom row of the live view: which rows these are, the order, the keys */
static TV_UNUSED size_t top_status(int first, int shown, int total, int by, char *out, size_t size, int width) {
    snprintf(out, size, "tasks %d-%d of %d, by %s   c m p: order   Up Down: move   q: end", total ? first + 1 : 0, first + shown,
             total, by == TOP_BY_CPU ? "CPU" : by == TOP_BY_MEM ? "memory" : "pid");
    return tv_cut(out, width);
}

/* the first row of the list to show after a move: never past the end, where the screen would have rows to spare */
static TV_UNUSED int top_scroll(int first, int total, int rows) {
    int last = total > rows ? total - rows : 0;
    return first > last ? last : first < 0 ? 0 : first;
}

/* the name of an order, as -s takes it: 0, or -1 for no such order */
static TV_UNUSED int top_order_name(const char *s, int *by) {
    if(!strcmp(s, "cpu")) *by = TOP_BY_CPU;
    else if(!strcmp(s, "mem")) *by = TOP_BY_MEM;
    else if(!strcmp(s, "pid")) *by = TOP_BY_PID;
    else return -1;
    return 0;
}

#endif
