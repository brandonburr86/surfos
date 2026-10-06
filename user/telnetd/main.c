/*
SurfOS telnet server: telnetd
--------------------
File: main.c    Date: 10/6/26
--------------------
telnetd, the telnet server: another computer on the network telnets to the laptop and
gets a SurfOS shell, surfsh with its banner, on a pseudo-terminal of its own. There is
no login and no encryption: whoever reaches the port has a shell with every kernel
command, so it runs only when it is turned on.

    telnetd on [port]   start the server in the background, on port 23 by default. A
                        server that runs already is said with its pid (status 1). The
                        port is taken here, so "in use" is said at once; the server
                        (server.c) gets it as its descriptor 0. Then the address to
                        telnet to, or that the interface has none yet (run dhcp: the
                        server listens all the same). Not in a telnet session: a server
                        started there would end with that session.
    telnetd off         stop the server, which ends every session: "stopped (n sessions
                        ended)" once they are gone; status 1 when it was not running.
    telnetd [status]    whether it runs, its pid, and its sessions: the pseudo-terminal
                        and the pid of each session's surfsh.

The running server is found in the process list (procinfo): a detached user task named
telnetd, not this one; its sessions are the tasks on a pseudo-terminal whose parent it
is. Statuses: 0 success, 1 failure, 2 usage. On a kernel without the telnet server's
system calls (ENOSYS) the commands say so.
*/

#include "telnetd.h"

#define WAIT_MS 3000                    /* how long on and off wait for the server */
#define STEP_MS 20

/* a call that failed: said plainly, and which part of the kernel is missing when it is ENOSYS */
static int failed(const char *call) {
    if(errno == ENOSYS)
        fprintf(stderr, "telnetd: this kernel cannot run a telnet server:\n"
                "    it has no %s() (ENOSYS), one of the telnet server's system calls\n", call);
    else
        fprintf(stderr, "telnetd: %s: %s\n", call, strerror(errno));
    return 1;
}

/* task number i of the process list: 1 and *pi, 0 after the last, -1 with errno */
static int task(int i, struct procinfo *pi) {
    if(procinfo(i, pi) == 0) return 1;
    return errno == ENOENT ? 0 : -1;
}

/* the running server's pid, 0 when there is none, -1 with errno */
static int find_server(void) {
    struct procinfo pi;
    int i, r, me = getpid();
    for(i = 0; (r = task(i, &pi)) > 0; i++) {
        if((pi.flags & PROC_USER) && (pi.flags & PROC_DETACHED) && pi.state != PROC_DEAD && pi.pid != me &&
           !strcmp(pi.name, "telnetd")) return pi.pid;
    }
    return r < 0 ? -1 : 0;
}

/* the task pid as the list has it: 1 and *pi while it lives, 0 when it is gone */
static int find_task(int pid, struct procinfo *pi) {
    int i;
    for(i = 0; task(i, pi) > 0; i++) if(pi->pid == pid) return pi->state != PROC_DEAD;
    return 0;
}

/* the server's sessions: the tasks on a pseudo-terminal whose parent it is (each session's
   surfsh); their pids into pids (up to max), listed when list; how many */
static int sessions(int server, int list, int *pids, int max) {
    struct procinfo pi;
    int i, n = 0;
    for(i = 0; task(i, &pi) > 0; i++) {
        if(pi.ppid != server || pi.tty < PROC_TTY_PTY || pi.state == PROC_DEAD) continue;
        if(list) printf("    pty%-2d %s (pid %d)\n", pi.tty - PROC_TTY_PTY, pi.name, pi.pid);
        if(n < max) pids[n] = pi.pid;
        n++;
    }
    return n;
}

static int cmd_status(void) {
    int pid = find_server(), n;
    if(pid < 0) return failed("procinfo");
    if(!pid) {
        printf("telnetd: not running (telnetd on starts it)\n");
        return 0;
    }
    n = sessions(pid, 0, NULL, 0);
    printf("telnetd: running (pid %d), %d session%s%s\n", pid, n, n == 1 ? "" : "s", n ? ":" : "");
    sessions(pid, 1, NULL, 0);
    return 0;
}

