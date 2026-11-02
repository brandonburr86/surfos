/*
SurfOS user program: dirname
--------------------
File: dirname.c  Date: 11/01/26
--------------------
dirname string...: each path without its last part, a line each: a/b is a, a name alone is ., and
/ and /a are /. The strings are only text: nothing looks at the disk.
*/

#include <stdio.h>
#include <string.h>

static void one(const char *s) {
    size_t n = strlen(s);
    while(n > 1 && s[n - 1] == '/') n--;        /* the slashes after the last part */
    while(n && s[n - 1] != '/') n--;            /* the last part */
    if(!n) {
        puts(*s == '/' ? "/" : ".");
        return;
    }
    while(n > 1 && s[n - 1] == '/') n--;        /* the slashes before it */
    printf("%.*s\n", (int)n, s);
}

int main(int argc, char **argv) {
    int i = 1;
    if(i < argc && !strcmp(argv[i], "--")) i++;
    if(i >= argc) {
        fprintf(stderr, "usage: dirname string...\n");
        return 2;
    }
    for(; i < argc; i++) one(argv[i]);
    return 0;
}
