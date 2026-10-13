/*
SurfOS Keyboard Driver
--------------------
File: keyboard.c    Date: Prior to 4/23/04, rebuilt 10/2026 (roadmap C2)
--------------------
(C)2004 Brandon Burr

Scan code set 1 with the E0 prefix: letters follow Caps Lock, the keypad follows
Num Lock, arrows/Home/End/Del/PgUp/PgDn/Ins/F-keys become the codes in keyboard.h,
Ctrl-letter becomes a control character. F1..F4 switch consoles, Ctrl-Alt-Del
reboots. Everything else goes to the active tty.

10/5/26 (USB boot): a laptop's built-in keyboard sits behind an 8042 that the
embedded controller emulates, and UEFI firmware may hand it over with its interrupt
or its scan-code translation switched off. With profile=laptop the controller is set
up again (every wait bounded, so a missing 8042 only means no keyboard), and it is
also polled on every timer tick in case IRQ 1 never reaches the CPU. The handler only
reads port 0x60 when the status register says a byte is waiting, so the interrupt and
the poll never take the same byte twice. 10/10/26: the same controller also supplies
standard PS/2 mouse packets from IRQ 12, demultiplexed from keyboard scan codes.
*/

#include <surfos/keyboard.h>
#include <surfos/console.h>
#include <surfos/tty.h>
#include <asm/io.h>
#include <surfos/system.h>
#include <surfos/interrupt.h>
#include <surfos/irq.h>
#include <surfos/task.h>
#include <surfos/kernel.h>
#include <surfos/timer.h>
#include <surfos/multiboot.h>
#include <blibc_common.h>
#include <sys/pointer.h>
#include <sys/ps2mouse.h>

static u_char led_status = NUM_LED;
static bool shift_l, shift_r, ctrl, alt, e0;
static u_long irqs, polled;

/* status register bits */
#define KBD_OBF  0x01           /* a byte is waiting in port 0x60 */
#define KBD_IBF  0x02           /* the controller has not taken our last byte yet */
#define KBD_AUX  0x20           /* the waiting byte is from the mouse port */
#define KBD_ERROR 0xC0         /* timeout or parity error */

static bool present = true;     /* an 8042 answers (assumed, as in 2004, unless profile=laptop probed it) */
static bool use_irq = true, polling;
static int config_before = -1, config_after = -1;
static struct ps2mouse mouse;
static bool mouse_present, controller_busy, led_dirty;
static const char *mouse_description = "none (not probed)";
static bool i8042_device_command(bool aux, u_char command);

static const u_char normmap[0x59] = {
/* 00 */ 0,    0x1B, '1',  '2',  '3',  '4',  '5',  '6',  '7',  '8',  '9',  '0',  '-',  '=',  '\b', '\t',
/* 10 */ 'q',  'w',  'e',  'r',  't',  'y',  'u',  'i',  'o',  'p',  '[',  ']',  '\n', 0,    'a',  's',
/* 20 */ 'd',  'f',  'g',  'h',  'j',  'k',  'l',  ';',  '\'', '`',  0,    '\\', 'z',  'x',  'c',  'v',
/* 30 */ 'b',  'n',  'm',  ',',  '.',  '/',  0,    '*',  0,    ' ',  0,    F1,   F2,   F3,   F4,   F5,
/* 40 */ F6,   F7,   F8,   F9,   F10,  0,    0,    HOME, UP,   PGUP, '-',  LEFT, '5',  RT,   '+',  END,
/* 50 */ DOWN, PGDN, INS,  DEL,  0,    0,    '\\', F11,  F12
};

static const u_char shiftmap[0x59] = {
/* 00 */ 0,    0x1B, '!',  '@',  '#',  '$',  '%',  '^',  '&',  '*',  '(',  ')',  '_',  '+',  '\b', '\t',
/* 10 */ 'Q',  'W',  'E',  'R',  'T',  'Y',  'U',  'I',  'O',  'P',  '{',  '}',  '\n', 0,    'A',  'S',
/* 20 */ 'D',  'F',  'G',  'H',  'J',  'K',  'L',  ':',  '"',  '~',  0,    '|',  'Z',  'X',  'C',  'V',
/* 30 */ 'B',  'N',  'M',  '<',  '>',  '?',  0,    '*',  0,    ' ',  0,    F1,   F2,   F3,   F4,   F5,
/* 40 */ F6,   F7,   F8,   F9,   F10,  0,    0,    HOME, UP,   PGUP, '-',  LEFT, '5',  RT,   '+',  END,
/* 50 */ DOWN, PGDN, INS,  DEL,  0,    0,    '|',  F11,  F12
};

