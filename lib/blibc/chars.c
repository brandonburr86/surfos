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
#include <surfos/wait.h>
#include <surfos/irq.h>
#include <surfos/interrupt.h>

extern surf_task *curTask;
extern surf_console conVideo;

u_char getc() { /*reads buffer*/
    return pop_key_queue();
}

u_char getch() { /*blocks until a key arrives*/
    u_char tmp;
    for(;;) {
        u_long flags = irq_save();
        tmp = pop_key_queue();
        if(!tmp) {
            if(curTask && curTask != idle_task && !in_interrupt()) {
                wait_prepare(&kbd_wq);   /* enqueued before interrupts come back: no lost wakeup */
                irq_restore(flags);
                yield();
            } else {
                irq_restore(flags);      /* boot code: nothing to switch to */
            }
            continue;
        }
        irq_restore(flags);
        if(isprint(tmp) || tmp == 0x08 || tmp == '\n' || tmp == '\t') return tmp;
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
