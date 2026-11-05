/*
SurfOS Keyboard Handler
----------------------
File: keyboard.h    Date: 4/23/04, rebuilt 10/2026 (roadmap C2)
----------------------
(C)2004 Brandon Burr
*/

#ifndef _SURFOS_KEYBOARD_H
#define _SURFOS_KEYBOARD_H

#include <surfos/types.h>
#include <surfos/interrupt.h>

/* Special keys arrive in the tty input stream as these codes (above ASCII) */
#define F1     0x80
#define F2    (F1 + 1)
#define F3    (F2 + 1)
#define F4    (F3 + 1)
#define F5    (F4 + 1)
#define F6    (F5 + 1)
#define F7    (F6 + 1)
#define F8    (F7 + 1)
#define F9    (F8 + 1)
#define F10   (F9 + 1)
#define F11   (F10 + 1)
#define F12   (F11 + 1)
#define INS   0x8C
#define DEL   (INS + 1)
#define HOME  (DEL + 1)
#define END   (HOME + 1)
#define PGUP  (END + 1)
#define PGDN  (PGUP + 1)
#define LEFT  0x92
#define UP    (LEFT + 1)
#define DOWN  (UP + 1)
#define RT    (DOWN + 1)
#define BACKTAB (RT + 1)        /* 10/25/26: Shift+Tab, in raw mode only (line mode keeps its Tab) */

/* 10/25/26 (track A, A4): the modifiers held with a key (tty_key()); in raw mode they become xterm's
   parameter m = 1 + these on the special keys' sequences, and Alt on any other key an ESC before it */
#define KEY_SHIFT 1
#define KEY_ALT   2
#define KEY_CTRL  4
#define KEY_REPEAT 16   /* 11/05/26: the keyboard repeats the key held (typematic); TTY_RAW_KEYS sends it (tty.h) */

#define KBD_PORT    0x60
#define KBD_STATUS  0x64

#define NUM_LED  0x02
#define CAPS_LED 0x04
#define SCROLL_LED 0x01

void init_keyboard(void);
int keyboard_handler(u_int irq, void *param);
void keyboard_poll(void);       /* from the timer tick; does nothing unless profile=laptop turned polling on */
u_int keyboard_mods(void);      /* 11/1/26: KEY_SHIFT, KEY_ALT, KEY_CTRL held now */
struct tty;
bool keyboard_print_taken(const struct tty *t);   /* 11/05/26: Print Screen pressed on console t, taken (the desktop's) */
const char *keyboard_describe(char *buf, size_t size);

int testLED(u_char ledStatus);
void switchLED(u_char ledStatus);
void setLED(u_char ledStatus, bool yn);

u_long keyboard_irqs(void);

#endif
