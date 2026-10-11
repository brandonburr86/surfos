/*
SurfOS Undertow: the text web browser
----------------------
File: main.c  Date: 10/11/26
----------------------
A stand-in with the usage only, until track S's session, screen and commands
land (docs/superpowers/plans/2026-10-11-undertow-tracks.md).
*/

#include <stdio.h>
#include <string.h>
#include <surf.h>
#include "undertow.h"

static void usage(FILE *f) {
    fprintf(f, "usage: undertow [ADDRESS]\n"
               "       undertow --dump ADDRESS\n"
               "       undertow --help\n");
}

int main(int argc, char **argv) {
    if(argc == 2 && !strcmp(argv[1], "--help")) {
        usage(stdout);
        return 0;
    }
    usage(stderr);
    fprintf(stderr, "undertow: this build of Undertow cannot browse yet\n");
    return 2;
}
