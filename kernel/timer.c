/*
SurfOS Timer functions (INT 0)
Copyright (C)2004 Brandon Burr

10/5/26 (USB boot): a modern laptop may have no working PIT (Intel chipsets can gate
its clock), so the delay calibration uses the first reference clock that is seen
counting: PIT channel 2, then (laptop profile) the ACPI PM timer, then the HPET. And
the 100 Hz tick is confirmed: if the PIT's IRQ 0 does not arrive, the local APIC
timer takes over (kernel/apic.c). With profile=laptop the keyboard is also polled on
every tick, in case its IRQ never reaches the CPU.
*/

#include <asm/io.h>
#include <surfos/timer.h>
#include <blibc_common.h>
#include <surfos/interrupt.h>
#include <surfos/console.h>
#include <surfos/task.h>
#include <surfos/ktimer.h>
#include <surfos/multiboot.h>
#include <surfos/acpi.h>
#include <surfos/apic.h>
#include <surfos/keyboard.h>
#include <surfos/system.h>
#include <surfos/math64.h>
#include <mm/paging.h>

volatile u_long sysTick;
static u_long loops_per_ms;

/* the clock udelay() was calibrated against */
enum { REF_NONE, REF_PIT, REF_PMTMR, REF_HPET };
static const char *ref_names[] = { "nothing counted (delays assumed)", "PIT channel 2", "ACPI PM timer", "HPET" };
static int ref;
static u32 ref_hz, ref_mask;
static volatile u32 *hpet;

/* what drives the 100 Hz tick */
enum { TICK_NONE, TICK_PIT, TICK_APIC };
static const char *tick_names[] = { "nothing", "PIT channel 0 (IRQ 0)", "local APIC timer" };
static int tick;
static u32 apic_count;

/* a loop the compiler cannot shorten, used for the calibrated delays */
static void delay_loop(u_long n) {
    volatile u_long i;
    for(i = 0; i < n; i++) asm volatile("" : : : "memory");
}

/**** PIT channel 2 (the speaker timer): one-shot, no interrupt needed ****/

#define PIT_HZ 1193182

static void pit2_oneshot(u16 count) {
    outb(0x61, (inb(0x61) & ~0x02) | 0x01);  /* gate 2 on, speaker off */
    outb(0x43, 0xB0);                        /* channel 2, lo/hi byte, mode 0: count down once */
    outb(0x42, count & 0xFF);
    outb(0x42, count >> 8);
}

static u16 pit2_read(void) {
    u16 count;
    outb(0x43, 0x80);                        /* latch channel 2 */
    count = inb(0x42);
    count |= inb(0x42) << 8;
    return count;
}

/* a missing or clock-gated PIT reads back the same value */
static bool pit2_counts(void) {
    u16 a, b;
    u_int i;
    pit2_oneshot(0xFFFF);
    a = pit2_read();
    for(i = 0; i < 1000; i++) io_wait();     /* about a millisecond */
    b = pit2_read();
    outb(0x61, inb(0x61) & ~0x03);
    return a != b;
}

/* PIT ticks (1.193 MHz) spent in delay_loop(n); 0 if the loop outran the 54 ms the counter holds. */
static u_long pit2_time(u_long n) {
    u_long count;
    pit2_oneshot(0xFFFF);
    delay_loop(n);
    count = pit2_read();
    if(inb(0x61) & 0x20) count = 0;          /* OUT2 went high: the count ran out */
    outb(0x61, inb(0x61) & ~0x03);
    return count ? 0xFFFF - count : 0;
}

/**** the ACPI PM timer and the HPET: free-running counters ****/

static bool pmtmr_counts(void) {
    u32 a, b;
    u_int i;
    if(!acpi.pm_timer_port) return false;
    ref_mask = acpi.pm_timer_32 ? 0xFFFFFFFF : 0xFFFFFF;
    a = ind(acpi.pm_timer_port) & ref_mask;
    for(i = 0; i < 200; i++) io_wait();
    b = ind(acpi.pm_timer_port) & ref_mask;
    ref_hz = 3579545;
    return a != b;
}

