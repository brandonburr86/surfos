/*
SurfOS blibc - getch(), getchar(), putch(), putchar()
(c) 2004 Brandon Burr
*/

#include <asm/io.h>
#include <surfos/keyboard.h>
#include <surfos/console.h>
#include <surfos/types.h>
#include <blibc_common.h>
#include <surfos/task.h>

extern surf_task *curTask;
extern surf_console conVideo;

u_char getc() { /*reads buffer*/
    return pop_key_queue();
}

u_char getch() { /*pauses and reads*/
    u_char tmp=0;
    clear_key_queue();
    while(tmp==0) {
        tmp=getc();
        if(!isprint(tmp) && tmp != 0x08 && tmp != '\n' && tmp != '\t') tmp=0;
    }
    return tmp;
}

u_char getchar() {
    unsigned char code=getch();
    putch(code);
    return code;
}



void putch(int c) {
    if(!curTask) return;
    kputch(curTask->con,curTask->con->txtColor,c);
   return;
}
