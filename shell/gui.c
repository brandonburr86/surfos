/*
SurfOS Desktop Command
--------------------
File: gui.c Date: 10/10/26
--------------------
Own the local graphics session, poll input, and present RAM-rendered rows in
small batches. No allocation, I/O or waiting inside a framebuffer callback.
*/
#include <stdio.h>
#include <string.h>
#include <mm/kalloc.h>
#include <surfos/task.h>
#include <surfos/timer.h>
#include <surfos/tty.h>
#include <surfos/irq.h>
#include <sys/gui.h>
#include <sys/touchpad.h>
#include "shell.h"

struct display {
    struct gui_state *gui;
    u32 *pixels;
    int width,height,scale,ox,oy;
    int left,top,right,bottom;
    bool first;
};
static void dimensions(const struct fb_surface *s,void *arg) {
    struct display *d=arg;
    d->scale=s->mode.width/640;
    if((int)s->mode.height/400<d->scale) d->scale=s->mode.height/400;
    if(d->scale<1) d->scale=1;
    d->width=s->mode.width/d->scale; d->height=s->mode.height/d->scale;
    if(d->width>1280) d->width=1280;
    if(d->height>800) d->height=800;
    d->ox=(s->mode.width-d->width*d->scale)/2;
    d->oy=(s->mode.height-d->height*d->scale)/2;
}
static void present_rows(const struct fb_surface *s,void *arg) {
    struct display *d=arg;
    int x,y,start;
    if(d->first) {
        fb_rect_rgb(s,0,0,s->mode.width,d->oy,0x102E3B);
        fb_rect_rgb(s,0,d->oy+d->height*d->scale,s->mode.width,s->mode.height-(d->oy+d->height*d->scale),0x102E3B);
        fb_rect_rgb(s,0,d->oy,d->ox,d->height*d->scale,0x102E3B);
        fb_rect_rgb(s,d->ox+d->width*d->scale,d->oy,s->mode.width-(d->ox+d->width*d->scale),d->height*d->scale,0x102E3B);
        d->first=false;
    }
    for(y=d->top;y<d->bottom;y++) for(x=d->left;x<d->right;) {
        u32 color=d->pixels[y*d->width+x];
        start=x++;
        while(x<d->right && d->pixels[y*d->width+x]==color) x++;
        fb_rect_rgb(s,d->ox+start*d->scale,d->oy+y*d->scale,(x-start)*d->scale,d->scale,color);
    }
}
static bool present(struct display *d,int x,int y,int w,int h) {
    int bottom=y+h,top;
    if(x<0) x=0;
    if(y<0) y=0;
    d->left=x; d->right=x+w;
    if(d->right>d->width) d->right=d->width;
    if(bottom>d->height) bottom=d->height;
    for(top=y;top<bottom;top+=16) {
        d->top=top; d->bottom=top+16<bottom?top+16:bottom;
        if(framebuffer_graphics_draw(present_rows,d)<0) return false;
    }
    return true;
}
static void pointer_paint(const struct fb_surface *s,void *arg) {
    struct display *d=arg;
    gui_cursor(s,d->ox+d->gui->x*d->scale,d->oy+d->gui->y*d->scale,d->scale);
}
/* Ownership and shared-pointer access are one transaction. A console switch can
   revoke us between any two instructions with interrupts enabled. */
