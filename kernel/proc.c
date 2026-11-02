/*
SurfOS processes, as programs see them
--------------------
File: proc.c    Date: 10/6/26
--------------------
SYS_procinfo and SYS_detach (surfos/proc.h). The task list itself is kernel/task.c's:
task_info() copies one entry while interrupts are off, and this file hands it to the
caller; task_detach() moves a process out of its shell's job.
11/01/26 (top): SYS_sysinfo, memstat's totals for a program.
*/

#include <surfos/types.h>
#include <surfos/task.h>
#include <surfos/proc.h>
#include <surfos/sysimpl.h>
#include <surfos/timer.h>
#include <fs/vfs.h>
#include <mm/kalloc.h>
#include <mm/memory.h>
#include <mm/pmm.h>
#include <blibc_common.h>

/* task number index (0, 1, ...): pid, parent, state, flags, terminal, name; -ENOENT after the last. 11/01/26: only as much
   as the caller has room for: a program built before struct procinfo grew passes no size (surfos/proc.h) */
int sys_procinfo(int index, struct procinfo *pi, u32 size) {
    struct procinfo k;
    u32 n = !size ? PROCINFO_SIZE_0 : size < sizeof(k) ? size : sizeof(k);
    int r;
    if(!user_buf(pi, n, true)) return -EFAULT;
    r = task_info(index, &k);
    if(r == 0) memcpy(pi, &k, n);
    return r;
}

/* the caller leaves its parent and its shell's job: init's child from now on */
int sys_detach(void) {
    return task_detach(curTask);
}

/* 11/01/26: the clock and the memory totals, gathered first and copied out at the end, as sys_procinfo() does */
int sys_sysinfo(struct sysinfo *si) {
    struct sysinfo k;
    struct heap_stats heap;
    u_long programs, dying;
    if(!user_buf(si, sizeof(*si), true)) return -EFAULT;
    kheap_stats(&heap);
    task_mem_held(&programs, &dying);
    memset(&k, 0, sizeof(k));
    k.hz = HZ;
    k.ticks = (unsigned int)getticks();
    k.page_size = PAGE_SIZE;
    k.total_pages = (unsigned int)pmm_total_frames();
    k.free_pages = (unsigned int)pmm_free_frames();
    k.program_pages = (unsigned int)programs;
    k.dying_pages = (unsigned int)dying;
    k.heap_used = (unsigned int)heap.bytes_used;
    memcpy(si, &k, sizeof(k));
    return 0;
}
