/*
SurfOS ring0 Shell for machine mode (riscv32)
--------------------
File: kshell.c  Date: 10/15/26 (SurfCPU), after shell/main.c (C)2004 Brandon Burr
--------------------
The kernel shell of shell/main.c for the machine-mode port: the same line handling
(words, $? as the last status, a status for every command: shell/shell.h), the same
prompt (shell/prompt.c, elevated: "SURF ≋#>"), paging (TTY_PAGE), Ctrl+C, the same
help layout and the same output for every command it shares with the x86 shell. The
file system commands of shell/fscmd.c and shell/fdisk.c are linked unchanged.

What it leaves out is PC hardware and what machine mode cannot do (surfcpu/docs/
PORTING-RISCV.md): PCI, USB, Wi-Fi, ACPI, the network, the framebuffer's graphics, the
PC speaker demos, user programs (run, spawn, die) and the x86 exception tests, which
become RISC-V ones here. The board adds its own commands (boards/<board>/board.c).

shell/main.c's small commands are static there, so the ones shared are repeated here
with the same words; a change to one belongs in both.

10/16/26 (milestone 8, files that persist): the card is taken out of an Arty, or the board
switched off, without a word. So a command that changes a file system (write, append, cp,
mv, rm, mkdir, rmdir, fsbench) is followed by a sync of every mounted volume, one at a time:
at the prompt what it did is on the card, and a write the card refuses is that command's
failure, said on its console with the volume it was for, not a surprise at the next eject.
A volume whose disk was taken out before the command began is left out (that was said when
it went); one taken out during the command is a failure. append adds a line to a file (this
shell has no redirection).
*/

#include <blibc_common.h>
#include <stdarg.h>
#include <surfos/types.h>
#include <surfos/kernel.h>
#include <surfos/timer.h>
#include <surfos/ktimer.h>
#include <surfos/task.h>
#include <surfos/console.h>
#include <surfos/tty.h>
#include <surfos/system.h>
#include <surfos/klog.h>
#include <surfos/interrupt.h>
#include <surfos/multiboot.h>
#include <surfos/cpu.h>
#include <surfos/version.h>
#include <surfos/prompt.h>
#include <surfos/kcmd.h>
#include <sys/driver.h>
#include <sys/bdev.h>
#include <fs/vfs.h>
#include <fs/bcache.h>
#include <mm/kalloc.h>
#include <mm/memory.h>
#include <riscv/csr.h>
#include <riscv/board.h>
#include <riscv/arch.h>
#include "../../shell/shell.h"

static const struct command commands[];
static const u_int ncommands;

/* 10/16/26: a command that changes a file system: the shell syncs them all after it (above) */
#define KCMD_SYNC 0x100
#define SYNC_MOUNTS 16                          /* the mounts it looks at; more are synced together */

u64 time_now(const char **source);             /* arch/riscv32/time.c */

/* a question on this task's console and its answer (as shell/main.c) */
void shell_ask(const char *question, char *answer, size_t size) {
    struct kcapture *kept = curTask->kcap;
    struct tty *tty = curTask->tty ? curTask->tty : tty_active;
    console_capture_end();
    if(console_paging(curTask->con)) console_page_new(curTask->con);
    printf("%s", question);
    memset(answer, 0, size);
    if(tty_readline(tty, answer, size) < 0 && !(curTask->intr & TI_INT)) printf("\n");
    if(kept) console_capture_begin(kept);
}

void shell_ask_secret(const char *question, char *answer, size_t size) {
    struct kcapture *kept = curTask->kcap;
    struct tty *tty = curTask->tty ? curTask->tty : tty_active;
    console_capture_end();
    if(console_paging(curTask->con)) console_page_new(curTask->con);
    printf("%s", question);
    memset(answer, 0, size);
    if(tty_readline_secret(tty, answer, size) < 0 && !(curTask->intr & TI_INT)) printf("\n");
    if(kept) console_capture_begin(kept);
}

/**** small commands ****/

static void help_line(const struct command *c) {
    printf("    %-9s %-14s %s\n", c->name, c->args, c->help);
}

static int cmd_help(int argc, char **argv) {
    const struct command *b = board_commands();
    u_int i;
    if(argc > 1) {
        for(i = 0; i < ncommands; i++) {
            if(!strcmp(commands[i].name, argv[1])) {
                printf("    %s %s\n    %s\n", commands[i].name, commands[i].args, commands[i].help);
                return CMD_OK;
            }
        }
        for(; b && b->name; b++) {
            if(!strcmp(b->name, argv[1])) {
                printf("    %s %s\n    %s\n", b->name, b->args, b->help);
                return CMD_OK;
            }
        }
        printf("    no such command: %s\n", argv[1]);
        return CMD_FAIL;
    }
    printf("\n    %s kernel shell %s\n", SURFOS_NAME, SURFOS_VERSION);
    printf("    -------------------------------\n");
    for(i = 0; i < ncommands; i++) help_line(&commands[i]);
    if(b && b->name) {
        printf("    ---- %s ----\n", board_name());
        for(; b->name; b++) help_line(b);
    }
    printf("    -------------------------------\n");
    printf("    (C)%s %s.\n\n", SURFOS_YEARS, SURFOS_AUTHOR);
    return CMD_OK;
}

static int cmd_clear(int argc, char **argv) { printf("\033[0m\033[?25h\033[2J\033[H"); return CMD_OK; }

static int cmd_echo(int argc, char **argv) {
    int i;
    for(i = 1; i < argc; i++) printf("%s%s", argv[i], i + 1 < argc ? " " : "");
    printf("\n");
    return CMD_OK;
}

static int cmd_reboot(int argc, char **argv) {
    printf("\n    Please wait... rebooting...");
    reboot();
    return CMD_FAIL;
}

static int cmd_tick(int argc, char **argv) {
    printf("\n    Ticks: %lu\n\n", getticks());
    return CMD_OK;
}

