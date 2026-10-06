/*
SurfOS user library: the system call numbers
----------------------
File: sysnum.h  Date: 10/5/26
----------------------
The numbers are the kernel's (include/surfos/syscall.h, which the library finds
through -idirafter include; programs never see it). That header includes the
kernel's <surfos/types.h>, whose bool and unsigned pid_t are not this library's;
the numbers need none of it, so its guard is set first.
*/

#ifndef _SURF_SYSNUM_H
#define _SURF_SYSNUM_H

#define _SURFOS_TYPES_H
#include <surfos/syscall.h>

#endif
