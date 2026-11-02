/*
SurfOS user program: basename
--------------------
File: basename.c  Date: 11/01/26
--------------------
basename string [suffix]: the last part of a path, without the slashes after it, and without the
suffix when one is given and is not the whole of that part. The string is only text: nothing
looks at the disk. / gives /, and the empty string an empty line, as elsewhere.
*/

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    const char *s, *part;
    size_t n, sn;
    int i = 1;
    if(i < argc && !strcmp(argv[i], "--")) i++;
    if(argc - i < 1 || argc - i > 2) {
        fprintf(stderr, "usage: basename string [suffix]\n");
        return 2;
    }
    s = argv[i];
    n = strlen(s);
    while(n > 1 && s[n - 1] == '/') n--;        /* a/b/ is b; / stays */
    part = s;
    if(n > 1 || *s != '/') {
        const char *p;
        for(p = s; p < s + n; p++)
            if(*p == '/') part = p + 1;
        n -= (size_t)(part - s);
    }
    if(argc - i == 2) {
        sn = strlen(argv[i + 1]);
        if(sn < n && !memcmp(part + n - sn, argv[i + 1], sn)) n -= sn;
    }
    printf("%.*s\n", (int)n, part);
    return 0;
}