static int cmd_uptime(int argc, char **argv) {
    u_long ms = uptime_ms();
    printf("    up %lu.%02lu s (%lu ticks at %i Hz), %u tasks, %u timers pending\n",
           ms / 1000, (ms % 1000) / 10, getticks(), HZ, task_count(), ktimers_count());
    printf("    mtime %llu at %lu Hz; heartbeat %lu\n", board_mtime(), (u_long)board_timebase_hz(), heartbeat_count);
    return CMD_OK;
}

static int cmd_date(int argc, char **argv) {
    time_t t;
    const char *source;
    time_now(&source);
    get_time(&t);
    printf("    %04u-%02u-%02u %02u:%02u:%02u (%s)\n", t.year, t.month, t.date, t.hour, t.minute, t.second, source);
    return CMD_OK;
}

static bool number_arg(const char *s, u_long *out) {
    char *end;
    if(!*s || *s == '-' || *s == '+') return false;
    *out = strtoul(s, &end, 10);
    return !*end;
}

static int cmd_sleep(int argc, char **argv) {
    u_long ms = 1000, t0;
    int plain = argc > 1 && !strcmp(argv[1], "-u"), r = 0;
    if(argc > 2 + plain || (argc == 2 + plain && !number_arg(argv[1 + plain], &ms))) {
        printf("    usage: sleep [-u] [ms]   (milliseconds, 1000 when not given; -u: not cut short by a kill or Ctrl+C)\n");
        return CMD_USAGE;
    }
    t0 = getticks();
    if(plain) sleep_ms(ms);
    else r = sleep_intr(ms);
    if(r) printf("    sleep cut short after %lu ms\n", (getticks() - t0) * (1000 / HZ));
    else printf("    slept %lu ms (%lu ticks)\n", ms, getticks() - t0);
    return r ? CMD_FAIL : CMD_OK;
}

static int cmd_ps(int argc, char **argv) { print_tasks(); return CMD_OK; }

static int cmd_kill(int argc, char **argv) {
    surf_task *t;
    u_long pid;
    char *end;
    if(argc < 2) { printf("    usage: kill <pid>\n"); return CMD_USAGE; }
    pid = strtoul(argv[1], &end, 10);
    if(!argv[1][0] || *end) { printf("    usage: kill <pid>   (a number: ps lists them)\n"); return CMD_USAGE; }
    t = task_find(pid);
    if(!t) { printf("    no task with pid %lu\n", pid); return CMD_FAIL; }
    if(t == idle_task || t == init_task_ptr) { printf("    not that one\n"); return CMD_FAIL; }
    if(t->flags & TF_SYSTEM) {          /* 10/18/26 (audit B59): killed, it left its driver's lock held for good */
        printf("    '%s' (pid %lu) is one of the kernel's own threads (a driver needs it): not killed\n", t->name, pid);
        return CMD_FAIL;
    }
    printf("    killing '%s' (pid %lu)\n", t->name, pid);
    kill_task(t);
    return CMD_OK;
}

static int cmd_memstat(int argc, char **argv) { printMemInfo(); return CMD_OK; }

int rv_heaptest(void);                          /* rvtest.c */
static int cmd_heaptest(int argc, char **argv) { return rv_heaptest() ? CMD_FAIL : CMD_OK; }
static int cmd_selftest(int argc, char **argv) { return rv_selftest() ? CMD_FAIL : CMD_OK; }

static int cmd_dmesg(int argc, char **argv) { klog_dump(); return CMD_OK; }

static int cmd_irqstat(int argc, char **argv) {
    int i;
    printf("    IRQ  count\n");
    for(i = 0; i < NR_IRQS; i++) if(irq_count[i]) printf("    %3i  %lu\n", i, irq_count[i]);
    printf("    timer: %lu\n", timer_irq_count);
    printf("    spurious: %lu\n", spurious_irq_count);
    for(i = 0; i < NR_VECTORS; i++) if(trap_counts[i]) printf("    exception %i (%s): %lu\n", i, trap_name(i), trap_counts[i]);
    return CMD_OK;
}

static int cmd_bootinfo(int argc, char **argv) {
    char buf[160];
    bootinfo_print();
    printf("  screen: %s\n", console_describe(buf, sizeof(buf)));
    return CMD_OK;
}

static int cmd_cpuinfo(int argc, char **argv) { cpu_print(); return CMD_OK; }
static int cmd_drivers(int argc, char **argv) { print_drivers(); return CMD_OK; }

static int cmd_rawkeys(int argc, char **argv) {
    struct tty *t = curTask->tty;
    char buf[16];
    int n, i, reads, size = tty_ctl(t, TTY_GETSIZE, 0);
    tty_ctl(t, TTY_RAW, 1);
    printf("    raw mode (%dx%d): press keys, q ends\n    ", size >> 16, size & 0xFFFF);
    for(reads = 0; reads < 256; reads++) {
        n = tty_read(t, buf, sizeof(buf));
        if(n < 0) break;
        for(i = 0; i < n; i++) printf("%02x ", (u_char)buf[i]);
        if(memchr(buf, 'q', n)) break;
    }
    tty_ctl(t, TTY_RAW, 0);
    printf("\n");
    return CMD_OK;
}

static int cmd_ansi(int argc, char **argv) {
    int size = tty_ctl(curTask->tty, TTY_GETSIZE, 0), cols = size >> 16, rows = size & 0xFFFF;
    printf("\033[2J\033[HANSI-TEST top left");
    printf("\033[3;10H\033[7mINVERSE\033[0m normal");
    printf("\033[4;10H\033[1;33;44mYELLOW-ON-BLUE\033[m");
    printf("\033[5;10HKEEP-ERASED\033[5;14H\033[K");
    printf("\033[8;10HA\033[BB\033[2AC\033[3DD\033[5CE");
    printf("\033[11;10H\033[5n\033[?7h\033(B\033[ q\033[38;5;196mUNKNOWN-IGNORED\033[0m");
    printf("\033[%d;%dHZ", rows, cols);
    printf("\033[13;1H\033[?25l\033[?25hANSI-TEST done, %dx%d\n", cols, rows);
    return CMD_OK;
}

