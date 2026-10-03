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
#include <mm/paging.h>
#include <surfos/klog.h>

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
    result = result < 0 ? -1 : (result > 0 ? 1 : 0);
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


/* Random allocate/free with a byte pattern per block, then everything freed and the
   heap walked and compared with the starting point. */
void heaptest() {
    enum { SLOTS = 192, ROUNDS = 20000 };
    static void *ptrs[SLOTS];
    static u_int sizes[SLOTS];
    struct heap_stats before, after;
    u_int i, r, allocs = 0, frees = 0, maxlive = 0, live = 0, bad = 0;

    memset(ptrs, 0, sizeof(ptrs));
    kheap_stats(&before);
    srand(getticks() + 7);
    for(r = 0; r < ROUNDS; r++) {
        i = rand() % SLOTS;
        if(ptrs[i]) {
            u_char *p = ptrs[i];
            u_int k;
            for(k = 0; k < sizes[i]; k++) if(p[k] != (u_char)(i + k)) { bad++; break; }
            kfree(ptrs[i]);
            ptrs[i] = NULL;
            frees++; live--;
        } else {
            u_int sz = (rand() % 16 == 0) ? 1 + rand() % 20000 : 1 + rand() % 512;
            u_char *p = kalloc(sz);
            u_int k;
            if(!p) { bad++; continue; }
            for(k = 0; k < sz; k++) p[k] = (u_char)(i + k);
            ptrs[i] = p; sizes[i] = sz;
            allocs++; live++;
            if(live > maxlive) maxlive = live;
        }
    }
    for(i = 0; i < SLOTS; i++) if(ptrs[i]) { kfree(ptrs[i]); ptrs[i] = NULL; frees++; }
    kheap_stats(&after);
    if(!kheap_check()) bad++;
    if(after.bytes_used != before.bytes_used || after.blocks_used != before.blocks_used) bad++;
    printf("heaptest: %u allocs, %u frees, %u live at peak, used %lu -> %lu bytes, %lu free blocks: %s\n",
           allocs, frees, maxlive, before.bytes_used, after.bytes_used, after.blocks_free, bad ? "HEAPTEST FAIL" : "HEAPTEST PASS");
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
    printf("    dmesg  (kernel log)\n");
    printf("    heaptest (random allocations with checksums, then verify the heap)\n");
    printf("    crashnull (write through a NULL pointer)\n");
    printf("    crashdiv/crashgp/crashint (raise a divide error / protection fault / unused vector)\n");
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
    } else if(!strcmp(line,"die")) { //a ring-3 task: faults until P1 exists
        new_task("ring3",conActive,USER, PL_NORMAL,(u_int*)funky);
    } else if(!strcmp(line,"demo")) {
        invokeDemo();
    } else if(!strcmp(line,"hanoi")) {
        runHanoi();
    } else if(!strcmp(line,"test")) {
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
    } else if(!strcmp(line,"ps")) {
        print_tasks();
    } else if(!strcmp(line,"inter")) {
        fake_inter();
    } else if(!strcmp(line,"time")) {
        //time t time;
        //get time(&time);
        //printf("The current time is: %i:%i:%i\n",time.hour,time.minute, time.second);
    } else if(!strcmp(line,"heaptest")) {
        heaptest();
    } else if(!strcmp(line,"crashnull")) {
        *(volatile int *)0 = 1; //page 0 is unmapped, so this is a page fault with an error code
    } else if(!strcmp(line,"dmesg")) {
        klog_dump();
    } else if(!strcmp(line,"crashdiv")) { //exercise the trap framework: real exceptions in this task
        volatile int one = 1, zero = 0; /* two volatiles: gcc folds 1/x into compares with no idiv */
        printf("%i\n", one/zero);
    } else if(!strcmp(line,"crashgp")) {
        asm volatile("mov %0, %%ds" :: "r"(0x1234));
    } else if(!strcmp(line,"crashint")) {
        asm volatile("int $0x50");
        printf("    int 0x50 returned\n");
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
