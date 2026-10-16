/*
SurfOS user library: the console's character set
----------------------
File: cp437.h   Date: 10/11/26
----------------------
The kernel's code page 437 (include/sys/cp437.h, driver/cp437.c, which libsurf builds as
user/libsurf/cp437.c) for user programs. The kernel's header cannot be included here as
the other surfos/ headers are: it needs the kernel's own types (surfos/types.h), whose
pid_t is not a program's. So the two names a program may use are declared again, in
standard types the same as the kernel's: u16 is an unsigned short, u32 an unsigned int.
*/

#ifndef _USER_SURFOS_CP437_H
#define _USER_SURFOS_CP437_H

/* index -> Unicode: the 256 characters the console draws (0x00 is the empty cell) */
extern const unsigned short cp437_unicode[256];

/* Unicode -> index, -1 when the console cannot draw the character (it would show '?') */
int cp437_from_unicode(unsigned int u);

#endif
