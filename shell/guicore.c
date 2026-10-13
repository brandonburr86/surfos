/*
SurfOS Desktop Model and Drawing
--------------------
File: guicore.c Date: 10/10/26
--------------------
A small immediate-mode desktop, rendered to ordinary RAM by its owning task.
All coordinates here are logical pixels. The shell handles input and presentation.
*/
#include <sys/gui.h>
#include <string.h>
#include <stdio.h>

#define NAVY 0x102E3B
#define INK 0x153D49
#define TEAL 0x197D83
#define FOAM 0xB8E9D9
#define PAPER 0xF4F1E6
#define MUTED 0x6C888C
#define CORAL 0xEE906D
static const u32 paints[] = { PAPER, INK, TEAL, CORAL, 0x659ECB, 0xD0B056 };
static const char *titles[] = { "Calculator", "Sketch", "About SurfOS", "Pointer & desktop" };
static const char keys[] = "789/456*123-C0=+";
static int clamp(int n, int a, int b) { return n < a ? a : n > b ? b : n; }
static bool inside(int x,int y,int l,int t,int w,int h) { return x>=l && y>=t && x<l+w && y<t+h; }

void gui_calc_key(struct gui_calc *c, int key) {
    i64 v;
    if(key=='c' || key=='C') { memset(c,0,sizeof(*c)); return; }
    if(c->error) return;
    if(key>='0' && key<='9') {
        v = (c->fresh ? 0 : (i64)c->value) * 10 + key-'0';
        if(v > 999999999 || v < -999999999) { c->error=true; return; }
        c->value=v; c->fresh=false; c->entered=true; return;
    }
    if(key==8 || key==127) { if(!c->fresh) c->value/=10; return; }
    if(key!='+' && key!='-' && key!='*' && key!='/' && key!='=' && key!='\r' && key!='\n') return;
    if(c->op && c->entered) {
        v=c->operand;
        switch(c->op) {
        case '+': v+=c->value; break;
        case '-': v-=c->value; break;
        case '*': v*=c->value; break;
        case '/': if(!c->value) { c->error=true; return; } v=c->operand/c->value; break;
        }
        if(v > 999999999 || v < -999999999) { c->error=true; return; }
        c->value=v;
    }
    c->operand=c->value; c->fresh=true; c->entered=false;
    c->op=(key=='=' || key=='\r' || key=='\n') ? 0 : key;
}