static int cmd_lsblk(int argc, char **argv) {
    if(argc > 2 || (argc == 2 && strcmp(argv[1], "-s"))) { printf("    usage: lsblk [-s]   (-s: blocks moved)\n"); return CMD_USAGE; }
    if(argc > 1) bdev_print_io();
    else bdev_print();
    return CMD_OK;
}

/**** tasks ****/

/* tasks: two kernel tasks that count, sleep and wake on this console, then end; the shell
   waits for both by pid (as the self test does), so other tasks are left alone */
struct ticker {
    const char *name;
    u_int rounds;
    u_long sleep;
    u_long count;
};

static void ticker_task(void *arg) {
    struct ticker *t = (struct ticker *)arg;
    u_int i;
    for(i = 0; i < t->rounds && !task_interrupted(); i++) {
        t->count++;
        printf("    %s: round %u of %u, tick %lu, sleeping %lu ms\n", t->name, i + 1, t->rounds, getticks(), t->sleep);
        sleep_ms(t->sleep);
    }
}

static int cmd_tasks(int argc, char **argv) {
    struct ticker a = { "task A", 4, 100, 0 }, b = { "task B", 3, 170, 0 };
    surf_task *ta, *tb;
    int sa = 0, sb = 0, pa, pb;
    u_long t0 = getticks();
    ta = kthread_create("task A", ticker_task, &a, PL_NORMAL, 0);
    tb = kthread_create("task B", ticker_task, &b, PL_NORMAL, 0);
    if(!ta || !tb) { printf("    tasks: no memory\n"); return CMD_FAIL; }
    pa = (int)ta->pid;
    pb = (int)tb->pid;
    printf("    started task A (pid %d) and task B (pid %d)\n", pa, pb);
    if(task_wait((u_long)pa, &sa) != pa || task_wait((u_long)pb, &sb) != pb) return CMD_FAIL;
    printf("    task A ran %lu rounds, task B %lu, in %lu ticks; both ended (status %d, %d)\n",
           a.count, b.count, getticks() - t0, sa, sb);
    return a.count == a.rounds && b.count == b.rounds ? CMD_OK : CMD_FAIL;
}

/**** scrolltest (10/16/26, audit C10) ****/

/* scrolltest [lines]: two kernel tasks print numbered lines on one console at once, as fast as
   they can, while it scrolls. Then every row of that console's cells (and of the screen, when it
   shows that console) must be one whole line, each task's lines running on by one to its last:
   what the console lock keeps (kernel/console.c). Before it, a task preempted inside a scroll let
   the other task's line land in rows the scroll then moved or cleared, and a line could be split
   by the other task's. Paging is off for it: its output is the test. */
#define SCROLL_TAIL "abcdefghijklmnopqrstuvwxyz0123456789"

extern surf_console conVideo;                   /* kernel/console.c: the cells the screen shows */

struct scroller {
    char name;
    u_int lines, done;
};

static void scroller_task(void *arg) {
    struct scroller *s = (struct scroller *)arg;
    u_int i;
    for(i = 1; i <= s->lines && !task_interrupted(); i++) {
        printf("scroll %c %03u " SCROLL_TAIL " end\n", s->name, i);
        s->done = i;
    }
}

/* row r of a grid as text, trailing blanks dropped */
static void grid_row(const u_char *g, u_int r, char *out) {
    u_int c;
    for(c = 0; c < COLUMNS; c++) out[c] = g[(r * COLUMNS + c) * 2] ? (char)g[(r * COLUMNS + c) * 2] : ' ';
    out[COLUMNS] = 0;
    for(c = COLUMNS; c > 0 && out[c - 1] == ' '; c--) out[c - 1] = 0;
}

/* All rows but the last: whole lines, each task's numbered on by one, ending at its last line;
   the last row, the cursor's, empty. 0, or 1 with the first bad row and what is wrong with it. */
static int scroll_check(const u_char *g, const struct scroller *s, char *why, size_t size) {
    char row[256], want[96];
    u_int r, num, last[2] = { 0, 0 };
    int k;
    for(r = 0; r < LINES; r++) {
        grid_row(g, r, row);
        if(r + 1 == LINES) {
            if(row[0]) { snprintf(why, size, "the cursor's row %u is not empty: '%s'", r, row); return 1; }
            break;
        }
        k = !strncmp(row, "scroll A ", 9) ? 0 : !strncmp(row, "scroll B ", 9) ? 1 : -1;
        num = k < 0 ? 0 : (u_int)strtoul(row + 9, NULL, 10);
        if(k >= 0) snprintf(want, sizeof(want), "scroll %c %03u " SCROLL_TAIL " end", s[k].name, num);
        if(k < 0 || strcmp(row, want)) { snprintf(why, size, "row %u is not a whole line: '%s'", r, row); return 1; }
        if(last[k] && num != last[k] + 1) {
            snprintf(why, size, "row %u: %c's line %u follows its line %u", r, s[k].name, num, last[k]);
            return 1;
        }
        last[k] = num;
    }
    for(k = 0; k < 2; k++) {
        if(last[k] && last[k] != s[k].lines) {
            snprintf(why, size, "%c's last line on the screen is %u of %u", s[k].name, last[k], s[k].lines);
            return 1;
        }
    }
    return 0;
}

