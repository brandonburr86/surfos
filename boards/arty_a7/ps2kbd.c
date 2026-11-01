/*
SurfOS board: SurfCPU on the Arty A7 -- the PS/2 keyboard
--------------------
File: ps2kbd.c  Date: 10/15/26 (SurfCPU)
--------------------
surf_ps2 (MEMORY-MAP.md) receives the keyboard's frames, checks start, odd parity and stop
bits, abandons a frame that stalls for 2 ms, and queues the bytes (16) with sticky error
bits; interrupt source SURF_IRQ_PS2. Here each byte goes through ps2set2.c (scan code set 2
to set 1, as a PC's 8042 would) and kbdset1.c (set 1 to keys, as kernel/keyboard.c), and
the result to the active tty, tty_switch() for F1-F4, or reboot() for Ctrl+Alt+Del, from
the interrupt handler, as on x86. A line error forgets the half-received sequence and the
modifiers held (keyboard.c does the same on an 8042 error).

The lock keys' LEDs are the keyboard's own: 0xED and the LED byte go to it through
surf_ps2's host-to-device path (open-drain, the keyboard clocks the bits), each answered
by an ACK (0xFA) in the receive FIFO. That runs as a small state machine driven by the
replies, never waiting in the handler; a reply that does not come within 100 ms gives up.
A keyboard plugged in announces itself (0xAA) and gets the LEDs again.
*/

#include <surfos/types.h>
#include <surfos/console.h>
#include <surfos/tty.h>
#include <surfos/task.h>
#include <surfos/timer.h>
#include <surfos/ktimer.h>
#include <surfos/interrupt.h>
#include <surfos/irq.h>
#include <surfos/system.h>
#include <surfos/keyboard.h>
#include <asm/io.h>
#include <surfcpu/soc.h>
#include "arty.h"
#include "ps2set2.h"
#include "kbdset1.h"
#include "../../shell/shell.h"
#include <blibc_common.h>

#define PREG(off) ((u_long)SURF_PS2_BASE + (off))
#define PS2_ERRORS (SURF_PS2_STATUS_OVF_MASK | SURF_PS2_STATUS_PARITY_MASK | SURF_PS2_STATUS_FRAME_MASK | \
                    SURF_PS2_STATUS_TIMEOUT_MASK)

enum { LED_IDLE, LED_SENT_COMMAND, LED_SENT_VALUE };

static struct ps2_set2 set2;
static struct kbd_set1 set1;
static bool up, answered;
static u_long bytes, irqs, line_errors, led_updates, led_failures;
static int led_state = LED_IDLE, last_reply = -1;
static u8 led_want;
static bool led_dirty;
static u_long led_sent_at;
static struct ktimer led_timer;

/* a byte to the keyboard, if the transmitter is free */
static bool ps2_send(u8 b) {
    if(mmio_read32(PREG(SURF_PS2_STATUS)) & SURF_PS2_STATUS_TX_BUSY_MASK) return false;
    mmio_write32(PREG(SURF_PS2_TXDATA), b);
    led_sent_at = getticks();
    return true;
}

/* interrupts off: start an LED update when one is wanted and none is under way */
static void led_kick(void) {
    if(led_state != LED_IDLE || !led_dirty) return;
    if(ps2_send(0xED)) {
        led_state = LED_SENT_COMMAND;
        led_dirty = false;
    }
}

/* the keyboard answered (interrupts off) */
static void led_reply(int reply) {
    last_reply = reply;
    answered = true;
    if(reply == PS2_BAT_OK) {               /* plugged in, or reset: its LEDs are dark again */
        led_state = LED_IDLE;
        led_dirty = true;
    } else if(reply == PS2_ACK && led_state == LED_SENT_COMMAND) {
        if(ps2_send(led_want)) led_state = LED_SENT_VALUE;
        else {                              /* still sending: the whole update again, from the timer */
            led_state = LED_IDLE;
            led_dirty = true;
        }
    } else if(reply == PS2_ACK && led_state == LED_SENT_VALUE) {
        led_state = LED_IDLE;
        led_updates++;
    } else if(reply == PS2_RESEND && led_state != LED_IDLE) {
        led_state = LED_IDLE;               /* start again from the command */
        led_dirty = true;
    }
    led_kick();
}

/* every 100 ms: an answer that never came (no keyboard, a NAK) ends the update */
static void led_check(void *arg) {
    (void)arg;
    if(led_state != LED_IDLE && getticks() - led_sent_at > ms_to_ticks(100)) {
        led_state = LED_IDLE;
        led_failures++;
    }
    led_kick();
}

static void ps2_byte(u8 b) {
    u8 out[2];
    int reply, n, i;
    struct kbd_event ev;
    bytes++;
    n = ps2_set2_byte(&set2, b, out, &reply);
    if(reply >= 0) {
        led_reply(reply);
        return;
    }
    for(i = 0; i < n; i++) {
        switch(kbd_set1_byte(&set1, out[i], tty_active->raw != 0, &ev)) {
        case KBD_KEY:    tty_key(tty_active, ev.key, 0); break;
        case KBD_SWITCH: tty_switch(ev.console); break;
        case KBD_LEDS:   led_want = ev.leds; led_dirty = true; led_kick(); break;
        case KBD_REBOOT: reboot(); break;
        default: break;
        }
    }
}

