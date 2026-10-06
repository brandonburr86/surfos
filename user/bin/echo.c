/*
SurfOS user program: echo
--------------------
File: echo.c    Date: 10/5/26
--------------------
echo [-n] [-e] [word...]: the words with single spaces between them and a
newline (none with -n); -e turns \n \r \t and \\ into the characters they
name. sh has the same as a built-in command.
*/

#include <stdio.h>
#include <string.h>

static void put_escaped(const char *s) {
    for(; *s; s++) {
        if(*s != '\\' || !s[1]) { putchar(*s); continue; }
        switch(*++s) {
        case 'n': putchar('\n'); break;
        case 'r': putchar('\r'); break;
        case 't': putchar('\t'); break;
        case '\\': putchar('\\'); break;
        default: putchar('\\'); putchar(*s); break;
        }
    }
}

int main(int argc, char **argv) {
    int i = 1, newline = 1, escapes = 0;
    for(; i < argc && argv[i][0] == '-' && (!strcmp(argv[i], "-n") || !strcmp(argv[i], "-e")); i++) {
        if(argv[i][1] == 'n') newline = 0;
        else escapes = 1;
    }
    for(; i < argc; i++) {
        if(escapes) put_escaped(argv[i]);
        else fputs(argv[i], stdout);
        if(i < argc - 1) putchar(' ');
    }
    if(newline) putchar('\n');
    return fflush(stdout) ? 1 : 0;
}