static int cmd_scrolltest(int argc, char **argv) {
    struct scroller s[2] = { { 'A', 60, 0 }, { 'B', 60, 0 } };
    surf_task *t[2] = { NULL, NULL };
    surf_console *con = NULL;
    int st[2] = { 0, 0 }, pid[2] = { 0, 0 }, i, bad, line;
    u_long n = 60, t0;
    char why[200];
    if(argc > 2 || (argc == 2 && (!number_arg(argv[1], &n) || n < 1 || n > 999))) {
        printf("    usage: scrolltest [lines]   (each task's, 1 to 999; 60 when not given)\n");
        return CMD_USAGE;
    }
    s[0].lines = s[1].lines = (u_int)n;
    tty_ctl(curTask->tty, TTY_PAGE, 0);
    t0 = getticks();
    for(i = 0; i < 2; i++) {
        t[i] = kthread_create(i ? "scroll B" : "scroll A", scroller_task, &s[i], PL_NORMAL, 0);
        if(t[i]) pid[i] = (int)t[i]->pid;
        if(t[i] && !con) con = t[i]->con;      /* (a kernel thread prints on F1); read while it lives */
    }
    for(i = 0; i < 2; i++) if(t[i] && task_wait((u_long)pid[i], &st[i]) != pid[i]) return CMD_FAIL;
    if(!t[0] || !t[1]) { printf("    scrolltest: no memory for the tasks\n"); return CMD_FAIL; }
    line = console_print_begin(con);            /* nobody draws while the cells are read */
    bad = scroll_check(con->vmem, s, why, sizeof(why));
    if(!bad && con == conActive && memcmp(conVideo.vmem, con->vmem, COLUMNS * LINES * 2)) {
        bad = 1;
        snprintf(why, sizeof(why), "the screen's cells are not the console's");
    }
    console_print_end(line);
    printf("    scrolltest: 2 tasks x %u lines at once in %lu ticks, the console scrolling: %s\n", s[0].lines,
           getticks() - t0, bad ? why : "every row a whole line, each task's in order");
    printf("%s\n", bad ? "SCROLLTEST FAIL" : "SCROLLTEST PASS");
    return bad || s[0].done != s[0].lines || s[1].done != s[1].lines ? CMD_FAIL : CMD_OK;
}

/**** files (as shell/main.c) ****/

static void fmt_time(u32 t, char *buf, size_t size) {
    u32 days = t / 86400, rem = t % 86400;
    i32 z = (i32)days + 719468, era = (z >= 0 ? z : z - 146096) / 146097;
    u32 doe = (u32)(z - era * 146097);
    u32 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    i32 y = (i32)yoe + era * 400;
    u32 doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    u32 d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
    if(m <= 2) y++;
    if(!t) { strlcpy(buf, "-", size); return; }
    snprintf(buf, size, "%04d-%02u-%02u %02u:%02u:%02u", y, m, d, rem / 3600, (rem / 60) % 60, rem % 60);
}

static int cmd_ls(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : ".";
    struct file *f;
    struct dirent de;
    struct stat st;
    char when[24];
    u_int n = 0;
    u_long total = 0;
    int r = vfs_stat(path, &st);
    if(r) { printf("    ls: %s: %s\n", path, strerror(r)); return CMD_FAIL; }
    if(st.type != VN_DIR) {
        fmt_time(st.mtime, when, sizeof(when));
        printf("    - %8lu  %s  %s\n", (u_long)st.size, when, path);
        return CMD_OK;
    }
    r = vfs_open(path, O_RDONLY | O_DIRECTORY, &f);
    if(r) { printf("    ls: %s: %s\n", path, strerror(r)); return CMD_FAIL; }
    while(!task_interrupted() && (r = vfs_readdir(f, &de)) == 0) {   /* 10/18/26 (B73): a kill waits for the close */
        fmt_time(de.mtime, when, sizeof(when));
        printf("    %c %8lu  %-19s  %s%s\n", de.type == VN_DIR ? 'd' : '-', (u_long)de.size, when, de.name, de.type == VN_DIR ? "/" : "");
        n++;
        total += de.size;
    }
    if(r < 0) printf("    ls: %s\n", strerror(r));
    vfs_close(f);
    printf("    %u entries, %lu bytes\n", n, total);
    return r < 0 ? CMD_FAIL : CMD_OK;
}

static int cmd_cat(int argc, char **argv) {
    int i, status = CMD_OK;
    if(argc < 2) { printf("    usage: cat <file>...\n"); return CMD_USAGE; }
    for(i = 1; i < argc; i++) {
        struct file *f;
        char buf[256];
        int r = vfs_open(argv[i], O_RDONLY, &f), last = '\n';
        if(r) { printf("    cat: %s: %s\n", argv[i], strerror(r)); status = CMD_FAIL; continue; }
        while((r = vfs_read(f, buf, sizeof(buf))) > 0 && !task_interrupted()) {
            int j;
            for(j = 0; j < r; j++) {
                u_char c = (u_char)buf[j];
                if(c == '\n' || c == '\t' || (c >= 32 && c < 127) || c >= 0x80) printf("%c", c);
                else if(c != '\r') printf(".");
                last = c;
            }
        }
        if(r < 0) { printf("\n    cat: %s: %s", argv[i], strerror(r)); status = CMD_FAIL; }
        if(last != '\n') printf("\n");
        vfs_close(f);
    }
    return status;
}

static int cmd_cd(int argc, char **argv) {
    int r = vfs_chdir(argc > 1 ? argv[1] : "/");
    if(r) printf("    cd: %s: %s\n", argc > 1 ? argv[1] : "/", strerror(r));
    return r ? CMD_FAIL : CMD_OK;
}

static int cmd_pwd(int argc, char **argv) { printf("    %s\n", vfs_getcwd()); return CMD_OK; }

static int cmd_stat(int argc, char **argv) {
    struct stat st;
    char when[24];
    int r;
    if(argc < 2) { printf("    usage: stat <path>\n"); return CMD_USAGE; }
    r = vfs_stat(argv[1], &st);
    if(r) { printf("    stat: %s: %s\n", argv[1], strerror(r)); return CMD_FAIL; }
    fmt_time(st.mtime, when, sizeof(when));
    printf("    %s: %s, %lu bytes, modified %s, inode %lu on %s (%s)\n", argv[1], st.type == VN_DIR ? "directory" : "file",
           (u_long)st.size, when, (u_long)st.ino, st.dev[0] ? st.dev : "none", st.fs);
    return CMD_OK;
}

