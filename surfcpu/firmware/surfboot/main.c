/*
SurfBoot: the boot ROM's main program
--------------------
File: main.c Date: 10/15/26 (SurfCPU)
--------------------
Power on -> banner -> CRC self-test -> buttons (BTN1 held: stay in the monitor; BTN0 held:
boot BACKUP.BIN) -> sb_boot() (DDR, card, FAT32, image, command line, boot information)
-> handoff. Any failure, and any trap, ends in the monitor on the UART with the reason
printed and a code on the LEDs (docs/FIRMWARE.md); SurfBoot never stops silently. In the
monitor, inserting a card (the socket's card-detect switch closing) retries the boot.
*/
#include "fw.h"
#include "boot.h"
#include "crc32.h"
#include "port.h"
#include "sberr.h"
#include "version.h"

void surfboot_handoff(uint32_t entry, uint32_t bootinfo) __attribute__((noreturn));

static struct sb_state st;
static fw_jmp_buf recover;
static char line[96];
static unsigned line_len;

static void banner(void)
{
    con_printf("\nSurfBoot %u.%u.%u (build %08x%s), the SurfCPU boot ROM\n", SURFBOOT_VERSION_MAJOR,
               SURFBOOT_VERSION_MINOR, SURFBOOT_VERSION_PATCH, (unsigned)FW_BUILD_ID,
               FW_BUILD_DIRTY ? ", dirty tree" : "");
    uint32_t magic = sysinfo(SURF_SYSINFO_MAGIC), board = sysinfo(SURF_SYSINFO_BOARD_ID);
    uint32_t soc = sysinfo(SURF_SYSINFO_SOC_VERSION), map = sysinfo(SURF_SYSINFO_MAP_VERSION);
    uint32_t flags = sysinfo(SURF_SYSINFO_BUILD_FLAGS);
    if (magic != 0x46525553u)
        con_printf("sysinfo: magic 0x%08x is not \"SURF\": the identity registers are unreadable\n", magic);
    con_printf("soc: %s (0x%x), SoC %u.%u.%u, memory map %u, %u Hz; bitstream %08x%s%s\n",
               fw_board_name(board), board, soc >> 16, (soc >> 8) & 0xFFu, soc & 0xFFu, map,
               sysinfo(SURF_SYSINFO_SYS_HZ), sysinfo(SURF_SYSINFO_BUILD_ID),
               (flags & SURF_SYSINFO_BUILD_FLAGS_DIRTY_MASK) ? " dirty" : "",
               (flags & SURF_SYSINFO_BUILD_FLAGS_SIM_MASK) ? " (simulation)" : "");
    if (map != SURF_MAP_VERSION)
        con_printf("soc: warning: SurfBoot was built for memory map %u\n", SURF_MAP_VERSION);
    if (REG(SURF_TIMER_BASE, SURF_TIMER_TIMEBASE) != 1000000u)
        con_printf("timer: warning: timebase %u Hz, SurfBoot assumes 1 MHz\n",
                   REG(SURF_TIMER_BASE, SURF_TIMER_TIMEBASE));
}

static void handoff(void) __attribute__((noreturn));
static void handoff(void)
{
    const struct surf_img_header *h = &st.img.h;
    con_printf("boot: starting %s at 0x%08x (a0 = 0, a1 = 0x%08x, a2 = 0x%08x)\n\n",
               st.used_backup ? "BACKUP.BIN" : "the kernel", h->entry, SURF_BOOTINFO_BASE,
               SURF_BOOTINFO_MAGIC);
    REG(SURF_INTC_BASE, SURF_INTC_ENABLE) = 0;
    timer_cmp_off();
    REG(SURF_SPI_BASE, SURF_SPI_CTRL) = 0;          /* chip select released, no interrupt */
    REG(SURF_UART_BASE, SURF_UART_CTRL) = 0;
    REG(SURF_GPIO_BASE, SURF_GPIO_IE) = 0;
    REG(SURF_VIDEO_BASE, SURF_VIDEO_IE) = 0;
    led_set(0, 0x002);                              /* LD0 green: handed over */
    uart_drain(100000);
    fw_trap_recover(0);
    surfboot_handoff(h->entry, SURF_BOOTINFO_BASE);
}

static void prompt(void)
{
    con_puts("surfboot> ");
}

