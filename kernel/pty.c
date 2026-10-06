/*
SurfOS pseudo-terminals
--------------------
File: pty.c     Date: 10/6/26
--------------------
SYS_openpty, SYS_spawnpty and SYS_ptyctl (surfos/pty.h). Not there yet: every call
answers -ENOSYS.
*/

#include <surfos/types.h>
#include <surfos/sysimpl.h>
#include <fs/vfs.h>

int sys_openpty(void) { return -ENOSYS; }
int sys_spawnpty(const char *path, char *const argv[], int masterfd) { (void)path; (void)argv; (void)masterfd; return -ENOSYS; }
int sys_ptyctl(int masterfd, int op, u32 arg) { (void)masterfd; (void)op; (void)arg; return -ENOSYS; }
