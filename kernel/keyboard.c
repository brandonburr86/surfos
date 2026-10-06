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
reads port 0x60 when the status register says a keyboard byte is waiting, so the
interrupt and the poll never take the same byte twice.
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

static u_char led_status = NUM_LED;
static bool shift_l, shift_r, ctrl, alt, e0;
static u_long irqs, polled;

/* status register bits */
#define KBD_OBF  0x01           /* a byte is waiting in port 0x60 */
#define KBD_IBF  0x02           /* the controller has not taken our last byte yet */
#define KBD_AUX  0x20           /* the waiting byte is from the mouse port */

static bool present = true;     /* an 8042 answers (assumed, as in 2004, unless profile=laptop probed it) */
static bool use_irq = true, polling;
static int config_before = -1, config_after = -1;

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

static void kbd_wait_write(void) {
    u_long timeout;
    for(timeout = 100000; timeout; timeout--) {
        if(!(inb(KBD_STATUS) & 0x02)) return;
    }
}

static void setleds(void) {
    u_long flags = irq_save();
    kbd_wait_write();
    outb(KBD_PORT, 0xED);
    kbd_wait_write();
    outb(KBD_PORT, led_status);
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

int keyboard_handler(u_int irq, void *param) {
    u_char status = inb(KBD_STATUS);
    u_char code;

    irqs++;
    if(!(status & KBD_OBF)) return 0;          /* the poll took it already, or a spurious IRQ */
    code = inb(KBD_PORT);
    if(status & KBD_AUX) return 0;             /* the mouse port is off; drop strays */
    keyboard_byte(code);
    return 0;
}

/* from the timer tick (profile=laptop): whatever IRQ 1 did not deliver */
void keyboard_poll(void) {
    int n;
    if(!polling) return;
    in_irq++;                                  /* the same rules as the interrupt handler */
    for(n = 0; n < 8; n++) {
        u_char status = inb(KBD_STATUS), code;
        if(status == 0xFF || !(status & KBD_OBF)) break;
        code = inb(KBD_PORT);
        polled++;
        if(!(status & KBD_AUX)) keyboard_byte(code);
    }
    in_irq--;
}

/**** the 8042 controller (profile=laptop) ****/

/* until the controller has taken our last byte; false after about 50 ms */
static bool i8042_ready(void) {
    u_int i;
    for(i = 0; i < 5000; i++) {
        if(!(inb(KBD_STATUS) & KBD_IBF)) return true;
        udelay(10);
    }
    return false;
}

/* a byte from the controller, or -1 after about 50 ms */
static int i8042_read(void) {
    u_int i;
    for(i = 0; i < 5000; i++) {
        if(inb(KBD_STATUS) & KBD_OBF) return inb(KBD_PORT);
        udelay(10);
    }
    return -1;
}

static bool i8042_command(u_char cmd) {
    if(!i8042_ready()) return false;
    outb(KBD_STATUS, cmd);
    return true;
}

static void i8042_flush(void) {
    u_int i;
    for(i = 0; i < 64 && (inb(KBD_STATUS) & KBD_OBF); i++) {
        inb(KBD_PORT);
        udelay(50);
    }
}

/* The controller as this driver expects it: keyboard port on, with its interrupt and
   the translation to scan code set 1; mouse port off. 1 when no 8042 answers. */
static int i8042_setup(void) {
    int cfg;
    if(inb(KBD_STATUS) == 0xFF) return 1;        /* nothing decodes the port */
    i8042_flush();
    if(!i8042_command(0x20) || (cfg = i8042_read()) < 0) return 1;   /* read the command byte */
    config_before = cfg;
    cfg = (cfg | 0x01 | 0x20 | 0x40) & ~(0x02 | 0x10);  /* IRQ 1, mouse clock off, translate; no IRQ 12, keyboard clock on */
    if(!i8042_command(0x60) || !i8042_ready()) return 1;
    outb(KBD_PORT, cfg);
    i8042_command(0xA7);                          /* mouse port off */
    i8042_command(0xAE);                          /* keyboard port on */
    if(i8042_ready()) {
        outb(KBD_PORT, 0xF4);                     /* to the keyboard: scanning on */
        i8042_read();                             /* its 0xFA, if it answers */
    }
    i8042_flush();
    if(i8042_command(0x20) && (cfg = i8042_read()) >= 0) config_after = cfg;
    return 0;
}

void init_keyboard() {
    kprintf("\nKeyboard Initialization\n");
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
    if(present) setleds();
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
