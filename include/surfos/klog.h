/*
SurfOS Kernel Log
----------------------
File: klog.h    Date: 10/3/26 (roadmap K3)
----------------------
Everything kprintf() prints also lands in a ring buffer; `dmesg` shows it.
*/

#ifndef _SURFOS_KLOG_H
#define _SURFOS_KLOG_H

#include <surfos/types.h>

#define KLOG_SIZE 16384

void klog_write(const char *s);
void klog_dump(void);          /* print the whole buffer to the calling task's console (the active one at boot) */
u_long klog_length(void);
/* 10/17/26: the last bytes (up to size) into buf, no NUL: how many; *written (may be NULL): how many ever were,
   up to the last byte copied (the copy is always the log's end as it was then) */
u_long klog_copy(char *buf, u_long size, u_long *written);

#endif
