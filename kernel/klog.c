/*
SurfOS Kernel Log Ring Buffer
--------------------
File: klog.c    Date: 10/3/26 (roadmap K3)
--------------------
10/17/26 (the desktop's Log): klog_copy() hands a copy of the log's end to a reader.
*/

#include <surfos/types.h>
#include <surfos/klog.h>
#include <surfos/console.h>
#include <surfos/irq.h>
#include <surfos/task.h>
#include <surfos/interrupt.h>

static char klog_buf[KLOG_SIZE];
static u_long klog_head;   /* next write position (always grows) */

void klog_write(const char *s) {
    u_long flags = irq_save();
    while(*s) {
        klog_buf[klog_head % KLOG_SIZE] = *s++;
        klog_head++;
    }
    irq_restore(flags);
}

u_long klog_length(void) {
    return klog_head < KLOG_SIZE ? klog_head : KLOG_SIZE;
}

/* kputch() directly, not kprintf(): dumping the log must not add to it. 10/6/26: on the calling
   task's console, as kprintf() prints (dmesg on F2 while F1 shows, or run by the user shell into a
   file); before there are tasks, on the one that shows */
void klog_dump(void) {
    u_long start = klog_head > KLOG_SIZE ? klog_head - KLOG_SIZE : 0;
    u_long i;
    surf_console *con = (curTask && curTask->con && !in_interrupt()) ? curTask->con : conActive;
    for(i = start; i < klog_head; i++) kputch(con, KERN_TXT_COLOR, klog_buf[i % KLOG_SIZE]);
}

/* 10/17/26: the log's last bytes, up to size of them, into buf (no NUL): how many; *written: how many were ever
   written (what a reader has seen tells it what is new). Copied under irq_save, a chunk at a time, so an interrupt
   that logs meanwhile never sees a torn ring; a line written between two chunks makes the copy start later */
u_long klog_copy(char *buf, u_long size, u_long *written) {
    u_long flags, head, start, n, at, k;
    flags = irq_save();
    head = klog_head;
    irq_restore(flags);
    n = head < KLOG_SIZE ? head : KLOG_SIZE;
    if(n > size) n = size;
    start = head - n;
    for(at = 0; at < n; at += k) {
        k = n - at < 1024 ? n - at : 1024;
        flags = irq_save();
        if(klog_head - (start + at) > KLOG_SIZE) {      /* overwritten meanwhile: what is left is still the end */
            irq_restore(flags);
            break;
        }
        {
            u_long i;
            for(i = 0; i < k; i++) buf[at + i] = klog_buf[(start + at + i) % KLOG_SIZE];
        }
        irq_restore(flags);
    }
    if(written) *written = head;
    return at;
}
