/*
SurfOS Desktop: the Calculator's Arithmetic
--------------------
File: guicalc.c Date: 10/25/26
--------------------
Whole numbers, + - * /, within plus or minus 999,999,999; division truncates toward zero, and
division by zero or a value out of range shows an error until C. Moved here from shell/guicore.c
(sys/guicalc.h) so that the Calculator as a program (user/bin/wcalc.c) counts as the desktop's
own does: libsurf builds this file too (user/libsurf/guicalc.c). Milestone M12, stage C6 of
docs/superpowers/plans/2026-10-25-app-windows.md.
*/
#include <sys/guicalc.h>
#include <string.h>

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
