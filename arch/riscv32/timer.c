/*
SurfOS riscv32 Timer
--------------------
File: timer.c   Date: 10/15/26 (SurfCPU)
--------------------
The interface of include/surfos/timer.h on the RISC-V machine timer instead of the
PIT: a 64-bit mtime counting at the board's timebase (1 MHz on the Arty, 10 MHz on
QEMU virt) and a 64-bit mtimecmp that raises mip.MTIP while mtime >= mtimecmp. The
tick is HZ (100) a second, as on x86: each timer interrupt moves mtimecmp to the next
tick boundary, which also clears MTIP.

The tick count follows mtime: when interrupts were held off for longer than a tick,
the ticks that passed are counted all at once and the next compare is set past them,
so uptime does not drift and sleepers and kernel timers that came due run at the next
interrupt. udelay() and mdelay() spin on mtime; nothing needs calibrating.

On RV32 a 64-bit register is read high, low, high (retrying when the high half moved)
and mtimecmp is written low = all ones, high, low, so it never passes through a value
that fires early (surfcpu/docs/MEMORY-MAP.md; the boards do both).
*/

#include <surfos/types.h>
#include <surfos/timer.h>
#include <surfos/task.h>
#include <surfos/ktimer.h>
#include <surfos/interrupt.h>
#include <surfos/console.h>
#include <surfos/system.h>
#include <surfos/irq.h>
#include <riscv/csr.h>
#include <riscv/board.h>
#include <riscv/arch.h>
#include <blibc_common.h>

static volatile u_long sysTick;
static u64 next_tick;           /* mtime of the next tick */
static u64 tick_base;           /* mtime when the current tick began */
static u_long period;           /* mtime units per tick */
static u_long per_us;           /* mtime units per microsecond (0: the timebase is not whole MHz) */
static u_long timebase;
static bool running;
u_long timer_late_ticks;        /* ticks that were counted late, all at once (interrupts held off) */

u_long getticks(void) {
    return sysTick;
}

u64 timer_mtime_at_tick(void) {
    return tick_base;
}

u_long timer_period(void) {
    return period;
}

/* spin until mtime has moved by us microseconds; a task should sleep_ms() instead */
void udelay(u_long us) {
    u64 start, units;
    if(!timebase) timebase = board_timebase_hz();
    if(!per_us && timebase % 1000000 == 0) per_us = timebase / 1000000;
    units = per_us ? (u64)us * per_us : (u64)us * timebase / 1000000;
    start = board_mtime();
    while(board_mtime() - start < units) ;
}

void mdelay(u_long ms) {
    while(ms--) udelay(1000);
}

u_long delay_loops_per_ms(void) {
    return timebase / 1000;         /* "loops": mtime units, the only clock a delay counts */
}

void init_delay(void) {
    timebase = board_timebase_hz();
    per_us = timebase % 1000000 == 0 ? timebase / 1000000 : 0;
    kprintf("\nDelays: spin on mtime at %lu Hz (no calibration)\n", timebase);
}

/* the machine timer interrupt (trap.c): count the tick(s), set the next compare, run what came due */
u_long *timer_tick(struct trapframe *tf) {
    u64 now = board_mtime();
    u_long n = 0;
    while(next_tick <= now && n < 1000) {   /* normally once; more after a long time with interrupts off */
        tick_base = next_tick;
        next_tick += period;
        n++;
    }
    if(next_tick <= now) {                  /* very far behind (a debugger stop): start again from now */
        next_tick = now + period;
        tick_base = now;
    }
    if(n > 1) timer_late_ticks += n - 1;
    sysTick += n;
    board_set_mtimecmp(next_tick);
    if(!n) return (u_long *)tf;             /* early (should not happen): nothing is due */
    ktimers_tick();
    if(sched_tick()) return schedule(tf);
    return (u_long *)tf;
}

void init_timer(void) {
    timebase = board_timebase_hz();
    period = timebase / HZ;
    kprintf("\nTimer\n");
    /* said before the first tick: that starts the shells, whose output would land inside this
       line on a slow serial line */
    kprintf("*machine timer at %lu Hz, %i Hz tick (%lu units each)\n", timebase, HZ, period);
    tick_base = board_mtime();
    next_tick = tick_base + period;
    board_set_mtimecmp(next_tick);
    running = true;
    csr_set(mie, MIE_MTIE);
}

/* The first tick must come, or nothing will ever run: wait up to 100 ms of mtime for it. */
void timer_confirm(void) {
    u64 start = board_mtime(), limit = (u64)timebase / 10;
    while(getticks() == 0 && board_mtime() - start < limit) rv_wfi();
    if(getticks() == 0) {
        kprintf("\nBOOT PROBLEM: no timer interrupt within 100 ms (mie 0x%lx, mip 0x%lx, mstatus 0x%lx). SYSTEM HALTED\n",
                csr_read(mie), csr_read(mip), csr_read(mstatus));
        halt();
    }
}

const char *timer_describe(char *buf, size_t size) {
    snprintf(buf, size, "machine timer, %lu Hz timebase, %i Hz tick%s, %lu ticks counted late",
             timebase, HZ, running ? "" : " (not started)", timer_late_ticks);
    return buf;
}