/* where to telnet to: the default interface's address */
static void say_address(int port) {
    struct netinfo ni;
    char ip[16];
    if(netinfo(&ni) < 0) {
        printf("telnetd: listening on port %d, but there is no network interface\n", port);
        return;
    }
    if(!ni.ip) {
        printf("telnetd: listening on port %d, but %.8s has no address yet: run dhcp first\n", port, ni.name);
        return;
    }
    printf("telnetd: listening on %s port %d\n", ipstr(ni.ip, ip), port);
}

static int cmd_on(const char *word) {
    char portword[8], *argv[] = { "telnetd", SERVER_WORD, portword, NULL };
    struct procinfo pi;
    int port = TELNET_PORT, pid, lfd, nul, fds[3], waited;

    if(word) {
        char *end;
        long v = strtol(word, &end, 10);
        if(!*word || *end || v < 1 || v > 65535) {
            fprintf(stderr, "telnetd: %s is no port (1-65535)\n", word);
            return 2;
        }
        port = (int)v;
    }
    pid = find_server();
    if(pid < 0) return failed("procinfo");
    if(pid) {
        printf("telnetd: running already (pid %d); telnetd off stops it\n", pid);
        return 1;
    }
    if(find_task(getpid(), &pi) && pi.tty >= PROC_TTY_PTY) {
        fprintf(stderr, "telnetd: start the server on one of the laptop's own consoles (F1-F4): "
                "one started in a telnet session would end with the session\n");
        return 1;
    }
    lfd = tcp_listen((unsigned short)port);
    if(lfd < 0) {
        if(errno == EADDRINUSE) {
            fprintf(stderr, "telnetd: port %d is in use\n", port);
            return 1;
        }
        return failed("tcp_listen");
    }
    nul = open("/dev/null", O_RDWR);
    fds[0] = lfd;                       /* the server's 0: the port; 1 and 2: nothing to see */
    fds[1] = fds[2] = nul;
    snprintf(portword, sizeof(portword), "%d", port);
    pid = spawnfd(TELNETD_PATH, argv, fds);
    if(pid < 0) failed("spawnfd");
    close(lfd);                         /* the server has its own reference */
    if(nul >= 0) close(nul);
    if(pid < 0) return 1;
    /* until it has left this shell: then status and off find it, and Ctrl+C here misses it */
    for(waited = 0; waited < WAIT_MS; waited += STEP_MS) {
        if(!find_task(pid, &pi)) {
            fprintf(stderr, "telnetd: the server (pid %d) ended at once\n", pid);
            return 1;
        }
        if(pi.flags & PROC_DETACHED) break;
        msleep(STEP_MS);
    }
    say_address(port);
    return 0;
}

static int cmd_off(void) {
    int pid = find_server(), pids[PTY_MAX], n, k, left, waited;
    struct procinfo pi;
    if(pid < 0) return failed("procinfo");
    if(!pid) {
        printf("telnetd: not running\n");
        return 1;
    }
    n = sessions(pid, 0, pids, PTY_MAX);
    if(kill(pid) < 0) {
        fprintf(stderr, "telnetd: cannot stop pid %d: %s\n", pid, strerror(errno));
        return 1;
    }
    /* the server, then its sessions' shells (they end as their pseudo-terminals hang up) */
    for(waited = 0; waited < WAIT_MS; waited += STEP_MS) {
        left = find_task(pid, &pi);
        for(k = 0; k < n && k < PTY_MAX; k++) left += find_task(pids[k], &pi);
        if(!left) break;
        msleep(STEP_MS);
    }
    if(find_task(pid, &pi)) {
        fprintf(stderr, "telnetd: the server (pid %d) did not stop\n", pid);
        return 1;
    }
    printf("telnetd: stopped (%d session%s ended)\n", n, n == 1 ? "" : "s");
    return 0;
}

int main(int argc, char **argv) {
    if(argc >= 2 && !strcmp(argv[1], SERVER_WORD)) return serve(0, argc > 2 ? atoi(argv[2]) : TELNET_PORT);
    if(argc == 1 || (argc == 2 && !strcmp(argv[1], "status"))) return cmd_status();
    if(!strcmp(argv[1], "on") && argc <= 3) return cmd_on(argc == 3 ? argv[2] : NULL);
    if(!strcmp(argv[1], "off") && argc == 2) return cmd_off();
    fprintf(stderr, "usage: telnetd [on [port] | off | status]\n");
    return 2;
}