static int ps2_isr(u_int irq, void *param) {
    u32 st = mmio_read32(PREG(SURF_PS2_STATUS));
    int n;
    (void)irq;
    (void)param;
    irqs++;
    if(st & PS2_ERRORS) {                   /* a frame dropped: what was half received means nothing now */
        line_errors++;
        ps2_set2_reset(&set2);
        kbd_set1_reset(&set1);
        mmio_write32(PREG(SURF_PS2_STATUS), st & PS2_ERRORS);
    }
    if(st & SURF_PS2_STATUS_TX_NAK_MASK) {
        mmio_write32(PREG(SURF_PS2_STATUS), SURF_PS2_STATUS_TX_NAK_MASK);
        if(led_state != LED_IDLE) { led_state = LED_IDLE; led_failures++; }
    }
    for(n = 0; n < 32; n++) {
        u32 v = mmio_read32(PREG(SURF_PS2_DATA));
        if(v & SURF_PS2_DATA_EMPTY_MASK) break;
        ps2_byte((u8)(v & SURF_PS2_DATA_DATA_MASK));
    }
    return 0;
}

int arty_ps2_init(void) {
    u_long flags;
    int n;
    if(!arty_has(SURF_SYSINFO_FEATURES_PS2_MASK)) return 1;
    ps2_set2_reset(&set2);
    kbd_set1_init(&set1, NUM_CONSOLES);
    flags = irq_save();
    for(n = 0; n < 32 && !(mmio_read32(PREG(SURF_PS2_DATA)) & SURF_PS2_DATA_EMPTY_MASK); n++) ;   /* stale bytes */
    mmio_write32(PREG(SURF_PS2_STATUS), PS2_ERRORS | SURF_PS2_STATUS_TX_NAK_MASK);
    mmio_write32(PREG(SURF_PS2_CTRL), SURF_PS2_CTRL_RX_EN_MASK | SURF_PS2_CTRL_RX_IE_MASK | SURF_PS2_CTRL_ERR_IE_MASK);
    led_want = set1.leds;
    led_dirty = true;
    led_kick();                             /* Num Lock's LED on: a keyboard that answers is there */
    irq_restore(flags);
    add_irq_handler(SURF_IRQ_PS2, ps2_isr, 0);
    ktimer_init(&led_timer, led_check, NULL);
    ktimer_start(&led_timer, 100, true);
    up = true;
    kprintf("+PS2: surf_ps2 at 0x%08lx, intc source %d: scan code set 2, translated to set 1 for the keyboard code\n",
            (u_long)SURF_PS2_BASE, SURF_IRQ_PS2);
    return 0;
}

const char *arty_ps2_describe(char *buf, size_t size) {
    if(!up) {
        strlcpy(buf, "none (no PS/2 block in this bitstream)", size);
        return buf;
    }
    snprintf(buf, size, "PS/2 on surf_ps2 (set 2 translated to set 1), %lu keys, %s", set2.keys,
             answered ? "a keyboard answered" : "no keyboard has answered yet");
    return buf;
}

int arty_cmd_kbdinfo(int argc, char **argv) {
    u32 e;
    (void)argc;
    (void)argv;
    if(!up) { printf("    no PS/2 block in this bitstream\n"); return CMD_FAIL; }
    e = mmio_read32(PREG(SURF_PS2_ERRCOUNT));
    printf("    surf_ps2 at 0x%08lx: %lu interrupts, %lu bytes: %lu keys, %lu replies, %lu unknown\n",
           (u_long)SURF_PS2_BASE, irqs, bytes, set2.keys, set2.replies, set2.unknown);
    printf("    line errors seen %lu; counters: parity %lu, framing %lu, timeouts %lu, lost %lu\n", line_errors,
           (u_long)(e & SURF_PS2_ERRCOUNT_PARITY_MASK), (u_long)((e & SURF_PS2_ERRCOUNT_FRAME_MASK) >> SURF_PS2_ERRCOUNT_FRAME_LSB),
           (u_long)((e & SURF_PS2_ERRCOUNT_TIMEOUT_MASK) >> SURF_PS2_ERRCOUNT_TIMEOUT_LSB),
           (u_long)((e & SURF_PS2_ERRCOUNT_OVF_MASK) >> SURF_PS2_ERRCOUNT_OVF_LSB));
    printf("    LEDs: Num %s, Caps %s, Scroll %s; %lu updates, %lu without an answer; last reply 0x%02x%s\n",
           (set1.leds & NUM_LED) ? "on" : "off", (set1.leds & CAPS_LED) ? "on" : "off",
           (set1.leds & SCROLL_LED) ? "on" : "off", led_updates, led_failures, last_reply < 0 ? 0 : last_reply,
           last_reply < 0 ? " (none)" : "");
    return CMD_OK;
}