static bool pointer_begin(struct display *d) {
    u_long flags=irq_save();
    bool owned=framebuffer_graphics_owned();
    if(owned) {
        struct pointer_event initial;
        pointer_device(POINTER_KEYS,false); pointer_speed(1);
        pointer_limits(d->width,d->height);
        pointer_snapshot(&initial);
        d->gui->buttons=initial.buttons;
    }
    irq_restore(flags); return owned;
}
static bool pointer_next(struct pointer_event *e) {
    u_long flags=irq_save();
    bool ready=framebuffer_graphics_owned() && pointer_read(e);
    irq_restore(flags); return ready;
}
static void pointer_speed_owned(int speed,bool release) {
    u_long flags=irq_save();
    if(framebuffer_graphics_owned()) {
        pointer_speed(speed);
        if(release) pointer_device(POINTER_KEYS,false);
    }
    irq_restore(flags);
}
static void pointer_keys_owned(int dx,int dy,unsigned buttons) {
    u_long flags=irq_save();
    if(framebuffer_graphics_owned()) pointer_report(POINTER_KEYS,dx,dy,0,buttons);
    irq_restore(flags);
}
static void input_key(struct gui_state *g,int key,unsigned *keys_down) {
    if(!g->menu && key>=GUI_UP && key<=GUI_RIGHT) {
        int dx=key==GUI_LEFT?-8:key==GUI_RIGHT?8:0;
        int dy=key==GUI_UP?-8:key==GUI_DOWN?8:0;
        pointer_keys_owned(dx,dy,*keys_down);
    } else if(!g->menu && g->focus==GUI_SKETCH && key==' ') {
        *keys_down ^= POINTER_LEFT;
        pointer_keys_owned(0,0,*keys_down);
    } else {
        if(key<32 && key!=9 && *keys_down) {
            *keys_down=0; pointer_keys_owned(0,0,0);
        }
        gui_key(g,key);
    }
}
int cmd_pointerinfo(int argc,char **argv) {
    struct pointer_event e;
    if(argc!=1) { printf("    usage: pointerinfo\n"); return CMD_USAGE; }
    pointer_snapshot(&e);
    printf("    PS/2: %s\n",mouse_ps2_describe());
    printf("    pointing devices: %u (USB, PS/2 and supported I2C HID)\n",pointer_devices());
    printf("    pointer: %d,%d buttons=%u; queue drops=%u\n",e.x,e.y,e.buttons,pointer_dropped());
    touchpad_print();
    return CMD_OK;
}
int cmd_gui(int argc,char **argv) {
    struct display d;
    struct fb_surface ram;
    struct fb_mode mode;
    struct pointer_event e;
    struct gui_state *g;
    struct tty *tty=curTask->tty;
    int oldraw,result=CMD_OK,esc=0;
    u32 esc_at=0;
    unsigned keys_down=0;
    if(argc!=1) { printf("    usage: gui\n"); return CMD_USAGE; }
    printf("    SurfOS desktop: Ctrl+Q returns to the shell; Ctrl+K calculator, Ctrl+D sketch\n");
    if(curTask->intr) return CMD_FAIL;
    if(!tty || framebuffer_graphics_begin()<0) {
        printf("    gui needs the active local UEFI framebuffer console\n"); return CMD_FAIL;
    }
    memset(&d,0,sizeof(d));
    if(framebuffer_graphics_draw(dimensions,&d)<0) { framebuffer_graphics_end(); return CMD_FAIL; }
    g=kcalloc(1,sizeof(*g)); d.gui=g;
    d.pixels=kalloc(d.width*d.height*sizeof(u32));
    if(!g || !d.pixels) {
        if(g) kfree(g);
        if(d.pixels) kfree(d.pixels);
        framebuffer_graphics_end(); printf("    gui: not enough memory\n"); return CMD_FAIL;
    }
    memset(&mode,0,sizeof(mode));
    mode.width=d.width; mode.height=d.height; mode.pitch=d.width*4;
    mode.bpp=32; mode.type=1; mode.addr=1;
    mode.red_pos=16; mode.green_pos=8; mode.blue_pos=0;
    mode.red_size=mode.green_size=mode.blue_size=8;
    fb_setup(&ram,(u8*)d.pixels,&mode);
    gui_init(g,d.width,d.height); g->devices=pointer_devices();
    if(!pointer_begin(&d)) {
        kfree(d.pixels); kfree(g); framebuffer_graphics_end(); return CMD_OK;
    }
    oldraw=tty->raw; tty_ctl(tty,TTY_RAW,1); d.first=true;
    while(!g->quit && !curTask->intr && framebuffer_graphics_owned()) {
        int n,key,oldx=g->x,oldy=g->y;
        unsigned devices;
        for(n=0;n<POINTER_QUEUE && pointer_next(&e);n++) gui_pointer(g,&e);
        for(n=0;n<32 && framebuffer_graphics_owned() && (key=tty_trygetc(tty))>=0;n++) {
            if(esc==1 && key=='[') { esc=2; continue; }
            if(esc==2) {
                esc=0;
                if(key>='A' && key<='D') {
                    static const int arrow[]={GUI_UP,GUI_DOWN,GUI_RIGHT,GUI_LEFT};
                    input_key(g,arrow[key-'A'],&keys_down);
                }
                continue;
            }
            if(esc) { gui_key(g,27); esc=0; }
            if(key==27) { esc=1; esc_at=getticks(); } else input_key(g,key,&keys_down);
        }
        if(esc && (u32)(getticks()-esc_at)>HZ/10) { gui_key(g,27); esc=0; }
        if(g->quit || !framebuffer_graphics_owned()) break;
        devices=pointer_devices();
        if(devices!=g->devices) { g->devices=devices; g->dirty=true; }
        pointer_speed_owned(g->speed,false);
        if(g->dirty) {
            gui_render(g,&ram); g->dirty=false;
            if(!present(&d,0,0,d.width,d.height)) break;
            if(framebuffer_graphics_draw(pointer_paint,&d)<0) break;
        } else if(oldx!=g->x || oldy!=g->y) {
            if(!present(&d,oldx,oldy,12,16)) break;
            if(framebuffer_graphics_draw(pointer_paint,&d)<0) break;
        }
        if(sleep_intr(16)) break;
    }
    tty_ctl(tty,TTY_RAW,oldraw); pointer_speed_owned(1,true);
    framebuffer_graphics_end();
    kfree(d.pixels); kfree(g);
    printf("    desktop closed; text console restored\n");
    return result;
}
