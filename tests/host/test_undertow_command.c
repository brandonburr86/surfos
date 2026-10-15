/*
SurfOS Undertow: tests of what a line typed at the prompt asks for
----------------------
File: test_undertow_command.c  Date: 10/11/26
----------------------
user/undertow/command.c: every command of the design's table and its aliases,
a link's number (go NUMBER too), an address typed alone, the blanks around
and inside a line, commands used the wrong way and words that are no command.
10/15/26 (review M3): an IPv4 address alone, with a port, a path, a query or a fragment.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "command.h"

static int checks, failures;

static void check(int ok, const char *what) {
    checks++;
    if(!ok) { failures++; printf("FAIL: %s\n", what); }
}

static const char *const names[] = {
    "NEXT", "PREV", "TOP", "OPEN", "LINK", "BACK", "FORWARD", "RELOAD", "LINKS", "FIND", "FINDNEXT", "HISTORY",
    "URL", "CLOCK", "HELP", "QUIT", "USAGE", "UNKNOWN"
};

struct want {
    const char *line;
    enum ut_command_kind kind;
    const char *arg;            /* NULL: not checked */
    unsigned long number;       /* UT_CMD_LINK */
    const char *message;        /* UT_CMD_USAGE, UT_CMD_UNKNOWN: in the message */
};