void gui_open(struct gui_state *g, int app) {
    int i,j;
    if(app<0 || app>=GUI_WINDOWS) return;
    for(i=0;i<GUI_WINDOWS;i++) if(g->order[i]==app) {
        for(j=i;j<GUI_WINDOWS-1;j++) g->order[j]=g->order[j+1];
        g->order[GUI_WINDOWS-1]=app; break;
    }
    g->win[app].open=true; g->focus=app; g->menu=0; g->dirty=true;
}
static void close_window(struct gui_state *g) {
    int i;
    if(g->focus>=0) g->win[g->focus].open=false;
    g->focus=-1;
    for(i=GUI_WINDOWS-1;i>=0;i--) if(g->win[g->order[i]].open) { g->focus=g->order[i]; break; }
    g->dirty=true;
}
void gui_init(struct gui_state *g, int width, int height) {
    int i;
    memset(g,0,sizeof(*g)); g->width=width; g->height=height;
    g->focus=g->drag=-1; g->brush=1; g->speed=1;
    g->x=width/2; g->y=height/2;
    for(i=0;i<GUI_WINDOWS;i++) g->order[i]=i;
    g->win[GUI_CALC]=(struct gui_window){width-272,50,248,300,false};
    g->win[GUI_SKETCH]=(struct gui_window){(width-432)/2,64,432,294,false};
    g->win[GUI_ABOUT]=(struct gui_window){(width-432)/2,(height-234)/2,432,234,false};
    g->win[GUI_SETTINGS]=(struct gui_window){(width-400)/2,72,400,244,false};
    gui_open(g,GUI_ABOUT);
}
static int menu_count(int menu) { return menu==2 ? 2 : 3; }
static int menu_x(int menu) { return menu==1 ? 8 : menu==2 ? 120 : 192; }
static void menu_action(struct gui_state *g, int item) {
    int m=g->menu;
    g->menu=0; g->dirty=true;
    if(m==1) {
        if(item==0) gui_open(g,GUI_ABOUT);
        if(item==1) close_window(g);
        if(item==2) g->quit=true;
    } else if(m==2) gui_open(g,item==0 ? GUI_CALC : GUI_SKETCH);
    else {
        if(item==0) gui_open(g,GUI_SETTINGS);
        if(item==1) g->night=!g->night;
        if(item==2) { memset(g->canvas,0,sizeof(g->canvas)); gui_open(g,GUI_SKETCH); }
    }
}
static void stroke(struct gui_state *g, int x, int y) {
    int dx=x-g->last_x, dy=y-g->last_y, n, i;
    int adx=dx<0?-dx:dx, ady=dy<0?-dy:dy;
    n=adx>ady?adx:ady;
    for(i=0;i<=n;i++) {
        int px=g->last_x+(n?dx*i/n:0), py=g->last_y+(n?dy*i/n:0);
        if(inside(px,py,0,0,GUI_CANVAS_W,GUI_CANVAS_H)) g->canvas[py*GUI_CANVAS_W+px]=g->brush;
    }
    g->last_x=x; g->last_y=y; g->dirty=true;
}
void gui_pointer(struct gui_state *g, const struct pointer_event *e) {
    bool press=(e->buttons&POINTER_LEFT) && !(g->buttons&POINTER_LEFT);
    int i,id,x,y;
    struct gui_window *w;
    g->x=clamp(e->x,0,g->width-1); g->y=clamp(e->y,0,g->height-1); g->buttons=e->buttons;
    if(!(e->buttons&POINTER_LEFT)) { g->drag=-1; g->drawing=false; }
    if(g->drag>=0) {
        w=&g->win[g->drag];
        w->x=clamp(g->x-g->drag_x,0,g->width-w->w);
        w->y=clamp(g->y-g->drag_y,34,g->height-w->h-28);
        g->dirty=true; return;
    }
    if(g->drawing && (e->buttons&POINTER_LEFT)) {
        w=&g->win[GUI_SKETCH]; x=g->x-w->x-16; y=g->y-w->y-70;
        if(inside(x,y,0,0,GUI_CANVAS_W,GUI_CANVAS_H)) {
            if(g->last_x<0) { g->last_x=x; g->last_y=y; }
            stroke(g,x,y);
        } else g->last_x=-1;
        return;
    }
    if(!press) return;
    if(g->y<28) {
        id=g->x<120?1:g->x<192?2:g->x<296?3:0;
        g->menu=g->menu==id?0:id; g->selection=0; g->dirty=true; return;
    }
    if(g->menu) {
        x=menu_x(g->menu);
        if(inside(g->x,g->y,x,30,216,28*menu_count(g->menu))) menu_action(g,(g->y-30)/28);
        else { g->menu=0; g->dirty=true; }
        return;
    }
    for(i=GUI_WINDOWS-1;i>=0;i--) {
        id=g->order[i]; w=&g->win[id];
        if(!w->open || !inside(g->x,g->y,w->x,w->y,w->w,w->h)) continue;
        gui_open(g,id); x=g->x-w->x; y=g->y-w->y;
        if(y<28) {
            if(x>=w->w-28) close_window(g);
            else { g->drag=id; g->drag_x=x; g->drag_y=y; }
            return;
        }
        if(id==GUI_CALC && inside(x,y,16,104,216,160)) {
            int col=(x-16)/54,row=(y-104)/40;
            if((x-16)%54<48 && (y-104)%40<34) gui_calc_key(&g->calc,keys[row*4+col]);
        }
        if(id==GUI_SKETCH) {
            if(inside(x,y,16,38,6*32,24)) g->brush=(x-16)/32;
            else if(inside(x,y,336,38,80,24)) memset(g->canvas,0,sizeof(g->canvas));
            else if(inside(x,y,16,70,GUI_CANVAS_W,GUI_CANVAS_H)) {
                g->drawing=true; g->last_x=x-16; g->last_y=y-70; stroke(g,x-16,y-70);
            }
        }
        if(id==GUI_ABOUT && inside(x,y,16,166,400,34)) gui_open(g,x<216 ? GUI_CALC : GUI_SKETCH);
        if(id==GUI_SETTINGS) {
            if(inside(x,y,16,100,3*112,30)) g->speed=(x-16)/112+1;
            if(inside(x,y,16,172,176,32)) g->night=!g->night;
        }
        return;
    }
    if(inside(g->x,g->y,24,120,144,44)) gui_open(g,GUI_CALC);
    if(inside(g->x,g->y,24,180,144,44)) gui_open(g,GUI_SKETCH);
}
void gui_key(struct gui_state *g, int key) {
    int i;
    if(key==3 || key==17) { g->quit=true; return; }
    if(key==11) { gui_open(g,GUI_CALC); return; }
    if(key==4) { gui_open(g,GUI_SKETCH); return; }
    if(key==16) { gui_open(g,GUI_SETTINGS); return; }
    if(key==9) {
        for(i=0;i<GUI_WINDOWS;i++) if(g->win[g->order[i]].open) { gui_open(g,g->order[i]); return; }
    }
    if(key==0 || key==7) { g->menu=g->menu?0:1; g->selection=0; g->dirty=true; return; } /* Ctrl+G (also terminal Ctrl+Space) */
    if(g->menu) {
        if(key==27) g->menu=0;
        else if(key==GUI_UP) g->selection=(g->selection+menu_count(g->menu)-1)%menu_count(g->menu);
        else if(key==GUI_DOWN) g->selection=(g->selection+1)%menu_count(g->menu);
        else if(key==GUI_LEFT || key==GUI_RIGHT) { g->menu=(g->menu+(key==GUI_RIGHT?0:1))%3+1; g->selection=0; }
        else if(key=='\r' || key=='\n') menu_action(g,g->selection);
        g->dirty=true; return;
    }
    if(key==27) { if(g->focus<0) g->quit=true; else close_window(g); return; }
    if(g->focus==GUI_CALC) { gui_calc_key(&g->calc,key); g->dirty=true; }
    if(g->focus==GUI_SKETCH && key==14) { memset(g->canvas,0,sizeof(g->canvas)); g->dirty=true; }
    if(g->focus==GUI_SETTINGS) {
        if(key>='1' && key<='3') g->speed=key-'0';
        if(key=='t' || key=='T') g->night=!g->night;
        g->dirty=true;
    }
}

