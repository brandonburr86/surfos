/*
SurfOS Pointer Input
--------------------
File: pointer.h Date: 10/10/26
--------------------
Relative pointing devices share a bounded screen-space queue. Kernel callers only.
*/
#ifndef _SYS_POINTER_H
#define _SYS_POINTER_H
#include <surfos/types.h>
#define POINTER_LEFT 1
#define POINTER_RIGHT 2
#define POINTER_MIDDLE 4
#define POINTER_PS2 0
#define POINTER_USB_FIRST 1
#define POINTER_USB_COUNT 8
#define POINTER_KEYS 9
#define POINTER_SOURCES 10
#define POINTER_QUEUE 64
struct pointer_event { int x, y, wheel; unsigned buttons; };
/* Safe from IRQ or task context. dx/dy are relative, +y moves down. */
void pointer_report(unsigned source, int dx, int dy, int wheel, unsigned buttons);
void pointer_device(unsigned source, bool present); /* removal releases only this device's buttons */
void pointer_limits(int width, int height); /* GUI entry: center, reset queue, keep device buttons */
bool pointer_read(struct pointer_event *event);
void pointer_snapshot(struct pointer_event *event);
void pointer_speed(int value); /* 1, 2 or 3 logical pixels per device count */
unsigned pointer_devices(void);
unsigned pointer_dropped(void);
const char *mouse_ps2_describe(void);
#endif
