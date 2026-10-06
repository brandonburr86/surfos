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
                command line has the word `ascii`.
*/

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
        printf("TTY_CAPS %d:%s%s\n", caps, (caps & TTY_CAP_UTF8) ? " UTF-8" : "", (caps & TTY_CAP_COLOR) ? " colour" : "");
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
        if(ttyctl(TTY_PAGE, 1) < 0) perror("pagetest: TTY_PAGE");
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
    fprintf(stderr, "usage: pagetest <lines> | ask <lines> | crash <lines> | hold <ms> | utf8 | caps\n");
    return 2;
}
