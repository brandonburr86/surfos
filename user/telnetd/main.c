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
    telnetd off         stop the server, which ends every session: "stopped (n session
                        shells ended)" once they are gone; the kernel's log says it
                        before the server stops; status 1 when it was not running. Typed
                        in a telnet session, it ends that session with the rest.
    telnetd [status]    whether it runs, its pid, and the shells of its sessions: the
                        pseudo-terminal and the pid of each session's surfsh.

The running server is found in the process list (procinfo): a detached user task named
telnetd, not this one; the shells of its sessions are the tasks on a pseudo-terminal whose
parent it is. (A session whose shell has ended while its last output still goes to a slow
client has no shell to count: hence "session shells".) The list is read by index, and a
task collected meanwhile shifts the rest, so a search that finds nothing looks again
(SCANS times in all). Statuses: 0 success, 1 failure, 2 usage. On a kernel without the
telnet server's system calls (ENOSYS) the commands say so. "telnetd --server port" is how
telnetd on starts the server (SERVER_WORD); typed by hand it is refused (status 2) unless
descriptor 0 is a listening socket.
*/

#include "telnetd.h"

#define WAIT_MS 3000                    /* how long on and off wait for the server */
#define STEP_MS 20
#define SCANS   3                       /* looks through the process list before "none" */

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
    int i, r = 0, k, me = getpid();
    for(k = 0; k < SCANS && r >= 0; k++) {
        for(i = 0; (r = task(i, &pi)) > 0; i++) {
            if((pi.flags & PROC_USER) && (pi.flags & PROC_DETACHED) && pi.state != PROC_DEAD && pi.pid != me &&
               !strcmp(pi.name, "telnetd")) return pi.pid;
        }
    }
    return r < 0 ? -1 : 0;
}

/* the task pid as the list has it: 1 and *pi while it lives, 0 when it is gone */
static int find_task(int pid, struct procinfo *pi) {
    int i, k;
    for(k = 0; k < SCANS; k++)
        for(i = 0; task(i, pi) > 0; i++) if(pi->pid == pid) return pi->state != PROC_DEAD;
    return 0;
}

/* a line for the kernel's log (dmesg) */
static void klog(const char *fmt, ...) {
    char line[96];
    va_list ap;
    int fd, n;
    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line) - 1, fmt, ap);
    va_end(ap);
    if(n < 0) return;
    if(n > (int)sizeof(line) - 2) n = (int)sizeof(line) - 2;
    line[n++] = '\n';
    if((fd = open("/dev/kmsg", O_WRONLY)) >= 0) {
        write(fd, line, (size_t)n);
        close(fd);
    }
}

/* a port number as typed: digits only (no sign, no blanks), 1-65535; 0 when it is none */
static int port_of(const char *word) {
    long v = 0;
    const char *p;
    if(!*word || strlen(word) > 5) return 0;
    for(p = word; *p; p++) {
        if(!isdigit((unsigned char)*p)) return 0;
        v = v * 10 + (*p - '0');
    }
    return v >= 1 && v <= 65535 ? (int)v : 0;
}

/* is descriptor fd a listening socket? A socket (sockfs) that would not take a byte (a
   connection would: POLLOUT), as telnetd on passes it to the server */
static int listening(int fd) {
    struct stat st;
    struct pollfd p;
    if(fstat(fd, &st) < 0 || strcmp(st.fs, "sockfs")) return 0;
    p.fd = fd;
    p.events = POLLOUT;
    p.revents = 0;
    return poll(&p, 1, 0) == 0;
}

struct shell {
    int pid, pty;
    char name[16];
};

/* the server's session shells: the tasks on a pseudo-terminal whose parent it is; into sh (PTY_MAX
   at most), how many. A task collected while the list is read (by index) shifts the rest and can
   hide one, so the list is read SCANS times and the reading that saw the most is kept. */
