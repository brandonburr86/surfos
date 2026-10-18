/*
SurfOS Kernel Log Ring Buffer
--------------------
File: klog.c    Date: 10/3/26 (roadmap K3)
--------------------
10/17/26 (the desktop's Log): klog_copy() hands a copy of the log's end to a reader, always its end.
*/

#include <string.h>
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
   written up to the last of them (what a reader has seen tells it what is new). Copied under irq_save a chunk at a
   time, so an interrupt that logs meanwhile never sees a torn ring; the chunks go from the end backwards, so that
   when a line written between two of them has overwritten the oldest bytes wanted, the copy starts later and what
   it holds is still the end as it was at *written */
u_long klog_copy(char *buf, u_long size, u_long *written) {
    u_long flags, head, n, at, k, i, from;
    flags = irq_save();
    head = klog_head;
    irq_restore(flags);
    n = head < KLOG_SIZE ? head : KLOG_SIZE;
    if(n > size) n = size;
    for(at = n; at > 0; at -= k) {                      /* buf[at - k, at) is the bytes from head - n + at - k on */
        k = at < 1024 ? at : 1024;
        from = head - n + at - k;
        flags = irq_save();
        if(klog_head - from > KLOG_SIZE) {              /* overwritten meanwhile: the copy starts after it */
            irq_restore(flags);
            break;
        }
        for(i = 0; i < k; i++) buf[at - k + i] = klog_buf[(from + i) % KLOG_SIZE];
        irq_restore(flags);
    }
    if(at) memmove(buf, buf + at, n - at);
    if(written) *written = head;
    return n - at;
}