/* keypad with Num Lock on (scan codes 0x47..0x53) */
static const u_char numpad[13] = { '7', '8', '9', '-', '4', '5', '6', '+', '1', '2', '3', '0', '.' };

/**** controller ****/

/* LED replies share port 0x60 with key codes and mouse packets. Consume only
   keyboard ACKs, dispatch everything else, and defer a nested lock-key change. */
static void setleds(void) {
    u_long flags = irq_save();
    led_dirty = true;
    if(present && !controller_busy) {
        u_char value = led_status;
        controller_busy = true;
        led_dirty = false;
        if(i8042_device_command(false, 0xED)) i8042_device_command(false, value);
        controller_busy = false;
    }
    irq_restore(flags);
}

void setLED(u_char ledStatus, bool yn) {
    if(yn) led_status |= ledStatus;
    else led_status &= ~ledStatus;
    setleds();
}

void switchLED(u_char ledStatus) {
    led_status ^= ledStatus;
    setleds();
}

int testLED(u_char ledStatus) {
    return (led_status & ledStatus) ? 1 : 0;
}

u_long keyboard_irqs(void) {
    return irqs;
}

/**** translation ****/

static void deliver(u_char c) {
    if(c >= F1 && c < F1 + NUM_CONSOLES) {      /* F1..Fn: switch console */
        tty_switch(c - F1);
        return;
    }
    tty_key(tty_active, c);                     /* raw mode turns special keys into escape sequences */
}

static void handle_e0(u_char code) {
    bool brk = code & 0x80;
    code &= 0x7F;
    switch(code) {
    case 0x1D: ctrl = !brk; return;           /* right control */
    case 0x38: alt = !brk; return;            /* right alt */
    case 0x1C: if(!brk) deliver(tty_active->raw ? '\r' : '\n'); return; /* keypad enter */
    case 0x35: if(!brk) deliver('/'); return;  /* keypad / */
    case 0x47: if(!brk) deliver(HOME); return;
    case 0x48: if(!brk) deliver(UP); return;
    case 0x49: if(!brk) deliver(PGUP); return;
    case 0x4B: if(!brk) deliver(LEFT); return;
    case 0x4D: if(!brk) deliver(RT); return;
    case 0x4F: if(!brk) deliver(END); return;
    case 0x50: if(!brk) deliver(DOWN); return;
    case 0x51: if(!brk) deliver(PGDN); return;
    case 0x52: if(!brk) deliver(INS); return;
    case 0x53:
        if(brk) return;
        if(ctrl && alt) { reboot(); return; }  /* never returns; spins its delays in ISR context */
        deliver(DEL);
        return;
    default: return;                          /* print screen, pause, media keys */
    }
}

/* one byte from the keyboard, from the interrupt or the poll */
static void keyboard_byte(u_char code) {
    bool brk, shift;
    u_char c;

    if(code == 0xE0) { e0 = true; return; }
    if(code == 0xE1) { return; }               /* pause: ignore the sequence */
    if(e0) { e0 = false; handle_e0(code); return; }

    brk = code & 0x80;
    code &= 0x7F;

    switch(code) {                             /* modifiers and locks */
    case 0x2A: shift_l = !brk; return;
    case 0x36: shift_r = !brk; return;
    case 0x1D: ctrl = !brk; return;
    case 0x38: alt = !brk; return;
    case 0x3A: if(!brk) switchLED(CAPS_LED); return;
    case 0x45: if(!brk) switchLED(NUM_LED); return;
    case 0x46: if(!brk) switchLED(SCROLL_LED); return;
    }
    if(brk || code >= sizeof(normmap)) return;

    shift = shift_l || shift_r;
    if(code >= 0x47 && code <= 0x53 && (led_status & NUM_LED) && !shift) {
        c = numpad[code - 0x47];
    } else {
        c = shift ? shiftmap[code] : normmap[code];
        if((led_status & CAPS_LED) && c >= 'a' && c <= 'z') c -= 32;
        else if((led_status & CAPS_LED) && c >= 'A' && c <= 'Z' && shift) c += 32;
    }
    if(!c) return;
    if(ctrl && c < 0x80) {
        if(c >= 'a' && c <= 'z') c -= 32;
        if(c >= '@' && c <= '_') c &= 0x1F;     /* Ctrl-C = 3, Ctrl-U = 21, ... */
        else return;
    }
    if(tty_active->raw && (code == 0x0E || code == 0x1C))
        c = code == 0x0E ? 0x7F : '\r';         /* raw mode: Backspace and Enter as a terminal sends them */
    deliver(c);
}

