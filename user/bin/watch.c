/*
SurfOS user program: watch
--------------------
File: watch.c   Date: 11/01/26
--------------------
watch [-n seconds] [-t] command...: runs the command again and again, every 2 seconds (-n, down
to 0.1) from the end of one run to the start of the next, and shows what it printed on the
alternate screen, with a title row saying what runs and when it last did (-t: none). q or Ctrl+C
ends it, also in the middle of a run, and the shell's screen comes back. Standard output must be
a terminal; a pipeline has no use for a screen, so it is refused (status 2).

The words of the command are joined with blanks and given to surfsh -c, so kernel commands,
pipelines and variables work; a pipeline must be quoted, or the shell it was typed at takes it.
One command is one program argument: 255 bytes at most (surfos/spawn.h).

A run is a job of its own (surfos/job.h): nothing of it can read the keys that are watch's, and
JOB_KILL ends all of it, however many programs a pipeline made, at q and at the end of every run:
what a run left in the background would otherwise stay, one task a run, each holding its pipe's end
until PIPE_MAX runs have used up the machine's pipes. The kernel's run starts its program in no job,
so that alone outlives a run, and q. A run's standard input is /dev/null,
and what it prints comes through one pipe, 1 and 2 alike. watch never waits for the command in a
call that a key could not end: it polls the keys and the pipe together, and a command that prints
without end or never ends is shown as far as it got, a second at a time. What was printed past
the last row is read and dropped (watchtext.h), so the command runs as it would anywhere.

Status 0 after q or Ctrl+C, 2 for a mistake or when a run cannot start.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <surf.h>
#include "watchtext.h"

#define SURFSH  "/initrd/bin/surfsh"
#define CTRL_C  3
#define SHOW_MS 1000                    /* how often a run in progress is shown */
#define LOOK_MS 100                     /* how often a run with its pipe quiet is looked at */

static struct wscreen scr;
static char text[WATCH_TEXT_MAX + 1];   /* the command */
static char *rowbuf;                    /* a row's bytes */
static unsigned int every_ms = 2000;
static int titled = 1, utf8;
static int keys = -1, devnull = -1, raw_mode;
static int kid, kid_ended;              /* the run: its first pid, which is its job's id too */
static int out_fd = -1;                 /* the pipe from it */
static struct tv_keys kstate;

static void leave_screen(void) {
    static const char bye[] = TV_LEAVE;
    if(!raw_mode) return;
    raw_mode = 0;
    write(1, bye, sizeof(bye) - 1);
    ttyctl(TTY_RAW, 0);
}

/* The rows the text has take the screen's size at the start of a run. 0, or -1 without memory */
static int fit_screen(void) {
    int top = titled ? 2 : 0;
    if(!tv_resize(ttyctl(TTY_GETSIZE, 0)) && scr.cells) {
        ws_clear(&scr);
        return 0;
    }
    free(rowbuf);
    rowbuf = malloc((size_t)tv_cols * 4 + 256);
    if(!rowbuf || ws_size(&scr, tv_rows - top > 1 ? tv_rows - top : 1, tv_cols) < 0) return -1;
    scr.utf8 = utf8;
    return 0;
}

static int start_run(void) {
    struct spawnx o;
    char *av[4];
    int p[2];
    if(fit_screen() < 0) {
        errno = ENOMEM;
        return -1;
    }
    if(pipe(p) < 0) return -1;
    av[0] = "surfsh";
    av[1] = "-c";
    av[2] = text;
    av[3] = NULL;
    memset(&o, 0, sizeof(o));
    o.fds[0] = devnull;
    o.fds[1] = p[1];
    o.fds[2] = p[1];
    o.fds[3] = SPAWN_CLOSED;
    o.envp = environ;
    o.job = SPAWN_JOB_NEW;
    o.flags = SPAWN_BG;                 /* never the pager's: what it prints to the console is a crash report */
    kid = spawnx(SURFSH, av, &o);
    close(p[1]);
    if(kid < 0) {
        close(p[0]);
        return -1;
    }
    kid_ended = 0;
    out_fd = p[0];
    return 0;
}

/* a run is over: nothing of it stays. JOB_KILL says so when nothing was left, and there is nothing more to do then */
static void end_run(void) {
    jobctl(JOB_KILL, kid);
    if(out_fd >= 0) close(out_fd);
    out_fd = -1;
    kid = 0;
}

static void draw(void) {
    int y, top = titled ? 2 : 0;
    if(titled) {
        time_t now = time(NULL);
        struct tm tm;
        char when[32];
        localtime_r(&now, &tm);
        strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tm);
        tv_row(0, rowbuf, ws_title(every_ms, text, when, tv_cols, utf8, rowbuf, (size_t)tv_cols * 4 + 256));
        tv_row(1, "", 0);
    }
    for(y = 0; y < scr.rows && y + top < tv_rows; y++)
        tv_row(y + top, rowbuf, ws_text(&scr, y, y + top == tv_rows - 1 ? tv_cols - 1 : tv_cols, rowbuf));
    tv_flush();
}