static int cmd_sync(int argc, char **argv) {
    u_long h, m, w;
    int r = vfs_sync();
    bcache_stats(&h, &m, &w);
    printf("    %s; block cache: %lu hits, %lu misses, %lu blocks written\n", r ? strerror(r) : "synced", h, m, w);
    return r ? CMD_FAIL : CMD_OK;
}

static int cmd_mkdir(int argc, char **argv) {
    int r;
    if(argc < 2) { printf("    usage: mkdir <dir>\n"); return CMD_USAGE; }
    r = vfs_mkdir(argv[1]);
    if(r) printf("    mkdir: %s: %s\n", argv[1], strerror(r));
    return r ? CMD_FAIL : CMD_OK;
}

static int cmd_rm(int argc, char **argv) {
    int i, status = CMD_OK;
    if(argc < 2) { printf("    usage: rm <file>...\n"); return CMD_USAGE; }
    for(i = 1; i < argc; i++) {
        int r = vfs_unlink(argv[i]);
        if(r) { printf("    rm: %s: %s\n", argv[i], strerror(r)); status = CMD_FAIL; }
    }
    return status;
}

static int cmd_rmdir(int argc, char **argv) {
    int r;
    if(argc < 2) { printf("    usage: rmdir <dir>\n"); return CMD_USAGE; }
    r = vfs_rmdir(argv[1]);
    if(r) printf("    rmdir: %s: %s\n", argv[1], strerror(r));
    return r ? CMD_FAIL : CMD_OK;
}

static int cmd_write(int argc, char **argv) {
    struct file *f;
    int r, c, i;
    if(argc < 3) { printf("    usage: write <file> <text>...\n"); return CMD_USAGE; }
    r = vfs_open(argv[1], O_WRONLY | O_CREAT | O_TRUNC, &f);
    if(r) { printf("    write: %s: %s\n", argv[1], strerror(r)); return CMD_FAIL; }
    for(i = 2; i < argc && r >= 0; i++) {
        r = vfs_write(f, argv[i], strlen(argv[i]));
        if(r >= 0) r = vfs_write(f, i + 1 < argc ? " " : "\n", 1);
    }
    if(r < 0) printf("    write: %s\n", strerror(r));
    c = vfs_close(f);
    if(c) printf("    write: close: %s\n", strerror(c));
    return r < 0 || c ? CMD_FAIL : CMD_OK;
}

/* append <file> <text>...: the words, space separated, plus a newline, at the end of the file
   (made when it is not there) */
static int cmd_append(int argc, char **argv) {
    struct file *f;
    int r, c, i;
    if(argc < 3) { printf("    usage: append <file> <text>...\n"); return CMD_USAGE; }
    r = vfs_open(argv[1], O_WRONLY | O_CREAT | O_APPEND, &f);
    if(r) { printf("    append: %s: %s\n", argv[1], strerror(r)); return CMD_FAIL; }
    for(i = 2; i < argc && r >= 0; i++) {
        r = vfs_write(f, argv[i], strlen(argv[i]));
        if(r >= 0) r = vfs_write(f, i + 1 < argc ? " " : "\n", 1);
    }
    if(r < 0) printf("    append: %s\n", strerror(r));
    c = vfs_close(f);
    if(c) printf("    append: close: %s\n", strerror(c));
    return r < 0 || c ? CMD_FAIL : CMD_OK;
}

/* crc32 <file>...: the CRC-32 of zlib (BOOT-ABI.md section 3) and the size, to compare a file
   with its copy on another computer (python3 -c 'import zlib...', or crc32 on macOS) */
static int cmd_crc32(int argc, char **argv) {
    int i, status = CMD_OK;
    char *buf;
    if(argc < 2) { printf("    usage: crc32 <file>...\n"); return CMD_USAGE; }
    buf = (char *)kalloc(4096);
    if(!buf) { printf("    crc32: out of memory\n"); return CMD_FAIL; }
    for(i = 1; i < argc; i++) {
        struct file *f;
        u32 crc = 0;
        u_long total = 0;
        int r = vfs_open(argv[i], O_RDONLY, &f);
        if(r) { printf("    crc32: %s: %s\n", argv[i], strerror(r)); status = CMD_FAIL; continue; }
        while((r = vfs_read(f, buf, 4096)) > 0 && !task_interrupted()) {
            crc = crc32_update(crc, buf, (u_long)r);
            total += (u_long)r;
        }
        vfs_close(f);
        if(r < 0) { printf("    crc32: %s: %s\n", argv[i], strerror(r)); status = CMD_FAIL; continue; }
        printf("    %08lx %8lu  %s\n", (u_long)crc, total, argv[i]);
    }
    kfree(buf);
    return status;
}

static int cmd_readbench(int argc, char **argv) {
    struct bdev *d;
    u_long mb, blocks, done = 0, t0, ms;
    u8 *buf;
    int r = 0;
    if(argc < 2) { printf("    usage: readbench <device> [MB]   (reads only; lsblk lists the devices)\n"); return CMD_USAGE; }
    d = bdev_find(argv[1]);
    if(!d) { printf("    no block device '%s'\n", argv[1]); return CMD_FAIL; }
    mb = argc > 2 ? strtoul(argv[2], NULL, 0) : 8;
    blocks = mb * 2048;
    if(blocks > d->nblocks) blocks = d->nblocks;
    buf = (u8 *)kalloc(65536);
    if(!buf) { printf("    readbench: out of memory\n"); return CMD_FAIL; }
    t0 = getticks();
    while(done < blocks && !task_interrupted()) {
        u_long n = blocks - done > 128 ? 128 : blocks - done;
        r = bdev_read(d, (u32)done, (u32)n, buf);
        if(r) {                                         /* 10/18/26 (audit B61): BDEV_EBUSY: another task's mkfs or fdisk */
            printf("    read of block %lu failed (%i%s)\n", done, r, r == BDEV_EBUSY ? ": mkfs or fdisk is changing it" : "");
            break;
        }
        done += n;
    }
    ms = (getticks() - t0) * (1000 / HZ);
    printf("    %lu KB from %s in %lu ms", done / 2, d->name, ms);
    if(ms) printf(": %lu KB/s", (done / 2) * 1000 / ms);
    printf("\n");
    kfree(buf);
    return r ? CMD_FAIL : CMD_OK;
}