/* All readers use this one demultiplexer. ACKs never enter the scan-code
   translator, including between an E0 prefix and its following key code. */
static void i8042_byte(u_char status, u_char byte) {
    if(status & KBD_ERROR) {
        if(status & KBD_AUX) ps2mouse_reset(&mouse);
        else { e0 = shift_l = shift_r = ctrl = alt = false; }
        return;
    }
    if(status & KBD_AUX) {
        struct ps2mouse_event event;
        if(mouse_present && ps2mouse_byte(&mouse, byte, (u32)getticks() * (1000 / HZ), &event))
            pointer_report(POINTER_PS2, event.dx, event.dy, 0, event.buttons);
    } else if(byte != 0xFA && byte != 0xFE && byte != 0xFC && byte != 0xFD) {
        keyboard_byte(byte);
    }
}

/* Drain both ports even when called by just one IRQ: an AUX byte at the front
   must not prevent the keyboard behind it from reaching its tty (or vice versa). */
static void i8042_drain(bool poll) {
    int n;
    for(n = 0; n < 32; n++) {
        u_char status = inb(KBD_STATUS);
        if(status == 0xFF || !(status & KBD_OBF)) break;
        if(poll) polled++;
        i8042_byte(status, inb(KBD_PORT));
    }
}

int keyboard_handler(u_int irq, void *param) {
    if(irq == IRQ_KEYBOARD) irqs++;
    i8042_drain(false);
    if(led_dirty && !controller_busy) setleds();
    return 0;
}

/* The laptop timer provides the same fallback for both PS/2 ports. */
void keyboard_poll(void) {
    if(!polling) return;
    in_irq++;
    i8042_drain(true);
    if(led_dirty && !controller_busy) setleds();
    in_irq--;
}

/**** the shared 8042 controller ****/

/* until the controller has taken our last byte; false after about 50 ms */
static bool i8042_ready(void) {
    u_int i;
    for(i = 0; i < 5000; i++) {
        u_char status = inb(KBD_STATUS);
        if(status == 0xFF) return false;
        if(!(status & KBD_IBF)) return true;
        udelay(10);
    }
    return false;
}

static bool i8042_command(u_char cmd) {
    if(!i8042_ready()) return false;
    outb(KBD_STATUS, cmd);
    return true;
}

/* A selected device's reply, never a byte from the other port. For keyboard
   commands only ACK/RESEND/error is a reply; intervening scan codes still go to
   the tty. Raw controller reads are used only while the keyboard is inhibited. */
static int i8042_reply(bool aux, bool ack, unsigned timeout_ms) {
    u_int i;
    for(i = 0; i < timeout_ms * 100; i++) {
        u_char status = inb(KBD_STATUS);
        if(status == 0xFF) return -1;
        if(status & KBD_OBF) {
            u_char byte = inb(KBD_PORT);
            if(!(status & KBD_ERROR) && !!(status & KBD_AUX) == !!aux &&
               (!ack || byte == 0xFA || byte == 0xFE || byte == 0xFC || byte == 0xFD))
                return byte;
            i8042_byte(status, byte);
        }
        udelay(10);
    }
    return -1;
}

static bool i8042_device_command(bool aux, u_char command) {
    unsigned retry;
    for(retry = 0; retry < 2; retry++) {
        int reply;
        if(aux && !i8042_command(0xD4)) return false;
        if(!i8042_ready()) return false;
        outb(KBD_PORT, command);
        reply = i8042_reply(aux, true, 50);
        if(reply == 0xFA) return true;
        if(reply != 0xFE) return false;
    }
    return false;
}

static bool i8042_config(u_char config) {
    if(!i8042_command(0x60) || !i8042_ready()) return false;
    outb(KBD_PORT, config);
    return true;
}

/* Inhibit both ports before a controller reply (untagged on the keyboard side).
   Preserve pending key codes rather than flushing them as command responses. */
static int i8042_read_config(void) {
    if(inb(KBD_STATUS) == 0xFF || !i8042_command(0xAD) || !i8042_command(0xA7)) return -1;
    if(!i8042_ready()) return -1;
    i8042_drain(false);
    if(!i8042_command(0x20)) return -1;
    return i8042_reply(false, false, 50);
}

