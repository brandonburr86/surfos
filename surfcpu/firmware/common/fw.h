/*
SurfCPU firmware runtime
--------------------
File: fw.h Date: 10/15/26 (SurfCPU)
--------------------
What SurfBoot and the diagnostics share on the device: CSR access, the trap frame and the
fault reporter, the UART, the VGA text console, the timer, the LEDs and the board identity.
Every address and register offset comes from the generated surfcpu/soc.h (spec/soc.toml).
No C library: string.c supplies the few functions GCC may call by itself.
*/
#ifndef SURFCPU_FW_H
#define SURFCPU_FW_H

#include <stddef.h>
#include <stdint.h>
#include "surfcpu/soc.h"
#include "fwdefs.h"
#include "fmt.h"

#define REG(base, off) SURF_REG(base, off)

/* ---- CSRs (docs/ISA.md section 2) ---- */
#define csr_read(csr) ({ uint32_t v_; __asm__ volatile ("csrr %0, " #csr : "=r"(v_)); v_; })
#define csr_write(csr, val) __asm__ volatile ("csrw " #csr ", %0" :: "rK"((uint32_t)(val)))
#define csr_set(csr, bits) __asm__ volatile ("csrs " #csr ", %0" :: "rK"((uint32_t)(bits)))
#define csr_clear(csr, bits) __asm__ volatile ("csrc " #csr ", %0" :: "rK"((uint32_t)(bits)))

#define MSTATUS_MIE   0x00000008u
#define MSTATUS_MPIE  0x00000080u
#define MIE_MSIE      0x00000008u
#define MIE_MTIE      0x00000080u
#define MIE_MEIE      0x00000800u
#define MCAUSE_INT    0x80000000u

static inline void fw_fence_i(void) { __asm__ volatile ("fence.i" ::: "memory"); }
static inline void fw_wfi(void) { __asm__ volatile ("wfi" ::: "memory"); }

/* ---- traps (trap.S, trap.c) ---- */
/* mscratch holds FW_TRAP_MAGIC while firmware owns the machine; anything else sends a trap
 * to the stackless reporter (a trap after a handoff, or a trap inside the trap handler). */
struct trap_frame {
    uint32_t x[32];            /* x[0] unused, x[2] = sp at the trap */
    uint32_t mepc, mcause, mtval, mstatus;
};

/* Called first for every trap; return 1 when handled (execution resumes at f->mepc, which
 * the hook may change), 0 to report the trap. */
typedef int (*trap_hook_fn)(struct trap_frame *f);
extern trap_hook_fn fw_trap_hook;

typedef struct { uint32_t r[14]; } fw_jmp_buf[1];     /* ra, sp, s0..s11 */
int fw_setjmp(fw_jmp_buf env);
void fw_longjmp(fw_jmp_buf env, int val) __attribute__((noreturn));

/* With a recovery point armed, a trap that no hook handles is reported and then resumes
 * there (fw_setjmp returns 1). Without one the reporter stops the machine. */
void fw_trap_recover(fw_jmp_buf *env);
extern volatile uint32_t fw_trap_count;
extern struct trap_frame fw_last_trap;

const char *fw_cause_name(uint32_t mcause);
void fw_trap_report(const struct trap_frame *f);
void fw_halt(unsigned code) __attribute__((noreturn));  /* blink a code on the LEDs forever */

/* ---- UART (uart.c) ---- */
void uart_putc(char c);
int uart_getc(void);                /* -1 when nothing arrived */
void uart_drain(uint32_t timeout_us);

/* ---- VGA text console (video.c); does nothing when the SoC has no video ---- */
void video_init(void);
void video_attach(void);            /* continue the text SurfBoot left on screen */
void video_putc(char c);
int video_present(void);

/* ---- the console: UART and screen together (console.c) ---- */
void con_putc(char c);
void con_puts(const char *s);
int con_printf(const char *fmt, ...) FMT_PRINTF(1, 2);
int con_vprintf(const char *fmt, va_list ap);
int con_getc(void);

/* ---- timer (timer.c); mtime counts microseconds (timebase 1 MHz) ---- */
uint64_t timer_now(void);
static inline uint32_t timer_now32(void) { return REG(SURF_TIMER_BASE, SURF_TIMER_MTIME_LO); }
void timer_delay_us(uint32_t us);
void timer_set_cmp(uint64_t when);
void timer_cmp_off(void);           /* mtimecmp = all ones, written without a spurious MTIP */

/* ---- LEDs (led.c) ---- */
void led_set(unsigned green4, unsigned rgb12);
void led_code(unsigned code);       /* a failure code 1..15 on LD4..LD7, LD0 red */

/* ---- identity ---- */
static inline uint32_t sysinfo(uint32_t off) { return REG(SURF_SYSINFO_BASE, off); }
const char *fw_board_name(uint32_t board_id);

/* Hot code for RAM images: runs from scratch RAM (ram.ld .fast), ordinary code in ROM images. */
#define FW_FAST __attribute__((section(".fast"), noinline))

/* ---- linker symbols ---- */
extern char __rom_end[], __data_start[], __data_end[], __bss_start[], __bss_end[];
extern char __stack_top[], __stack_bottom[];

#endif /* SURFCPU_FW_H */
