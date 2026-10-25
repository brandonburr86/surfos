/*
SurfOS Desktop: the Kernel's struct gui_sys
--------------------
File: guisys.c  Date: 10/17/26
--------------------
What the desktop's applications ask of the kernel (sys/gui_sys.h): the tasks as ps lists them,
memory as memstat counts it, the machine, ending a task as kill does (but only a program, and
never the desktop's own task), and a copy of the kernel log. Disks' block devices are in
guisys_disk.c and the Terminal's sessions in guisys_term.c (shell/guisys.h). Nothing here waits:
each call copies what is there now.
10/18/26: the tasks are copied in one go; End task never ends the shells the desktop runs in,
and its kill waits for the end of the call however long (kill_task_patient()).
10/25/26: programs' windows in the table (shell/guisys_win.c); End the program uses kill.
*/
#include <stdio.h>
#include <string.h>
#include <surfos/types.h>
#include <surfos/task.h>
#include <surfos/irq.h>
#include <surfos/proc.h>
#include <surfos/timer.h>
#include <surfos/ktimer.h>
#include <surfos/klog.h>
#include <surfos/cpu.h>
#include <surfos/multiboot.h>
#include <mm/kalloc.h>
#include <mm/pmm.h>
#include <sys/framebuffer.h>
#include <fs/vfs.h>
#include "guisys.h"

/* 10/18/26: copied in one go (task_info_all()), so that a task collected meanwhile cannot shift another out of a
   reading; the newest max when there are more */
static int tasks(void *ctx, struct gui_task *out, int max) {
    struct procinfo *pi;
    int i, n;
    (void)ctx;
    if(max <= 0) return 0;
    pi = (struct procinfo *)kalloc(max * sizeof(*pi));
    if(!pi) return -ENOMEM;
    n = task_info_all(pi, max);
    for(i = 0; i < n; i++) {
        out[i].pid = pi[i].pid;
        out[i].ppid = pi[i].ppid;
        out[i].state = pi[i].state;
        out[i].tty = pi[i].tty;
        out[i].flags = pi[i].flags;
        strlcpy(out[i].name, pi[i].name, sizeof(out[i].name));
    }
    kfree(pi);
    return n;
}

static int memory(void *ctx, struct gui_memory *out) {
    struct heap_stats k, d;
    (void)ctx;
    memset(out, 0, sizeof(*out));
    out->ram_kb = (u32)(pmm_ram_bytes() / 1024);
    out->high_kb = (u32)(pmm_ram_high_bytes() / 1024);
    out->free_pages = (u32)pmm_free_frames();
    out->total_pages = (u32)pmm_total_frames();
    kheap_stats(&k);
    dmaheap_stats(&d);
    out->heap_used = (u32)k.bytes_used;
    out->heap_free = (u32)k.bytes_free;
    out->heap_size = (u32)(k.limit - k.start);
    out->dma_used = (u32)d.bytes_used;
    out->dma_free = (u32)d.bytes_free;
    out->dma_size = (u32)(d.limit - d.start);
    return 0;
}

/* the build's id: /initrd/etc/build-id's first line, "" when there is none */
static void build_id(char *out, size_t n) {
    struct file *f;
    char buf[96];
    int r, i;
    out[0] = 0;
    if(vfs_open("/initrd/etc/build-id", O_RDONLY, &f)) return;
    r = vfs_read(f, buf, sizeof(buf) - 1);
    vfs_close(f);
    if(r <= 0) return;
    buf[r] = 0;
    for(i = 0; buf[i] && buf[i] != '\n' && buf[i] != '\r'; i++) ;
    buf[i] = 0;
    strlcpy(out, buf, n);
}