/* The laptop keyboard needs translation enabled explicitly. The emulator keeps
   its firmware's keyboard configuration; mouse setup changes only AUX bits. */
static int i8042_setup(void) {
    int cfg = i8042_read_config();
    if(cfg < 0) { i8042_command(0xAE); return 1; }
    config_before = cfg;
    cfg = (cfg | 0x01 | 0x20 | 0x40) & ~(0x02 | 0x10);
    if(!i8042_config(cfg)) { i8042_command(0xAE); return 1; }
    i8042_command(0xAE);
    i8042_device_command(false, 0xF4);
    config_after = cfg;
    return 0;
}

static void i8042_mouse_setup(void) {
    int cfg;
    ps2mouse_reset(&mouse);
    pointer_device(POINTER_PS2, false);
    if(bootinfo_has_arg(&bootinfo, "mouse=off")) {
        mouse_description = "disabled (mouse=off)";
        return;
    }
    mouse_description = "none (no PS/2 mouse answered)";
    if(!present) return;
    cfg = i8042_read_config();
    if(cfg < 0) { i8042_command(0xAE); return; }
    /* read_config inhibited the keyboard; restore it while waiting on the AUX
       device, so real keyboard input remains live and is demultiplexed below. */
    cfg = (cfg | 0x20) & ~(0x02 | 0x10);
    if(!i8042_config(cfg) || !i8042_command(0xAE) || !i8042_command(0xA8)) goto failed;
    /* Reset also clears any wheel protocol left by firmware. F6 alone does not.
       BAT can take 500 ms on a physical device; every wait remains bounded. */
    if(!i8042_device_command(true, 0xFF) || i8042_reply(true, false, 750) != 0xAA ||
       i8042_reply(true, false, 50) != 0x00) goto failed;
    if(!i8042_device_command(true, 0xF6) || !i8042_device_command(true, 0xEA)) goto failed;
    if(!i8042_config((cfg | 0x02) & ~0x20)) goto failed;
    if(!i8042_device_command(true, 0xF4)) goto failed;
    config_after = (cfg | 0x02) & ~0x20;
    mouse_present = true;
    ps2mouse_reset(&mouse);
    pointer_device(POINTER_PS2, true);
    add_irq_handler(IRQ_MOUSE, keyboard_handler, 0);
    mouse_description = polling ? "PS/2 three-button mouse (IRQ 12 + poll)" : "PS/2 three-button mouse (IRQ 12)";
    return;
failed:
    i8042_command(0xA7);
    i8042_config(cfg);
    i8042_command(0xAE);
    config_after = cfg;
}

void init_keyboard() {
    /* IRQ1 is installed below before AUX probing. Keep its handler from taking
       a controller reply away from the bounded synchronous setup transaction. */
    u_long flags = irq_save();
    kprintf("\nKeyboard Initialization\n");
    controller_busy = true;
    if(bootinfo.laptop) {
        if(i8042_setup() != 0) {
            present = false;
            kprintf("*no 8042 keyboard controller answers: no keyboard (USB keyboards are not supported)\n");
        } else {
            kprintf("*8042 command byte 0x%02x -> 0x%02x\n", config_before & 0xFF, config_after & 0xFF);
        }
        use_irq = present && !bootinfo_has_arg(&bootinfo, "kbd=poll");
        polling = present && !bootinfo_has_arg(&bootinfo, "kbd=irq");
    }
    if(use_irq) {
        kprintf("*Load ISR Handler for IRQ1\n");
        add_irq_handler(IRQ_KEYBOARD, keyboard_handler, 0);
    }
    if(polling) kprintf("*and polled on every timer tick\n");
    i8042_mouse_setup();
    kprintf("*Mouse: %s\n", mouse_description);
    controller_busy = false;
    if(present) setleds();
    irq_restore(flags);
    kprintf("*DONE\n\n");
}

const char *keyboard_describe(char *buf, size_t size) {
    if(!present) {
        strlcpy(buf, "none (no 8042 controller answered)", size);
        return buf;
    }
    snprintf(buf, size, "8042 (scan code set 1)%s%s, %lu IRQs, %lu bytes polled", use_irq ? ", IRQ 1" : "",
             polling ? ", polled" : "", irqs, polled);
    if(config_after >= 0) {
        size_t n = strlen(buf);
        snprintf(buf + n, size - n, ", command byte 0x%02x", config_after & 0xFF);
    }
    return buf;
}

const char *mouse_ps2_describe(void) {
    return mouse_description;
}