/* what the pipe has now */
static void read_output(void) {
    char buf[4096];
    ssize_t n = read(out_fd, buf, sizeof(buf));
    if(n < 0 && errno == EINTR) return;
    if(n > 0) {
        ws_feed(&scr, buf, (size_t)n);
        return;
    }
    close(out_fd);                      /* the end of what it prints */
    out_fd = -1;
}

/* the keys there are: 1 when watch is to end, also when no key can ever come (the terminal is gone) */
static int read_keys(void) {
    unsigned char buf[32];
    ssize_t n = read(keys, buf, sizeof(buf)), i;
    if(n < 0 && errno == EINTR) return 0;
    if(n <= 0) return 1;
    for(i = 0; i < n; i++) {
        int k = tv_key(&kstate, buf[i]);
        if(k == 'q' || k == 'Q' || k == CTRL_C) return 1;
    }
    return 0;
}

/* Runs until a key ends it: 0, or -1 with errno when a run cannot start. next: when the next run is due. Once the run's
   first program has ended, whatever it left in the pipe is read before the run counts as over, and no longer: a program
   it left behind in the background may hold the pipe open for good. */
static int loop(void) {
    unsigned int next = uptime(), shown = next;
    int running = 0, runs = 0;
    for(;;) {
        struct pollfd p[2];
        unsigned int now = uptime();
        int was_ended = running && kid_ended, timeout;
        if(!running && (int)(now - next) >= 0) {
            if(start_run() < 0) return -1;
            running = 1;
            shown = runs++ ? now : now - SHOW_MS;       /* the first time the title shows at once */
        }
        p[0].fd = keys;
        p[1].fd = running ? out_fd : -1;
        p[0].events = p[1].events = POLLIN;
        p[0].revents = p[1].revents = 0;
        timeout = was_ended ? 0 : running ? LOOK_MS : (int)(next - now);
        if(poll(p, 2, timeout) < 0 && errno != EINTR) return -1;
        if((p[0].revents & POLLIN) && read_keys()) return 0;
        if(!running) continue;
        if(out_fd >= 0 && (p[1].revents & (POLLIN | POLLHUP))) read_output();
        if(!kid_ended) {
            struct waitx w;
            int r = waitx(kid, &w, WNOHANG);
            if(r == kid || (r < 0 && errno != EINTR)) kid_ended = 1;
        }
        if(was_ended && !(p[1].revents & POLLIN)) {
            end_run();
            running = 0;
            ws_finish(&scr);
            draw();
            next = uptime() + every_ms;
        } else if(uptime() - shown >= SHOW_MS) {
            shown = uptime();
            draw();
        }
    }
}

static int usage(void) {
    fprintf(stderr, "usage: watch [-n seconds] [-t] command...\n");
    return 2;
}

int main(int argc, char **argv) {
    int i, caps, r;
    for(i = 1; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        const char *o = argv[i];
        if(!strcmp(o, "--")) {
            i++;
            break;
        }
        if(o[1] == 't' && !o[2]) titled = 0;
        else if(o[1] == 'n') {
            const char *value = o[2] ? o + 2 : ++i < argc ? argv[i] : NULL;
            if(!value) return usage();
            if(tv_seconds(value, &every_ms) < 0) {
                fprintf(stderr, "watch: %s: no time to wait (seconds, 0.1 to 86400)\n", value);
                return 2;
            }
        } else return usage();
    }
    if(i >= argc) return usage();
    if(ws_join(argc - i, argv + i, text, sizeof(text)) < 0) {
        fprintf(stderr, "watch: the command is longer than %d bytes\n", WATCH_TEXT_MAX);
        return 2;
    }
    if(!isatty(1)) {
        fprintf(stderr, "watch: standard output is no terminal\n");
        return 2;
    }
    keys = open("/dev/tty", O_RDONLY);
    devnull = open("/dev/null", O_RDWR);
    if(keys < 0 || devnull < 0) {
        fprintf(stderr, "watch: %s: %s\n", keys < 0 ? "/dev/tty" : "/dev/null", strerror(errno));
        return 2;
    }
    caps = ttyctl(TTY_CAPS, 0);
    utf8 = caps > 0 && (caps & TTY_CAP_UTF8);
    if(ttyctl(TTY_RAW, 1) < 0) {
        fprintf(stderr, "watch: raw mode: %s\n", strerror(errno));
        return 2;
    }
    raw_mode = 1;
    tv_str(TV_ENTER);
    tv_flush();
    r = loop();
    i = errno;
    if(kid > 0) end_run();
    leave_screen();
    if(r < 0) {
        fprintf(stderr, "watch: %s\n", strerror(i));
        return 2;
    }
    return 0;
}
