/*
SurfOS PS/2 Mouse Packet Decoder
--------------------
File: ps2mouse.h Date: 10/10/26
--------------------
Pure standard three-byte protocol; the 8042 lives in kernel/keyboard.c.
*/
#ifndef _SYS_PS2MOUSE_H
#define _SYS_PS2MOUSE_H
#include <surfos/types.h>

struct ps2mouse {
    u8 packet[3];
    unsigned used;
    u32 last_ms;
};
struct ps2mouse_event { int dx, dy; unsigned buttons; };
/* Reset after controller parity/timeout errors or starting a new device. */
void ps2mouse_reset(struct ps2mouse *mouse);
/* +y is down. A complete packet produces one event, including button-only changes. */
bool ps2mouse_byte(struct ps2mouse *mouse, u8 byte, u32 now_ms, struct ps2mouse_event *event);
#endif
