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
10/25/26 (track A, A4): each key goes with the modifiers held (KEY_SHIFT, KEY_ALT,
KEY_CTRL), which raw mode sends as xterm does (kernel/tty.c); in raw mode Ctrl+Space
is NUL, Ctrl+/ 0x1f, Shift+Tab ESC[Z, and Ctrl leaves Backspace, Enter and Tab alone.
Line mode is as it was.
10/25/26 (track A, A6): Shift+PgUp, PgDn, Home and End move the screen through the
console's scrollback (console.c) before the tty sees them, unless a graphics session
(the desktop) holds the screen or there is no framebuffer; any other key shows the
live screen again first.

10/5/26 (USB boot): a laptop's built-in keyboard sits behind an 8042 that the
embedded controller emulates, and UEFI firmware may hand it over with its interrupt
or its scan-code translation switched off. With profile=laptop the controller is set
up again (every wait bounded, so a missing 8042 only means no keyboard), and it is
also polled on every timer tick in case IRQ 1 never reaches the CPU. The handler only
reads port 0x60 when the status register says a byte is waiting, so the interrupt and
the poll never take the same byte twice. 10/10/26: the same controller also supplies
standard PS/2 mouse packets from IRQ 12, demultiplexed from keyboard scan codes.
10/25/26 (robustness, L23): Ctrl-Alt-Del starts the shutdown, through init (kernel/shutdown.c),
and pressed again it resets the machine at once.
10/26/26 (track C's C8): the IntelliMouse handshake finds a wheel mouse, whose wheel is
reported with its motion.
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
#include <surfos/shutdown.h>
#include <blibc_common.h>
#include <sys/pointer.h>
#include <sys/ps2mouse.h>

static u_char led_status = NUM_LED;
static bool shift_l, shift_r, ctrl, alt, e0;
static struct del_key del_key;      /* 10/25/26 (the review of B4): Delete held down, for Ctrl+Alt+Del's presses */
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

/* 10/25/26 (track A, A4): the modifiers held now, which raw mode sends with a key (tty.c) */
static u_int mods_held(void) {
    return (shift_l || shift_r ? KEY_SHIFT : 0) | (alt ? KEY_ALT : 0) | (ctrl ? KEY_CTRL : 0);
}

u_int keyboard_mods(void) { return mods_held(); }

static void deliver(u_char c) {
    u_int mods = mods_held();
    if(c >= F1 && c < F1 + NUM_CONSOLES) {      /* F1..Fn: switch console */
        tty_switch(c - F1);
        return;
    }
    if(mods == KEY_SHIFT && console_scroll_key(c)) return;     /* 10/25/26 (A6): the scrollback's keys */
    console_view_live();
    tty_key(tty_active, c, mods);               /* raw mode turns special keys into escape sequences */
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
    case 0x53:                                /* 10/25/26 (L23): Ctrl+Alt+Del: init starts the shutdown; again: a reset */
        switch(del_key_event(&del_key, !brk, ctrl && alt)) {
        case DEL_PRESS: shutdown_key(); break;
        case DEL_DELETE: deliver(DEL); break;
        }
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
        else if(tty_active->raw && c == ' ') c = 0;         /* 10/25/26 (A4): Ctrl+Space is NUL, as xterm sends it, */
        else if(tty_active->raw && c == '/') c = 0x1F;      /* and Ctrl+/ 0x1f */
        else if(!tty_active->raw || (code != 0x0E && code != 0x1C && code != 0x0F)) return;
                                                /* raw mode: Backspace, Enter and Tab stay themselves */
    }
    if(tty_active->raw && code == 0x0F && shift) c = BACKTAB;   /* 10/25/26 (A4): Shift+Tab, ESC[Z (tty.c) */
    if(tty_active->raw && (code == 0x0E || code == 0x1C))
        c = code == 0x0E ? 0x7F : '\r';         /* raw mode: Backspace and Enter as a terminal sends them */
    deliver(c);
}

/* All readers use this one demultiplexer. ACKs never enter the scan-code
   translator, including between an E0 prefix and its following key code. */
static void i8042_byte(u_char status, u_char byte) {
    if(status & KBD_ERROR) {
        if(status & KBD_AUX) ps2mouse_reset(&mouse);
        else { e0 = shift_l = shift_r = ctrl = alt = false; del_key.down = false; }
        return;
    }
    if(status & KBD_AUX) {
        struct ps2mouse_event event;
        if(mouse_present && ps2mouse_byte(&mouse, byte, (u32)getticks() * (1000 / HZ), &event))
            pointer_report(POINTER_PS2, event.dx, event.dy, event.wheel, event.buttons);
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

/* the mouse reset to its defaults, three-byte packets in stream mode: the reset (BAT can take 500 ms on a physical
   device; every wait stays bounded), defaults, stream mode */
static bool i8042_mouse_reset(void) {
    return i8042_device_command(true, 0xFF) && i8042_reply(true, false, 750) == 0xAA &&
           i8042_reply(true, false, 50) == 0x00 && i8042_device_command(true, 0xF6) && i8042_device_command(true, 0xEA);
}

/* 10/26/26 (C8): the IntelliMouse handshake: sample rates 200, 100 and 80, then the ID, 3 for a wheel mouse, whose
   packets have a fourth byte from then on: 1 for one, 0 for a three-button mouse, -1 when the mouse could not be
   put back. A mouse takes the four-byte packets at the third rate, before the ID is asked: once 80 was taken, or
   may have been (the third pair failed: its ACK lost, or later than the wait), the ID says which; one that does not
   come is asked once more, and a mouse that still says none, or names another protocol, is reset to three-byte
   packets (F6 alone does not undo it). The rate is 100 a second again whatever happens (the reset's) */
static int i8042_wheel(void) {
    static const u_char rates[] = { 200, 100, 80 };
    int i, id = 0;
    for(i = 0; i < 3; i++)
        if(!i8042_device_command(true, 0xF3) || !i8042_device_command(true, rates[i])) break;
    if(i >= 2) {
        id = i8042_device_command(true, 0xF2) ? i8042_reply(true, false, 50) : -1;
        if(id < 0) id = i8042_device_command(true, 0xF2) ? i8042_reply(true, false, 50) : -1;
        if(id != 0 && id != 3) return i8042_mouse_reset() ? 0 : -1;
    }
    if(i8042_device_command(true, 0xF3)) i8042_device_command(true, 100);
    return id == 3;
}

static void i8042_mouse_setup(void) {
    int cfg, wheel;
    ps2mouse_set_wheel(&mouse, false);
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
    /* Reset also clears any wheel protocol left by firmware. F6 alone does not. */
    if(!i8042_mouse_reset()) goto failed;
    wheel = i8042_wheel();                              /* 10/26/26 (C8) */
    if(wheel < 0) goto failed;
    if(!i8042_config((cfg | 0x02) & ~0x20)) goto failed;
    if(!i8042_device_command(true, 0xF4)) goto failed;
    config_after = (cfg | 0x02) & ~0x20;
    mouse_present = true;
    ps2mouse_set_wheel(&mouse, wheel == 1);
    pointer_device(POINTER_PS2, true);
    add_irq_handler(IRQ_MOUSE, keyboard_handler, 0);
    mouse_description = wheel ? (polling ? "PS/2 wheel mouse (IRQ 12 + poll)" : "PS/2 wheel mouse (IRQ 12)")
                              : (polling ? "PS/2 three-button mouse (IRQ 12 + poll)" : "PS/2 three-button mouse (IRQ 12)");
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
