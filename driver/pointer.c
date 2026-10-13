/*
SurfOS Pointer Input
--------------------
File: pointer.c Date: 10/10/26
--------------------
Single-CPU producer/consumer queue, shared by native pointing devices. Drop the
oldest event on overflow: the newest position and release always reach the GUI.
*/
#include <sys/pointer.h>
#include <surfos/irq.h>
static struct pointer_event queue[POINTER_QUEUE], state;
static unsigned head, count, dropped, buttons[POINTER_SOURCES];
static bool devices[POINTER_SOURCES];
static int width = 640, height = 400, speed = 1;
static void enqueue(void) {
    if(count == POINTER_QUEUE) { head = (head + 1) % POINTER_QUEUE; count--; dropped++; }
    queue[(head + count++) % POINTER_QUEUE] = state;
}
static int clamp(i64 n, int limit) { return n < 0 ? 0 : n >= limit ? limit - 1 : (int)n; }
static unsigned all_buttons(void) {
    unsigned i, b = 0;
    for(i = 0; i < POINTER_SOURCES; i++) b |= buttons[i];
    return b;
}
void pointer_report(unsigned source, int dx, int dy, int wheel, unsigned b) {
    unsigned old;
    u_long f;
    int x, y;
    if(source >= POINTER_SOURCES) return;
    f = irq_save();
    old = state.buttons;
    buttons[source] = b & 7;
    state.buttons = all_buttons();
    x = clamp((i64)state.x + (i64)dx * speed, width);
    y = clamp((i64)state.y + (i64)dy * speed, height);
    if(x != state.x || y != state.y || state.buttons != old || wheel) {
        state.x = x; state.y = y; state.wheel = wheel;
        enqueue();
    }
    state.wheel = 0;
    irq_restore(f);
}
void pointer_device(unsigned source, bool present) {
    u_long f;
    unsigned old;
    if(source >= POINTER_SOURCES) return;
    f = irq_save();
    devices[source] = present;
    old = state.buttons;
    if(!present) buttons[source] = 0;
    state.buttons = all_buttons();
    if(old != state.buttons) enqueue();
    irq_restore(f);
}
void pointer_limits(int w, int h) {
    u_long f = irq_save();
    width = w > 0 ? w : 1; height = h > 0 ? h : 1;
    state.x = width / 2; state.y = height / 2; state.wheel = 0;
    head = count = 0;
    irq_restore(f);
}
void pointer_speed(int value) {
    u_long f = irq_save();
    speed = value >= 1 && value <= 3 ? value : 1;
    irq_restore(f);
}
bool pointer_read(struct pointer_event *out) {
    u_long f = irq_save();
    if(!count) { irq_restore(f); return false; }
    *out = queue[head]; head = (head + 1) % POINTER_QUEUE; count--;
    irq_restore(f);
    return true;
}
void pointer_snapshot(struct pointer_event *out) {
    u_long f = irq_save(); *out = state; irq_restore(f);
}
unsigned pointer_devices(void) {
    unsigned i, n = 0;
    u_long f = irq_save();
    for(i = 0; i < POINTER_KEYS; i++) if(devices[i]) n++;
    irq_restore(f);
    return n;
}
unsigned pointer_dropped(void) {
    unsigned n;
    u_long f = irq_save(); n = dropped; irq_restore(f); return n;
}
