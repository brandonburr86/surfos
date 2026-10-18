/* The bridge of tests/host/test_rx.c (10/17/26): user/bin/rx.c as it is, built for the host as edit.c is
   (tests/host/edit_bridge.c), on the file system of tests/host/sim_fs.c. There is no network: listening
   fails (ENOSYS), so a run ends there, after its start, which is what the test looks at. */
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

/* rx 5000 <path>, as from the shell: its exit status */
int rx_run(const char *path) {
    char *argv[] = { "rx", "5000", (char *)path, NULL };
    file = conn = listener = -1;
    return rx_main(3, argv);
}
