/*
SurfOS ring0 Shell (v0.001)
(C)2004 Brandon Burr
*/

#include <blibc_common.h>

#include <asm/io.h>
#include <surfos/keyboard.h>
#include <surfos/types.h>
#include <surfos/kernel.h>
#include <surfos/timer.h>
#include <sys/parport.h>
#include <surfos/task.h>
#include <surfos/console.h>
#include <surfos/system.h>

#include <mm/kalloc.h>
#include <mm/memory.h>

#include <sys/parport.h>

void fake_inter();
void parseCommand(const char line[]);
void startShell();
void invokeHelp();
void invokeDemo();
void invokeRS232Term(int doSend);
void demoException();
void demoStrcmp();
void demoColor();

char *prompt = "SurfOS*> ";

#define COM1 0x3F8
  /* COM1 0x3F8                        */
  /* COM2 0x2F8                */
  /* COM3 0x3E8                */
  /* COM4 0x2E8                */

void invokeRS232Term(int doSend) {
    char chIn, chOut;
    chOut=chIn=(char)0;

    outb(COM1+1, 0); /* turn off interrupts */

    /*comm settings*/
    outb(COM1+3, 0x80); /* set DLAB on*/
    outb(COM1+0, 0x0C); /* 9600bps - baud rate low latch byte*/
                  /* Default 0x03 =  38,400 BPS */
                  /*         0x01 = 115,200 BPS */
                  /*         0x02 =  57,600 BPS */
                  /*         0x06 =  19,200 BPS */
                  /*         0x0C =   9,600 BPS */
                  /*         0x18 =   4,800 BPS */
                  /*         0x30 =   2,400 BPS */
    outb(COM1+1, 0x00); /* set baud rate high latch byte*/
    outb(COM1+3, 0x03); /* 8-N-1 */
    outb(COM1+2, 0xC7); /* FIFO control register*/
    outb(COM1+4, 0x0B); /* turn on dtr,rts, out2*/

    printf("\n\nSurfOS RS-232 Communication (COM1)\n");
    printf("Press Escape to Exit Terminal\n");
    printf("--------------------------------------\n\n");

    do {
        chIn =inb(COM1+5); /* new char? */

        if(chIn & 1) { /* read in from port*/
            chIn=inb(COM1);
            putch(chIn);
        }

        /*if(kb hit()) {
            chOut = getc();
            if(doSend) outb(COM1,chOut);
            putch(chOut);
        }*/

    } while(chOut != 27); /* break on escape */
    putch('\n');
}

void dohanoi(int N, int from, int to, int use) {
    if (N > 0) {
        dohanoi(N-1, from, use, to);
        printf ("move %d --> %d\n", from, to);
        dohanoi(N-1, use, to, from);
    }
}

void runHanoi() {
    int n,from,to,use;
    char ch;
    clearScreen();
    n=0;
    from=1;
    to=3;
    use=2;
    cputs(RED_TXT, "SurfOS Towers of Hanoi Calculator\n");
    cputs(BLUE_TXT,"---------------------------------\n");
    do {
        printf("\nHanoi Number# ");
        ch=getchar();
        printf("\n");
        n=ch-'0';
        if(isdigit(ch)) dohanoi(n,from,to,use);
    } while(n!=0);

}
void demoBeep() {
    u_long freq,dur;
    printf("\n Beeping @ 2000 hertz for 1000 ms\n");
    freq=2000;
    dur=1000;
    sysbeep(freq,dur);
}

void demoException() {
    char ch;
    clearScreen();
    cputs(YELLOW_TXT,"                       [ SurfOS Exception Handler Demo ]\n");
    printf("           Press (1-7) to invoke that exception, or press 'q' to quit. \n");
    printf(" -----------------------------------------------------------------------------\n");
    do {
        cputs(GREEN_TXT,"\nInvoke exception: ");
        ch=(char)0;
        ch=getch();
        printf("\n");
        switch(ch) {
            case 'q':
                return;
                break;
            case '1':
                asm("int $01");
                break;
            case '2':
                asm("int $02");
                break;
            case '3':
                asm("int $03");
                break;
            case '4':
                asm("int $04");
                break;
            case '5':
                asm("int $05");
                break;
            case '6':
                asm("int $06");
                break;
            case '7':
                asm("int $07");
                break;

        }

    } while(1);
}

void demoStrcmp() {
    char str1[255];
    char str2[255];
    int result;
    memset(str1,0,255);
    memset(str2,0,255);

    clearScreen();
    printf("Enter a string: ");
    gets(str1);
    printf("Enter another string: ");
    gets(str2);
    result=strcmp(str1,str2);
    switch(result) {
        case -1:
            printf("\nThe first string is less than the second string!\n\n");
            break;
        case 0:
            printf("\nThe strings are equal!\n");
            break;
        case 1:
            printf("\nThe first string is greater than the second string\n\n");
            break;
        default:
            printf("\nSomething is funky\n\n");
    }
    printf("Press any key to continue...");
    getch();
}

