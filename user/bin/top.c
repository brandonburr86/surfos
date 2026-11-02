/*
SurfOS user program: top
--------------------
File: top.c     Date: 11/01/26
--------------------
top [-d seconds] [-n count] [-s cpu|mem|pid]: the tasks and what each takes of the processor and
of memory, read again every 2 seconds (-d, down to 0.1), after count reports the end (-n, no end
without it). -s says the order to begin with: by processor (cpu, the default), memory (mem) or
pid. The first report's shares are since boot, the others' since the report before. The rows
and how they are worked out are in topmodel.h.

On a terminal this is a live view on the alternate screen, in raw mode, in the ANSI subset of
include/surfos/tty.h, so it works on a console, in the desktop's Terminal and over telnet. c, m
and p change the order, the arrows, PgUp, PgDn, Home and End move through a list longer than
the screen, Space reads again now, q and Ctrl+C end, and the shell's screen comes back (from the
console, when top is killed). After the last of count reports it stays for one more wait, so
that top -n 1 can be read. With standard output anything else, the same report as plain text
again and again, a blank line between them, and top ends when its reader goes, as a filter does.
The keys come from /dev/tty, so standard input can be anything.

Status 0, or 2 for a mistake in the options, a kernel that does not answer, or output that cannot be written.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <surf.h>
#include "topmodel.h"

#define CTRL_C 3

static struct top_list lists[2];        /* the reading on show and the one before */
static struct sysinfo infos[2];
static int current;                     /* which of them is the reading on show */
static int have_reading;
static int by = TOP_BY_CPU;
static int first_row;                   /* the live view's first row of the list */
static int keys = -1, raw_mode, utf8;
static struct tv_keys kstate;
static unsigned int delay_ms = 2000;

/* A new reading: 0, or -1 with errno. The clock first: a task's ticks can then only run ahead of it, and
   top_share() holds a share to 100%. */
static int reading(void) {
    int next = have_reading ? !current : current, i;
    struct top_list *now = &lists[next], *before = &lists[!next];
    struct sysinfo *si = &infos[next];
    struct procinfo pi;
    now->n = 0;
    if(sysinfo(si) < 0) return -1;
    for(i = 0; i < TOP_MAX_TASKS; i++) {
        if(procinfo(i, &pi) < 0) {
            if(errno == ENOENT) break;
            return -1;
        }
        if(top_add(now, &pi) < 0) {
            errno = ENOMEM;
            return -1;
        }
    }
    top_tidy(now);
    if(have_reading) {
        top_order(before, TOP_BY_PID);
        top_shares(now, before, si->ticks - infos[!next].ticks);
    } else top_shares(now, NULL, si->ticks);
    current = next;
    have_reading = 1;
    return 0;
}

/**** plain reports ****/

static void report(void) {
    struct top_list *l = &lists[current];
    struct sysinfo *si = &infos[current];
    char row[TOP_ROW_MAX];
    int i;
    top_order(l, by);
    top_summary(l, si, row, sizeof(row), TV_MAX_COLS);
    printf("%s\n", row);
    top_memory(si, row, sizeof(row), TV_MAX_COLS);
    printf("%s\n\n%s\n", row, TOP_HEADING);
    for(i = 0; i < l->n; i++) {
        top_row(&l->t[i], si, 1, row, sizeof(row), TV_MAX_COLS);
        printf("%s\n", row);
    }
    fflush(stdout);
}

static int reports(int count) {
    int i;
    for(i = 0; !count || i < count; i++) {
        if(i) {
            msleep(delay_ms);
            putchar('\n');
        }
        if(reading() < 0) {
            fprintf(stderr, "top: reading the tasks: %s\n", strerror(errno));
            return 2;
        }
        report();
        if(ferror(stdout)) {            /* a full disk, say: a report nobody gets is no reason to go on for ever */
            fprintf(stderr, "top: standard output: %s\n", strerror(errno));
            return 2;
        }
    }
    return 0;
}

/**** the live view ****/

static void leave_screen(void) {
    static const char bye[] = TV_LEAVE;
    if(!raw_mode) return;
    raw_mode = 0;
    write(1, bye, sizeof(bye) - 1);
    ttyctl(TTY_RAW, 0);
}

/* the rows the list has: below the two about the machine, an empty one and the heading, above the status row */
static int list_rows(void) {
    return tv_rows - 5 > 0 ? tv_rows - 5 : 0;
}