static int cmd_hexdump(int argc, char **argv) {
    struct bdev *d;
    u_long lba, nblk, i, j;
    u8 *buf;
    int rc;
    if(argc < 2) { printf("    usage: hexdump <device> [block] [count]   (lsblk lists the devices)\n"); return CMD_USAGE; }
    d = bdev_find(argv[1]);
    if(!d) {
        struct file *f;
        u_long off = argc > 2 ? strtoul(argv[2], NULL, 0) : 0;
        int r = vfs_open(argv[1], O_RDONLY, &f);
        if(r) { printf("    no block device or file '%s' (%s)\n", argv[1], strerror(r)); return CMD_FAIL; }
        buf = (u8 *)kalloc(512);
        if(!buf) { printf("    hexdump: out of memory\n"); vfs_close(f); return CMD_FAIL; }
        vfs_lseek(f, (i32)off, SEEK_SET);
        r = vfs_read(f, buf, 512);
        for(i = 0; (int)i < r; i += 16) {
            printf("%08lx  ", off + i);
            for(j = 0; j < 16; j++) { if(i + j < (u_long)r) printf("%02x%s", buf[i + j], j == 7 ? "  " : " "); else printf("   %s", j == 7 ? " " : ""); }
            printf(" |");
            for(j = 0; j < 16 && i + j < (u_long)r; j++) printf("%c", (buf[i + j] >= 32 && buf[i + j] < 127) ? buf[i + j] : '.');
            printf("|\n");
        }
        if(r < 0) printf("    read failed: %s\n", strerror(r));
        kfree(buf);
        vfs_close(f);
        return r < 0 ? CMD_FAIL : CMD_OK;
    }
    lba = argc > 2 ? strtoul(argv[2], NULL, 0) : 0;
    nblk = argc > 3 ? strtoul(argv[3], NULL, 0) : 1;
    if(nblk < 1 || nblk > 8) { printf("    1 to 8 blocks at a time\n"); return CMD_USAGE; }
    buf = (u8 *)kalloc(nblk * d->block_size);
    if(!buf) { printf("    out of memory\n"); return CMD_FAIL; }
    rc = bdev_read(d, lba, nblk, buf);
    if(rc) printf("    read of %s block %lu failed (%i%s)\n", d->name, lba, rc,
                  rc == BDEV_EBUSY ? ": mkfs or fdisk is changing it" : "");   /* 10/18/26 (audit B61) */
    else for(i = 0; i < nblk * d->block_size; i += 16) {
        printf("%08lx  ", lba * d->block_size + i);
        for(j = 0; j < 16; j++) printf("%02x%s", buf[i + j], j == 7 ? "  " : " ");
        printf(" |");
        for(j = 0; j < 16; j++) printf("%c", (buf[i + j] >= 32 && buf[i + j] < 127) ? buf[i + j] : '.');
        printf("|\n");
    }
    kfree(buf);
    return rc ? CMD_FAIL : CMD_OK;
}

/**** RISC-V exceptions on purpose (x86's crashdiv, crashgp, crashnull, crashint) ****/

/* RISC-V division by zero does not trap: the quotient is all ones (ISA.md, the M extension) */
static int cmd_crashdiv(int argc, char **argv) {
    volatile int one = 1, zero = 0;
    printf("    1 / 0 = %i: RISC-V division by zero does not trap (the result is -1)\n", one / zero);
    return CMD_OK;
}

/* an illegal instruction: unimp, a write to the read-only cycle CSR (ISA.md section 2) */
static int cmd_crashill(int argc, char **argv) {
    asm volatile("unimp");
    return CMD_FAIL;
}

/* a word load from an odd address: SurfCPU traps misaligned accesses (QEMU may not) */
static int cmd_crashmis(int argc, char **argv) {
    volatile u32 *p = (volatile u32 *)((u_long)&p + 1);
    u32 v;
    asm volatile("lw %0, 0(%1)" : "=r"(v) : "r"(p));
    printf("    a misaligned load returned 0x%08lx: this machine allows them (QEMU does; SurfCPU traps)\n", (u_long)v);
    return CMD_OK;
}

/* a store to address 0: the boot ROM on SurfCPU (not writable), nothing on QEMU virt */
static int cmd_crashnull(int argc, char **argv) { *(volatile int *)0 = 1; return CMD_FAIL; }
/* 10/18/26 (audit B56): the same store while this task counts as inside a file system (as in a device request): the
   system stops (arch/riscv32/trap.c), since nothing could give back what a task held there */
static int cmd_crashfs(int argc, char **argv) { fs_enter(); *(volatile int *)0 = 1; fs_leave(); return CMD_FAIL; }
/* 10/18/26 (audit B62): the same store while this task holds a lock (a mutex of its own): the system stops, since the
   lock would stay with the dead task for good (the SD card's watch holds the card's lock outside any request) */
static mutex_t crash_lock = MUTEX_INIT("crashlock");
static int cmd_crashlock(int argc, char **argv) {
    mutex_lock(&crash_lock);
    *(volatile int *)0 = 1;
    mutex_unlock(&crash_lock);
    return CMD_FAIL;
}

/* EBREAK: reported and stepped over (as an unused vector on x86) */
static int cmd_crashint(int argc, char **argv) {
    console_capture_end();
    asm volatile("ebreak");
    printf("    ebreak returned\n");
    return CMD_OK;
}

