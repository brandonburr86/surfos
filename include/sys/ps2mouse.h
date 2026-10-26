/*
SurfOS PS/2 Mouse Packet Decoder
--------------------
File: ps2mouse.h Date: 10/10/26
--------------------
Pure standard three-byte protocol; the 8042 lives in kernel/keyboard.c.
10/26/26 (stage C8 of docs/superpowers/plans/2026-10-25-app-windows.md): a wheel mouse's four-byte
packets too, once kernel/keyboard.c's IntelliMouse handshake has found one (ID 3).
*/
#ifndef _SYS_PS2MOUSE_H
#define _SYS_PS2MOUSE_H
#include <surfos/types.h>

struct ps2mouse {
    u8 packet[4];
    unsigned used;
    u32 last_ms;
    bool wheel;                         /* 10/26/26: four-byte packets, the fourth the wheel's */
};
/* wheel: notches, + away from the user (10/26/26) */
struct ps2mouse_event { int dx, dy, wheel; unsigned buttons; };
/* Reset after controller parity/timeout errors or starting a new device. 10/26/26: the protocol stays (wheel). */
void ps2mouse_reset(struct ps2mouse *mouse);
/* 10/26/26: a new device's protocol: four-byte packets for a wheel mouse (IntelliMouse, ID 3), else three; a reset */
void ps2mouse_set_wheel(struct ps2mouse *mouse, bool wheel);
/* +y is down. A complete packet produces one event, including button-only changes. */
bool ps2mouse_byte(struct ps2mouse *mouse, u8 byte, u32 now_ms, struct ps2mouse_event *event);
#endif
