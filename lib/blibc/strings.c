/*
SurfOS blibc - string functions puts(), cputs();
*/

#include <surfos/types.h>
#include <surfos/console.h>

#include <blibc_common.h>

extern surf_console *conActive;
extern surf_console conVideo;

void puts(char *s) { /* same path as putch(), so the serial mirror and scrolling agree */
    int i;
    if(!conActive || !s) return;
    for(i=0; s[i] && i<1024; i++) kputch(conActive, conActive->txtColor, s[i]);
}

void cputs(u_char atr, char *str) {
    TEXTCOLOR tmp=conActive->txtColor;
    conActive->txtColor=atr;
    puts(str);
    conActive->txtColor = tmp;
}

extern surf_console *conActive;

char *gets(char *str) { //single tasking version of gets()
    surf_console *startCon=conActive; //so different console's dont interlap
    int i=0;
    int chPrint=0;
    char chEnd='\n';
    char tmp;
    for(i=0;;i++) {
        tmp=getch();

        if(tmp==0 || conActive!=startCon) { //null or not on same console, skip
            i--;
            continue;
        }
        if(tmp=='\b') {
            if(chPrint>0) {
                chPrint--;
                putch(tmp);
            }
            if(i>0) i-=2;
            continue;
        } else {
            putch(tmp);
            if(tmp==chEnd) {
                *(str+i)=0;
                break;
            } else {
                *(str+i)=tmp;
                chPrint++;
            }
        }
    }
    return str;
}

char *cgets(int color,char *str) {
    char *ptr;
    TEXTCOLOR tmp=conActive->txtColor;
    conActive->txtColor=color;
    ptr=gets(str);
    conActive->txtColor=tmp;
    return ptr;
}
