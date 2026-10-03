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
void klog_dump(void);          /* print the whole buffer to the active console */
u_long klog_length(void);

#endif
