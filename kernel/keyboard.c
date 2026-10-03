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
#include <blibc_common.h>

static u_char led_status = NUM_LED;
static bool shift_l, shift_r, ctrl, alt, e0;
static u_long irqs;

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
    tty_input(tty_active, c);
}

static void handle_e0(u_char code) {
    bool brk = code & 0x80;
    code &= 0x7F;
    switch(code) {
    case 0x1D: ctrl = !brk; return;           /* right control */
    case 0x38: alt = !brk; return;            /* right alt */
    case 0x1C: if(!brk) deliver('\n'); return; /* keypad enter */
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

int keyboard_handler(u_int irq, void *param) {
    u_char code = inb(KBD_PORT);
    bool brk, shift;
    u_char c;

    irqs++;
    if(code == 0xE0) { e0 = true; return 0; }
    if(code == 0xE1) { return 0; }             /* pause: ignore the sequence */
    if(e0) { e0 = false; handle_e0(code); return 0; }

    brk = code & 0x80;
    code &= 0x7F;

    switch(code) {                             /* modifiers and locks */
    case 0x2A: shift_l = !brk; return 0;
    case 0x36: shift_r = !brk; return 0;
    case 0x1D: ctrl = !brk; return 0;
    case 0x38: alt = !brk; return 0;
    case 0x3A: if(!brk) switchLED(CAPS_LED); return 0;
    case 0x45: if(!brk) switchLED(NUM_LED); return 0;
    case 0x46: if(!brk) switchLED(SCROLL_LED); return 0;
    }
    if(brk || code >= sizeof(normmap)) return 0;

    shift = shift_l || shift_r;
    if(code >= 0x47 && code <= 0x53 && (led_status & NUM_LED) && !shift) {
        c = numpad[code - 0x47];
    } else {
        c = shift ? shiftmap[code] : normmap[code];
        if((led_status & CAPS_LED) && c >= 'a' && c <= 'z') c -= 32;
        else if((led_status & CAPS_LED) && c >= 'A' && c <= 'Z' && shift) c += 32;
    }
    if(!c) return 0;
    if(ctrl && c < 0x80) {
        if(c >= 'a' && c <= 'z') c -= 32;
        if(c >= '@' && c <= '_') c &= 0x1F;     /* Ctrl-C = 3, Ctrl-U = 21, ... */
        else return 0;
    }
    deliver(c);
    return 0;
}

void init_keyboard() {
    kprintf("\nKeyboard Initialization\n");
    kprintf("*Load ISR Handler for IRQ1\n");
    add_irq_handler(IRQ_KEYBOARD, keyboard_handler, 0);
    setleds();
    kprintf("*DONE\n\n");
}