void demoColor() {
    char i=1;
    clearScreen();
    putch('\n');
    for(i=1;i<16;i++) {
        cputs(i,"Welcome to SurfOS!!!\n");
    }
    printf("Press any key to continue...");
    getch();
}

void invokeDemo() {
    char choice;
    int skip=false;
    do {
        do {
            if(skip==false) {
                clearScreen();
                cputs(RED_TXT,"\nSurfOS Demonstration Menu\n");
                printf("- - - - - - - - - - - - - -\n");
                printf("1) Exception Handling\n");
                printf("2) String comparison\n");
                printf("3) Colorful console\n");
                printf("q) Quit\n");
            }
            skip=false;
            choice=getchar();
            putch('\n');
            if(choice=='q') break;
            if(choice<'1' || choice>'3') { skip=true; putch('\b'); continue; }

            switch(choice) {
            case '1':
                demoException();
                break;
            case '2':
                demoStrcmp();
                break;
            case '3':
                demoColor();
                break;
            }
        } while(1);
    } while(choice!='q');
    putch('\n');

}

#define sz 1024
void runTest() {
    //char *str;
    //printf("Starting memory speed test pretr(1)...");
    char *memory[2000];
  u_long startTick,endTick,diff,i,j;
  startTick=getticks();
  for(j=0;j<1000;j++) {
  for(i=0;i<2000;i++) {
        memory[i]=(char*)kalloc(3);
  }
  for(i=0;i<500;i++) {
        kfree(memory[i]);
  }
  for(i=0;i<250;i++) {
        memory[i]=kalloc(3);
  }
  for(i=1000;i<2000;i++) {
        kfree(memory[i]);
  }
  }
  print_lst_count();
  //for(i=0;
        //kprintf("Allocating %i bytes to memory address 0x%x\n",i*50,memory[i]);

  endTick=getticks();
  diff=endTick-startTick;
  printf("\nOperation took %i milliseconds\n",TICKS_TO_USEC(diff));


    /*for(;;) {
        str=getpage();
        if(!str) {
            printf("Out of memory!\n");
            return;
        }
    }*/
}


void getFunky(int tempo)
{

float x = ((float)tempo / (float)10);

sysbeep(2093,250*x);//C
            sleep(50*x);

            sysbeep(2093,250*x);//C
            sleep(50*x);

            sysbeep(1865,250*x);//A#
            sleep(50*x);

            sysbeep(2093,250*x);//C

            sleep(250*x);//1*8 rest

            sysbeep(1568,500*x);//G
            sleep(50*x);

            sysbeep(1568,250*x);//G
            sleep(50*x);

            sysbeep(2093,250*x);//C
            sleep(50*x);

            sysbeep(2794,250*x);//F
            sleep(50*x);

            sysbeep(2637,250*x);//E
            sleep(50*x);

            sysbeep(2093,250*x);//E
            sleep(50*x);

        sleep(5000*x);

        sysbeep(2093,250*x);//C
        sleep(50*x);

        sysbeep(2093,250*x);//C
        sleep(50*x);

        sysbeep(1865,250*x);//A#
        sleep(50*x);

        sysbeep(2093,250*x);//C

        sleep(250*x);//1*8 rest

        sysbeep(1568,500*x);//G
        sleep(50*x);

        sysbeep(1568,250*x);//G
        sleep(50*x);

        sysbeep(2093,250*x);//C
        sleep(50*x);

        sysbeep(2794,250*x);//F
        sleep(50*x);

        sysbeep(2637,250*x);//E
        sleep(50*x);

        sysbeep(2093,250*x);//E
        sleep(50*x);

        sleep(5000*x);

    sysbeep(1568,250*x);//G
    sleep(50*x);

    sysbeep(1568,250*x);//G
    sleep(50*x);

    sysbeep(1396,250*x);//F
    sleep(50*x);

    sysbeep(1568,250*x);//G

    sleep(250*x);//1*8 rest

    sysbeep(1175,500*x);//d
    sleep(50*x);

    sysbeep(1175,250*x);//d
    sleep(50*x);

    sysbeep(1568,250*x);//G
    sleep(50*x);

    sysbeep(2093, 250*x);//c
    sleep(50*x);

    sysbeep(1976, 250*x);//b
    sleep(50*x);

    sysbeep(1568,250*x);//G
    sleep(50*x);

    sleep(5000*x);

    sysbeep(2093,250*x);//C
        sleep(50*x);

        sysbeep(2093,250*x);//C
        sleep(50*x);

        sysbeep(1865,250*x);//A#
        sleep(50*x);

        sysbeep(2093,250*x);//C

        sleep(250*x);//1*8 rest

        sysbeep(1568,500*x);//G
        sleep(50*x);

        sysbeep(1568,250*x);//G
        sleep(50*x);

        sysbeep(2093,250*x);//C
        sleep(50*x);

        sysbeep(2794,250*x);//F
        sleep(50*x);

        sysbeep(2637,250*x);//E
        sleep(50*x);

        sysbeep(2093,250*x);//E
        sleep(50*x);


}


