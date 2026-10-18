/* The bridge of tests/host/test_rx.c (10/17/26): user/bin/rx.c as it is, built for the host as edit.c is
   (tests/host/edit_bridge.c), on the file system of tests/host/sim_fs.c. The test asks only whether rx
   would start over an earlier rx's .part (part_in_the_way()), so the network calls below are never made;
   they and the host's own printf and stdio are there for the link. */
#define main    rx_main
#include "../../user/bin/rx.c"

int tcp_listen(unsigned short port) { (void)port; errno = ENOSYS; return -1; }
int tcp_accept(int fd, struct netaddr *peer) { (void)fd; (void)peer; errno = ENOSYS; return -1; }
char *ipstr(unsigned int ip, char buf[16]) { (void)ip; buf[0] = 0; return buf; }

/* 1: rx would not start over path's .part (its twin), 0: it would, -1: there is no telling */
int rx_part_check(const char *path) {
    char p[PATH_MAX];
    snprintf(p, sizeof(p), "%s.part", path);
    return part_in_the_way(path, p);
}