static int machine(void *ctx, struct gui_machine *out) {
    char video[200];
    (void)ctx;
    memset(out, 0, sizeof(*out));
    strlcpy(out->cpu, cpu.brand[0] ? cpu.brand : cpu.vendor[0] ? cpu.vendor : "unknown", sizeof(out->cpu));
    strlcpy(out->firmware, bootinfo.efi ? "UEFI" : "BIOS", sizeof(out->firmware));
    strlcpy(out->profile, bootinfo.laptop ? "laptop" : "standard", sizeof(out->profile));
    if(bootinfo.fb.present)
        snprintf(out->video, sizeof(out->video), "%ux%ux%u", (u_int)bootinfo.fb.width, (u_int)bootinfo.fb.height,
                 (u_int)bootinfo.fb.bpp);
    else strlcpy(out->video, framebuffer_describe(video, sizeof(video)), sizeof(out->video));
    build_id(out->build, sizeof(out->build));
    out->uptime_s = (u32)(uptime_ms() / 1000);
    out->tasks = task_count();
    out->timers = ktimers_count();
    return 0;
}

/* as kill does, but only a program (a kernel thread is the kernel's: the kill command ends one), and never the
   desktop's own task. 10/17/26 (System Monitor): the task is found and killed with interrupts off and its name
   copied first, since a zombie may be collected (and freed) by its parent at any time; each refusal says why.
   10/18/26: nor the shells this desktop runs in (curTask's parents): init would start a new shell on the
   desktop's console, which would take the keys meant for the desktop. And the kill is the one that waits for
   the end of the call however long (kill_task_patient()): kill_task()'s, 2 s later, kills at once inside a
   call, and a zombie holding a volume's lock would stop the desktop at Disks' next reading. A task in task_exit()
   (TC_EXIT: its files are closing) is ending already, and says so */
static int kill(void *ctx, int pid, char *msg, size_t n) {
    char name[TASK_NAME_LEN];
    const char *no = NULL, *call = NULL;
    bool ended = false, ending = false, above = false;
    surf_task *t, *p;
    u_long flags;
    int depth;
    (void)ctx;
    flags = irq_save();
    t = task_find((u_long)pid);
    if(t) {
        strlcpy(name, t->name, sizeof(name));
        for(p = curTask ? curTask->parent : NULL, depth = 0; p && depth < 64; p = p->parent, depth++)
            if(p == t) above = true;
        if(t == idle_task) no = "is the idle task: the kernel's own";
        else if(t == init_task_ptr) no = "starts the consoles' shells: it is never ended";
        else if(t == curTask) no = "runs this desktop: Ctrl+Q leaves it";
        else if(above) no = "runs the shell this desktop runs in";
        else if(!(t->flags & TF_USER)) no = "is a kernel thread: the shell's kill command ends one";
        else if(t->state == TS_DEAD) ended = true;
        else if(t->in_call & TC_EXIT) ending = true;
        else {
            call = !t->in_call ? NULL : (t->in_call & TC_COMMAND) ? "kernel command" : "system call";
            if(!kill_task_patient(t)) call = NULL;      /* it ended at once */
        }
    }
    irq_restore(flags);
    if(!t) {
        snprintf(msg, n, "no task with pid %d any more", pid);
        return -ENOENT;
    }
    if(no) {
        snprintf(msg, n, "%s (pid %d) %s", name, pid, no);
        return -EACCES;
    }
    if(ended) snprintf(msg, n, "%s (pid %d) had ended already", name, pid);
    else if(ending) snprintf(msg, n, "%s (pid %d) is ending already", name, pid);
    else if(call) snprintf(msg, n, "%s (pid %d) ends when its %s does", name, pid, call);
    else snprintf(msg, n, "ended %s (pid %d)", name, pid);
    return 0;
}

static int log_copy(void *ctx, char *buf, u32 size, u32 *len, u32 *written) {
    u_long w = 0;
    (void)ctx;
    *len = (u32)klog_copy(buf, size, &w);
    *written = (u32)w;
    return 0;
}

static const struct gui_sys kernel_sys = {
    NULL, tasks, memory, machine, kill, guisys_disks, log_copy,
    guisys_term_open, guisys_term_read, guisys_term_write, guisys_term_resize, guisys_term_close,
    guisys_win_accept, guisys_win_update, guisys_win_event, guisys_win_close,  /* 10/25/26: programs' windows */
    guisys_win_run,                                                             /* the launcher */
};

const struct gui_sys *gui_kernel_sys(void) {
    return &kernel_sys;
}
