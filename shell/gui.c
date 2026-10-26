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
10/25/26 (programs' windows, docs/superpowers/plans/2026-10-25-app-windows.md): the desktop sleeps until
something happens (poll_sleep(): a key, the pointer, a terminal's output) or 100 ms pass, a frame's time
while something moves or works; gfxinfo shows each desktop's frame times (composing and presenting,
measured with the TSC); and a 32-bit framebuffer gets its rows expanded once and copied in order.
10/25/26 (stage C4): the desktop registers itself for programs' windows (kernel/win.c) once it runs,
with its logical size, the largest content a window may have and its scale, and lets the
registration go after gui_finish(); the registration is a descriptor of this task, so a desktop
that dies lets them go when it is reaped.
10/25/26 (the review of C1-C3): the pace. A pass with work (a frame drawn, a button held, an
escape's end awaited, keys left past a pass's 32, an application at work that draws nothing every
pass: gui_working()) is followed by the next a frame after it began (FRAME_MS), slept out whatever
wakes meanwhile. An idle pass naps until something wakes it or 100 ms pass, but a pass woken
within a frame of the one before waits out a frame first: wakes are the whole system's (a pipe, a
TCP segment), so a flood of them gets two passes a frame, not one each. While another console has
the screen the desktop looks for its own every frame (a switch wakes nobody).
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
#include <surfos/cpu.h>
#include <surfos/klog.h>
#include <surfos/win.h>
#include <fs/vfs.h>
#include <sys/gui.h>
#include <sys/touchpad.h>
#include "shell.h"

struct display {
    struct gui_state *gui;
    u32 *pixels, *shown;        /* the frame, and what the screen shows of it (NULL: present it all) */
    u32 *row;                   /* 10/25/26: a row of the screen, for the 32-bit path (NULL: no such path) */
    int width,height,scale,ox,oy,screen_w;
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
    d->screen_w=s->mode.width;
}
/* 10/25/26: a 32-bit framebuffer with 8-bit channels: the frame's rows go to it as they are (expanded and
   converted once into d->row, then copied to each of its scale rows in order: the framebuffer is
   write-combining), instead of a rectangle for each run of one colour */
