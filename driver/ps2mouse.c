/*
SurfOS PS/2 Mouse Packet Decoder
--------------------
File: ps2mouse.c Date: 10/10/26
--------------------
No port access: the keyboard controller demultiplexes bytes and consumes replies.
*/
#include <sys/ps2mouse.h>

void ps2mouse_reset(struct ps2mouse *mouse) {
    mouse->used = 0;
    mouse->last_ms = 0;
}

bool ps2mouse_byte(struct ps2mouse *mouse, u8 byte, u32 now_ms, struct ps2mouse_event *event) {
    u8 header;
    if(mouse->used && (u32)(now_ms - mouse->last_ms) > 100) mouse->used = 0;
    mouse->last_ms = now_ms;
    if(!mouse->used) {
        /* These replies cannot be ordinary non-overflow packet headers. Only
           filter at the packet boundary: every byte is legal coordinate data. */
        if(!(byte & 8) || byte == 0xfa || byte == 0xfe || byte == 0xfc ||
           byte == 0xfd || byte == 0xaa) return false;
    }
    mouse->packet[mouse->used++] = byte;
    if(mouse->used != 3) return false;
    mouse->used = 0;
    header = mouse->packet[0];
    event->buttons = header & 7;
    event->dx = event->dy = 0;
    if(!(header & 0xc0)) {
        /* Some mice leave a sign bit set when that axis has stopped moving. */
        if(mouse->packet[1]) event->dx = (int)mouse->packet[1] - ((header & 0x10) ? 256 : 0);
        if(mouse->packet[2]) event->dy = -((int)mouse->packet[2] - ((header & 0x20) ? 256 : 0));
    }
    return true;
}
