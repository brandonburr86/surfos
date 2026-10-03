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

/* kputch() directly, not kprintf(): dumping the log must not add to it */
void klog_dump(void) {
    u_long start = klog_head > KLOG_SIZE ? klog_head - KLOG_SIZE : 0;
    u_long i;
    for(i = start; i < klog_head; i++) kputch(conActive, KERN_TXT_COLOR, klog_buf[i % KLOG_SIZE]);
}