static const struct command commands[] = {
    { "help",      "[command]", "this menu, or one command in detail", cmd_help },
    { "clear",     "",          "clear the console", cmd_clear },
    { "echo",      "words...",  "print the arguments", cmd_echo },
    { "ps",        "",          "list tasks", cmd_ps },
    { "kill",      "<pid>",     "kill a task", cmd_kill },
    { "sleep",     "[-u] [ms]", "sleep this shell for a while", cmd_sleep },
    { "tasks",     "",          "two kernel tasks that count, sleep and wake, then end", cmd_tasks },
    { "scrolltest", "[lines]",  "two kernel tasks print lines at once; then every row must be whole", cmd_scrolltest },
    { "tick",      "",          "print the tick count", cmd_tick },
    { "uptime",    "",          "time since boot", cmd_uptime },
    { "date",      "",          "the time of day (real-time clock, or the build time)", cmd_date },
    { "memstat",   "",          "memory statistics", cmd_memstat },
    { "heaptest",  "",          "random allocations with patterns, then verify the heap", cmd_heaptest },
    { "selftest",  "",          "run the kernel self tests", cmd_selftest },
    { "dmesg",     "",          "kernel log", cmd_dmesg },
    { "irqstat",   "",          "interrupt and exception counts", cmd_irqstat },
    { "bootinfo",  "",          "what SurfBoot passed, and the board", cmd_bootinfo },
    { "cpuinfo",   "",          "the hart: ISA, ID registers, counters", cmd_cpuinfo },
    { "drivers",   "",          "driver status, console, timer and interrupts", cmd_drivers },
    { "rawkeys",   "",          "raw mode: show the bytes keys send (q ends)", cmd_rawkeys, CMD_ASKS },
    { "ansi",      "",          "draw a test pattern with escape sequences", cmd_ansi },
    { "lsblk",     "[-s]",      "disks, partitions, ramdisks (-s: blocks moved)", cmd_lsblk },
    { "fdisk",     "<disk> [cmd]", "partition tables: new mbr|gpt, add, delete, type", cmd_fdisk },
    { "mount",     "[-r] [dev [path]]", "list mounts, or mount a block device (-r: read-only)", cmd_mount },
    { "umount",    "<path|dev>", "unmount", cmd_umount },
    { "eject",     "<disk>",    "unmount all of a disk and flush it: safe to pull out", cmd_eject },
    { "df",        "",          "size, use and free space of mounted volumes", cmd_df },
    { "mkfs",      "[-t fat16|fat32] <dev> [label]", "make a FAT file system (asks first)", cmd_mkfs },
    { "ls",        "[path]",    "list a directory", cmd_ls },
    { "cat",       "<file>...", "print files", cmd_cat },
    { "cd",        "[dir]",     "change the current directory", cmd_cd },
    { "pwd",       "",          "print the current directory", cmd_pwd },
    { "stat",      "<path>",    "size, time and location of a file", cmd_stat },
    { "cp",        "<src> <dst>", "copy a file", cmd_cp, KCMD_SYNC },
    { "mv",        "<from> <to>", "rename or move a file or directory", cmd_mv, KCMD_SYNC },
    { "write",     "<file> <text>...", "create a file with one line of text", cmd_write, KCMD_SYNC },
    { "append",    "<file> <text>...", "add a line of text to the end of a file", cmd_append, KCMD_SYNC },
    { "mkdir",     "<dir>",     "create a directory", cmd_mkdir, KCMD_SYNC },
    { "rm",        "<file>...", "delete files", cmd_rm, KCMD_SYNC },
    { "rmdir",     "<dir>",     "delete an empty directory", cmd_rmdir, KCMD_SYNC },
    { "sync",      "",          "write cached blocks to the disks", cmd_sync },
    { "fsbench",   "<dir> [MB]", "time writing and reading a file", cmd_fsbench, KCMD_SYNC },
    { "crc32",     "<file>...", "CRC-32 (as zlib) and size of files", cmd_crc32 },
    { "hexdump",   "<dev> [blk] [n]", "dump blocks of a block device", cmd_hexdump },
    { "readbench", "<dev> [MB]", "read speed of a block device (reads only)", cmd_readbench },
    { "crashdiv",  "",          "divide by zero in this shell (RISC-V: no trap)", cmd_crashdiv },
    { "crashill",  "",          "an illegal instruction in this shell", cmd_crashill },
    { "crashmis",  "",          "a misaligned word load in this shell", cmd_crashmis },
    { "crashnull", "",          "write through a NULL pointer", cmd_crashnull },
    { "crashfs",   "",          "a NULL pointer inside a file system: the system stops", cmd_crashfs },
    { "crashlock", "",          "a NULL pointer holding a lock: the system stops", cmd_crashlock },
    { "crashint",  "",          "a breakpoint (ebreak): reported and stepped over", cmd_crashint },
    { "reboot",    "",          "reboot the machine", cmd_reboot },
};
static const u_int ncommands = sizeof(commands) / sizeof(commands[0]);

/**** line handling (as shell/main.c) ****/

static int split_args(char *line, char **argv, int last, char *status, size_t size) {
    int argc = 0;
    char *save;
    char *tok = strtok_r(line, " \t", &save);
    while(tok && argc < SHELL_MAX_ARGS) {
        if(!strcmp(tok, "$?")) {
            snprintf(status, size, "%d", last);
            tok = status;
        }
        argv[argc++] = tok;
        tok = strtok_r(NULL, " \t", &save);
    }
    return argc;
}

/* 10/25/26 (SurfGPU G3): commands only some boards have. The board's own table comes first, so
   these answer only where it has none: on QEMU's virt machine `gfx` says there is no GPU, as it
   does on an Arty bitstream built without one (boards/arty_a7/gfx.c). Not listed by help. */
static int cmd_nogfx(int argc, char **argv) {
    printf("    gfx: no GPU on this board (%s): SurfGPU is the Arty A7's\n", board_name());
    return CMD_FAIL;
}

static const struct command absent_cmds[] = {
    { "gfx",       "[what ...]", "the GPU (not on this board)", cmd_nogfx },
    { NULL, NULL, NULL, NULL, 0 }
};