void invokeHelp() {
    printf("\n    SurfOS ring0 Debug Shell v0.008\n");
    printf("    -------------------------------\n");
    printf("    beep   (beeps w/ user given params)\n");
    printf("    funky  (gets down and funky)\n");
    printf("    clear  (clears the active console)\n");
    printf("    term   (opens up a terminal on COM1 with 9600-8-N-1 settings)\n");
    printf("    reboot (reboot the machine)\n");
    printf("    ps     (list processes)\n");
    printf("    tick   (print the tick count)\n");
    printf("    lpstat (display status of the parallel port)\n");
    printf("    memstat (display memory statistics)\n");
    printf("    demo   (demonstration of some of the capabilities of SurfOS)\n");
    printf("    hanoi  (Computes the Towers of Hanoi algoritm)\n");
    printf("    help   (this menu)\n");
    printf("    -------------------------------\n");
    printf("    (C)2004 Brandon Burr.\n\n");
}

void funky() {
    getFunky(10);
}

extern surf_task *curTask;

void nt() {
    int i=0;
    for(i=0;i<3;i++) {
        printf("pid: %i esp=0x%x\n",curTask->pid,curTask->esp);
        sleep(500);
    }
    kprintf("exiting\n");
}

void runTest2() {
    asm("int3");
}
void d1() {
    int i=0;
    for(i=0;i<4;i++) {
        printf("crasher1\n",curTask->pid,curTask->esp);
        sleep(250);
    }
    asm("int3");
}

void d2() {
    int i=0;
    for(i=0;i<3;i++) {
        printf("crasher1\n",curTask->pid,curTask->esp);
        sleep(250);
    }
    asm("movl $0,%eax");
    asm("jmpl *(%eax)");
}

void recurse() {
top:
    //new task("recurse",conActive,KERNEL,PL_LOW,(u_int*)recurse);
    goto top;
}

void parseCommand(const char line[]) {
    if(!strlen(line)) return; /* no command entered */
    if(!strcmp(line,"help")) {
        invokeHelp();
    } else if(!strcmp(line,"clear")) {
        clearScreen();
    } else if(!strcmp(line,"reboot")) {
        printf("\n    Please wait... rebooting...");
        reboot();
    } else if(!strcmp(line,"beep")) {
        demoBeep();
    } else if(!strcmp(line,"term")) {
        //invokeRS232Term(0);
    } else if(!strcmp(line,"funky")) {
        new_task("funky",conActive,KERNEL, PL_NORMAL,(u_int*)funky);
    } else if(!strcmp(line,"die")) {
        new_task("recurse",conActive,USER, PL_NORMAL,(u_int*)recurse);
    } else if(!strcmp(line,"pl")) {
        print_lst_count();
    } else if(!strcmp(line,"demo")) {
        invokeDemo();
    } else if(!strcmp(line,"hanoi")) {
        runHanoi();
    } else if(!strcmp(line,"test")) {
        /*for(i=0;i<600000000;i++) {
            new task("tester",conActive,KERNEL,PL_NORMAL,(u_int*)runTest2);
            sleep(30);
        }*/
        void *tmp = palloc(1024);
        void *tmp2= mm_lookup_linear(tmp);
        kprintf("0x%x linear is using physical 0x%x\n",tmp,tmp2);
        pfree(tmp);
    } else if(!strcmp(line,"tick")) {
        printf("\n    Ticks: %i\n\n",getticks());
    } else if(!strcmp(line,"lpstat")) {
        printParStatus();
    } else if(!strcmp(line,"memstat")) {
        printMemInfo();
    } else if(!strcmp(line,"kalloc")) {
        kalloc(1024);
    } /*else if(!strcmp(line,"kalloca")) {
        char *ptr;
        for(;;) { ptr = kalloc(1024), memset(ptr,65,1024); }
    }*/ else if(!strcmp(line,"ps")) {
        print_tasks();
    } else if(!strcmp(line,"inter")) {
        fake_inter();
    } else if(!strcmp(line,"time")) {
        //time t time;
        //get time(&time);
        //printf("The current time is: %i:%i:%i\n",time.hour,time.minute, time.second);
    } else {
        printf("    Invalid command.\n");
    }
}

void startShell() {
    char line[255];
    memset(line,0,255);
    for(;;) {
        cputs(LBLUE_TXT,prompt);
        memset(line,0,255);
        cgets(TEAL_TXT,line);
        parseCommand(line);
    }
}