static const struct want table[] = {
    /* Enter alone and next */
    { "", UT_CMD_NEXT, NULL, 0, NULL },
    { "   \t ", UT_CMD_NEXT, NULL, 0, NULL },
    { "next", UT_CMD_NEXT, NULL, 0, NULL },
    { "  NEXT  ", UT_CMD_NEXT, NULL, 0, NULL },
    /* the aliases */
    { "prev", UT_CMD_PREV, NULL, 0, NULL },
    { "top", UT_CMD_TOP, NULL, 0, NULL },
    { "b", UT_CMD_BACK, NULL, 0, NULL },
    { "back", UT_CMD_BACK, NULL, 0, NULL },
    { "Back", UT_CMD_BACK, NULL, 0, NULL },
    { "f", UT_CMD_FORWARD, NULL, 0, NULL },
    { "forward", UT_CMD_FORWARD, NULL, 0, NULL },
    { "r", UT_CMD_RELOAD, NULL, 0, NULL },
    { "reload", UT_CMD_RELOAD, NULL, 0, NULL },
    { "links", UT_CMD_LINKS, NULL, 0, NULL },
    { "findnext", UT_CMD_FINDNEXT, NULL, 0, NULL },
    { "history", UT_CMD_HISTORY, NULL, 0, NULL },
    { "url", UT_CMD_URL, NULL, 0, NULL },
    { "help", UT_CMD_HELP, NULL, 0, NULL },
    { "?", UT_CMD_HELP, NULL, 0, NULL },
    { "q", UT_CMD_QUIT, NULL, 0, NULL },
    { "quit", UT_CMD_QUIT, NULL, 0, NULL },
    { "QUIT", UT_CMD_QUIT, NULL, 0, NULL },
    { "quit now", UT_CMD_QUIT, NULL, 0, NULL },             /* quit takes what it is given: it quits */
    /* open and an address typed alone */
    { "o example.com", UT_CMD_OPEN, "example.com", 0, NULL },
    { "open   https://a.test/x  ", UT_CMD_OPEN, "https://a.test/x", 0, NULL },
    { "OPEN\texample.org/news", UT_CMD_OPEN, "example.org/news", 0, NULL },
    { "open a b", UT_CMD_OPEN, "a b", 0, NULL },             /* url.c says what is wrong with it */
    { "open", UT_CMD_USAGE, NULL, 0, "usage: open ADDRESS" },
    { "o   ", UT_CMD_USAGE, NULL, 0, "usage: open ADDRESS" },
    { "example.com", UT_CMD_OPEN, "example.com", 0, NULL },
    { "  Example.COM/a  ", UT_CMD_OPEN, "Example.COM/a", 0, NULL },
    { "https://x.test/", UT_CMD_OPEN, "https://x.test/", 0, NULL },
    { "http://10.0.2.2:8080/", UT_CMD_OPEN, "http://10.0.2.2:8080/", 0, NULL },
    { "1.5", UT_CMD_UNKNOWN, NULL, 0, "there is no command 1.5" },
    { "b.", UT_CMD_UNKNOWN, NULL, 0, "there is no command b." },
    { ".com", UT_CMD_UNKNOWN, NULL, 0, "there is no command .com" },
    { "example.com now", UT_CMD_UNKNOWN, NULL, 0, "there is no command example.com" },
    /* 10/15/26 (review M3): an IPv4 address alone, with or without a port, a path, a query or a fragment */
    { "10.0.2.2", UT_CMD_OPEN, "10.0.2.2", 0, NULL },
    { "  10.0.2.2:8080 ", UT_CMD_OPEN, "10.0.2.2:8080", 0, NULL },
    { "192.168.1.1:8080", UT_CMD_OPEN, "192.168.1.1:8080", 0, NULL },
    { "10.0.2.2/", UT_CMD_OPEN, "10.0.2.2/", 0, NULL },
    { "10.0.2.2:80/a?b", UT_CMD_OPEN, "10.0.2.2:80/a?b", 0, NULL },
    { "10.0.2.2?x=1", UT_CMD_OPEN, "10.0.2.2?x=1", 0, NULL },
    { "10.0.2.2#top", UT_CMD_OPEN, "10.0.2.2#top", 0, NULL },
    { "999.1.1.1", UT_CMD_OPEN, "999.1.1.1", 0, NULL },        /* the transport says it is no address */
    { "10.0.2.2 now", UT_CMD_UNKNOWN, NULL, 0, "there is no command 10.0.2.2" },
    { "1.2.3", UT_CMD_UNKNOWN, NULL, 0, "there is no command 1.2.3" },
    { "1.2.3.4.5", UT_CMD_UNKNOWN, NULL, 0, "there is no command 1.2.3.4.5" },
    { "1.2.3.", UT_CMD_UNKNOWN, NULL, 0, "there is no command 1.2.3." },
    { "1..2.3", UT_CMD_UNKNOWN, NULL, 0, "there is no command 1..2.3" },
    { "1234.1.1.1", UT_CMD_UNKNOWN, NULL, 0, "there is no command 1234.1.1.1" },
    { "10.0.2.2:", UT_CMD_UNKNOWN, NULL, 0, "there is no command 10.0.2.2:" },
    { "10.0.2.2:123456", UT_CMD_UNKNOWN, NULL, 0, "there is no command 10.0.2.2:123456" },
    { "10.0.2.2:80:80", UT_CMD_UNKNOWN, NULL, 0, "there is no command 10.0.2.2:80:80" },
    { "10.0.2.2-1", UT_CMD_UNKNOWN, NULL, 0, "there is no command 10.0.2.2-1" },
    { "-1.2.3.4", UT_CMD_UNKNOWN, NULL, 0, "there is no command -1.2.3.4" },
    /* a link's number */
    { "12", UT_CMD_LINK, NULL, 12, NULL },
    { "007", UT_CMD_LINK, NULL, 7, NULL },
    { " 5 ", UT_CMD_LINK, NULL, 5, NULL },
    { "999999999", UT_CMD_LINK, NULL, 999999999, NULL },
    { "1234567890", UT_CMD_LINK, NULL, 0, NULL },             /* more than 9 digits: no such link */
    { "99999999999999999999999", UT_CMD_LINK, NULL, 0, NULL },
    { "go 3", UT_CMD_LINK, NULL, 3, NULL },
    { "GO  42 ", UT_CMD_LINK, NULL, 42, NULL },
    { "go", UT_CMD_USAGE, NULL, 0, "usage: go NUMBER" },
    { "go x", UT_CMD_USAGE, NULL, 0, "usage: go NUMBER" },
    { "go 3 4", UT_CMD_USAGE, NULL, 0, "usage: go NUMBER" },
    { "go -3", UT_CMD_USAGE, NULL, 0, "usage: go NUMBER" },
    { "5 x", UT_CMD_USAGE, NULL, 0, "a link's number alone follows it (5)" },
    /* commands that take nothing */
    { "back 2", UT_CMD_USAGE, NULL, 0, "usage: back (it takes nothing after it)" },
    { "f x", UT_CMD_USAGE, NULL, 0, "usage: forward (it takes nothing after it)" },
    { "reload now", UT_CMD_USAGE, NULL, 0, "usage: reload" },
    { "next 2", UT_CMD_USAGE, NULL, 0, "usage: next" },
    { "prev 2", UT_CMD_USAGE, NULL, 0, "usage: prev" },
    { "top x", UT_CMD_USAGE, NULL, 0, "usage: top" },
    { "links all", UT_CMD_USAGE, NULL, 0, "usage: links" },
    { "findnext x", UT_CMD_USAGE, NULL, 0, "usage: findnext" },
    { "history x", UT_CMD_USAGE, NULL, 0, "usage: history" },
    { "url x", UT_CMD_USAGE, NULL, 0, "usage: url" },
    { "help me", UT_CMD_USAGE, NULL, 0, "usage: help" },
    { "? x", UT_CMD_USAGE, NULL, 0, "usage: help" },
    /* find and clock */
    { "find foo bar", UT_CMD_FIND, "foo bar", 0, NULL },
    { "find   x  ", UT_CMD_FIND, "x", 0, NULL },
    { "FIND Caf\xc3\xa9", UT_CMD_FIND, "Caf\xc3\xa9", 0, NULL },
    { "find", UT_CMD_USAGE, NULL, 0, "usage: find WORDS" },
    { "clock", UT_CMD_CLOCK, "", 0, NULL },
    { "clock 2026-10-11 12:00:00", UT_CMD_CLOCK, "2026-10-11 12:00:00", 0, NULL },
    { "clock\t 2026-10-11 12:00:00 ", UT_CMD_CLOCK, "2026-10-11 12:00:00", 0, NULL },
    /* no command */
    { "frobnicate now", UT_CMD_UNKNOWN, NULL, 0, "there is no command frobnicate: help lists them" },
    { "nexxt", UT_CMD_UNKNOWN, NULL, 0, "there is no command nexxt" },
    { "\033[2J", UT_CMD_UNKNOWN, NULL, 0, "there is no command" },
};

