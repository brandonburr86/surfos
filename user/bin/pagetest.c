/*
SurfOS user program: pagetest
--------------------
File: pagetest.c  Date: 10/6/26
--------------------
pagetest utf8: one line of UTF-8 samples for the console's character set (code page 437,
driver/cp437.c): SurfOS's symbols, accented letters, signs, box drawing, and one
character the console does not have, the euro sign, which it draws as '?'. The smoke
tests read the line back from the screen (tools/qemu-run.py from VGA memory,
tools/qemu-usb.py from a framebuffer screenshot).
*/

#include <stdio.h>
#include <string.h>

/* the source file is UTF-8, and so is what is printed */
#define SAMPLE "UTF8: SURF ▸ ≋ … café ÄÖÜ ñ ½ ° ± ┌─┐ € END"

int main(int argc, char **argv) {
    if(argc == 2 && !strcmp(argv[1], "utf8")) {
        printf("%s\n", SAMPLE);
        return 0;
    }
    fprintf(stderr, "usage: pagetest utf8\n");
    return 2;
}
