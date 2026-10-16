/*
SurfCPU firmware: trap dispatch and the fault reporter
--------------------
File: trap.c Date: 10/15/26 (SurfCPU)
--------------------
trap.S saves the machine state and calls fw_trap_dispatch(). A program may install a hook
(the diagnostics do, to check deliberate traps and step over them); otherwise the trap is
reported on the console (cause, mepc, mtval, mstatus and all 31 registers) and then either
resumes at the armed recovery point (SurfBoot's monitor) or stops with LED code 8.
*/
#include "fw.h"

trap_hook_fn fw_trap_hook;
volatile uint32_t fw_trap_count;
struct trap_frame fw_last_trap;
static fw_jmp_buf *recover_env;

static const char *const reg_names[32] = {
    "zero", "ra", "sp", "gp", "tp", "t0", "t1", "t2", "s0", "s1", "a0", "a1", "a2", "a3",
    "a4", "a5", "a6", "a7", "s2", "s3", "s4", "s5", "s6", "s7", "s8", "s9", "s10", "s11",
    "t3", "t4", "t5", "t6",
};

const char *fw_cause_name(uint32_t mcause)
{
    static const char *const exc[] = {
        "instruction address misaligned", "instruction access fault", "illegal instruction",
        "breakpoint", "load address misaligned", "load access fault",
        "store address misaligned", "store access fault", "ecall from U-mode",
        "ecall from S-mode", "reserved", "ecall from M-mode",
    };
    if (mcause & MCAUSE_INT) {
        switch (mcause & ~MCAUSE_INT) {
        case 3: return "machine software interrupt";
        case 7: return "machine timer interrupt";
        case 11: return "machine external interrupt";
        default: return "unknown interrupt";
        }
    }
    return mcause < sizeof exc / sizeof exc[0] ? exc[mcause] : "unknown exception";
}

void fw_trap_report(const struct trap_frame *f)
{
    con_printf("\n*** trap: %s (mcause 0x%08x)\n", fw_cause_name(f->mcause), f->mcause);
    con_printf("    mepc 0x%08x  mtval 0x%08x  mstatus 0x%08x\n", f->mepc, f->mtval, f->mstatus);
    for (int i = 1; i < 32; i++) {
        con_printf("%s%-4s %08x", (i % 4 == 1) ? "    " : "  ", reg_names[i], f->x[i]);
        if (i % 4 == 0 || i == 31)
            con_putc('\n');
    }
}

void fw_trap_recover(fw_jmp_buf *env)
{
    recover_env = env;
}

void fw_trap_dispatch(struct trap_frame *f);

void fw_trap_dispatch(struct trap_frame *f)
{
    fw_trap_count++;
    fw_last_trap = *f;
    if (fw_trap_hook && fw_trap_hook(f))
        return;
    fw_trap_report(f);
    if (recover_env) {
        fw_jmp_buf *env = recover_env;
        /* Leaving the handler without MRET: mark firmware context again first. */
        csr_write(mscratch, FW_TRAP_MAGIC);
        fw_longjmp(*env, 1);
    }
    con_printf("*** stopped: LEDs blink code %d\n", LED_CODE_TRAP);
    fw_halt(LED_CODE_TRAP);
}

void fw_halt(unsigned code)
{
    uart_drain(100000);
    for (;;) {
        led_code(code);
        for (volatile uint32_t i = 0; i < 1500000u; i++)
            ;
        led_set(0, 0);
        for (volatile uint32_t i = 0; i < 1500000u; i++)
            ;
    }
}

const char *fw_board_name(uint32_t board_id)
{
    switch (board_id) {
    case SURF_BOARD_ARTY_A7_100T: return "Arty A7-100T";
    case SURF_BOARD_ARTY_A7_35T: return "Arty A7-35T";
    case SURF_BOARD_SIM: return "simulation";
    default: return "unknown board";
    }
}
