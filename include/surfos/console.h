/*
SurfOS Console I/O
----------------------
File: console.h Date: Prior than 4/23/04
----------------------
(C)2004 Brandon Burr
*/

#ifndef _SURFOS_CONSOLE_H
#define _SURFOS_CONSOLE_H

#include <surfos/types.h>

#define COLUMNS     80
#define LINES       25

#define VMEM_ADDR   0xB8000
#define VIDPORT 0x3D4

#define BUF_SIZE 1024

#define KERN_TXT_COLOR YELLOW_TXT //sets the output color for kernel messages

/* Virtual Console memory allocation*/
#define NUM_CONSOLES 4 //F1..F4; each has a shell and a tty
#define VMEM_SIZE 4000 //each virtual console is 4000 bytes
#define CONSOLE_BASE 0xC000000//0x800 //start storing the first console @
//0x800 /***********************************/

typedef struct surf_point {
    u_int x;
    u_int y;
} surf_point;

typedef struct surf_console {
    TEXTCOLOR txtColor;
    surf_point loc;
    u_char *vmem;
    u_char memBuf[4001];
} surf_console;

void sscroll(surf_console *con);
inline void setpos(surf_console *con);
void kputch(surf_console* con,TEXTCOLOR color, int c);
void kprintf(const char *format, ...);
void kcprintf(surf_console *con, const char *format, ...);

void clearScreen();
void clearConsole(surf_console *con);
void setConTextColor(surf_console *con, TEXTCOLOR color);
void setPoint(surf_point *pt, u_int x, u_int y);
void switchConsole(u_int conNum);

void syncVideoConsole(bool);

extern surf_console *conActive;
extern surf_console conArray[];

#endif
