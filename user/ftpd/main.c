/*
SurfOS file transfer server: ftpd
--------------------
File: main.c    Date: 10/24/26
--------------------
ftpd, the file transfer server: other computers on the network (the Mac's Finder and
curl, Python's ftplib, FTP programs such as Cyberduck and FileZilla) copy files from and to
a folder of SurfOS over FTP. Any user name and password are taken: there are no logins and
nothing is encrypted, as with telnetd.

    ftpd on [-r] [-a address] [folder] [port] [first-last]
                                  start the server in the background: the folder (by default
                                  /, every mounted volume at its own path) on the port (21 by
                                  default), its passive data connections on the ports first
                                  to last (50000-50999 by default). -r serves it read-only;
                                  -a gives the address passive replies name, for a server that
                                  clients reach through a router forwarding its ports (or
                                  QEMU's user network: -a 127.0.0.1), rather than the one they
                                  reached. The words come in any order: -r, -a and its address,
                                  a word of digits the port, two numbers joined by a '-' the
                                  range, anything else the folder (a relative one taken from
                                  the current directory). A
                                  server that runs already is said with its pid (status 1).
                                  The port is taken here, so "in use" is said at once; the
                                  server (server.c) gets it as its descriptor 0. Then the
                                  address to give a client, or that the interface has none yet
                                  (run dhcp: the server listens all the same). Started in a
                                  telnet session, the server is on the session's
                                  pseudo-terminal and ends with it: said too.
    ftpd serve [-r] [-a address] [folder] [port] [first-last]
                                  the same server on this console, not in the background: each
                                  command a client sends is printed (passwords as ****), with its
                                  reply's code, until Ctrl+C.
    ftpd off                      stop the background server: "stopped" once it is gone; the
                                  kernel's log says it before the server stops; status 1 when
                                  it was not running.
    ftpd [status]                 whether the background server runs, and its pid.

The running server is found in the process list (procinfo): a detached user task named
ftpd, not this one; the list is read by index, and a task collected meanwhile shifts the
rest, so a search that finds nothing looks again (SCANS times in all). Statuses: 0 success,
1 failure, 2 usage. On a kernel without the network calls (ENOSYS) the commands say so.
"ftpd --server port folder rw|ro first-last [address]" is how ftpd on starts the server
(SERVER_WORD); typed by hand it is refused (status 2) unless descriptor 0 is a listening socket.
*/

#include "ftpd.h"

#define WAIT_MS 3000                    /* how long on and off wait for the server */
#define STEP_MS 20
#define SCANS   3                       /* looks through the process list before "none" */

static int usage(void) {
    fprintf(stderr, "usage: ftpd [on [-r] [-a address] [folder] [port] [first-last] | serve [the same] | off | status]\n");
    return 2;
}

/* a call that failed: said plainly, and which part of the kernel is missing when it is ENOSYS */
static int failed(const char *call) {
    if(errno == ENOSYS)
        fprintf(stderr, "ftpd: this kernel cannot run a file transfer server:\n"
                "    it has no %s() (ENOSYS), one of the server's system calls\n", call);
    else
        fprintf(stderr, "ftpd: %s: %s\n", call, strerror(errno));
    return 1;
}

/* task number i of the process list: 1 and *pi, 0 after the last, -1 with errno */
static int task(int i, struct procinfo *pi) {
    if(procinfo(i, pi) == 0) return 1;
    return errno == ENOENT ? 0 : -1;
}

