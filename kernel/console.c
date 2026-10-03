/*
SurfOS Console I/O
--------------------
File: console.c Date: Prior to 4/23/04
--------------------
(C)2004 Brandon Burr
*/

#include <asm/io.h>
#include <surfos/types.h>
#include <surfos/kernel.h>
#include <surfos/keyboard.h>
#include <surfos/task.h>
#include <surfos/console.h>
#include <sys/serial.h>

#include <mm/kalloc.h>

#include <blibc_common.h>

/* Initial consoles */
surf_console conArray[NUM_CONSOLES]; //array of available consoles
surf_console conVideo; //the console mapped to the actual video memory
surf_console *conActive; //the pointer to the active console
/********************/

void init_console(void) {
    int i=0;
    if(NUM_CONSOLES<1) return;

    //init the video console
    conVideo.vmem = (u_char*)VMEM_ADDR; //normally 0xB8000
    conVideo.txtColor=TEAL_TXT; //for posterity
    clearConsole((surf_console*)&conVideo);
    setPoint((surf_point*)&conVideo.loc,0,0);

    //init the virtual consoles
    for(i=0;i<NUM_CONSOLES;i++) {
        conArray[i].vmem = &conArray[i].memBuf;//( CONSOLE_BASE + (i*VMEM_SIZE) );
      conArray[i].txtColor=WHITE_TXT;
      clearConsole((surf_console*)&conArray[i]);
        setPoint((surf_point*)&conArray[i].loc,0,0);
    }

    conActive = (surf_console*)&conArray[0]; //set active console to first.*/

    return;
}

void syncVideoConsole(bool memcopy) {
    surf_console *con = conActive;
    if(!con) return;
    KCRIT_ENTER
    conVideo.loc=con->loc;
    conVideo.txtColor=con->txtColor;
    setpos(con);
    if(memcopy) memcpy(conVideo.vmem,con->vmem,VMEM_SIZE);
    KCRIT_LEAVE
}

bool isConsoleActive(surf_console *con) {
    return con == conActive ? true:false;
}

bool isConsoleValid(surf_console *con) {
    if(!con) return false;
    if(!con->vmem) return false;
    if(con->loc.x >= COLUMNS || con->loc.y >= LINES) return false;
    return true;

}

void switchConsole(u_int conNum) {
    if(conNum < 0 || conNum >= NUM_CONSOLES) return;
    if(isConsoleValid((surf_console*)&conArray[conNum]) == false) return;
    KCRIT_ENTER
    conActive = &conArray[conNum];
    KCRIT_LEAVE
}

void clearConsole(surf_console *con) { //re-initializes a console
  int i;
  if(!con) return;
  for (i=0; i< (COLUMNS*LINES*2); i+=2) {
        *(con->vmem + i) = 0;
       *(con->vmem + i + 1) = 7;
  }
  con->loc.x = 0;
  con->loc.y = 0;
  setpos(con);
  if(isConsoleActive(con)) {
      syncVideoConsole(true);
      serial_console_clear();
  }
}

void clearScreen() {
    clearConsole(conActive);
}

void setConTextColor(surf_console *con, TEXTCOLOR color) { //sets the text color of a console
    if(con) {
        con->txtColor = color;
    }
}

void setPoint(surf_point *pt, u_int x, u_int y) { //sets the effective x and y of the console
    if(!pt) return;
    pt->x = x;
    pt->y = y;
}

void sscroll(surf_console *con) { //scrolls the console
    int i,j;
    u_char *vmem;
    if(!con) return;
    vmem=con->vmem;

    for(i=COLUMNS*2,j=0;i<4000;i++,j++) { //shift the console up
        *(vmem+j) = *(vmem+i);
    }

    for(;j<4000;j+=2) { //clean the last line
        *(vmem+j)=0; //char byte
        *(vmem+j+1)=7; //attrib byte
    }

    syncVideoConsole(true);
    return;
}


int tX,tY;
inline void setpos(surf_console *con) { //Sets the hardware cursor correctly for the console
    if(!con) return;
    if(isConsoleActive(con)==false) return;
    tX=con->loc.x;
    tY=con->loc.y;
    KCRIT_ENTER
    asm("mov $0, %ebx");
    asm("mov (%0), %%bl" :: "m"(tX));
    asm("mov $0, %eax");
    asm("mov (%0),%%al" :: "m"(tY));
    asm("movl $80, %edx\n"
        "mul %edx\n"
        "add %eax, %ebx\n"

        "mov $0xf, %al\n"
        "mov $0x03d4, %dx\n"
        "out %al, %dx\n"

        "mov %bl,%al\n"
        "mov $0x03d5, %dx\n"
        "out %al, %dx\n"

        "mov $0xe, %al\n"
        "mov $0x03d4,%dx\n"
        "out %al,%dx\n"

        "mov %bh, %al\n"
        "mov $0x03d5, %dx\n"
        "out %al, %dx\n");
   KCRIT_LEAVE
}