static int shells_of(int server, struct shell *sh) {
    struct shell cur[PTY_MAX];
    struct procinfo pi;
    int i, k, n, best = -1;
    for(k = 0; k < SCANS; k++) {
        for(i = n = 0; task(i, &pi) > 0; i++) {
            if(pi.ppid != server || pi.tty < PROC_TTY_PTY || pi.state == PROC_DEAD) continue;
            if(n < PTY_MAX) {
                cur[n].pid = pi.pid;
                cur[n].pty = pi.tty - PROC_TTY_PTY;
                memcpy(cur[n].name, pi.name, sizeof(cur[n].name));
                cur[n].name[sizeof(cur[n].name) - 1] = 0;
            }
            n++;
        }
        if(n > best) {
            best = n;
            memcpy(sh, cur, sizeof(cur));
        }
    }
    return best > PTY_MAX ? PTY_MAX : best;
}

static int cmd_status(void) {
    struct shell sh[PTY_MAX];
    int pid = find_server(), n, i;
    if(pid < 0) return failed("procinfo");
    if(!pid) {
        printf("telnetd: not running (telnetd on starts it)\n");
        return 0;
    }
    n = shells_of(pid, sh);
    printf("telnetd: running (pid %d), %d session shell%s%s\n", pid, n, n == 1 ? "" : "s", n ? ":" : "");
    for(i = 0; i < n; i++) printf("    pty%-2d %s (pid %d)\n", sh[i].pty, sh[i].name, sh[i].pid);
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

    if(word && !(port = port_of(word))) {
        fprintf(stderr, "telnetd: %s is no port (1-65535)\n", word);
        return 2;
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
        if(errno != EADDRINUSE) return failed("tcp_listen");
        if((pid = find_server()) > 0) {        /* started meanwhile, or missed by the search above */
            printf("telnetd: running already (pid %d); telnetd off stops it\n", pid);
            return 1;
        }
        fprintf(stderr, "telnetd: port %d is in use\n", port);
        return 1;
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
    struct shell sh[PTY_MAX];
    int pid = find_server(), n, k, left, waited, e;
    struct procinfo pi;
    if(pid < 0) return failed("procinfo");
    if(!pid) {
        printf("telnetd: not running\n");
        return 1;
    }
    n = shells_of(pid, sh);
    /* the kernel's log says it before the kill (the server's own log ends with it): a telnetd
       off typed in a telnet session hangs up with that session as the server ends */
    klog("telnetd: telnetd off stops the server (pid %d, %d session shell%s)", pid, n, n == 1 ? "" : "s");
    if(kill(pid) < 0) {
        e = errno;
        klog("telnetd: telnetd off could not stop pid %d: %s", pid, strerror(e));
        fprintf(stderr, "telnetd: cannot stop pid %d: %s\n", pid, strerror(e));
        return 1;
    }
    /* the server, then its sessions' shells (they end as their pseudo-terminals hang up) */
    for(waited = 0; waited < WAIT_MS; waited += STEP_MS) {
        left = find_task(pid, &pi);
        for(k = 0; k < n; k++) left += find_task(sh[k].pid, &pi);
        if(!left) break;
        msleep(STEP_MS);
    }
    if(find_task(pid, &pi)) {
        fprintf(stderr, "telnetd: the server (pid %d) did not stop\n", pid);
        return 1;
    }
    printf("telnetd: stopped (%d session shell%s ended)\n", n, n == 1 ? "" : "s");
    return 0;
}

int main(int argc, char **argv) {
    if(argc >= 2 && !strcmp(argv[1], SERVER_WORD)) {
        int port = argc == 3 ? port_of(argv[2]) : 0;
        if(!port || !listening(0)) {
            fprintf(stderr, "telnetd: %s is how telnetd on starts the server; type telnetd on\n", SERVER_WORD);
            return 2;
        }
        return serve(0, port);
    }
    if(argc == 1 || (argc == 2 && !strcmp(argv[1], "status"))) return cmd_status();
    if(!strcmp(argv[1], "on") && argc <= 3) return cmd_on(argc == 3 ? argv[2] : NULL);
    if(!strcmp(argv[1], "off") && argc == 2) return cmd_off();
    fprintf(stderr, "usage: telnetd [on [port] | off | status]\n");
    return 2;
}