/* HPET comparators off and legacy replacement off, so IRQ 0 belongs to the PIT again;
   the main counter runs (with no comparator armed it interrupts nobody) */
static void hpet_quiesce(void) {
    u32 timers, i;
    if(!acpi.hpet || acpi.hpet_addr >= 0x100000000ULL) return;
    hpet = (volatile u32 *)ioremap((u_long)acpi.hpet_addr, 1024, false);
    if(!hpet || hpet[0] == 0xFFFFFFFF) { hpet = NULL; return; }
    timers = ((hpet[0] >> 8) & 0x1F) + 1;
    for(i = 0; i < timers; i++) hpet[(0x100 + 0x20 * i) / 4] &= ~0x4u;   /* Tn_INT_ENB_CNF */
    hpet[0x10 / 4] = (hpet[0x10 / 4] & ~0x2u) | 0x1u;                   /* LEG_RT_CNF off, ENABLE_CNF on */
}

static bool hpet_counts(void) {
    u32 period, a, b, i;
    if(!hpet) return false;
    period = hpet[1];                                                    /* femtoseconds per count */
    if(!period || period > 100000000) return false;                      /* the spec allows at most 100 ns */
    ref_hz = (u32)div64_32(1000000000000000ULL, period, NULL);
    ref_mask = 0xFFFFFFFF;
    a = hpet[0xF0 / 4];
    for(i = 0; i < 200; i++) io_wait();
    b = hpet[0xF0 / 4];
    return a != b;
}

static u32 ref_read(void) {
    if(ref == REF_PMTMR) return ind(acpi.pm_timer_port) & ref_mask;
    return hpet[0xF0 / 4];
}

/* 10 ms on the reference clock, bounded even if it stops */
static void ref_wait_10ms(void) {
    u_long i;
    if(ref == REF_PIT) {
        pit2_oneshot(PIT_HZ / 100);
        for(i = 0; i < 1000000 && !(inb(0x61) & 0x20); i++);
        outb(0x61, inb(0x61) & ~0x03);
    } else if(ref == REF_PMTMR || ref == REF_HPET) {
        u32 t0 = ref_read();
        for(i = 0; i < 100000000 && ((ref_read() - t0) & ref_mask) < ref_hz / 100; i++);
    } else {
        mdelay(10);
    }
}

/**** delays ****/

/* Measure how many delay_loop() iterations make a millisecond. Runs before the tick is
   enabled, so udelay() works for every driver and nothing preempts the measurement. */
static void calibrate_pit(void) {
    u_long n = 1 << 10, ticks;
    for(;;) {
        ticks = pit2_time(n);
        if(!ticks) { ticks = 0xFFFF; break; }   /* cannot happen after the doubling below */
        if(ticks >= PIT_HZ / 100) break;        /* at least 10 ms measured: good enough */
        n <<= 1;
    }
    /* n * 1193.182 / ticks without 64-bit arithmetic */
    loops_per_ms = (n / ticks) * (PIT_HZ / 1000) + ((n % ticks) * (PIT_HZ / 1000)) / ticks;
}

static void calibrate_counter(void) {
    u_long n = 1 << 10;
    u32 t0, dt = 0;
    for(;;) {
        t0 = ref_read();
        delay_loop(n);
        dt = (ref_read() - t0) & ref_mask;
        if(dt >= ref_hz / 100 || n >= 0x40000000) break;
        n <<= 1;
    }
    loops_per_ms = dt ? (u_long)div64_32((u64)n * (ref_hz / 1000), dt, NULL) : 0;
}

void udelay(u_long us) {
    delay_loop(loops_per_ms * us / 1000);
}

void mdelay(u_long ms) {
    while(ms--) delay_loop(loops_per_ms);
}

u_long delay_loops_per_ms(void) {
    return loops_per_ms;
}

