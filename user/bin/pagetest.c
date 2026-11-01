/*
SurfOS user program: pagetest
--------------------
File: pagetest.c  Date: 10/6/26
--------------------
Tests of the console and the terminal (include/surfos/tty.h, driver/cp437.c), for the
smoke tests (tools/qemu-run.py, tools/qemu-usb.py) and for trying them by hand.

pagetest N      turns the pager on (ttyctl TTY_PAGE 1), prints N numbered lines
                ("pagetest line 001", ...) and then "PAGETEST DONE". After a screenful
                the console asks --More--: Space shows the next screenful, Enter one
                more line, q the rest is dropped. Paging ends with the program.
pagetest ask N  the same lines, then a question ("pagetest question: ") and the line
                typed as the answer ("pagetest answer: ..."): after q the question
                still shows, when the program reads.
pagetest crash N  the same lines, then a write through a NULL pointer: after q the
                kernel's report of the crash still shows.
pagetest hold MS  turns the pager on, waits MS milliseconds and says "PAGETEST HELD":
                a program in the background that holds the pager.
pagetest utf8   one line of UTF-8 samples: SurfOS's symbols, accented letters, signs,
                box drawing, and the euro sign, which code page 437 lacks and the
                console draws as '?'. The tests read the line back from the screen.
pagetest caps   what TTY_CAPS says: UTF-8 and colour, or colour alone when the kernel
                command line has the word `ascii`; 10/25/26: and the alternate screen.
pagetest size   what TTY_GETSIZE says: "TTY_GETSIZE 80x25" (10/6/26: a pseudo-terminal
                answers the size PTY_SETSIZE gave it; ptytest size).
pagetest alt    10/25/26 (track A, A8): the alternate screen (ESC[?1049h) with "PAGETEST
                ALT-SCREEN" at its top left and a question, then ESC[?1049l and the line
                typed as the answer ("PAGETEST MAIN SCREEN: ...") where the cursor was on the
                main screen. Ctrl+C at the question ends the program on the alternate
                screen: the console gives the main screen back by itself.
*/

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <surf.h>

/* the source file is UTF-8, and so is what is printed */
#define SAMPLE "UTF8: SURF ▸ ≋ … café ÄÖÜ ñ ½ ° ± ┌─┐ € END"

static void lines(int n) {
    int i;
    if(ttyctl(TTY_PAGE, 1) < 0) perror("pagetest: TTY_PAGE");
    for(i = 1; i <= n; i++) printf("pagetest line %03d\n", i);
}

int main(int argc, char **argv) {
    char answer[64];
    volatile int *nowhere = NULL;
    int caps, n;
    if(argc == 2 && !strcmp(argv[1], "utf8")) {
        printf("%s\n", SAMPLE);
        return 0;
    }
    if(argc == 2 && !strcmp(argv[1], "caps")) {
        caps = ttyctl(TTY_CAPS, 0);
        if(caps < 0) {
            perror("pagetest: TTY_CAPS");
            return 1;
        }
        printf("TTY_CAPS %d:%s%s%s\n", caps, (caps & TTY_CAP_UTF8) ? " UTF-8" : "", (caps & TTY_CAP_COLOR) ? " colour" : "",
               (caps & TTY_CAP_ALTSCREEN) ? " alternate-screen" : "");
        return 0;
    }
    if(argc == 2 && !strcmp(argv[1], "size")) {
        n = ttyctl(TTY_GETSIZE, 0);
        if(n < 0) {
            perror("pagetest: TTY_GETSIZE");
            return 1;
        }
        printf("TTY_GETSIZE %dx%d\n", (n >> 16) & 0xffff, n & 0xffff);
        return 0;
    }
    if(argc == 2 && !strcmp(argv[1], "alt")) {
        printf("\033[?1049h\033[HPAGETEST ALT-SCREEN\nalternate question: ");
        fflush(stdout);
        if(!fgets(answer, sizeof(answer), stdin)) answer[0] = 0;
        answer[strcspn(answer, "\r\n")] = 0;
        printf("\033[?1049lPAGETEST MAIN SCREEN: %s\n", answer);
        return 0;
    }
    if(argc == 3 && !strcmp(argv[1], "ask")) {
        lines(atoi(argv[2]));
        printf("pagetest question: ");
        fflush(stdout);
        if(!fgets(answer, sizeof(answer), stdin)) answer[0] = 0;
        answer[strcspn(answer, "\r\n")] = 0;
        printf("pagetest answer: %s\nPAGETEST DONE\n", answer);
        return 0;
    }
    if(argc == 3 && !strcmp(argv[1], "crash")) {
        lines(atoi(argv[2]));
        fflush(stdout);
        *nowhere = 1;
        return 0;
    }
    if(argc == 3 && !strcmp(argv[1], "hold")) {
        /* 11/1/26: a job in the background can get here while its shell still holds the pager for the line that
           started it, which is EBUSY (tty.h), and its message came below the shell's next prompt: the shell lets
           go in a moment */
        int r, tries = 200;
        while((r = ttyctl(TTY_PAGE, 1)) < 0 && errno == EBUSY && --tries) msleep(10);
        if(r < 0) perror("pagetest: TTY_PAGE");
        msleep((unsigned int)atoi(argv[2]));
        printf("PAGETEST HELD\n");
        return 0;
    }
    n = argc == 2 ? atoi(argv[1]) : 0;
    if(n > 0) {
        lines(n);
        printf("PAGETEST DONE\n");
        return 0;
    }
    fprintf(stderr, "usage: pagetest <lines> | ask <lines> | crash <lines> | hold <ms> | utf8 | caps | size | alt\n");
    return 2;
}
