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

#define KBD_PORT    0x60
#define KBD_STATUS  0x64

#define NUM_LED  0x02
#define CAPS_LED 0x04
#define SCROLL_LED 0x01

void init_keyboard(void);
int keyboard_handler(u_int irq, void *param);

int testLED(u_char ledStatus);
void switchLED(u_char ledStatus);
void setLED(u_char ledStatus, bool yn);

u_long keyboard_irqs(void);

#endif
