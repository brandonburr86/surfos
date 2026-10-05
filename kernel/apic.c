/*
SurfOS Local APIC
--------------------
File: apic.c    Date: 10/5/26 (USB boot)
--------------------
See include/surfos/apic.h. The registers are the Intel SDM's (volume 3A, chapter 11);
in x2APIC mode register r is MSR 0x800 + r / 16.
*/

#include <surfos/types.h>
#include <surfos/apic.h>
#include <surfos/acpi.h>
#include <surfos/cpu.h>
#include <surfos/console.h>
#include <surfos/trap.h>
#include <surfos/timer.h>
#include <mm/memory.h>
#include <mm/paging.h>
#include <blibc_common.h>

#define APIC_ID      0x020
#define APIC_VERSION 0x030
#define APIC_TPR     0x080
#define APIC_EOI     0x0B0
#define APIC_SVR     0x0F0
#define APIC_ESR     0x280
#define APIC_LVT_CMCI 0x2F0
#define APIC_LVT_TIMER 0x320
#define APIC_LVT_THERMAL 0x330
#define APIC_LVT_PERF 0x340
#define APIC_LVT_LINT0 0x350
#define APIC_LVT_LINT1 0x360
#define APIC_LVT_ERROR 0x370
#define APIC_TIMER_INIT 0x380
#define APIC_TIMER_CUR  0x390
#define APIC_TIMER_DIV  0x3E0

#define LVT_MASKED   0x10000
#define LVT_PERIODIC 0x20000
#define LVT_EXTINT   0x700
#define LVT_NMI      0x400

#define APIC_BASE_ENABLE 0x800
#define APIC_BASE_X2APIC 0x400

static volatile u32 *regs;
static bool active, x2apic;
static u32 apic_id, version, max_lvt;
static u_int ioapic_pins;
static volatile u_long timer_irqs;

static u32 apic_read(u32 reg) {
    if(x2apic) return (u32)rdmsr(0x800 + (reg >> 4));
    return regs[reg >> 2];
}

static void apic_write(u32 reg, u32 val) {
    if(x2apic) wrmsr(0x800 + (reg >> 4), val);
    else regs[reg >> 2] = val;
}

void apic_eoi(void) {
    if(active) apic_write(APIC_EOI, 0);
}

bool apic_active(void) {
    return active;
}

/* every input of every I/O APIC masked: SurfOS takes its IRQs from the 8259s */
static void mask_ioapics(void) {
    u_int i, pin;
    for(i = 0; i < acpi.nioapics; i++) {
        volatile u32 *io = (volatile u32 *)ioremap(acpi.ioapic[i].addr, 0x20, false);
        u32 pins;
        if(!io) continue;
        io[0] = 0x01;                                  /* IOREGSEL: version register */
        pins = ((io[4] >> 16) & 0xFF) + 1;             /* IOWIN */
        for(pin = 0; pin < pins && pin < 240; pin++) {
            io[0] = 0x10 + pin * 2;
            io[4] = io[4] | LVT_MASKED;
        }
        ioapic_pins += pins;
    }
}

/* An interrupt the firmware took but never acknowledged (or one that arrived from an I/O
   APIC before it was masked) blocks every vector of its priority and below, the local
   APIC timer's included: one EOI per in-service bit clears them. */
static void drain_in_service(void) {
    int n, i;
    for(n = 0; n < 256; n++) {
        bool any = false;
        for(i = 0; i < 8; i++) if(apic_read(0x100 + i * 0x10)) any = true;   /* ISR, 8 x 32 vectors */
        if(!any) break;
        apic_write(APIC_EOI, 0);
    }
    if(n) kprintf("*%d interrupts left in service by the firmware were acknowledged\n", n);
}

static u_long *spurious_trap(struct trapframe *tf) {
    return (u_long *)tf;                               /* no EOI for a spurious interrupt */
}

static u_long *timer_trap(struct trapframe *tf) {
    timer_irqs++;
    apic_write(APIC_EOI, 0);
    return timer_tick(tf);
}

