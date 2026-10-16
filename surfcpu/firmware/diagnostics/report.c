/*
SurfCPU diagnostics: results
--------------------
File: report.c Date: 10/15/26 (SurfCPU)
--------------------
Each section prints one line: its name, then "ok (n checks)" or the FAIL lines of the
checks that failed. The summary line is what a test harness (and the owner) looks for:
"<PROGRAM>: PASS (n checks)" or "<PROGRAM>: FAIL (k of n checks failed)".
*/
#include "diag.h"

uint32_t diag_checks, diag_failures;
static uint32_t section_checks, section_failures;
static const char *section_name;

void diag_section(const char *name)
{
    section_name = name;
    section_checks = section_failures = 0;
    con_printf("%-34s ", name);
}

int diag_check(int ok, const char *fmt, ...)
{
    diag_checks++;
    section_checks++;
    if (ok)
        return 1;
    diag_failures++;
    if (section_failures++ == 0)
        con_puts("\n");
    con_puts("  FAIL ");
    va_list ap;
    va_start(ap, fmt);
    con_vprintf(fmt, ap);
    va_end(ap);
    con_puts("\n");
    return 0;
}

void diag_section_end(void)
{
    if (section_failures)
        con_printf("  %s: %u of %u checks failed\n", section_name, section_failures, section_checks);
    else
        con_printf("ok (%u checks)\n", section_checks);
}

void diag_summary(const char *program)
{
    if (diag_failures) {
        con_printf("\n%s: FAIL (%u of %u checks failed)\n", program, diag_failures, diag_checks);
        led_set(0xF, 0x001);                        /* LD0 red */
    } else {
        con_printf("\n%s: PASS (%u checks)\n", program, diag_checks);
        led_set(0xF, 0x002);                        /* LD0 green */
    }
}

/* Runs fn(arg); a trap no hook expected is counted as a failed check of the current
 * section and the program goes on with the next one. */
void diag_guard(void (*fn)(uint32_t), uint32_t arg)
{
    static fw_jmp_buf env;
    if (fw_setjmp(env) == 0) {
        fw_trap_recover(&env);
        fn(arg);
    } else {
        csr_write(mie, 0);                          /* leave no interrupt source armed */
        csr_clear(mstatus, MSTATUS_MIE);
        timer_cmp_off();
        REG(SURF_INTC_BASE, SURF_INTC_ENABLE) = 0;
        diag_check(0, "unexpected trap: %s, mepc 0x%08x, mtval 0x%08x (the section was abandoned)",
                   fw_cause_name(fw_last_trap.mcause), fw_last_trap.mepc, fw_last_trap.mtval);
        diag_section_end();
    }
    fw_trap_recover(0);
}
