/*
SurfOS Keyboard Driver
--------------------
File: keyboard.c    Date: Prior to 4/23/04
--------------------
(C)2004 Brandon Burr
*/


#include <surfos/keyboard.h>
#include <surfos/console.h>
#include <asm/io.h>
#include <surfos/system.h>
#include <surfos/interrupt.h>
#include <surfos/timer.h>
#include <surfos/task.h>

#include <surfos/kernel.h>
#include <blibc_common.h>

u_char led_status =0x02;
u_char kbd_status=0;

#define CTRL_BIT 0x1
#define ALT_BIT 0x2
#define SHIFT_BIT 0x3

u_char keyQueue[KEY_QUEUE_LEN];
u_int kqPos;

void keybISR();
void timerISR();

void init_keyboard() {
    kprintf("\nKeyboard Initialization\n");
    memset(keyQueue,0,KEY_QUEUE_LEN);
    kqPos=0;
    kprintf("*Load ISR Handler for IRQ1\n");

    add_irq_handler(IRQ_KEYBOARD,(FuncIRQHandler)keyboard_handler,0);

    kprintf("*DONE\n\n");
}


void write_kbd(u_int adr, u_int data) {
    u_long timeout;
    u_int stat;
    KCRIT_ENTER
    for(timeout=500000L;timeout!=0;timeout--) {
        stat=inb_p(0x64);
        if((stat & 0x02) == 0) break;
    }
    if(timeout != 0) outb_p(adr,data);\
    KCRIT_LEAVE
}

void setleds() {
    KCRIT_ENTER
    outb(0x60, 0xED);
    while(inb(0x64) & 2);
    outb(0x60, led_status);
    while(inb_p(0x64) & 2);
    KCRIT_LEAVE
}

void setKey(u_char bit, bool yn) {
    KCRIT_ENTER
    if(yn) {
        kbd_status = kbd_status | bit;
    } else {
        kbd_status = kbd_status & ~bit;
    }
    KCRIT_LEAVE
}

bool testKey(u_char bit) {
    bool ret;
    KCRIT_ENTER
    ret = (bool)(kbd_status & bit);
    KCRIT_LEAVE;
    return ret;
}

void setLED(u_char ledStatus, bool yn) {
  KCRIT_ENTER
  if(yn) {
    led_status = led_status | ledStatus;
  } else {
    led_status = led_status & ~ledStatus;
  }
  setleds();
  KCRIT_LEAVE
}

void switchLED(u_char ledStatus) {
  KCRIT_ENTER
    if(led_status & ledStatus) {
        led_status = led_status & ~ledStatus;
    } else {
        led_status = led_status | ledStatus;
    }
    setleds();
  KCRIT_LEAVE
}

int testLED(u_char ledStatus) {
    int ret=0;
    KCRIT_ENTER
    if(led_status & ledStatus) ret= 1;
    KCRIT_LEAVE
    return ret;
}

void clear_key_queue() {
    KCRIT_ENTER
    memset(keyQueue,0,KEY_QUEUE_LEN);
    kqPos=0;
    KCRIT_LEAVE
}

void push_key_queue(u_char ch) {
    KCRIT_ENTER
    if(kqPos>KEY_QUEUE_LEN-1) kqPos=0; //somethings wrong w/ getch
    keyQueue[kqPos]=ch;
    kqPos++;
    KCRIT_LEAVE
}

void shiftQueueLeft() {
    u_int i;
    u_char kTmp=0;
    KCRIT_ENTER
    kTmp=keyQueue[0];
    for(i=0;i<=kqPos;i++) {
        kTmp=keyQueue[i+1];
        keyQueue[i]=kTmp;
    }
    KCRIT_LEAVE
}

u_char pop_key_queue() {
    u_char tmp=0;
    if(kqPos>KEY_QUEUE_LEN) kqPos=0; //probably no getch running
    tmp=keyQueue[0]; //get next in line
    if(tmp==0) {
      return 0;
    }
    shiftQueueLeft();
    kqPos--;
    return tmp;
}