static bool direct(const struct fb_surface *s,const struct display *d) {
    return d->row && s->mode.bpp==32 && s->bytespp==4 && s->mode.red_size==8 && s->mode.green_size==8 &&
           s->mode.blue_size==8;
}
static inline void copy32(u32 *to,const u32 *from,u32 n) {
    asm volatile("cld; rep movsl" : "+D"(to), "+S"(from), "+c"(n) : : "memory");
}
static void present_direct(const struct fb_surface *s,const struct display *d) {
    const struct fb_mode *m=&s->mode;
    bool same=m->red_pos==16 && m->green_pos==8 && m->blue_pos==0;     /* 0x00RRGGBB as it is */
    u32 n=(u32)((d->right-d->left)*d->scale), c, p;
    int x,y,k;
    for(y=d->top;y<d->bottom;y++) {
        const u32 *from=d->pixels+y*d->width;
        u32 *out=d->row;
        for(x=d->left;x<d->right;x++) {
            c=from[x];
            p=same ? (c&0xFFFFFF) : ((c>>16&0xFF)<<m->red_pos)|((c>>8&0xFF)<<m->green_pos)|((c&0xFF)<<m->blue_pos);
            for(k=0;k<d->scale;k++) *out++=p;
        }
        for(k=0;k<d->scale;k++)
            copy32((u32*)(s->base+(u32)(d->oy+y*d->scale+k)*m->pitch)+d->ox+d->left*d->scale,d->row,n);
    }
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
    if(direct(s,d)) { present_direct(s,d); return; }
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
/* 10/25/26: the frame times gfxinfo shows, a record for each console's desktop (written by that desktop alone,
   read by gfxinfo with interrupts off). The TSC's cycles are turned into microseconds at a rate measured against
   the timer over the desktop's first 100 ms, in 32 bits (the kernel has no 64-bit division); without a TSC, or
   before that, no times. Composing is gui_render() into RAM; presenting, the changed rows and the pointer. A
   record whose desktop's task is gone (killed at once) is not shown. */
struct frame_times { bool on, timed; u_long pid; int width,height,scale; u32 frames,compose_us,present_us,worst_compose_us,worst_present_us; };
static struct frame_times frame_record[NUM_CONSOLES];
struct frame_clock { u64 start; u_long ticks; u32 per_us; bool started; };
static void frame_calibrate(struct frame_clock *c) {
    u_long t=getticks();
    u64 cycles;
    if(c->per_us || !cpu.tsc) return;
    if(!c->started) { c->start=rdtsc(); c->ticks=t; c->started=true; return; }
    if(t-c->ticks<HZ/10) return;
    cycles=rdtsc()-c->start;
    if(cycles>>32) { c->started=false; return; }        /* over 4e9 cycles in a few ticks: start again */
    c->per_us=(u32)cycles/((u32)(t-c->ticks)*(1000/HZ))/1000;
    if(!c->per_us) c->per_us=1;
}
static u64 frame_now(void) { return cpu.tsc ? rdtsc() : 0; }
static u32 frame_us(const struct frame_clock *c,u64 from,u64 to) {
    u64 cycles=to-from;
    if(!c->per_us) return 0;
    return cycles>>32 ? 0xFFFFFFFF : (u32)cycles/c->per_us;
}
static struct frame_times *frame_mine(void) {
    int i=(int)(curTask->con-conArray);
    return i>=0 && i<NUM_CONSOLES ? &frame_record[i] : NULL;
}
static void frame_note(const struct frame_clock *c,u64 t0,u64 t1,u64 t2) {
    struct frame_times *f=frame_mine();
    u32 compose=frame_us(c,t0,t1), present=frame_us(c,t1,t2);
    u_long flags;
    if(!f) return;
    flags=irq_save();
    f->frames++;
    if(!c->per_us) { irq_restore(flags); return; }
    f->timed=true;
    f->compose_us=compose; f->present_us=present;
    if(compose>f->worst_compose_us) f->worst_compose_us=compose;
    if(present>f->worst_present_us) f->worst_present_us=present;
    irq_restore(flags);
}
static void frame_begin(const struct display *d,bool on) {
    struct frame_times *f=frame_mine();
    u_long flags;
    if(!f) return;
    flags=irq_save();
    memset(f,0,sizeof(*f));
    f->on=on; f->pid=curTask->pid; f->width=d->width; f->height=d->height; f->scale=d->scale;
    irq_restore(flags);
}
static void ms_text(u32 us,char *out,size_t n) { snprintf(out,n,"%u.%u ms",us/1000,us%1000/100); }
/* shell.h: gfxinfo's lines about the desktops that run, their size and frame times */
void gui_frame_info(void) {
    struct frame_times f;
    surf_task *t;
    char a[16],b[16],c[16],e[16];
    u_long flags;
    bool alive;
    int i;
    for(i=0;i<NUM_CONSOLES;i++) {
        flags=irq_save();
        f=frame_record[i];
        alive=f.on && (t=task_find(f.pid)) && t->state!=TS_DEAD;
        irq_restore(flags);
        if(!alive) continue;
        printf("    desktop on F%d: %dx%d at %dx, %u frames drawn\n",i+1,f.width,f.height,f.scale,f.frames);
        if(!f.timed) continue;
        ms_text(f.compose_us,a,sizeof(a)); ms_text(f.present_us,b,sizeof(b));
        ms_text(f.worst_compose_us,c,sizeof(c)); ms_text(f.worst_present_us,e,sizeof(e));
        printf("      the last: composing %s, presenting %s; the slowest: %s, %s\n",a,b,c,e);
    }
}
/* 10/25/26 (the review): a pass with work, followed by the next a frame after it began: a frame drawn (a picture
   being read, a terminal's flood), a button held (a drag that scrolls), an escape sequence's end to wait for, keys
   left past a pass's 32, or an application at work that draws nothing every pass (gui_working()) */
#define FRAME_MS 16
static bool busy(const struct gui_state *g,bool drew,int esc,unsigned keys_down,bool keys_left) {
    return drew || esc || keys_down || keys_left || g->buttons || gui_working(g);
}
static u32 ms_now(void) { return (u32)getticks()*(1000/HZ); }
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
    struct gui_fs *own_fs;                              /* 10/18/26: this desktop's, keeping read_at's file in a frame */
    struct tty *tty=curTask->tty;
    struct frame_clock fc;                              /* 10/25/26: the TSC's rate, for the frame times */
    struct win_desk desk;                               /* 10/25/26: what programs' windows may be here */
    int oldraw,result=CMD_OK,esc=0,number=0,reg;
    u32 esc_at=0,clock_at,pass_ms=0,last_ms;
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
    d.row=kalloc(d.screen_w*sizeof(u32));           /* 10/25/26: without it rectangles, as before */
    memset(&mode,0,sizeof(mode));
    mode.width=d.width; mode.height=d.height; mode.pitch=d.width*4;
    mode.bpp=32; mode.type=1; mode.addr=1;
    mode.red_pos=16; mode.green_pos=8; mode.blue_pos=0;
    mode.red_size=mode.green_size=mode.blue_size=8;
    fb_setup(&ram,(u8*)d.pixels,&mode);
    own_fs=gui_kernel_fs_begin();
    gui_init(g,d.width,d.height); g->devices=pointer_devices(); g->sys=gui_kernel_sys();
    g->fs=own_fs ? own_fs : gui_kernel_fs();
    clock_tick(g); clock_at=getticks();
    if(!pointer_begin(&d)) {
        gui_kernel_fs_end(own_fs);
        if(d.shown) kfree(d.shown);
        if(d.row) kfree(d.row);
        kfree(d.pixels); kfree(g); framebuffer_graphics_end(); return CMD_OK;
    }
    oldraw=tty->raw; tty_ctl(tty,TTY_RAW,1); d.first=d.full=true;
    memset(&fc,0,sizeof(fc)); frame_begin(&d,true);
    desk.width=d.width; desk.height=d.height; desk.scale=d.scale;      /* 10/25/26: programs' windows */
    gui_client_room(g,&desk.max_w,&desk.max_h);
    reg=win_register(&desk);
    if(reg<0) klog_write("gui: programs cannot open windows on this desktop\n");
    while(!g->quit && !curTask->intr) {
        int n,key,oldx=g->x,oldy=g->y,drawn=0;
        unsigned devices;
        u_long gen=poll_generation();                   /* 10/25/26: before looking: whatever comes later ends the nap */
        bool keys_left;
        u64 t0,t1;
        last_ms=pass_ms; pass_ms=ms_now();              /* 10/25/26 (the review): this pass's start, and the last's */
        frame_calibrate(&fc);
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
            gui_kernel_fs_idle(g->fs);                  /* 10/18/26: no file open while the desktop sleeps */
            if(sleep_intr(FRAME_MS)) break;             /* 10/25/26 (the review): a switch wakes nobody */
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
        keys_left=n==32;                                /* 10/25/26: the rest in the next pass, a frame on */
        if(esc && (u32)(getticks()-esc_at)>HZ/10) { if(esc==1) gui_key(g,27); esc=0; }
        gui_kernel_fs_idle(g->fs);                      /* 10/18/26: the frame's reads are done (the tick, the input) */
        if(g->quit) break;
        if(!framebuffer_graphics_owned()) continue;
        devices=pointer_devices();
        if(devices!=g->devices) { g->devices=devices; g->dirty=true; }
        pointer_speed_owned(g->speed,false);
        if(g->dirty) {
            t0=frame_now();
            gui_render(g,&ram); g->dirty=false;
            t1=frame_now();
            if((drawn=present_changes(&d))<0) continue;
            if((drawn || oldx!=g->x || oldy!=g->y) &&
               (!present(&d,oldx,oldy,12,16) || framebuffer_graphics_draw(pointer_paint,&d)<0)) continue;
            if(drawn) frame_note(&fc,t0,t1,frame_now());
        } else if(oldx!=g->x || oldy!=g->y) {
            if(!present(&d,oldx,oldy,12,16)) continue;
            if(framebuffer_graphics_draw(pointer_paint,&d)<0) continue;
        }
        if(busy(g,drawn>0,esc,keys_down,keys_left)) {   /* 10/25/26 (the review): the next pass a frame after this one */
            n=(int)(ms_now()-pass_ms);
            if(n<FRAME_MS && sleep_intr(FRAME_MS-n)) break;
        } else if(pass_ms-last_ms<FRAME_MS) {           /* woken within a frame of the last pass: a frame first */
            if(sleep_intr(FRAME_MS)) break;
        } else if(poll_sleep(gen,100)) break;
    }
    gui_finish(g);                                      /* 10/17/26: a Terminal's session hangs up */
    if(reg>=0) win_unregister(reg);                     /* 10/25/26: every program's window let go */
    gui_kernel_fs_end(own_fs);                          /* 10/18/26: read_at's file closed, if one is open */
    tty_ctl(tty,TTY_RAW,oldraw); pointer_speed_owned(1,true);
    framebuffer_graphics_end();
    frame_begin(&d,false);
    if(d.shown) kfree(d.shown);
    if(d.row) kfree(d.row);
    kfree(d.pixels); kfree(g);
    printf("    desktop closed; text console restored\n");
    return result;
}
