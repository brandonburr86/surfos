/*
SurfOS shell: surfsh
--------------------
File: parse.c   Date: 10/6/26
--------------------
A line becomes commands. Words are separated by blanks (spaces and tabs).
"double" and 'single' quotes keep blanks and the special characters in a word;
a backslash takes the next character as it is (in double quotes only before "
and \, as in sh; single quotes keep everything). Unquoted, these are special:

    < file  > file  >> file    redirection, the file name may follow without a blank
    &                          after a command: run it in the background
    ;                          between commands
    #                          at the start of a word: the rest of the line is a comment
    |                          refused: there are no pipes

There is no $ and no * : no variables and no wildcards, every word is taken as
it is typed. An empty command (";;", a leading ";") is skipped.
*/

#include "surfsh.h"

#define BLANK(c) ((c) == ' ' || (c) == '\t')
#define ENDS_WORD(c) (BLANK(c) || (c) == ';' || (c) == '&' || (c) == '|' || (c) == '<' || (c) == '>')

/* a word that parse_line would split or change: it needs quotes to stay as it is */
int needs_quotes(const char *s) {
    if(!*s) return 1;
    if(*s == '#') return 1;
    for(; *s; s++) {
        if(ENDS_WORD(*s) || *s == '"' || *s == '\'' || *s == '\\') return 1;
    }
    return 0;
}

static int error(char *err, int size, const char *msg) {
    snprintf(err, (size_t)size, "%s", msg);
    return -1;
}

/* the command being built is complete: keep it, or skip it when it is empty */
static int finish(struct parsed *p, struct command *c, char *err, int size) {
    if(!c->argc) {
        if(c->in || c->out) return error(err, size, "a redirection needs a command before it");
        if(c->bg) return error(err, size, "& needs a command before it");
        return 0;
    }
    if(p->ncmd == MAX_CMDS) return snprintf(err, (size_t)size, "too many commands on one line (at most %d)", MAX_CMDS), -1;
    c->argv[c->argc] = NULL;
    p->ncmd++;
    return 0;
}

/* where the next command is built: its place in p, or (past MAX_CMDS) extra, which finish() refuses */
static struct command *next_command(struct parsed *p, struct command *extra) {
    struct command *c = p->ncmd < MAX_CMDS ? &p->cmd[p->ncmd] : extra;
    memset(c, 0, sizeof(*c));
    return c;
}

int parse_line(const char *line, struct parsed *p, char *err, int size) {
    const char *s = line;
    char *w = p->words, *wend = p->words + sizeof(p->words) - 1;
    char **target = NULL;           /* < or > was seen: the next word is a file name */
    const char *op = "";
    struct command *c, extra;

    p->ncmd = 0;
    c = next_command(p, &extra);
    for(;;) {
        char *start;
        while(BLANK(*s)) s++;
        if(!*s || *s == '#') break;                     /* the end, or a comment */
        if(*s == ';' || *s == '&' || *s == '|') {
            if(*s == '|') return error(err, size, "there are no pipes (|); quote it to use it as text");
            if(s[0] == '&' && s[1] == '&') return error(err, size, "there is no && ; separate the commands with ;");
            if(target) return snprintf(err, (size_t)size, "a file name must follow %s", op), -1;
            if(*s == '&') c->bg = 1;
            s++;
            if(finish(p, c, err, size)) return -1;
            c = next_command(p, &extra);
            continue;
        }
        if(*s == '<' || *s == '>') {
            if(target) return snprintf(err, (size_t)size, "a file name must follow %s", op), -1;
            if(*s == '<') {
                target = &c->in;
                op = "<";
                s++;
            } else {
                target = &c->out;
                c->append = s[1] == '>';
                op = c->append ? ">>" : ">";
                s += c->append ? 2 : 1;
            }
            continue;
        }

        /* a word: up to a blank or a special character outside quotes */
        start = w;
        while(*s && !ENDS_WORD(*s)) {
            if(w >= wend) return error(err, size, "the line is too long");
            if(*s == '\\') {
                s++;
                if(*s) *w++ = *s++;
                else *w++ = '\\';                       /* at the end of the line: itself */
            } else if(*s == '\'') {
                for(s++; *s && *s != '\''; s++) {
                    if(w >= wend) return error(err, size, "the line is too long");
                    *w++ = *s;
                }
                if(!*s) return error(err, size, "a ' is not closed");
                s++;
            } else if(*s == '"') {
                for(s++; *s && *s != '"'; s++) {
                    if(w >= wend) return error(err, size, "the line is too long");
                    if(*s == '\\' && (s[1] == '"' || s[1] == '\\')) s++;
                    *w++ = *s;
                }
                if(!*s) return error(err, size, "a \" is not closed");
                s++;
            } else {
                *w++ = *s++;
            }
        }
        if(w >= wend) return error(err, size, "the line is too long");
        *w++ = 0;
        if(target) {
            *target = start;
            target = NULL;
        } else {
            if(c->argc == MAX_WORDS) return snprintf(err, (size_t)size, "too many words in a command (at most %d)", MAX_WORDS), -1;
            c->argv[c->argc++] = start;
        }
    }
    if(target) return snprintf(err, (size_t)size, "a file name must follow %s", op), -1;
    return finish(p, c, err, size);
}