/* the running background server's pid, 0 when there is none, -1 with errno */
static int find_server(void) {
    struct procinfo pi;
    int i, r = 0, k, me = getpid();
    for(k = 0; k < SCANS && r >= 0; k++) {
        for(i = 0; (r = task(i, &pi)) > 0; i++) {
            if((pi.flags & PROC_USER) && (pi.flags & PROC_DETACHED) && pi.state != PROC_DEAD && pi.pid != me &&
               !strcmp(pi.name, "ftpd")) return pi.pid;
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
static void klog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
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

/* is word digits only (no sign, no blanks)? */
static int digits(const char *word) {
    if(!*word) return 0;
    for(; *word; word++) if(!isdigit((unsigned char)*word)) return 0;
    return 1;
}

/* a port number as typed: digits only, 1-65535; 0 when it is none */
static int port_of(const char *word) {
    long v = 0;
    const char *p;
    if(!digits(word) || strlen(word) > 5) return 0;
    for(p = word; *p; p++) v = v * 10 + (*p - '0');
    return v >= 1 && v <= 65535 ? (int)v : 0;
}

/* "first-last", two ports: 1 and the range (first <= last), 0 when word is no such pair of numbers,
   -1 when it is one but no range of ports */
static int range_of(const char *word, int *first, int *last) {
    char a[8], b[8];
    const char *dash = strchr(word, '-');
    size_t n;
    if(!dash || dash == word || !dash[1] || (n = (size_t)(dash - word)) >= sizeof(a) || strlen(dash + 1) >= sizeof(b))
        return 0;
    memcpy(a, word, n);
    a[n] = 0;
    strcpy(b, dash + 1);
    if(!digits(a) || !digits(b)) return 0;
    *first = port_of(a);
    *last = port_of(b);
    return *first && *last && *first <= *last ? 1 : -1;
}

/* is descriptor fd a listening socket? A socket (sockfs) that would not take a byte (a
   connection would: POLLOUT), as ftpd on passes it to the server */
static int listening(int fd) {
    struct stat st;
    struct pollfd p;
    if(fstat(fd, &st) < 0 || strcmp(st.fs, "sockfs")) return 0;
    p.fd = fd;
    p.events = POLLOUT;
    p.revents = 0;
    return poll(&p, 1, 0) == 0;
}

/* word as an absolute folder without "." or "..", into out (FTP_PATH_MAX bytes): 0, or the status to end
   with when it is no folder ftpd may serve */
static int folder_of(const char *word, char *out) {
    char cwd[FTP_PATH_MAX];
    struct stat st;
    int r;
    if(word[0] == '/') r = ftp_path("/", word, out, FTP_PATH_MAX);
    else if(!getcwd(cwd, sizeof(cwd))) return failed("getcwd");
    else r = ftp_path(cwd, word, out, FTP_PATH_MAX);
    if(r < 0) {
        fprintf(stderr, "ftpd: %s: %s\n", word, r == -2 ? "a control character is no part of a folder's name" :
                "the path is too long");
        return 1;
    }
    if(ftp_reserved(out)) {
        fprintf(stderr, "ftpd: %s is not for serving (%s)\n", out, !strncasecmp(out, "/dev", 4) ? "the devices" :
                "the network manager's");
        return 1;
    }
    if(stat(out, &st) < 0) {
        if(errno == ENOENT || errno == ENOTDIR) fprintf(stderr, "ftpd: %s: no such folder\n", out);
        else fprintf(stderr, "ftpd: %s: %s\n", out, strerror(errno));
        return 1;
    }
    if(!S_ISDIR(st.st_mode)) {
        fprintf(stderr, "ftpd: %s is a file, not a folder\n", out);
        return 1;
    }
    return 0;
}

/* the folder, the port, the passive range, -r and -a of ftpd on or serve (the n words after it, in any order):
   0, or the status to end with */
static int where(int n, char **words, char *folder, struct ftpd_conf *c) {
    int i, folders = 0, ports = 0, ranges = 0, r;
    c->port = FTP_PORT;
    c->readonly = 0;
    c->pasv_first = PASV_FIRST;
    c->pasv_last = PASV_LAST;
    c->pasv_addr = 0;
    for(i = 0; i < n; i++) {
        const char *w = words[i];
        if(!strcmp(w, "-r")) {
            c->readonly = 1;
            continue;
        }
        if(!strcmp(w, "-a")) {          /* the address passive replies give, for a server behind a router */
            if(i + 1 == n || ipparse(words[i + 1], &c->pasv_addr) < 0 || !c->pasv_addr) {
                fprintf(stderr, "ftpd: -a needs an IPv4 address (the one clients reach this server at)\n");
                return 2;
            }
            i++;
            continue;
        }
        if(w[0] == '-') {
            fprintf(stderr, "ftpd: no option %s\n", w);
            return usage();
        }
        if(digits(w)) {
            if(ports++) return usage();
            if(!(c->port = port_of(w))) {
                fprintf(stderr, "ftpd: %s is no port (1-65535)\n", w);
                return 2;
            }
        } else if((r = range_of(w, &c->pasv_first, &c->pasv_last)) != 0) {
            if(ranges++) return usage();
            if(r < 0) {
                fprintf(stderr, "ftpd: %s is no range of ports (first-last, from 1 to 65535)\n", w);
                return 2;
            }
        } else {
            if(folders++) return usage();
            if((r = folder_of(w, folder))) return r;
        }
    }
    if(c->port >= c->pasv_first && c->port <= c->pasv_last) {
        fprintf(stderr, "ftpd: port %d is inside the passive range %d-%d\n", c->port, c->pasv_first, c->pasv_last);
        return 2;
    }
    return folders ? 0 : folder_of(DEFAULT_ROOT, folder);
}

/* where to point a client: the default interface's address */
static void say_address(const struct ftpd_conf *c) {
    struct netinfo ni;
    char ip[16], at[8] = "";
    const char *how = c->readonly ? "read-only" : "read-write";
    if(c->port != FTP_PORT) snprintf(at, sizeof(at), ":%d", c->port);
    if(netinfo(&ni) < 0) {
        printf("ftpd: serving %s on port %d (%s), but there is no network interface\n", c->root, c->port, how);
        return;
    }
    if(!ni.ip) {
        printf("ftpd: serving %s on port %d (%s), but %.8s has no address yet: run dhcp first\n", c->root, c->port, how,
               ni.name);
        return;
    }
    printf("ftpd: serving %s at ftp://%s%s/ (%s)\n", c->root, ipstr(ni.ip, ip), at, how);
}

/* the port, taken for a server: the listening descriptor, or -1 with what went wrong said */
static int take_port(int port) {
    int lfd = tcp_listen_backlog((unsigned short)port, BACKLOG), pid;
    if(lfd >= 0) return lfd;
    if(errno != EADDRINUSE) {
        failed("tcp_listen");
        return -1;
    }
    if((pid = find_server()) > 0) fprintf(stderr, "ftpd: port %d is in use (the server ftpd on started, pid %d, "
                                          "may have it: ftpd off stops it)\n", port, pid);
    else fprintf(stderr, "ftpd: port %d is in use\n", port);
    return -1;
}

static int cmd_status(void) {
    int pid = find_server();
    if(pid < 0) return failed("procinfo");
    if(!pid) printf("ftpd: not running (ftpd on starts it)\n");
    else printf("ftpd: running (pid %d)\n", pid);
    return 0;
}

static int cmd_on(int n, char **words) {
    char folder[FTP_PATH_MAX], portword[8], rangeword[16], addrword[16], *argv[8];
    struct ftpd_conf c;
    struct procinfo pi;
    int pid, lfd, nul, fds[3], waited, r, remote;

    memset(&c, 0, sizeof(c));
    if((r = where(n, words, folder, &c))) return r;
    c.root = folder;
    pid = find_server();
    if(pid < 0) return failed("procinfo");
    if(pid) {
        printf("ftpd: running already (pid %d); ftpd off stops it\n", pid);
        return 1;
    }
    if((lfd = take_port(c.port)) < 0) return 1;
    remote = find_task(getpid(), &pi) && pi.tty >= PROC_TTY_PTY;
    nul = open("/dev/null", O_RDWR);
    fds[0] = lfd;                       /* the server's 0: the port; 1 and 2: nothing to see */
    fds[1] = fds[2] = nul;
    snprintf(portword, sizeof(portword), "%d", c.port);
    snprintf(rangeword, sizeof(rangeword), "%d-%d", c.pasv_first, c.pasv_last);
    argv[0] = "ftpd";
    argv[1] = SERVER_WORD;
    argv[2] = portword;
    argv[3] = folder;
    argv[4] = c.readonly ? "ro" : "rw";
    argv[5] = rangeword;
    argv[6] = c.pasv_addr ? ipstr(c.pasv_addr, addrword) : NULL;
    argv[7] = NULL;
    pid = spawnfd(FTPD_PATH, argv, fds);
    if(pid < 0) failed("spawnfd");
    close(lfd);                         /* the server has its own reference */
    if(nul >= 0) close(nul);
    if(pid < 0) return 1;
    /* until it has left this shell: then status and off find it, and Ctrl+C here misses it */
    for(waited = 0; waited < WAIT_MS; waited += STEP_MS) {
        if(!find_task(pid, &pi)) {
            fprintf(stderr, "ftpd: the server (pid %d) ended at once\n", pid);
            return 1;
        }
        if(pi.flags & PROC_DETACHED) break;
        msleep(STEP_MS);
    }
    say_address(&c);
    if(remote) printf("ftpd: started in a telnet session, the server ends with the session "
                      "(ftpd on at one of the laptop's own consoles keeps it)\n");
    return 0;
}

static int cmd_serve(int n, char **words) {
    char folder[FTP_PATH_MAX];
    struct ftpd_conf c;
    int lfd, r;
    memset(&c, 0, sizeof(c));
    if((r = where(n, words, folder, &c))) return r;
    c.root = folder;
    c.verbose = 1;
    if((lfd = take_port(c.port)) < 0) return 1;
    say_address(&c);
    printf("ftpd: each command is printed here; Ctrl+C stops the server\n");
    fflush(stdout);
    setvbuf(stdout, NULL, _IOLBF, 0);
    return serve(lfd, &c);
}

static int cmd_off(void) {
    int pid = find_server(), waited, e;
    struct procinfo pi;
    if(pid < 0) return failed("procinfo");
    if(!pid) {
        printf("ftpd: not running\n");
        return 1;
    }
    klog("ftpd: ftpd off stops the server (pid %d)", pid);
    if(kill(pid) < 0) {
        e = errno;
        klog("ftpd: ftpd off could not stop pid %d: %s", pid, strerror(e));
        fprintf(stderr, "ftpd: cannot stop pid %d: %s\n", pid, strerror(e));
        return 1;
    }
    for(waited = 0; waited < WAIT_MS && find_task(pid, &pi); waited += STEP_MS) msleep(STEP_MS);
    if(find_task(pid, &pi)) {
        fprintf(stderr, "ftpd: the server (pid %d) did not stop\n", pid);
        return 1;
    }
    printf("ftpd: stopped\n");
    return 0;
}

int main(int argc, char **argv) {
    if(argc >= 2 && !strcmp(argv[1], SERVER_WORD)) {
        struct ftpd_conf c;
        int port = argc == 6 || argc == 7 ? port_of(argv[2]) : 0;
        memset(&c, 0, sizeof(c));
        if(!port || argv[3][0] != '/' || (strcmp(argv[4], "rw") && strcmp(argv[4], "ro")) ||
           range_of(argv[5], &c.pasv_first, &c.pasv_last) != 1 || (argc == 7 && ipparse(argv[6], &c.pasv_addr) < 0) ||
           !listening(0)) {
            fprintf(stderr, "ftpd: %s is how ftpd on starts the server; type ftpd on\n", SERVER_WORD);
            return 2;
        }
        c.root = argv[3];
        c.port = port;
        c.readonly = !strcmp(argv[4], "ro");
        return serve(0, &c);
    }
    if(argc == 1 || (argc == 2 && !strcmp(argv[1], "status"))) return cmd_status();
    if(!strcmp(argv[1], "on")) return cmd_on(argc - 2, argv + 2);
    if(!strcmp(argv[1], "serve")) return cmd_serve(argc - 2, argv + 2);
    if(!strcmp(argv[1], "off") && argc == 2) return cmd_off();
    return usage();
}