static void rect(const struct fb_surface *s,int x,int y,int w,int h,u32 c) {
    if(w>0 && h>0) fb_rect_rgb(s,x,y,w,h,c);
}
static void text(const struct fb_surface *s,int x,int y,const char *str,u32 c,int scale) {
    int row,col;
    while(*str) {
        const u8 *glyph=font8x16[(u8)*str++];
        for(row=0;row<16;row++) for(col=0;col<8;col++)
            if(glyph[row]&(0x80>>col)) rect(s,x+col*scale,y+row*scale,scale,scale,c);
        x+=8*scale;
    }
}
static void button(const struct fb_surface *s,int x,int y,int w,int h,const char *label,u32 bg,u32 fg) {
    rect(s,x,y,w,h,bg); text(s,x+(w-(int)strlen(label)*8)/2,y+(h-16)/2,label,fg,1);
}
static void wave(const struct fb_surface *s,int x,int y,int w,int h,u32 c) {
    int i;
    for(i=0;i<w;i++) {
        int t=i%80, py=(t<20?t:t<60?40-t:t-80)*h/40;
        rect(s,x+i,y+py,1,3,c);
    }
}
static void window(const struct gui_state *g,const struct fb_surface *s,int id) {
    const struct gui_window *w=&g->win[id];
    int x=w->x,y=w->y,i,j;
    char b[48];
    rect(s,x+5,y+6,w->w,w->h,0x0B2530);
    rect(s,x,y,w->w,w->h,INK); rect(s,x+1,y+1,w->w-2,w->h-2,PAPER);
    rect(s,x+1,y+1,w->w-2,27,g->focus==id?TEAL:MUTED);
    text(s,x+12,y+6,titles[id],PAPER,1);
    button(s,x+w->w-26,y+4,20,20,"x",g->focus==id?INK:MUTED,PAPER);
    if(id==GUI_CALC) {
        rect(s,x+16,y+44,216,44,INK);
        snprintf(b,sizeof(b),"%ld",(long)g->calc.value);
        text(s,x+28,y+58,g->calc.error?"Error - press C":b,FOAM,1);
        if(g->calc.op && !g->calc.error) { b[0]=g->calc.op; b[1]=0; text(s,x+212,y+58,b,CORAL,1); }
        for(i=0;i<16;i++) {
            b[0]=keys[i]; b[1]=0;
            button(s,x+16+(i%4)*54,y+104+(i/4)*40,48,34,b,i%4==3?TEAL:i==14?CORAL:0xDEE3D8,INK);
        }
        text(s,x+16,y+278,"Integers | C clears",MUTED,1);
    } else if(id==GUI_SKETCH) {
        for(i=0;i<6;i++) {
            rect(s,x+16+i*32,y+38,26,24,i==g->brush?INK:MUTED);
            rect(s,x+19+i*32,y+41,20,18,paints[i]);
        }
        button(s,x+336,y+38,80,24,"Clear",0xDEE3D8,INK);
        rect(s,x+15,y+69,402,182,MUTED);
        for(j=0;j<GUI_CANVAS_H;j++) {
            for(i=0;i<GUI_CANVAS_W;) {
                int start=i, c=g->canvas[j*GUI_CANVAS_W+i];
                while(i<GUI_CANVAS_W && g->canvas[j*GUI_CANVAS_W+i]==c) i++;
                rect(s,x+16+start,y+70+j,i-start,1,paints[c%6]);
            }
        }
        text(s,x+16,y+266,"Temporary. Drag/Space draws. Ctrl+N clears.",INK,1);
    } else if(id==GUI_ABOUT) {
        text(s,x+16,y+44,"SurfOS",INK,3);
        text(s,x+16,y+104,"A small desktop for catching ideas.",INK,1);
        text(s,x+16,y+128,"Calculate, sketch, and return to the shell.",MUTED,1);
        button(s,x+16,y+166,192,34,"Calculator",TEAL,PAPER);
        button(s,x+224,y+166,192,34,"Sketch",TEAL,PAPER);
        text(s,x+16,y+212,"Brandon Burr / 2004 - 2026",MUTED,1);
    } else {
        snprintf(b,sizeof(b),"%u pointing device%s connected",g->devices,g->devices==1?"":"s");
        text(s,x+16,y+44,b,INK,1);
        text(s,x+16,y+76,"Pointer speed (keys 1, 2, 3)",INK,1);
        for(i=0;i<3;i++) button(s,x+16+i*112,y+100,104,30,i==0?"1 / Slow":i==1?"2 / Medium":"3 / Fast",g->speed==i+1?TEAL:0xDEE3D8,g->speed==i+1?PAPER:INK);
        text(s,x+16,y+148,"Desktop colors (T)",INK,1);
        button(s,x+16,y+172,176,32,g->night?"Dusk / change":"Ocean / change",TEAL,PAPER);
        text(s,x+16,y+220,"Changes last for this desktop session.",MUTED,1);
    }
}
void gui_render(const struct gui_state *g,const struct fb_surface *s) {
    int i,x,y;
    u32 bg=g->night?0x142638:0x245A67;
    static const char *menus[3][3]={{"About SurfOS","Close window","Return to shell"},{"Calculator  Ctrl+K","Sketch      Ctrl+D",NULL},{"Pointer & desktop","Ocean / dusk colors","Clear sketch"}};
    rect(s,0,0,g->width,g->height,bg);
    /* An oversized horizon and the recurring SurfOS wave mark. */
    for(i=0;i<5;i++) wave(s,0,g->height-70+i*8,g->width,14,g->night?0x244454:0x3B7980);
    text(s,24,54,"SurfOS",FOAM,3);
    button(s,24,120,144,44,"Calculator",INK,FOAM);
    button(s,24,180,144,44,"Sketch",INK,FOAM);
    rect(s,0,0,g->width,28,PAPER);
    text(s,16,6,"~ SurfOS",INK,1); text(s,128,6,"Apps",INK,1); text(s,200,6,"Options",INK,1);
    text(s,g->width-128,6,"Ride the wave.",TEAL,1);
    rect(s,0,g->height-26,g->width,26,NAVY);
    text(s,12,g->height-20,"Ctrl+K Calc  Ctrl+D Sketch  Ctrl+P Options  Ctrl+Q Shell",FOAM,1);
    for(i=0;i<GUI_WINDOWS;i++) if(g->win[g->order[i]].open) window(g,s,g->order[i]);
    if(g->menu) {
        x=menu_x(g->menu); y=30;
        rect(s,x+4,y+4,216,28*menu_count(g->menu),NAVY);
        rect(s,x,y,216,28*menu_count(g->menu),PAPER);
        for(i=0;i<menu_count(g->menu);i++) {
            if(i==g->selection) rect(s,x+2,y+i*28+2,212,24,TEAL);
            text(s,x+12,y+i*28+6,menus[g->menu-1][i],i==g->selection?PAPER:INK,1);
        }
    }
}
void gui_cursor(const struct fb_surface *s,int x,int y,int scale) {
    int row,col;
    static const char *shape[]={"X           ","XX          ","X.X         ","X..X        ","X...X       ","X....X      ","X.....X     ","X......X    ","X.......X   ","X........X  ","X.....XXXXX ","X..X..X     ","X.X X..X    ","XX  X..X    ","X    XX     ",NULL};
    for(row=0;shape[row];row++) for(col=0;shape[row][col];col++)
        if(shape[row][col]!=' ') rect(s,x+col*scale,y+row*scale,scale,scale,shape[row][col]=='X'?NAVY:PAPER);
}
