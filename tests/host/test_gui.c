#include <sys/gui.h>
#include <stdio.h>
#include <string.h>
static int checks, failed;
#define CHECK(x) do { checks++; if(!(x)) { failed++; printf("FAIL line %d: %s\n", __LINE__, #x); } } while(0)
static struct gui_state g;
static u32 pixels[8 + 1280*800 + 8];
static void rendering(int width, int height) {
    struct fb_surface screen;
    struct fb_mode mode;
    int i;
    memset(&mode,0,sizeof(mode));
    mode.addr=1; mode.width=width; mode.height=height; mode.pitch=width*4;
    mode.bpp=32; mode.type=1; mode.red_pos=16; mode.green_pos=8;
    mode.red_size=mode.green_size=mode.blue_size=8;
    for(i=0;i<(int)(sizeof(pixels)/sizeof(pixels[0]));i++) pixels[i]=0xa55a1234;
    fb_setup(&screen,(u8*)(pixels+8),&mode);
    gui_init(&g,width,height);
    gui_open(&g,GUI_SKETCH); gui_open(&g,GUI_CALC); gui_open(&g,GUI_SETTINGS);
    gui_render(&g,&screen);
    CHECK(pixels[8]==0xF4F1E6); /* actual menubar color in rendered RGB buffer */
    gui_cursor(&screen,width-1,height-1,2); /* clipped at bottom/right edge */
    for(i=0;i<8;i++) CHECK(pixels[i]==0xa55a1234);
    for(i=8+width*height;i<(int)(sizeof(pixels)/sizeof(pixels[0]));i++)
        if(pixels[i]!=0xa55a1234) break;
    CHECK(i==(int)(sizeof(pixels)/sizeof(pixels[0])));
}
static void click(int x,int y) {
    struct pointer_event e = {x,y,0,1};
    gui_pointer(&g,&e); e.buttons=0; gui_pointer(&g,&e);
}
int main(void) {
    const char *p;
    struct pointer_event e;
    gui_init(&g,640,400);
    CHECK(g.focus == GUI_ABOUT && g.win[GUI_ABOUT].open);
    gui_key(&g,11); CHECK(g.win[GUI_CALC].open && g.focus == GUI_CALC);
    for(p="12+30="; *p; p++) gui_key(&g,*p);
    CHECK(g.calc.value==42 && !g.calc.error);
    for(p="c7/0="; *p; p++) gui_key(&g,*p);
    CHECK(g.calc.error);
    for(p="c999999999*9="; *p; p++) gui_key(&g,*p);
    CHECK(g.calc.error);
    for(p="c8-12="; *p; p++) gui_key(&g,*p);
    CHECK(g.calc.value == -4);
    gui_key(&g,4); CHECK(g.focus == GUI_SKETCH);
    click(g.win[GUI_SKETCH].x+22,g.win[GUI_SKETCH].y+74);
    CHECK(g.canvas[4*GUI_CANVAS_W+6] == g.brush);
    gui_key(&g,14); CHECK(g.canvas[4*GUI_CANVAS_W+6] == 0);
    /* Drag title well past bounds; still reachable below the menu. */
    e.x=g.win[GUI_SKETCH].x+100; e.y=g.win[GUI_SKETCH].y+10; e.buttons=1; e.wheel=0;
    gui_pointer(&g,&e); e.x=-2000; e.y=-1000; gui_pointer(&g,&e);
    CHECK(g.win[GUI_SKETCH].x==0 && g.win[GUI_SKETCH].y==34);
    e.buttons=0; gui_pointer(&g,&e); CHECK(g.drag==-1);
    gui_key(&g,16); CHECK(g.focus == GUI_SETTINGS);
    click(20,14); CHECK(g.menu==1);
    gui_key(&g,GUI_DOWN); gui_key(&g,27); CHECK(!g.menu && !g.quit);
    gui_key(&g,17); CHECK(g.quit);
    rendering(640,400); rendering(960,540); rendering(1280,800);
    printf("gui: %d checks, %d failures\n",checks,failed);
    return !!failed;
}
