/*
SurfOS Desktop Command
--------------------
File: gui.c Date: 10/10/26
--------------------
Own the local graphics session, poll input, and present RAM-rendered rows in
small batches. No allocation, I/O or waiting inside a framebuffer callback.
10/17/26: Notepad and Files reach the file systems through shell/guifs.c; the
key parser knows Home, End, Page Up and Down, Insert and Delete; the model gets
the clock and the time (double-clicks); only the rows that changed since the
last frame are presented (a copy of what the screen shows); and switching
consoles no longer ends the desktop: it waits for its own console to come back
and draws itself again, so Notepad's unsaved text is not lost.
10/17/26 (the suite): the applications reach the kernel through shell/guisys.c
(struct gui_sys); the model gets the whole date and time each second, gui_tick()
every frame (also while another console has the screen, so a Terminal's session
keeps being read) and gui_finish() before the state is freed.
*/
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <mm/kalloc.h>
#include <surfos/task.h>
#include <surfos/timer.h>
#include <surfos/tty.h>
#include <surfos/irq.h>
#include <surfos/console.h>
#include <sys/gui.h>
#include <sys/touchpad.h>
#include "shell.h"

struct display {
    struct gui_state *gui;
    u32 *pixels, *shown;        /* the frame, and what the screen shows of it (NULL: present it all) */
    int width,height,scale,ox,oy;
    int left,top,right,bottom;
    bool first, full;
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
/* 10/17/26: what changed since the last frame, band by band of 16 rows (all of it when full):
   1 when something was drawn, 0 when nothing changed, -1 when the screen was lost */
static int present_changes(struct display *d) {
    int top,bottom,y,x,l,r,w=d->width,drawn=0;
    for(top=0;top<d->height;top=bottom) {
        bottom=top+16<d->height?top+16:d->height;
        l=w; r=0;
        if(d->full || !d->shown) { l=0; r=w; }
        else for(y=top;y<bottom;y++) {
            const u32 *now=d->pixels+y*w, *was=d->shown+y*w;
            for(x=0;x<w && now[x]==was[x];x++) ;
            if(x==w) continue;
            if(x<l) l=x;
            for(x=w;x>0 && now[x-1]==was[x-1];x--) ;
            if(x>r) r=x;
        }
        if(l>=r) continue;
        d->left=l; d->right=r; d->top=top; d->bottom=bottom;
        if(framebuffer_graphics_draw(present_rows,d)<0) return -1;
        if(d->shown) for(y=top;y<bottom;y++) memcpy(d->shown+y*w+l,d->pixels+y*w+l,(r-l)*4);
        drawn=1;
    }
    d->full=false;
    return drawn;
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
    if(key>=GUI_UP && key<=GUI_RIGHT && !gui_wants_arrows(g)) {
        int dx=key==GUI_LEFT?-8:key==GUI_RIGHT?8:0;
        int dy=key==GUI_UP?-8:key==GUI_DOWN?8:0;
        pointer_keys_owned(dx,dy,*keys_down);
    } else if(!g->menu && !g->dialog.kind && g->focus==GUI_SKETCH && key==' ') {
        *keys_down ^= POINTER_LEFT;
        pointer_keys_owned(0,0,*keys_down);
    } else {
        if(key<32 && key!=9 && *keys_down) {
            *keys_down=0; pointer_keys_owned(0,0,0);
        }
        gui_key(g,key);
    }
}
/* 10/17/26: the end of a terminal key sequence (ESC [ or ESC O, a number, the final byte) as a key */
static int sequence(int number,int final) {
    switch(final) {
    case 'A': return GUI_UP;
    case 'B': return GUI_DOWN;
    case 'C': return GUI_RIGHT;
    case 'D': return GUI_LEFT;
    case 'H': return GUI_HOME;
    case 'F': return GUI_END;
    case '~':
        switch(number) {
        case 1: case 7: return GUI_HOME;
        case 2: return GUI_INSERT;
        case 3: return GUI_DELETE;
        case 4: case 8: return GUI_END;
        case 5: return GUI_PGUP;
        case 6: return GUI_PGDN;
        }
    }
    return 0;
}
/* 10/17/26: the taskbar's clock, from the CMOS clock; and the whole date and time, for the suite's Clock */
static void clock_tick(struct gui_state *g) {
    time_t t;
    char now[8];
    get_time(&t);
    snprintf(now,sizeof(now),"%02u:%02u",t.hour%24,t.minute%60);
    if(strcmp(now,g->clock)) { strlcpy(g->clock,now,sizeof(g->clock)); g->dirty=true; }
    if(g->time.second!=t.second || g->time.minute!=t.minute || g->time.date!=t.date) g->dirty=true;
    g->time.year=t.year; g->time.month=t.month; g->time.date=t.date; g->time.weekday=t.day;
    g->time.hour=t.hour; g->time.minute=t.minute; g->time.second=t.second;
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
    int oldraw,result=CMD_OK,esc=0,number=0;
    u32 esc_at=0,clock_at;
    unsigned keys_down=0;
    if(argc!=1) { printf("    usage: gui\n"); return CMD_USAGE; }
    printf("    SurfOS desktop: Ctrl+Q returns to the shell; Ctrl+E Notepad, Ctrl+B Files\n");
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
    d.shown=kalloc(d.width*d.height*sizeof(u32));   /* without it every frame is presented whole */
    memset(&mode,0,sizeof(mode));
    mode.width=d.width; mode.height=d.height; mode.pitch=d.width*4;
    mode.bpp=32; mode.type=1; mode.addr=1;
    mode.red_pos=16; mode.green_pos=8; mode.blue_pos=0;
    mode.red_size=mode.green_size=mode.blue_size=8;
    fb_setup(&ram,(u8*)d.pixels,&mode);
    gui_init(g,d.width,d.height); g->devices=pointer_devices(); g->fs=gui_kernel_fs(); g->sys=gui_kernel_sys();
    clock_tick(g); clock_at=getticks();
    if(!pointer_begin(&d)) {
        if(d.shown) kfree(d.shown);
        kfree(d.pixels); kfree(g); framebuffer_graphics_end(); return CMD_OK;
    }
    oldraw=tty->raw; tty_ctl(tty,TTY_RAW,1); d.first=d.full=true;
    while(!g->quit && !curTask->intr) {
        int n,key,oldx=g->x,oldy=g->y,drawn;
        unsigned devices;
        g->now=(u32)getticks()*(1000/HZ);
        if((u32)(getticks()-clock_at)>=HZ) { clock_tick(g); clock_at=getticks(); }
        gui_tick(g);                                    /* 10/17/26: the suite reads what is new, screen or not */
        if(!framebuffer_graphics_owned()) {
            /* 10/17/26: another console has the screen: wait for ours, then draw it all again */
            if(curTask->con==conActive && framebuffer_graphics_begin()==0 && pointer_begin(&d)) {
                g->x=d.width/2; g->y=d.height/2; g->buttons=0;  /* 10/18/26: no button held, so the next */
                gui_release(g);                                 /* press is one; drags end as at a release */
                d.first=d.full=true; g->dirty=true; esc=0; keys_down=0;
                continue;
            }
            if(sleep_intr(16)) break;
            continue;
        }
        for(n=0;n<POINTER_QUEUE && pointer_next(&e);n++) gui_pointer(g,&e);
        for(n=0;n<32 && framebuffer_graphics_owned() && (key=tty_trygetc(tty))>=0;n++) {
            if(esc==1 && (key=='[' || key=='O')) { esc=2; number=0; continue; }
            if(esc>=2) {
                if(key>='0' && key<='9') { if(esc==2 && number<1000) number=number*10+key-'0'; continue; }
                if(key==';') { esc=3; continue; }            /* a modifier: the key is the same */
                esc=0;
                if((key=sequence(number,key))) input_key(g,key,&keys_down);
                continue;
            }
            if(esc) { gui_key(g,27); esc=0; }
            if(key==27) { esc=1; esc_at=getticks(); } else input_key(g,key,&keys_down);
        }
        if(esc && (u32)(getticks()-esc_at)>HZ/10) { if(esc==1) gui_key(g,27); esc=0; }
        if(g->quit) break;
        if(!framebuffer_graphics_owned()) continue;
        devices=pointer_devices();
        if(devices!=g->devices) { g->devices=devices; g->dirty=true; }
        pointer_speed_owned(g->speed,false);
        if(g->dirty) {
            gui_render(g,&ram); g->dirty=false;
            if((drawn=present_changes(&d))<0) continue;
            if((drawn || oldx!=g->x || oldy!=g->y) &&
               (!present(&d,oldx,oldy,12,16) || framebuffer_graphics_draw(pointer_paint,&d)<0)) continue;
        } else if(oldx!=g->x || oldy!=g->y) {
            if(!present(&d,oldx,oldy,12,16)) continue;
            if(framebuffer_graphics_draw(pointer_paint,&d)<0) continue;
        }
        if(sleep_intr(16)) break;
    }
    gui_finish(g);                                      /* 10/17/26: a Terminal's session hangs up */
    tty_ctl(tty,TTY_RAW,oldraw); pointer_speed_owned(1,true);
    framebuffer_graphics_end();
    if(d.shown) kfree(d.shown);
    kfree(d.pixels); kfree(g);
    printf("    desktop closed; text console restored\n");
    return result;
}