static const struct command *find_command(const char *name) {
    const struct command *b;
    u_int i;
    for(i = 0; i < ncommands; i++) if(!strcmp(commands[i].name, name)) return &commands[i];
    for(b = board_commands(); b && b->name; b++) if(!strcmp(b->name, name)) return b;
    for(b = absent_cmds; b->name; b++) if(!strcmp(b->name, name)) return b;
    return NULL;
}

/* the disks of the mounts that are unplugged now (their mounts stay until umount): up to max */
static int gone_disks(struct bdev **gone, int max) {
    struct vfs_mount_view *v = (struct vfs_mount_view *)kalloc(SYNC_MOUNTS * sizeof(struct vfs_mount_view));
    int n, i, k = 0;
    if(!v) return 0;
    n = vfs_mount_views(v, SYNC_MOUNTS);
    for(i = 0; i < n && i < SYNC_MOUNTS && k < max; i++) if(v[i].dev && v[i].dev->gone) gone[k++] = v[i].dev;
    kfree(v);
    return k;
}

/* After a KCMD_SYNC command: each mounted volume that can be written goes to its disk (vfs_fsync
   on its root: the file system's own state, the cached blocks, the disk's cache), and a failure
   says which. gone[] are the disks that were unplugged before the command. 0, or an error. */
static int sync_mounts(const char *cmd, struct bdev **gone, int ngone) {
    struct vfs_mount_view *v = (struct vfs_mount_view *)kalloc(SYNC_MOUNTS * sizeof(struct vfs_mount_view));
    int n, i, j, bad = 0;
    if(!v) {
        bad = vfs_sync();
        if(bad) printf("    %s: not all of it reached the disk: %s\n", cmd, strerror(bad));
        return bad;
    }
    n = vfs_mount_views(v, SYNC_MOUNTS);
    for(i = 0; i < n && i < SYNC_MOUNTS; i++) {
        struct file *f;
        int r;
        if(!v[i].dev || v[i].readonly) continue;
        for(j = 0; j < ngone && gone[j] != v[i].dev; j++) ;
        if(j < ngone) continue;                                 /* unplugged before the command */
        if(v[i].dev->gone) {
            printf("    %s: not all of it reached %s: its disk was taken out\n", cmd, v[i].path);
            bad = -ENODEV;
            continue;
        }
        r = vfs_open(v[i].path, O_RDONLY, &f);
        if(!r) {
            r = vfs_fsync(f);
            vfs_close(f);
        }
        if(r) {
            printf("    %s: not all of it reached %s: %s\n", cmd, v[i].path, strerror(r));
            bad = r;
        }
    }
    kfree(v);
    if(n > SYNC_MOUNTS && vfs_sync() && !bad) {                 /* the rest, all at once */
        printf("    %s: not all of it reached the disks\n", cmd);
        bad = -EIO;
    }
    return bad;
}

int shell_run_command(int argc, char **argv) {
    const struct command *c;
    struct bdev *gone[SYNC_MOUNTS];
    int r, ngone = 0;
    if(argc < 1 || !argv[0]) return -ENOENT;
    c = find_command(argv[0]);
    if(!c) return -ENOENT;
    if((c->flags & CMD_ASKS) && curTask->kcap) {
        struct kcapture *kept = curTask->kcap;
        console_capture_end();
        printf("    %s reads the keyboard as it goes: not with its output in a file\n", argv[0]);
        console_capture_begin(kept);
        return CMD_USAGE;
    }
    if(c->flags & KCMD_SYNC) ngone = gone_disks(gone, SYNC_MOUNTS);
    r = c->fn(argc, argv);
    if((c->flags & KCMD_SYNC) && sync_mounts(argv[0], gone, ngone) && r == CMD_OK) r = CMD_FAIL;  /* 10/16/26 (above) */
    return r < 0 ? CMD_FAIL : r;
}

int shell_command_info(int index, struct kcmd_info *out) {
    if(index < 0 || (u_int)index >= ncommands) return -ENOENT;
    memset(out, 0, sizeof(*out));
    strlcpy(out->name, commands[index].name, sizeof(out->name));
    strlcpy(out->args, commands[index].args, sizeof(out->args));
    strlcpy(out->help, commands[index].help, sizeof(out->help));
    return 0;
}

static int parseCommand(char *line, int last) {
    char *argv[SHELL_MAX_ARGS], status[12];
    int argc = split_args(line, argv, last, status, sizeof(status)), r;
    if(!argc) return last;
    r = shell_run_command(argc, argv);
    if(r != -ENOENT) return r;
    printf("    Invalid command: %s (try help)\n", argv[0]);
    return 127;
}

static void show_prompt(int status, int diagnostic) {
    struct prompt_ctx c;
    char buf[PROMPT_MAX];
    int caps = tty_ctl(curTask->tty, TTY_CAPS, 0);
    memset(&c, 0, sizeof(c));
    c.cwd = vfs_getcwd();
    c.last_status = status;
    c.elevated = 1;
    c.diagnostic = diagnostic;
    c.width = (int)COLUMNS;
    c.unicode = caps > 0 && (caps & TTY_CAP_UTF8);
    c.color = caps > 0 && (caps & TTY_CAP_COLOR);
    prompt_render(&c, buf, sizeof(buf));
    puts(buf);
}

static void startShell(int diagnostic) {
    char line[SHELL_LINE_LEN];
    int status = 0;
    for(;;) {
        show_prompt(status, diagnostic);
        memset(line, 0, sizeof(line));
        cgets(TEAL_TXT, line);
        task_call_enter(TC_COMMAND);
        tty_ctl(curTask->tty, TTY_PAGE, 1);
        status = parseCommand(line, status);
        tty_ctl(curTask->tty, TTY_PAGE, 0);
        if(task_call_leave(TC_COMMAND)) status = KCMD_INTERRUPTED;
    }
}

/* the shell task body, started by init for each console (kernel/task.c) */
void shell(void *why) {
    do_banner();
    if(why) {
        cputs(KERN_TXT_COLOR, (char *)why);
        printf("\n");
    }
    startShell(why != NULL);
}
