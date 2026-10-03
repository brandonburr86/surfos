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
#include <surfos/tty.h>

extern surf_task *curTask;
extern surf_console conVideo;

static struct tty *my_tty(void) {
    return (curTask && curTask->tty) ? curTask->tty : tty_active;
}

u_char getc() { /*reads the queue, does not wait*/
    int c = tty_trygetc(my_tty());
    return c < 0 ? 0 : (u_char)c;
}

u_char getch() { /*blocks until a usable key arrives*/
    for(;;) {
        int c = tty_getc(my_tty());
        if(isprint(c) || c == 0x08 || c == '\n' || c == '\t') return (u_char)c;
    }
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
