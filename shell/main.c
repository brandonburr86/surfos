/*
SurfOS ring0 Shell (v0.002)
(C)2004 Brandon Burr, command table 10/2026 (roadmap U1)

A line is split into words; the first one selects a command from the table below,
the rest arrive as argv. `help` is generated from the same table.
*/

#include <blibc_common.h>
#include <stdarg.h>

#include <asm/io.h>
#include <surfos/keyboard.h>
#include <surfos/types.h>
#include <surfos/kernel.h>
#include <surfos/timer.h>
#include <surfos/ktimer.h>
#include <sys/parport.h>
#include <sys/serial.h>
#include <sys/pci.h>
#include <sys/driver.h>
#include <surfos/task.h>
#include <surfos/console.h>
#include <surfos/system.h>
#include <surfos/klog.h>
#include <surfos/interrupt.h>
#include <surfos/multiboot.h>

#include <mm/kalloc.h>
#include <mm/memory.h>
#include <mm/paging.h>
#include <mm/pmm.h>

#include "shell.h"

char *prompt = "SurfOS*> ";

static const struct command commands[];
static u_int ncommands;

/**** small commands ****/

static void cmd_help(int argc, char **argv) {
    u_int i;
    if(argc > 1) {
        for(i = 0; i < ncommands; i++) {
            if(!strcmp(commands[i].name, argv[1])) {
                printf("    %s %s\n    %s\n", commands[i].name, commands[i].args, commands[i].help);
                return;
            }
        }
        printf("    no such command: %s\n", argv[1]);
        return;
    }
    printf("\n    SurfOS ring0 Debug Shell v0.009\n");
    printf("    -------------------------------\n");
    for(i = 0; i < ncommands; i++) {
        printf("    %-9s %-14s %s\n", commands[i].name, commands[i].args, commands[i].help);
    }
    printf("    -------------------------------\n");
    printf("    (C)2004 Brandon Burr.\n\n");
}

static void cmd_clear(int argc, char **argv) { clearScreen(); }

static void cmd_echo(int argc, char **argv) {
    int i;
    for(i = 1; i < argc; i++) printf("%s%s", argv[i], i + 1 < argc ? " " : "");
    printf("\n");
}

static void cmd_reboot(int argc, char **argv) {
    printf("\n    Please wait... rebooting...");
    reboot();
}

static void cmd_tick(int argc, char **argv) {
    printf("\n    Ticks: %lu\n\n", getticks());
}

static void cmd_uptime(int argc, char **argv) {
    u_long ms = uptime_ms();
    printf("    up %lu.%02lu s (%lu ticks at %i Hz), %u tasks, %u timers pending\n",
           ms / 1000, (ms % 1000) / 10, getticks(), HZ, task_count(), ktimers_count());
}

static void cmd_date(int argc, char **argv) {
    time_t t;
    get_time(&t);
    printf("    %04u-%02u-%02u %02u:%02u:%02u (CMOS clock)\n", t.year, t.month, t.date, t.hour, t.minute, t.second);
}

static void cmd_sleep(int argc, char **argv) {
    u_long ms = argc > 1 ? strtoul(argv[1], NULL, 10) : 1000;
    u_long t0 = getticks();
    sleep_ms(ms);
    printf("    slept %lu ms (%lu ticks)\n", ms, getticks() - t0);
}

static void cmd_ps(int argc, char **argv) { print_tasks(); }

static void cmd_kill(int argc, char **argv) {
    surf_task *t;
    u_long pid;
    if(argc < 2) { printf("    usage: kill <pid>\n"); return; }
    pid = strtoul(argv[1], NULL, 10);
    t = task_find(pid);
    if(!t) { printf("    no task with pid %lu\n", pid); return; }
    if(t == idle_task || t == init_task_ptr) { printf("    not that one\n"); return; }
    printf("    killing '%s' (pid %lu)\n", t->name, pid);
    kill_task(t);
    if(t == curTask) yield();
}

static void cmd_memstat(int argc, char **argv) { printMemInfo(); }

static void cmd_heaptest(int argc, char **argv) { heaptest(); }

static void cmd_dmesg(int argc, char **argv) { klog_dump(); }

static void cmd_irqstat(int argc, char **argv) {
    int i;
    printf("    IRQ  count\n");
    for(i = 0; i < NR_IRQS; i++) if(irq_count[i]) printf("    %3i  %lu\n", i, irq_count[i]);
    printf("    spurious: %lu\n", spurious_irq_count);
}

static void cmd_bootinfo(int argc, char **argv) { mb_print(); }
static void cmd_lspci(int argc, char **argv) { pci_print(); }
static void cmd_drivers(int argc, char **argv) { print_drivers(); }

static void cmd_lpstat(int argc, char **argv) { printParStatus(); }