static void cases(void) {
    struct ut_command c;
    char line[256], what[400];
    size_t i;
    for(i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        const struct want *w = &table[i];
        int ok;
        strcpy(line, w->line);
        memset(&c, 0x55, sizeof(c));
        ut_command_parse(line, &c);
        ok = c.kind == w->kind;
        if(ok && w->arg) ok = c.arg && !strcmp(c.arg, w->arg);
        if(ok && w->kind == UT_CMD_LINK) ok = c.number == w->number && c.digits && c.digits[0];
        if(ok && w->message) ok = strstr(c.message, w->message) != NULL;
        if(ok && (w->kind == UT_CMD_USAGE || w->kind == UT_CMD_UNKNOWN)) ok = strlen(c.message) < sizeof(c.message);
        if(!ok) printf("  \"%s\": %s, arg \"%s\", number %lu, message \"%.80s\"\n", w->line,
                       c.kind <= UT_CMD_UNKNOWN ? names[c.kind] : "?", c.kind == UT_CMD_OPEN || c.kind == UT_CMD_FIND ||
                       c.kind == UT_CMD_CLOCK ? c.arg : "", c.kind == UT_CMD_LINK ? c.number : 0,
                       c.kind >= UT_CMD_USAGE ? c.message : "");
        snprintf(what, sizeof(what), "\"%s\" is %s", w->line, names[w->kind]);
        check(ok, what);
    }
}

static void limits(void) {
    struct ut_command c;
    char *line = malloc(5000);
    memset(line, 'x', 4096);
    line[4096] = 0;
    ut_command_parse(line, &c);
    check(c.kind == UT_CMD_UNKNOWN && strlen(c.message) < sizeof(c.message) && strstr(c.message, "...: help lists them"),
          "a 4096-byte word: no command, its name cut short in the message");
    strcpy(line, "go ");
    memset(line + 3, '7', 30);
    line[33] = 0;
    ut_command_parse(line, &c);
    check(c.kind == UT_CMD_LINK && c.number == 0 && strlen(c.digits) == 30, "go and 30 digits: no such link, the digits kept");
    strcpy(line, "1234567890");
    ut_command_parse(line, &c);
    check(c.kind == UT_CMD_LINK && c.number == 0 && !strcmp(c.digits, "1234567890"), "10 digits: the number as typed");
    free(line);
}

int main(void) {
    cases();
    limits();
    printf("UNDERTOW COMMAND TESTS %s: %d checks, %d failures\n", failures ? "FAILED" : "PASSED", checks, failures);
    return failures ? 1 : 0;
}
