/*
SurfOS web server: httpd
--------------------
File: main.c    Date: 10/17/26
--------------------
httpd, the web server: other computers on the network open the laptop's address in a
browser and get the pages of a folder, HTML, CSS, pictures, as a website. It only sends
files: no programs run on the server, nothing is uploaded and there are no logins.

    httpd on [folder] [port]      start the server in the background: the folder (by default
                                  /initrd/www, the sample site) on the port (80 by default).
                                  The folder and the port come in either order, a word of
                                  digits being the port; a relative folder is taken from the
                                  current directory. A server that runs already is said with
                                  its pid (status 1). The port is taken here, so "in use" is
                                  said at once; the server (server.c) gets it as its
                                  descriptor 0. Then the address to open, or that the
                                  interface has none yet (run dhcp: the server listens all
                                  the same). Started in a telnet session, the server is on
                                  the session's pseudo-terminal and ends with it: said too.
    httpd serve [folder] [port]   the same server on this console, not in the background:
                                  each request is printed (the client, the method, the
                                  target, the status, the bytes of the body) until Ctrl+C.
    httpd off                     stop the background server: "stopped" once it is gone; the
                                  kernel's log says it before the server stops; status 1 when
                                  it was not running.
    httpd [status]                whether the background server runs, and its pid.

The running server is found in the process list (procinfo): a detached user task named
httpd, not this one; the list is read by index, and a task collected meanwhile shifts the
rest, so a search that finds nothing looks again (SCANS times in all). Statuses: 0
success, 1 failure, 2 usage. On a kernel without the network calls (ENOSYS) the commands
say so. "httpd --server port folder" is how httpd on starts the server (SERVER_WORD);
typed by hand it is refused (status 2) unless descriptor 0 is a listening socket.
*/

#include "httpd.h"

#define WAIT_MS 3000                    /* how long on and off wait for the server */
#define STEP_MS 20
#define SCANS   3                       /* looks through the process list before "none" */

static int usage(void) {
    fprintf(stderr, "usage: httpd [on [folder] [port] | serve [folder] [port] | off | status]\n");
    return 2;
}

/* a call that failed: said plainly, and which part of the kernel is missing when it is ENOSYS */
static int failed(const char *call) {
    if(errno == ENOSYS)
        fprintf(stderr, "httpd: this kernel cannot run a web server:\n"
                "    it has no %s() (ENOSYS), one of the web server's system calls\n", call);
    else
        fprintf(stderr, "httpd: %s: %s\n", call, strerror(errno));
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
               !strcmp(pi.name, "httpd")) return pi.pid;
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

/* is descriptor fd a listening socket? A socket (sockfs) that would not take a byte (a
   connection would: POLLOUT), as httpd on passes it to the server */
static int listening(int fd) {
    struct stat st;
    struct pollfd p;
    if(fstat(fd, &st) < 0 || strcmp(st.fs, "sockfs")) return 0;
    p.fd = fd;
    p.events = POLLOUT;
    p.revents = 0;
    return poll(&p, 1, 0) == 0;
}

/* word as an absolute folder without "." or "..", into out (HTTP_PATH_MAX bytes): 0, or the status
   to end with when it is no folder */
static int folder_of(const char *word, char *out) {
    char cwd[HTTP_PATH_MAX], joined[2 * HTTP_PATH_MAX + 2];
    struct stat st;
    if(word[0] == '/') snprintf(joined, sizeof(joined), "%s", word);
    else {
        if(!getcwd(cwd, sizeof(cwd))) return failed("getcwd");
        snprintf(joined, sizeof(joined), "%s/%s", cwd, word);
    }
    if(http_normalize(joined, out, HTTP_PATH_MAX) < 0) {
        fprintf(stderr, "httpd: %s: the path is too long\n", word);
        return 1;
    }
    if(stat(out, &st) < 0) {
        if(errno == ENOENT || errno == ENOTDIR) fprintf(stderr, "httpd: %s: no such folder\n", out);
        else fprintf(stderr, "httpd: %s: %s\n", out, strerror(errno));
        return 1;
    }
    if(!S_ISDIR(st.st_mode)) {
        fprintf(stderr, "httpd: %s is a file, not a folder\n", out);
        return 1;
    }
    return 0;
}

/* the folder and the port of httpd on or serve (the n words after it, in either order): 0, or the status to
   end with */
static int where(int n, char **words, char *folder, int *port) {
    int i, folders = 0, ports = 0, r;
    *port = HTTP_PORT;
    for(i = 0; i < n; i++) {
        const char *w = words[i];
        if(w[0] == '-') {
            fprintf(stderr, "httpd: no option %s\n", w);
            return usage();
        }
        if(digits(w)) {
            if(ports++) return usage();
            if(!(*port = port_of(w))) {
                fprintf(stderr, "httpd: %s is no port (1-65535)\n", w);
                return 2;
            }
        } else {
            if(folders++) return usage();
            if((r = folder_of(w, folder))) return r;
        }
    }
    return folders ? 0 : folder_of(DEFAULT_ROOT, folder);
}