void itoa (char *buf, u_int base, u_int d) {
  char *p = buf;
  char *p1, *p2;
  unsigned int ud = d;
  int divisor = 10;

  if (base == 'd' && d < 0) {
      *p++ = '-';
      buf++;
      ud = -d;
  } else if (base == 'x')
    divisor = 16;

  do {
      int remainder = ud % divisor;
      *p++ = (remainder < 10) ? remainder + '0' : remainder + 'a' - 10;
  } while (ud /= divisor);


  *p = 0; //null terminate


  p1 = buf;
  p2 = p - 1;
  while (p1 < p2) {
      char tmp = *p1;
      *p1 = *p2;
      *p2 = tmp;
      p1++;
      p2--;
  }
}

/* write one character cell at the console's cursor, without moving the cursor */
static void putcell(surf_console *con, TEXTCOLOR color, int c) {
    u_int off = (con->loc.x + con->loc.y * COLUMNS) * 2;
    KCRIT_ENTER
    *(con->vmem + off) = c & 0xFF;
    *(con->vmem + off + 1) = color;
    if(isConsoleActive(con)) {
        *(conVideo.vmem + off) = c & 0xFF;
        *(conVideo.vmem + off + 1) = color;
    }
    KCRIT_LEAVE
}

void kputch(surf_console *con,TEXTCOLOR color, int c) {
    /* The serial console shows whatever the active console shows, plus anything
       printed before the video console exists (con == NULL during early boot). */
    if(!con || con == conActive) serial_console_putc(c);
    if(!con) return;
    switch (c) {
    case '\n':
        con->loc.x = 0;
        if(++con->loc.y >= LINES) {
            sscroll(con);
            con->loc.y--;
        }
        syncVideoConsole(false);
        break;
    case 0x08:
        /* erase the cell just typed and stay there (the 2004 code stepped back twice) */
        if(con->loc.x > 0) {
            con->loc.x--;
            putcell(con, color, ' ');
        }
        syncVideoConsole(false);
        break;
    default:
        if(c>=32 && c<=126) {
            putcell(con, color, c);
            if(++con->loc.x >= COLUMNS) {
                con->loc.x = 0;
                if (++con->loc.y >= LINES) {
                    sscroll(con);
                    con->loc.y--;
                }
            }
            if(isConsoleActive(con)) syncVideoConsole(false);
        }
        break;
    }
    setpos(con);
    return;
}

void kprintf(const char *format, ...) {
  char **arg = (char **) &format;
  int c;
  char buf[20];

  arg++;

  while ((c = *format++) != 0) {
    if (c != '%') {
    kputch(conActive,KERN_TXT_COLOR,c);
   } else {
    char *p;
      c = *format++;
      switch (c) {
        case 'i':
         case 'd':
         case 'u':
         case 'x':
           itoa (buf, c, *((int *) arg++));
           p = buf;
           goto string;
           break;
         case 's':
           p = *arg++;
           if(!p) p = "(null)";
string:
              while(*p) kputch(conActive,KERN_TXT_COLOR,*p++);
              break;
         default:
           kputch(conActive,KERN_TXT_COLOR, *((int *) arg++));
           break;
      }
    }
  }
}

void kcprintf(surf_console *con, const char *format, ...) {
  char **arg = (char **) &format;
  int c;
  char buf[20];

  arg++;

  while ((c = *format++) != 0) {
    if (c != '%') {
    kputch(con,KERN_TXT_COLOR,c);
   } else {
    char *p;
      c = *format++;
      switch (c) {
        case 'i':
         case 'd':
         case 'u':
         case 'x':
           itoa (buf, c, *((int *) arg++));
           p = buf;
           goto string;
           break;
         case 's':
           p = *arg++;
           if(!p) p = "(null)";
string:
              while(*p) kputch(con,KERN_TXT_COLOR,*p++);
              break;
         default:
           kputch(con,KERN_TXT_COLOR, *((int *) arg++));
           break;
      }
    }
  }
}