static void draw(void) {
    struct top_list *l = &lists[current];
    struct sysinfo *si = &infos[current];
    char row[TOP_ROW_MAX];
    int y, n;
    size_t len;
    tv_resize(ttyctl(TTY_GETSIZE, 0));
    n = list_rows();
    top_order(l, by);
    first_row = top_scroll(first_row, l->n, n);
    /* on a screen of a very few rows only the status row and what fits above it */
    if(tv_rows > 4) {
        len = top_summary(l, si, row, sizeof(row), tv_cols);
        tv_row(0, row, len);
        len = top_memory(si, row, sizeof(row), tv_cols);
        tv_row(1, row, len);
        tv_row(2, "", 0);
        snprintf(row, sizeof(row), "\x1b[7m%-*.*s\x1b[0m", tv_cols, tv_cols, TOP_HEADING);    /* across the whole width */
        tv_row(3, row, strlen(row));
    }
    for(y = 0; y < n; y++) {
        if(first_row + y < l->n) {
            len = top_row(&l->t[first_row + y], si, utf8, row, sizeof(row), tv_cols);
            tv_row(4 + y, row, len);
        } else tv_row(4 + y, "", 0);
    }
    len = top_status(first_row, l->n - first_row < n ? l->n - first_row : n, l->n, by, row, sizeof(row), tv_cols - 1);
    tv_row(tv_rows - 1, row, len);
    tv_flush();
}

/* one key: 1 when top is to end, 2 when it is to read again now */
static int key(int k) {
    int page = list_rows();
    switch(k) {
    case 'q': case 'Q': case CTRL_C: return 1;
    case ' ': return 2;
    case 'c': by = TOP_BY_CPU; break;
    case 'm': by = TOP_BY_MEM; break;
    case 'p': by = TOP_BY_PID; break;
    case TV_UP: first_row--; break;
    case TV_DOWN: first_row++; break;
    case TV_PGUP: first_row -= page; break;
    case TV_PGDN: first_row += page; break;
    case TV_HOME: first_row = 0; break;
    case TV_END: first_row = TOP_MAX_TASKS; break;     /* top_scroll() brings it back */
    default: return 0;
    }
    draw();
    return 0;
}

/* the keys for the length of a wait: as key(); 1 too when no key can ever come (the terminal is gone) */
static int wait_keys(unsigned int ms) {
    unsigned int t0 = uptime();
    for(;;) {
        struct pollfd p;
        unsigned char buf[32];
        unsigned int gone = uptime() - t0;
        ssize_t n, i;
        if(gone >= ms) return 0;
        p.fd = keys;
        p.events = POLLIN;
        p.revents = 0;
        if(poll(&p, 1, (int)(ms - gone)) <= 0 || !(p.revents & POLLIN)) continue;
        n = read(keys, buf, sizeof(buf));
        if(n < 0 && errno == EINTR) continue;
        if(n <= 0) return 1;
        for(i = 0; i < n; i++) {
            int r = key(tv_key(&kstate, buf[i]));
            if(r) return r;
        }
    }
}

static int live(int count) {
    int i, caps = ttyctl(TTY_CAPS, 0);
    static const char hello[] = TV_ENTER;
    utf8 = caps > 0 && (caps & TTY_CAP_UTF8);
    keys = open("/dev/tty", O_RDONLY);
    if(keys < 0) {
        fprintf(stderr, "top: /dev/tty: %s\n", strerror(errno));
        return 2;
    }
    if(ttyctl(TTY_RAW, 1) < 0) {
        fprintf(stderr, "top: raw mode: %s\n", strerror(errno));
        return 2;
    }
    raw_mode = 1;
    write(1, hello, sizeof(hello) - 1);
    for(i = 0; !count || i < count; ) {
        int r;
        if(reading() < 0) {
            leave_screen();
            fprintf(stderr, "top: reading the tasks: %s\n", strerror(errno));
            return 2;
        }
        draw();
        r = wait_keys(delay_ms);
        if(r == 1) break;
        i++;                            /* a report that Space cut short counts too: it was seen */
    }
    leave_screen();
    return 0;
}

static int usage(void) {
    fprintf(stderr, "usage: top [-d seconds] [-n count] [-s cpu|mem|pid]\n");
    return 2;
}

int main(int argc, char **argv) {
    long count = 0;
    int i;
    for(i = 1; i < argc; i++) {
        const char *o = argv[i], *value;
        char *end;
        if(!strcmp(o, "--")) {
            i++;
            break;
        }
        if(o[0] != '-' || !o[1]) break;
        if((o[1] != 'd' && o[1] != 'n' && o[1] != 's')) return usage();
        value = o[2] ? o + 2 : ++i < argc ? argv[i] : NULL;
        if(!value) return usage();
        if(o[1] == 'd') {
            if(tv_seconds(value, &delay_ms) < 0) {
                fprintf(stderr, "top: %s: no time to wait (seconds, 0.1 to 86400)\n", value);
                return 2;
            }
        } else if(o[1] == 'n') {
            count = strtol(value, &end, 10);
            if(!*value || *end || count < 1 || count > 1000000000) {
                fprintf(stderr, "top: %s: no count of reports (1 or more)\n", value);
                return 2;
            }
        } else if(top_order_name(value, &by) < 0) {
            fprintf(stderr, "top: %s: no order (cpu, mem or pid)\n", value);
            return 2;
        }
    }
    if(i < argc) return usage();
    return isatty(1) ? live((int)count) : reports((int)count);
}
