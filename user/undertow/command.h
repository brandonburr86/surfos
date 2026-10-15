/*
SurfOS Undertow: what a line typed at the undertow> prompt asks for
----------------------
File: command.h  Date: 10/11/26
----------------------
main.c reads a line at the prompt (edit.c) and carries out what it asks for;
this is the asking: the commands of the design's table and their aliases
(docs/superpowers/specs/2026-10-10-undertow-design.md), a link's number, and
an address typed alone, which is opened as with open. Nothing here prints,
keeps state or allocates: tests/host/test_undertow_command.c tests it alone.
*/

#ifndef UNDERTOW_COMMAND_H
#define UNDERTOW_COMMAND_H

enum ut_command_kind {
    UT_CMD_NEXT,        /* Enter alone, next */
    UT_CMD_PREV,        /* prev */
    UT_CMD_TOP,         /* top */
    UT_CMD_OPEN,        /* open ADDRESS, o ADDRESS, or an address alone ("://" in it, a dot and a
                           letter after it, or an IPv4 address with or without a port): arg */
    UT_CMD_LINK,        /* NUMBER, go NUMBER: digits and number */
    UT_CMD_BACK,        /* back, b */
    UT_CMD_FORWARD,     /* forward, f */
    UT_CMD_RELOAD,      /* reload, r */
    UT_CMD_LINKS,       /* links */
    UT_CMD_FIND,        /* find WORDS: arg */
    UT_CMD_FINDNEXT,    /* findnext */
    UT_CMD_HISTORY,     /* history */
    UT_CMD_URL,         /* url */
    UT_CMD_CLOCK,       /* clock, clock YYYY-MM-DD HH:MM:SS: arg ("" to show the time) */
    UT_CMD_HELP,        /* help, ? */
    UT_CMD_QUIT,        /* quit, q (whatever follows) */
    UT_CMD_USAGE,       /* a command used the wrong way: message is how to use it */
    UT_CMD_UNKNOWN      /* no command: message says so */
};

struct ut_command {
    enum ut_command_kind kind;
    const char *arg;            /* in the line: the address, the words, the time; "" for none */
    const char *digits;         /* UT_CMD_LINK: the number as typed */
    unsigned long number;       /* UT_CMD_LINK: its value; 0 when it has more than 9 digits (no
                                   page has that many links) */
    char message[160];          /* UT_CMD_USAGE, UT_CMD_UNKNOWN: what to say; it may hold what was
                                   typed, so it is made safe when it is shown */
};

/* What line asks for. The line is changed in place: the blanks (spaces, tabs) at both ends go,
   and the command's word is cut from the rest at its first blank, the blanks after it skipped. A
   command's name may be in any case; an address keeps its own. */
void ut_command_parse(char *line, struct ut_command *c);

#endif