/* where to point a browser: the default interface's address */
static void say_address(const char *folder, int port) {
    struct netinfo ni;
    char ip[16], at[8] = "";
    if(port != HTTP_PORT) snprintf(at, sizeof(at), ":%d", port);
    if(netinfo(&ni) < 0) {
        printf("httpd: serving %s on port %d, but there is no network interface\n", folder, port);
        return;
    }
    if(!ni.ip) {
        printf("httpd: serving %s on port %d, but %.8s has no address yet: run dhcp first\n", folder, port, ni.name);
        return;
    }
    printf("httpd: serving %s at http://%s%s/\n", folder, ipstr(ni.ip, ip), at);
}

/* the port, taken for a server: the listening descriptor, or -1 with what went wrong said */
static int take_port(int port) {
    int lfd = tcp_listen_backlog((unsigned short)port, BACKLOG), pid;
    if(lfd >= 0) return lfd;
    if(errno != EADDRINUSE) {
        failed("tcp_listen");
        return -1;
    }
    if((pid = find_server()) > 0) fprintf(stderr, "httpd: port %d is in use (the server httpd on started, pid %d, "
                                          "may have it: httpd off stops it)\n", port, pid);
    else fprintf(stderr, "httpd: port %d is in use\n", port);
    return -1;
}

static int cmd_status(void) {
    int pid = find_server();
    if(pid < 0) return failed("procinfo");
    if(!pid) printf("httpd: not running (httpd on starts it)\n");
    else printf("httpd: running (pid %d)\n", pid);
    return 0;
}

static int cmd_on(int n, char **words) {
    char folder[HTTP_PATH_MAX], portword[8], *argv[] = { "httpd", SERVER_WORD, portword, folder, NULL };
    struct procinfo pi;
    int port, pid, lfd, nul, fds[3], waited, r, remote;

    if((r = where(n, words, folder, &port))) return r;
    pid = find_server();
    if(pid < 0) return failed("procinfo");
    if(pid) {
        printf("httpd: running already (pid %d); httpd off stops it\n", pid);
        return 1;
    }
    if((lfd = take_port(port)) < 0) return 1;
    remote = find_task(getpid(), &pi) && pi.tty >= PROC_TTY_PTY;
    nul = open("/dev/null", O_RDWR);
    fds[0] = lfd;                       /* the server's 0: the port; 1 and 2: nothing to see */
    fds[1] = fds[2] = nul;
    snprintf(portword, sizeof(portword), "%d", port);
    pid = spawnfd(HTTPD_PATH, argv, fds);
    if(pid < 0) failed("spawnfd");
    close(lfd);                         /* the server has its own reference */
    if(nul >= 0) close(nul);
    if(pid < 0) return 1;
    /* until it has left this shell: then status and off find it, and Ctrl+C here misses it */
    for(waited = 0; waited < WAIT_MS; waited += STEP_MS) {
        if(!find_task(pid, &pi)) {
            fprintf(stderr, "httpd: the server (pid %d) ended at once\n", pid);
            return 1;
        }
        if(pi.flags & PROC_DETACHED) break;
        msleep(STEP_MS);
    }
    say_address(folder, port);
    if(remote) printf("httpd: started in a telnet session, the server ends with the session "
                      "(httpd on at one of the laptop's own consoles keeps it)\n");
    return 0;
}

static int cmd_serve(int n, char **words) {
    char folder[HTTP_PATH_MAX];
    int port, lfd, r;
    if((r = where(n, words, folder, &port))) return r;
    if((lfd = take_port(port)) < 0) return 1;
    say_address(folder, port);
    printf("httpd: each request is printed here; Ctrl+C stops the server\n");
    return serve(lfd, port, folder, 1);
}

static int cmd_off(void) {
    int pid = find_server(), waited, e;
    struct procinfo pi;
    if(pid < 0) return failed("procinfo");
    if(!pid) {
        printf("httpd: not running\n");
        return 1;
    }
    klog("httpd: httpd off stops the server (pid %d)", pid);
    if(kill(pid) < 0) {
        e = errno;
        klog("httpd: httpd off could not stop pid %d: %s", pid, strerror(e));
        fprintf(stderr, "httpd: cannot stop pid %d: %s\n", pid, strerror(e));
        return 1;
    }
    for(waited = 0; waited < WAIT_MS && find_task(pid, &pi); waited += STEP_MS) msleep(STEP_MS);
    if(find_task(pid, &pi)) {
        fprintf(stderr, "httpd: the server (pid %d) did not stop\n", pid);
        return 1;
    }
    printf("httpd: stopped\n");
    return 0;
}

int main(int argc, char **argv) {
    if(argc >= 2 && !strcmp(argv[1], SERVER_WORD)) {
        int port = argc == 4 ? port_of(argv[2]) : 0;
        if(!port || argv[3][0] != '/' || !listening(0)) {
            fprintf(stderr, "httpd: %s is how httpd on starts the server; type httpd on\n", SERVER_WORD);
            return 2;
        }
        return serve(0, port, argv[3], 0);
    }
    if(argc == 1 || (argc == 2 && !strcmp(argv[1], "status"))) return cmd_status();
    if(!strcmp(argv[1], "on")) return cmd_on(argc - 2, argv + 2);
    if(!strcmp(argv[1], "serve")) return cmd_serve(argc - 2, argv + 2);
    if(!strcmp(argv[1], "off") && argc == 2) return cmd_off();
    return usage();
}
