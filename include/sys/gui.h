/*
SurfOS Desktop
--------------------
File: gui.h Date: 10/10/26
--------------------
Pure desktop model and renderer; no hardware, allocation or blocking calls.
*/
#ifndef _SYS_GUI_H
#define _SYS_GUI_H
#include <sys/framebuffer.h>
#include <sys/pointer.h>
#define GUI_CALC 0
#define GUI_SKETCH 1
#define GUI_ABOUT 2
#define GUI_SETTINGS 3
#define GUI_WINDOWS 4
#define GUI_CANVAS_W 400
#define GUI_CANVAS_H 180
#define GUI_UP 0x100
#define GUI_DOWN 0x101
#define GUI_LEFT 0x102
#define GUI_RIGHT 0x103
struct gui_window { int x,y,w,h; bool open; };
struct gui_calc { i32 value, operand; char op; bool fresh, error, entered; };
struct gui_state {
    int width, height, menu, selection, focus, drag, drag_x, drag_y;
    int x, y, last_x, last_y, brush, speed;
    unsigned buttons, devices;
    bool quit, dirty, night, drawing;
    int order[GUI_WINDOWS];
    struct gui_window win[GUI_WINDOWS];
    struct gui_calc calc;
    u8 canvas[GUI_CANVAS_W * GUI_CANVAS_H];
};
void gui_init(struct gui_state *g, int width, int height);
void gui_open(struct gui_state *g, int app);
void gui_pointer(struct gui_state *g, const struct pointer_event *e);
void gui_key(struct gui_state *g, int key);
void gui_render(const struct gui_state *g, const struct fb_surface *s);
void gui_cursor(const struct fb_surface *s, int x, int y, int scale);
void gui_calc_key(struct gui_calc *c, int key);
#endif
