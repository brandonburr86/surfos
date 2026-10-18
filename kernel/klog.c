/*
SurfOS Kernel Log Ring Buffer
--------------------
File: klog.c    Date: 10/3/26 (roadmap K3)
--------------------
10/17/26 (the desktop's Log): klog_copy() hands a copy of the log's end to a reader, always its end.
10/18/26: the head counts modulo 2^32: whether the ring is full is a flag of its own, so that the log
still has its 16 KB after 4 GB were written (klog_length, klog_dump, klog_copy).
*/

#include <string.h>
#include <surfos/types.h>
#include <surfos/klog.h>
#include <surfos/console.h>
#include <surfos/irq.h>
#include <surfos/task.h>
#include <surfos/interrupt.h>

static char klog_buf[KLOG_SIZE];
static u_long klog_head;   /* next write position (always grows, modulo 2^32: a multiple of KLOG_SIZE) */
static bool klog_full;     /* 10/18/26: KLOG_SIZE bytes or more were ever written: the ring is full */

void klog_write(const char *s) {
    u_long flags = irq_save();
    while(*s) {
        klog_buf[klog_head % KLOG_SIZE] = *s++;
        klog_head++;
    }
    if(klog_head >= KLOG_SIZE) klog_full = true;
    irq_restore(flags);
}

u_long klog_length(void) {
    return klog_full ? KLOG_SIZE : klog_head;
}

/* kputch() directly, not kprintf(): dumping the log must not add to it. 10/6/26: on the calling
   task's console, as kprintf() prints (dmesg on F2 while F1 shows, or run by the user shell into a
   file); before there are tasks, on the one that shows */
void klog_dump(void) {
    u_long n = klog_length(), start = klog_head - n, i;
    surf_console *con = (curTask && curTask->con && !in_interrupt()) ? curTask->con : conActive;
    for(i = 0; i < n; i++) kputch(con, KERN_TXT_COLOR, klog_buf[(start + i) % KLOG_SIZE]);
}

/* 10/17/26: the log's last bytes, up to size of them, into buf (no NUL): how many; *written: how many were ever
   written up to the last of them (what a reader has seen tells it what is new). Copied under irq_save a chunk at a
   time, so an interrupt that logs meanwhile never sees a torn ring; the chunks go from the end backwards, so that
   when a line written between two of them has overwritten the oldest bytes wanted, the copy starts later and what
   it holds is still the end as it was at *written. 10/18/26: when so much was written meanwhile that nothing at all
   was left to copy, it starts again from the new head (three times at most) */
u_long klog_copy(char *buf, u_long size, u_long *written) {
    u_long flags, head = 0, n = 0, at = 0, k, i, from;
    int tries;
    for(tries = 0; tries < 3; tries++) {                /* 10/18/26: nothing copied (a storm): again, from the new head */
        flags = irq_save();
        head = klog_head;
        n = klog_full ? KLOG_SIZE : head;
        irq_restore(flags);
        if(n > size) n = size;
        for(at = n; at > 0; at -= k) {                  /* buf[at - k, at) is the bytes from head - n + at - k on */
            k = at < 1024 ? at : 1024;
            from = head - n + at - k;
            flags = irq_save();
            if(klog_head - from > KLOG_SIZE) {          /* overwritten meanwhile: the copy starts after it */
                irq_restore(flags);
                break;
            }
            for(i = 0; i < k; i++) buf[at - k + i] = klog_buf[(from + i) % KLOG_SIZE];
            irq_restore(flags);
        }
        if(at < n || !n) break;
    }
    if(at) memmove(buf, buf + at, n - at);
    if(written) *written = head;
    return n - at;
}