static void run_line(void)
{
    line[line_len] = 0;
    int r = sb_monitor_line(&st, line);
    line_len = 0;
    if (r == MON_BOOT)
        handoff();
    if (r == MON_RESET) {
        con_puts("restarting\n");
        uart_drain(100000);
        ((void (*)(void))SURF_RESET_PC)();
    }
    led_code(st.boot_err ? sb_err_led(st.boot_err) : LED_CODE_MONITOR);     /* why booting failed */
}

/* State of the monitor loop; static so a trap's longjmp back into the loop finds it intact. */
static int last_char, card_in, card_armed;
static uint32_t card_since;

static void card_watch(void)
{
    int now = port_card_present();
    if (now != card_in) {
        card_in = now;
        if (now) {
            card_armed = 1;                         /* boot once it has been in for 250 ms */
            card_since = timer_now32();
        } else {
            card_armed = 0;
            con_puts("\n(card removed)\n");
            prompt();
        }
    }
    if (card_armed && (uint32_t)(timer_now32() - card_since) >= 250000u) {
        card_armed = 0;
        con_puts("\ncard inserted: booting\n");
        if (sb_boot(&st, 0, SB_BOOT_NORMAL) == SB_OK)
            handoff();
        led_code(sb_err_led(st.boot_err));
        con_printf("boot failed: %s (%s)\n", sb_err_text(st.boot_err), sb_err_name(st.boot_err));
        prompt();
    }
}

static void monitor(int err) __attribute__((noreturn));
static void monitor(int err)
{
    unsigned code = sb_err_led(err);
    led_code(code);
    con_printf("\nSurfBoot stopped: %s (%s)\n", sb_err_text(err), sb_err_name(err));
    if (err != SB_E_STOPPED)
        con_printf("  %s\n", sb_last_msg);
    con_printf("The LEDs LD4-LD7 show code %u. Type help for the monitor's commands; inserting a "
               "card retries the boot.\n", code);
    card_in = port_card_present();
    card_armed = 0;
    last_char = 0;
    if (fw_setjmp(recover) != 0) {
        con_puts("(the command was abandoned after the trap)\n");
        line_len = 0;
    }
    fw_trap_recover(&recover);
    prompt();
    for (;;) {
        card_watch();
        int c = con_getc();
        if (c < 0)
            continue;
        int prev = last_char;
        last_char = c;
        if (c == '\n' && prev == '\r')
            continue;                               /* CR LF from the terminal: one line */
        if (c == '\r' || c == '\n') {
            con_putc('\n');
            run_line();
            prompt();
        } else if (c == 8 || c == 127) {
            if (line_len) {
                line_len--;
                con_puts("\b \b");
            }
        } else if (c == 3) {
            con_puts("^C\n");
            line_len = 0;
            prompt();
        } else if (c >= 32 && c < 127 && line_len < sizeof line - 1) {
            line[line_len++] = (char)c;
            con_putc((char)c);
        }
    }
}

int main(void)
{
    video_init();
    led_set(0, 0x004);                              /* LD0 blue: SurfBoot is running */
    banner();
    int err;
    if (crc32_selftest() != 0) {
        err = sb_note_boot_failure(&st, sb_fail(SB_E_CRC_SELFTEST, "the CRC-32 test vectors do not match: "
                      "the CPU or the ROM is faulty, and no package could be checked"));
        monitor(err);
    }
    if (!port_timer_check())
        con_printf("timer: mtime does not advance (stuck at 0x%08x): the SoC's timer is faulty; SurfBoot goes "
                   "on with approximate timeouts\n", timer_now32());
    uint32_t t0 = port_now_us();
    while ((uint32_t)(port_now_us() - t0) < 20000u && timer_now() < 20000u)
        ;                                           /* let the button debouncers settle */
    uint32_t btn = port_buttons();
    if (fw_setjmp(recover) == 0) {
        fw_trap_recover(&recover);
        if (btn & 2u) {
            con_puts("BTN1 held: staying in the monitor\n");
            err = sb_note_boot_failure(&st, sb_fail(SB_E_STOPPED, "BTN1 was held at power-on"));
        } else {
            if (btn & 1u)
                con_puts("BTN0 held: booting /BOOT/BACKUP.BIN\n");
            err = sb_boot(&st, 0, (btn & 1u) ? SB_BOOT_BACKUP : SB_BOOT_NORMAL);
            if (err == SB_OK)
                handoff();
        }
    } else {
        err = sb_note_boot_failure(&st, sb_fail(SB_E_TRAP, "%s at 0x%08x (mtval 0x%08x) while booting",
                      fw_cause_name(fw_last_trap.mcause), fw_last_trap.mepc, fw_last_trap.mtval));
    }
    monitor(err);
}
