/*
SurfOS user program: wsettings
--------------------
File: sys.h     Date: 11/05/26 (the desktop applications, track settings)
--------------------
sys.c: SYS_netctl for the model (its ws_netctl_fn), and the About page's figures.
*/
#ifndef _WSETTINGS_SYS_H
#define _WSETTINGS_SYS_H

#include "settings.h"

int ws_sys_netctl(void *ctx, int op, void *arg, unsigned size);    /* the kernel's answer, -errno included */
void ws_sys_about(struct ws_about *ab, int all);    /* the memory and the time; all: the build and the processor too */

#endif
