/*
SurfOS Network Stack: connections for programs
--------------------
File: sock.c    Date: 10/6/26
--------------------
SYS_listen, SYS_accept, SYS_connect and SYS_netinfo (surfos/sock.h). Not there yet:
every call answers -ENOSYS.
*/

#include <surfos/types.h>
#include <surfos/sysimpl.h>
#include <fs/vfs.h>

int sys_listen(u16 port) { (void)port; return -ENOSYS; }
int sys_accept(int fd, struct netaddr *peer) { (void)fd; (void)peer; return -ENOSYS; }
int sys_connect(u32 ip, u16 port) { (void)ip; (void)port; return -ENOSYS; }
int sys_netinfo(struct netinfo *ni) { (void)ni; return -ENOSYS; }