FuncIRQHandler keyboard_handler() {
    u_char key,conv;
    key=inb(0x60);


    if(key & 0x80)  {// If a break code
        switch(key) {
        case CAPS_LOCK:
            switchLED(CAPS_LED);
            break;
        case NUM_LOCK:
            switchLED(NUM_LED);
            break;
        case SCROLL_LOCK:
             switchLED(SCROLL_LED);
             break;

        case KBD_BRK_CTRL:
             setKey((u_char)CTRL_BIT,false);
             break;
        case KBD_BRK_ALT:
             setKey((u_char)ALT_BIT,false);
             break;
        case KBD_BRK_LSHIFT:
        case KBD_BRK_RSHIFT:
             setKey((u_char)SHIFT_BIT,false);
             break;
         }

    } else {

        switch(key) {
        case KBD_RAW_CTRL:
             setKey((u_char)CTRL_BIT,true);
             break;
        case KBD_RAW_ALT:
             setKey((u_char)ALT_BIT,true);
             break;
        case KBD_RAW_LSHIFT:
        case KBD_RAW_RSHIFT:
             setKey((u_char)SHIFT_BIT,true);
             break;
        }

        conv = map_scancode(key);
        push_key_queue(conv);
    }
    return NULL;
}

extern void nt(),d1(),d2(),funky();

u_char map_scancode(int code) {
    u_int converted;
    u_short temp;
    static u_short prev=0;

    static const u_char normmap[100] =
    {/* 00    01    02    03    04    05    06    07    08    09    0A    0B    0C    0D    0E    0F   */
     /*00*/ 0,    0x1B, '1',  '2',  '3',  '4',  '5',  '6',  '7',  '8',  '9',  '0',  '-',  '=',  '\b', '\t',
     /*10*/ 'q',  'w',  'e',  'r',  't',  'y',  'u',  'i',  'o',  'p',  '[',  ']',  '\n', CTRL, 'a',  's',
     /*20*/ 'd',  'f',  'g',  'h',  'j',  'k',  'l',  ';',  '\'', '`',  SHIFT,'\\', 'z',  'x',  'c',  'v',
     /*30*/ 'b',  'n',  'm',  ',',  '.',  '/',  SHIFT,0,    ALT,  ' ',  CAPS, F1,   F2,   F3,   F4,   F5,
     /*40*/ F6,   F7,   F8,   F9,   F10,  NUM,  SCROL,HOME, UP,   PGUP, '-',  LEFT, '5',  RT,   '+',  END,
     /*50*/ DOWN, PGDN, INS,  DEL,  0,    0,    '\\', F11,  F12
    };


    if((u_int)code >= sizeof(normmap) / sizeof(normmap[0])) return 0;

    temp = normmap[code];
    if(temp == 0) return temp;

       if(code > 0x58) return 0;
       converted = normmap[code];


       if( testKey((u_char)SHIFT_BIT) ) { //switch to shift
         if(converted=='\\') converted='|';
           if(isalpha(converted)) converted=toupper(converted);
             if(isdigit(converted)) {
                 unsigned char nC[] = {'!','@','#','$','%','^','&','*','(',')'};
                 if(converted-'1'>=0 && converted-'1'<10) converted=nC[converted-'1'];
             }
             if(converted=='`') converted='~';
             if(converted=='-') converted='_';
             if(converted=='=') converted='+';
             if(converted=='[') converted='{';
             if(converted==']') converted='}';
             if(converted==';') converted=':';
             if(converted=='\'') converted='"';
             if(converted==',') converted='<';
             if(converted=='.') converted='>';
             if(converted=='/') converted='?';
             //kbd_status = kbd_status | 0;
           }

         /* If a function key is pressed, switch to the correct console */
         if(converted>=F1 && converted < (F1+NUM_CONSOLES)) {
           switchConsole(converted-F1);
           syncVideoConsole(true);
         }

         /*if(converted==DEL && testKey((u_char)CTRL_BIT) && testKey((u char)ALT BIT)) {
            new_task("reboot",&conArray[0],KERNEL,PL_FIFO,(u_long*)reboot);
         }*/

          if(converted==F12) new_task("reboot",&conArray[0],KERNEL,PL_FIFO,(u_long*)reboot);
         if(converted==F9) new_task("nt",&conArray[0],KERNEL,PL_LOW,(u_long*)nt);

         if(converted==F5) new_task("d1",&conArray[0],KERNEL,PL_LOW,(u_long*)d1);
         if(converted==F6) new_task("d2",&conArray[0],KERNEL,PL_LOW,(u_long*)d2);
         if(converted==F7) fd_set_motor(0,1);
         if(converted==F8) fd_set_motor(0,0);
         prev=converted;

         return converted;
}
