/*
SurfOS Driver Header
----------------------
File: driver.h  Date: 4/23/04, driver table 10/2026 (roadmap D1)
----------------------
(C)2004 Brandon Burr
*/

#ifndef _SYS_DRIVERS_H
#define _SYS_DRIVERS_H

#include <surfos/types.h>

/* init returns 0 when the device is up, 1 when it is not present, < 0 on an error */
struct driver {
    const char *name;
    int (*init)(void);
    int status;             /* filled in by init_drivers() */
    u_int flags;            /* 1: a legacy ISA device, skipped with profile=laptop; 2 (10/10/26): the
                               laptop's own hardware, skipped without it */
};

#define DRV_SKIPPED 2       /* status: not probed in this profile */

void init_drivers(void);
void print_drivers(void);

#endif