static void cmd_test(int argc, char **argv) {
    void *tmp = palloc(1024);
    void *tmp2 = mm_lookup_linear(tmp);
    printf("0x%lx linear is using physical 0x%lx\n", (u_long)tmp, (u_long)tmp2);
    pfree(tmp);
}

static void cmd_beep(int argc, char **argv) { demoBeep(); }
static void cmd_funky(int argc, char **argv) { new_task("funky", conActive, KERNEL, PL_NORMAL, (u_long*)funky); }
static void cmd_die(int argc, char **argv) { new_task("ring3", conActive, USER, PL_NORMAL, (u_long*)funky); } /* a ring-3 task: faults until P1 exists */
static void cmd_demo(int argc, char **argv) { invokeDemo(); }
static void cmd_hanoi(int argc, char **argv) { runHanoi(); }

static void cmd_selftest(int argc, char **argv) { run_selftest(); }

/* exercise the trap framework: real exceptions in this task */
static void cmd_crashdiv(int argc, char **argv) {
    volatile int one = 1, zero = 0; /* two volatiles: gcc folds 1/x into compares with no idiv */
    printf("%i\n", one/zero);
}
static void cmd_crashgp(int argc, char **argv) { asm volatile("mov %0, %%ds" :: "r"(0x1234)); }
static void cmd_crashint(int argc, char **argv) {
    asm volatile("int $0x50");
    printf("    int 0x50 returned\n");
}
static void cmd_crashnull(int argc, char **argv) { *(volatile int *)0 = 1; } /* page 0 is unmapped */

static const struct command commands[] = {
    { "help",      "[command]", "this menu, or one command in detail", cmd_help },
    { "clear",     "",          "clear the active console", cmd_clear },
    { "echo",      "words...",  "print the arguments", cmd_echo },
    { "ps",        "",          "list tasks", cmd_ps },
    { "kill",      "<pid>",     "kill a task", cmd_kill },
    { "sleep",     "[ms]",      "sleep this shell for a while", cmd_sleep },
    { "tick",      "",          "print the tick count", cmd_tick },
    { "uptime",    "",          "time since boot", cmd_uptime },
    { "date",      "",          "CMOS clock", cmd_date },
    { "memstat",   "",          "memory statistics", cmd_memstat },
    { "heaptest",  "",          "random allocations with patterns, then verify the heap", cmd_heaptest },
    { "selftest",  "",          "run the kernel self tests", cmd_selftest },
    { "dmesg",     "",          "kernel log", cmd_dmesg },
    { "irqstat",   "",          "interrupt counts", cmd_irqstat },
    { "bootinfo",  "",          "what the boot loader passed", cmd_bootinfo },
    { "lspci",     "",          "PCI devices with names", cmd_lspci },
    { "drivers",   "",          "driver init status", cmd_drivers },
    { "lpstat",    "",          "parallel port status", cmd_lpstat },
    { "test",      "",          "DMA heap allocation and physical lookup", cmd_test },
    { "beep",      "",          "beep the PC speaker", cmd_beep },
    { "funky",     "",          "play a tune in a new task", cmd_funky },
    { "die",       "",          "start a ring-3 task (faults until user mode exists)", cmd_die },
    { "demo",      "",          "the 2004 demonstration menu", cmd_demo },
    { "hanoi",     "",          "Towers of Hanoi", cmd_hanoi },
    { "crashdiv",  "",          "divide by zero in this shell", cmd_crashdiv },
    { "crashgp",   "",          "general protection fault in this shell", cmd_crashgp },
    { "crashnull", "",          "write through a NULL pointer", cmd_crashnull },
    { "crashint",  "",          "raise an unused vector", cmd_crashint },
    { "reboot",    "",          "reboot the machine", cmd_reboot },
};

/**** line handling ****/

static int split_args(char *line, char **argv) {
    int argc = 0;
    char *save;
    char *tok = strtok_r(line, " \t", &save);
    while(tok && argc < SHELL_MAX_ARGS) {
        argv[argc++] = tok;
        tok = strtok_r(NULL, " \t", &save);
    }
    return argc;
}

void parseCommand(char *line) {
    char *argv[SHELL_MAX_ARGS];
    int argc = split_args(line, argv);
    u_int i;

    if(!argc) return; /* no command entered */
    for(i = 0; i < ncommands; i++) {
        if(!strcmp(commands[i].name, argv[0])) {
            commands[i].fn(argc, argv);
            return;
        }
    }
    printf("    Invalid command: %s (try help)\n", argv[0]);
}

/* the shell task body (started by init for each console) */
void startShell(void);

void shell() {
    do_banner();
    startShell();
}

void startShell() {
    char line[SHELL_LINE_LEN];
    ncommands = sizeof(commands) / sizeof(commands[0]);
    for(;;) {
        cputs(LBLUE_TXT, prompt);
        memset(line, 0, sizeof(line));
        cgets(TEAL_TXT, line);
        parseCommand(line);
    }
}
