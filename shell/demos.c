/*
SurfOS ring0 Shell - the 2004 demonstrations
(C)2004 Brandon Burr

Moved out of main.c in 10/2026; the code is the original.
*/

#include <blibc_common.h>
#include <surfos/types.h>
#include <surfos/kernel.h>
#include <surfos/console.h>
#include <surfos/system.h>
#include "shell.h"

static void dohanoi(int N, int from, int to, int use) {
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

static void demoException() {
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

static void demoStrcmp() {
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

static void demoColor() {
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

void funky() {
    getFunky(10);
}
