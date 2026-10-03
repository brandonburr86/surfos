/*
SurfOS blibc - string functions puts(), cputs();
*/

#include <surfos/types.h>
#include <surfos/console.h>
#include <surfos/task.h>
#include <surfos/tty.h>

#include <blibc_common.h>

extern surf_console *conActive;
extern surf_console conVideo;

/* the console this task prints on (the active one before there are tasks) */
static surf_console *my_con(void) {
    return (curTask && curTask->con) ? curTask->con : conActive;
}

void puts(char *s) { /* same path as putch(), so the serial mirror and scrolling agree */
    surf_console *con = my_con();
    int i;
    if(!con || !s) return;
    for(i=0; s[i] && i<1024; i++) kputch(con, con->txtColor, s[i]);
}

void cputs(u_char atr, char *str) {
    surf_console *con = my_con();
    TEXTCOLOR tmp;
    if(!con) return;
    tmp = con->txtColor;
    con->txtColor = atr;
    puts(str);
    con->txtColor = tmp;
}

char *gets(char *str) { /* a line from this task's tty, with editing and history; 255 bytes at most */
    struct tty *t = (curTask && curTask->tty) ? curTask->tty : tty_active;
    tty_readline(t, str, 255);
    return str;
}

char *cgets(int color,char *str) {
    surf_console *con = my_con();
    char *ptr;
    TEXTCOLOR tmp = con->txtColor;
    con->txtColor = color;
    ptr = gets(str);
    con->txtColor = tmp;
    return ptr;
}
