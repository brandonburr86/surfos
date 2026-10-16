/*
SurfOS riscv32 Panic and Backtraces
--------------------
File: panic.c   Date: 10/15/26 (SurfCPU)
--------------------
panic(), panic_at() and BUG() as in kernel/panic.c, and the backtrace of kernel/ksym.c
for the RISC-V frame layout. The kernel is built with frame pointers
(-fno-omit-frame-pointer): s0 holds the address just above a function's frame (the
sp it was called with), the return address is at s0 - 4 and the caller's s0 at s0 - 8.
A frame is followed only while it lies in RAM, is word aligned and moves up the stack,
so a corrupt chain ends the walk instead of faulting inside a panic. The symbol table
is tools/gensyms.py's, from the same two-pass link as on x86.
*/

#include <surfos/types.h>
#include <surfos/panic.h>
#include <surfos/console.h>
#include <surfos/interrupt.h>
#include <surfos/irq.h>
#include <surfos/task.h>
#include <surfos/ksym.h>
#include <surfos/system.h>
#include <mm/memory.h>
#include <riscv/csr.h>
#include <riscv/arch.h>
#include <blibc_common.h>
#include <stdarg.h>

extern char __ram_end[];                    /* the linker script: the end of the board's RAM */

const char *ksym_lookup(u_long addr, u_long *offset) {
    u_long lo = 0, hi = ksym_count;

    if(!ksym_count || addr < ksym_table[0].addr) return NULL;
    while(hi - lo > 1) {                    /* the last symbol at or below addr */
        u_long mid = (lo + hi) / 2;
        if(ksym_table[mid].addr <= addr) lo = mid;
        else hi = mid;
    }
    if(offset) *offset = addr - ksym_table[lo].addr;
    return ksym_names + ksym_table[lo].name_off;
}

static void print_frame(u_long pc) {
    u_long off;
    const char *name = ksym_lookup(pc, &off);
    if(name) kprintf("  [<0x%08lx>] %s+0x%lx\n", pc, name, off);
    else kprintf("  [<0x%08lx>] ?\n", pc);
}

static bool frame_ok(u_long fp) {
    return fp >= KERNEL_START + 8 && fp <= (u_long)__ram_end && !(fp & 3);
}

/* fp: a frame pointer (s0); pc: where to start, 0 for none. Each caller is shown at its call
   instruction (the return address less 4): after a call that never returns, the return address
   is already the next function's first byte. */
void backtrace(u_long fp, u_long pc) {
    int depth;
    u_long prev = 0;

    kprintf("Backtrace:\n");
    if(pc) print_frame(pc);
    for(depth = 0; depth < 24; depth++) {
        u_long ra;
        if(!frame_ok(fp) || fp <= prev) break;
        ra = *(u_long *)(fp - 4);
        if(!ra) break;
        print_frame(ra - 4);
        prev = fp;
        fp = *(u_long *)(fp - 8);
    }
}

void backtrace_here(void) {
    backtrace(rv_read_fp(), 0);
}

void panic(const char *fmt, ...) {
    char buf[256];
    va_list ap;

    irq_disable();
    console_capture_end();
    console_panic();
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    kprintf("\nKernel panic: %s\n", buf);
    if(curTask) kprintf("task '%s' (pid %lu), in_irq %i\n", curTask->name, curTask->pid, in_irq);
    backtrace(rv_read_fp(), 0);
    kprintf("\nSYSTEM HALTED\n");
    halt();
}

void panic_at(const char *file, int line, const char *what) {
    panic("%s:%i: %s", file, line, what);
}

void BUG(void) {
    panic("Woah.. this ain't supposdta happen.. a bug!");
}
