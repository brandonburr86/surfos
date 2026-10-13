/*
SurfOS Undertow: what a line typed at the undertow> prompt asks for
----------------------
File: command.c  Date: 10/11/26
----------------------
command.h gives the commands. A line is a word and maybe an argument; a word
of digits is a link's number, a word that names a page an address; any
other word is looked up, in lowercase, in the table below, which also says
whether the command takes an argument (open and find need one, clock may
have one, quit ignores one, the rest take none).
*/

#include <stdio.h>
#include <string.h>
#include "command.h"

#define NONE     0              /* the command takes nothing after it */
#define NEEDS    1              /* ... needs something after it */
#define MAY      2              /* ... may have something after it */
#define IGNORES  3              /* ... takes whatever follows and ignores it */

static const struct {
    const char *name;           /* as typed, in lowercase */
    const char *full;           /* its name in a usage line */
    enum ut_command_kind kind;
    int arg;
    const char *usage;          /* for NEEDS: the usage line */
} commands[] = {
    { "open", "open", UT_CMD_OPEN, NEEDS, "usage: open ADDRESS (for example open https://example.com/)" },
    { "o", "open", UT_CMD_OPEN, NEEDS, "usage: open ADDRESS (for example open https://example.com/)" },
    { "go", "go", UT_CMD_LINK, NEEDS, "usage: go NUMBER (a link's number; links lists them)" },
    { "back", "back", UT_CMD_BACK, NONE, NULL },
    { "b", "back", UT_CMD_BACK, NONE, NULL },
    { "forward", "forward", UT_CMD_FORWARD, NONE, NULL },
    { "f", "forward", UT_CMD_FORWARD, NONE, NULL },
    { "reload", "reload", UT_CMD_RELOAD, NONE, NULL },
    { "r", "reload", UT_CMD_RELOAD, NONE, NULL },
    { "next", "next", UT_CMD_NEXT, NONE, NULL },
    { "prev", "prev", UT_CMD_PREV, NONE, NULL },
    { "top", "top", UT_CMD_TOP, NONE, NULL },
    { "links", "links", UT_CMD_LINKS, NONE, NULL },
    { "find", "find", UT_CMD_FIND, NEEDS, "usage: find WORDS (findnext finds them again)" },
    { "findnext", "findnext", UT_CMD_FINDNEXT, NONE, NULL },
    { "history", "history", UT_CMD_HISTORY, NONE, NULL },
    { "url", "url", UT_CMD_URL, NONE, NULL },
    { "clock", "clock", UT_CMD_CLOCK, MAY, NULL },
    { "help", "help", UT_CMD_HELP, NONE, NULL },
    { "?", "help", UT_CMD_HELP, NONE, NULL },
    { "quit", "quit", UT_CMD_QUIT, IGNORES, NULL },
    { "q", "quit", UT_CMD_QUIT, IGNORES, NULL },
};

static int blank(char c) {
    return c == ' ' || c == '\t';
}

static int is_number(const char *s) {
    if(!*s) return 0;
    for(; *s; s++) if(*s < '0' || *s > '9') return 0;
    return 1;
}

/* a word that names a page rather than a command: "://" in it, or a dot with a letter after it */
static int is_address(const char *s) {
    const char *dot = strchr(s, '.');
    if(strstr(s, "://")) return 1;
    if(!dot || dot == s) return 0;
    for(dot++; *dot; dot++) if((*dot >= 'a' && *dot <= 'z') || (*dot >= 'A' && *dot <= 'Z')) return 1;
    return 0;
}

/* a link's number: as typed, and its value when it has at most 9 digits (else 0) */
static void link_number(struct ut_command *c, const char *digits) {
    size_t len = strlen(digits);
    c->kind = UT_CMD_LINK;
    c->digits = digits;
    c->number = 0;
    if(len <= 9)
        while(*digits) c->number = c->number * 10 + (unsigned long)(*digits++ - '0');
}

void ut_command_parse(char *line, struct ut_command *c) {
    char *word, *arg, *end;
    size_t i;
    memset(c, 0, sizeof(*c));
    c->arg = "";
    c->digits = "";
    while(blank(*line)) line++;
    for(end = line + strlen(line); end > line && blank(end[-1]); end--);
    *end = 0;
    if(!*line) {                                /* Enter alone */
        c->kind = UT_CMD_NEXT;
        return;
    }
    word = line;
    for(arg = line; *arg && !blank(*arg); arg++);
    if(*arg) {
        *arg++ = 0;
        while(blank(*arg)) arg++;
    }
    if(is_number(word)) {
        if(*arg) {
            c->kind = UT_CMD_USAGE;
            snprintf(c->message, sizeof(c->message), "undertow: a link's number alone follows it (%.20s)", word);
            return;
        }
        link_number(c, word);
        return;
    }
    if(!*arg && is_address(word)) {
        c->kind = UT_CMD_OPEN;
        c->arg = word;
        return;
    }
    for(i = 0; word[i]; i++) if(word[i] >= 'A' && word[i] <= 'Z') word[i] = (char)(word[i] - 'A' + 'a');
    for(i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
        if(strcmp(word, commands[i].name)) continue;
        c->kind = commands[i].kind;
        if(commands[i].arg == NONE && *arg) {
            c->kind = UT_CMD_USAGE;
            snprintf(c->message, sizeof(c->message), "usage: %s (it takes nothing after it)", commands[i].full);
        } else if(commands[i].arg == NEEDS && (!*arg || (c->kind == UT_CMD_LINK && !is_number(arg)))) {
            c->kind = UT_CMD_USAGE;
            snprintf(c->message, sizeof(c->message), "%s", commands[i].usage);
        } else if(c->kind == UT_CMD_LINK) {
            link_number(c, arg);
        } else if(commands[i].arg != IGNORES) {
            c->arg = arg;
        }
        return;
    }
    c->kind = UT_CMD_UNKNOWN;
    snprintf(c->message, sizeof(c->message), "undertow: there is no command %.40s%s: help lists them", word,
             strlen(word) > 40 ? "..." : "");
}
