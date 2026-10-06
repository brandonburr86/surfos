/*
SurfOS Kernel Log Ring Buffer
--------------------
File: klog.c    Date: 10/3/26 (roadmap K3)
--------------------
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
