/*
SurfOS processes, as programs see them
--------------------
File: proc.c    Date: 10/6/26
--------------------
SYS_procinfo and SYS_detach (surfos/proc.h). Not there yet: both answer -ENOSYS.
*/

#include <surfos/types.h>
#include <surfos/sysimpl.h>
#include <fs/vfs.h>

int sys_procinfo(int index, struct procinfo *pi) { (void)index; (void)pi; return -ENOSYS; }
int sys_detach(void) { return -ENOSYS; }
