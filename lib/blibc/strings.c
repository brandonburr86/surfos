/*
SurfOS blibc - string functions puts(), cputs();
*/

#include <surfos/types.h>
#include <surfos/console.h>

#include <blibc_common.h>

extern surf_console *conActive;
extern surf_console conVideo;

void puts(char *s) {

    int i;
    int c;
    if(!conActive) return;

    for(i=0;i<1024;i++)   {
    if(s[i]=='\0') break;
    c = s[i];
        switch (c) {
        case '\n':
        {
        conActive->loc.x = 0;

        if (++conActive->loc.y >= LINES) {
            sscroll(conActive);
            conActive->loc.y--;
        }

        if (++conVideo.loc.y >= LINES) {
            syncVideoConsole(false);
            conVideo.loc.y--;
        }
        }
        break;
    default:
        if(c>=32 && c<=126)
        {
        *(conActive->vmem + (conActive->loc.x + conActive->loc.y * COLUMNS) * 2) = c & 0xFF;
        *(conActive->vmem + (conActive->loc.x + conActive->loc.y * COLUMNS) * 2 + 1) = conActive->txtColor;

        *(conVideo.vmem + (conVideo.loc.x + conVideo.loc.y * COLUMNS) * 2) = c & 0xFF;
        *(conVideo.vmem + (conVideo.loc.x + conVideo.loc.y * COLUMNS) * 2 + 1) = conActive->txtColor;

        if (++conActive->loc.x >= COLUMNS) {
            conActive->loc.x = 0;
            if (++conActive->loc.y >= LINES) {
            sscroll(conActive);
            conActive->loc.y--;
            }
        }
        syncVideoConsole(false);
                }
        break;
    }
    }
    //syncVideoConsole(false);
    return;
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