void init_delay() {
    bool laptop = bootinfo.laptop;
    kprintf("\nDelay calibration\n");
    if(laptop) hpet_quiesce();
    ref = REF_NONE;
    loops_per_ms = 0;
    if(!(laptop && (bootinfo_has_arg(&bootinfo, "clock=pmtmr") || bootinfo_has_arg(&bootinfo, "clock=hpet"))) &&
       pit2_counts()) ref = REF_PIT;
    else if(laptop && !bootinfo_has_arg(&bootinfo, "clock=hpet") && pmtmr_counts()) ref = REF_PMTMR;
    else if(laptop && hpet_counts()) ref = REF_HPET;
    if(ref == REF_PIT) calibrate_pit();
    else if(ref != REF_NONE) calibrate_counter();
    if(!loops_per_ms) {                      /* nothing counts: assume a 5 GHz CPU, so delays only get longer */
        ref = REF_NONE;
        loops_per_ms = 5000000;
    }
    kprintf("*%lu delay loops per ms, measured on the %s\n*DONE\n", loops_per_ms, ref_names[ref]);
}

/**** the tick ****/

static void start_pit(void) {
    u_long divisor = CPU_FREQ/HZ;
    kprintf("*PIT channel 0: %i Hz\n", HZ);
    /* The 2004 code wrote the command byte to port 0x36 (arguments swapped), so the PIT
       stayed in whatever mode the BIOS left and only the divisor took effect. */
    outb(0x43, 0x36);               /* channel 0, lobyte/hibyte, mode 3 (square wave) */
    outb(0x40, divisor & 0xff);
    outb(0x40, divisor >> 8);
    tick = TICK_PIT;
    kprintf("*Enable IRQ0\n*DONE\n");
    _enable_irq(IRQ_TIMER);                  /* from here on the scheduler runs */
}

static bool start_apic(void) {
    if(!apic_active()) return false;
    if(!apic_count) apic_count = apic_timer_calibrate(ref_wait_10ms);   /* 10 ms: one tick at 100 Hz */
    if(!apic_count) return false;
    kprintf("*local APIC timer: %u counts per tick, calibrated on the %s\n*DONE\n", apic_count, ref_names[ref]);
    tick = TICK_APIC;
    apic_timer_start(apic_count);
    return true;
}

void init_timer() {
    kprintf("\nTimer Initialization\n");
    sysTick=0;
    if(bootinfo.laptop && bootinfo_has_arg(&bootinfo, "timer=lapic") && start_apic()) return;
    start_pit();
}

/* Up to 100 ms for the clock to move. The first tick hands the CPU to init, so the idle
   task (this code) only comes back once everything else waits: by then it has ticked. */
static bool ticking(void) {
    u_long start = sysTick;
    u_int i;
    for(i = 0; i < 100 && sysTick == start; i++) mdelay(1);
    return sysTick != start;
}

void timer_confirm(void) {
    if(ticking()) return;
    kprintf("\nTimer: no tick from the %s within 100 ms\n", tick_names[tick]);
    if(tick == TICK_PIT && bootinfo.laptop) {
        _disable_irq(IRQ_TIMER);
        if(start_apic() && ticking()) return;   /* `drivers` and the log above say which timer won */
    }
    kprintf("*no timer interrupts at all: tasks cannot be scheduled. SYSTEM HALTED\n");
    halt();
}

const char *timer_describe(char *buf, size_t size) {
    if(tick == TICK_APIC)
        snprintf(buf, size, "%s, %i Hz (%u counts per tick, %lu interrupts); delays from the %s", tick_names[tick], HZ,
                 apic_count, apic_timer_irqs(), ref_names[ref]);
    else
        snprintf(buf, size, "%s, %i Hz; delays from the %s (%lu loops per ms)", tick_names[tick], HZ,
                 ref_names[ref], loops_per_ms);
    return buf;
}

u_long getticks() {
    return sysTick;
}

/* Every tick: time, the polled keyboard, timers, then let the scheduler choose the frame to resume. */
u_long *timer_tick(struct trapframe *tf) {
    sysTick++;
    keyboard_poll();
    ktimers_tick();
    if(sched_tick()) return schedule(tf);
    return (u_long*)tf;
}
