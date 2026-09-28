/*
SurfOS Keyboard Handler
----------------------
File: template.h    Date: 4/23/04
----------------------
(C)2004 Brandon Burr
*/

#ifndef _SURFOS_KEYBOARD_H
#define _SURFOS_KEYBOARD_H

#include <surfos/types.h>
#include <surfos/interrupt.h>

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
#define SCROL 0x96
#define CAPS  (SCROL + 1)
#define NUM   (CAPS + 1)
#define SHIFT 0x99
#define ALT   (SHIFT + 1)
#define CTRL  (ALT + 1)

#define KEY_INS     0x90
#define KEY_DEL     (KEY_INS + 1)
#define KEY_HOME    (KEY_DEL + 1)
#define KEY_END     (KEY_HOME + 1)
#define KEY_PGUP    (KEY_END + 1)
#define KEY_PGDN    (KEY_PGUP + 1)
#define KEY_LFT     (KEY_PGDN + 1)
#define KEY_UP      (KEY_LFT + 1)
#define KEY_DN      (KEY_UP + 1)
#define KEY_RT      (KEY_DN + 1)
/* print screen/sys rq and pause/break */
#define KEY_PRNT    (KEY_RT + 1)
#define KEY_PAUSE   (KEY_PRNT + 1)
/* these return a value but they could also act as additional bucky keys */
#define KEY_LWIN    (KEY_PAUSE + 1)
#define KEY_RWIN    (KEY_LWIN + 1)
#define KEY_MENU    (KEY_RWIN + 1)

#define KBD_PORT    0x60

#define KBD_CMD_RESET 0x0FF /* Reset the keyboard and start internal diagnostics*/
#define KBD_CMD_RESEND 0x0FE /* Resend the last transmission */
#define KBD_CMD_NOP 0x0FD /* NOP */
#define KBD_CMD_DEFAULT_CONTINUE 0x0F6 /* Set keyboard to defaults and continue scanning */
#define KBD_CMD_DEFAULT_DISABLE 0x0F5 /* Set keyboard to defaults and disable keyboard scanning */
#define KBD_CMD_ENABLE 0x0F4 /* Enable the keyboard. Kybd sends 'ACK', clears buffer, and starts scanning */

#define KEY_QUEUE_LEN 255 //length of the key buffer queue

#define NUM_LED  0x02
#define CAPS_LED 0x04
#define SCROLL_LED 0x01

#define CAPS_LOCK  0xBA
#define NUM_LOCK  0xC5
#define SCROLL_LOCK 0xC6
#define LEFT_SHIFT 0x2A
#define RIGHT_SHIFT 0x36

#define RAW_CTRL    0x1D
#define RAW_ALT 0x38
#define RAW_LSHIFT 0x2A
#define RAW_RSHIFT 0x36

#define KBD_RAW_ALT 0x38
#define KBD_RAW_CTRL    0x1D
#define KBD_RAW_LSHIFT  0x2A
#define KBD_RAW_RSHIFT  0x36

#define KBD_BRK_ALT 0xb8
#define KBD_BRK_CTRL    0x9d
#define KBD_BRK_LSHIFT  0xaA
#define KBD_BRK_RSHIFT  0xb6

void init_keyboard();
FuncIRQHandler keyboard_handler();
u_char map_scancode(int);

void clear_key_queue();
void push_key_queue(u_char ch);
u_char pop_key_queue();

int testLED(u_char ledStatus);
void switchLED(u_char ledStatus);
void setLED(u_char ledStatus, bool yn);

#endif