int init_apic(void) {
    u64 base;
    kprintf("\nLocal APIC\n");
    if(!cpu.apic || !cpu.msr) {
        kprintf("*none: the 8259s must reach the CPU directly\n");
        return 1;
    }
    base = rdmsr(MSR_APIC_BASE);
    x2apic = (base & APIC_BASE_X2APIC) ? true : false;
    if(!(base & APIC_BASE_ENABLE)) wrmsr(MSR_APIC_BASE, base | APIC_BASE_ENABLE);
    if(!x2apic) {
        u64 phys = base & 0xFFFFFF000ULL;
        if(phys >= 0x100000000ULL) {
            kprintf("*registers at 0x%llx, above 4 GB: left alone\n", phys);
            return 1;
        }
        regs = (volatile u32 *)ioremap((u_long)phys, PAGE_SIZE, false);
        if(!regs) return -1;
    }
    active = true;
    apic_id = x2apic ? apic_read(APIC_ID) : apic_read(APIC_ID) >> 24;
    version = apic_read(APIC_VERSION);
    max_lvt = (version >> 16) & 0xFF;

    trap_set_handler(T_APIC_SPURIOUS, spurious_trap);
    trap_set_handler(T_APIC_TIMER, timer_trap);
    apic_write(APIC_TPR, 0);
    apic_write(APIC_SVR, 0x100 | T_APIC_SPURIOUS);    /* software enable */
    apic_write(APIC_LVT_TIMER, LVT_MASKED | T_APIC_TIMER);
    apic_write(APIC_LVT_ERROR, LVT_MASKED | T_APIC_SPURIOUS);
    if(max_lvt >= 4) apic_write(APIC_LVT_PERF, LVT_MASKED);
    if(max_lvt >= 5 && (apic_read(APIC_LVT_THERMAL) & 0x700) != 0x200)   /* SMI delivery: the firmware's own */
        apic_write(APIC_LVT_THERMAL, LVT_MASKED);
    if(max_lvt >= 6) apic_write(APIC_LVT_CMCI, LVT_MASKED);
    apic_write(APIC_LVT_LINT0, LVT_EXTINT);           /* virtual wire: the 8259s' INTR */
    apic_write(APIC_LVT_LINT1, LVT_NMI);
    if(!x2apic) apic_write(APIC_ESR, 0);              /* ESR writes are needed to update it (not in x2APIC) */
    mask_ioapics();
    drain_in_service();
    kprintf("*%s, id %u, version 0x%x, %u LVT entries: virtual wire mode (LINT0 ExtINT, LINT1 NMI)\n",
            x2apic ? "x2APIC" : "xAPIC", apic_id, version & 0xFF, max_lvt + 1);
    kprintf("*%u I/O APIC inputs masked\n", ioapic_pins);
    return 0;
}

/* how far the timer counts (divide by 16) while wait_10ms() waits */
u32 apic_timer_calibrate(void (*wait_10ms)(void)) {
    u32 left;
    if(!active) return 0;
    apic_write(APIC_TIMER_DIV, 0x3);
    apic_write(APIC_LVT_TIMER, LVT_MASKED | T_APIC_TIMER);
    apic_write(APIC_TIMER_INIT, 0xFFFFFFFF);
    wait_10ms();
    left = apic_read(APIC_TIMER_CUR);
    apic_write(APIC_TIMER_INIT, 0);
    return 0xFFFFFFFF - left;
}

void apic_timer_start(u32 count_per_tick) {
    if(!active || !count_per_tick) return;
    apic_write(APIC_TIMER_DIV, 0x3);
    apic_write(APIC_LVT_TIMER, LVT_PERIODIC | T_APIC_TIMER);
    apic_write(APIC_TIMER_INIT, count_per_tick);
}

void apic_timer_stop(void) {
    if(!active) return;
    apic_write(APIC_LVT_TIMER, LVT_MASKED | T_APIC_TIMER);
    apic_write(APIC_TIMER_INIT, 0);
}

u_long apic_timer_irqs(void) {
    return timer_irqs;
}

const char *apic_describe(char *buf, size_t size) {
    if(!active) strlcpy(buf, "not used (8259s only)", size);
    else snprintf(buf, size, "%s id %u, virtual wire mode, %u I/O APIC inputs masked", x2apic ? "x2APIC" : "xAPIC",
                  apic_id, ioapic_pins);
    return buf;
}
