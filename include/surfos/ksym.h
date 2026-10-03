/*
SurfOS Kernel Symbol Table
----------------------
File: ksym.h    Date: 10/3/26 (roadmap K3)
----------------------
Generated at link time by tools/gensyms.py; used to put names on addresses in
panics and backtraces.
*/

#ifndef _SURFOS_KSYM_H
#define _SURFOS_KSYM_H

#include <surfos/types.h>

struct ksym {
    u_long addr;
    u_long name_off; /* offset into ksym_names */
};

extern const u_long ksym_count;
extern const struct ksym ksym_table[];
extern const char ksym_names[];

/* name of the function containing addr, or NULL; *offset receives addr - start */
const char *ksym_lookup(u_long addr, u_long *offset);

/* print "[<addr>] name+0xoff" style lines for a frame chain */
void backtrace(u_long ebp, u_long eip);
void backtrace_here(void);

#endif
