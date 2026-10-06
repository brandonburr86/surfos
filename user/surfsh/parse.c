/*
SurfOS shell: surfsh
--------------------
File: parse.c   Date: 10/6/26
--------------------
A line becomes commands. Words are separated by blanks (spaces and tabs).
"double" and 'single' quotes keep blanks and the special characters in a word;
a backslash takes the next character as it is (in double quotes only before ",
\ and $, as in sh; single quotes keep everything). Unquoted, these are special:

    < file  > file  >> file    redirection, the file name may follow without a blank
    &                          after a command: run it in the background
    ;                          between commands
    #                          at the start of a word: the rest of the line is a comment
    |                          refused: there are no pipes

$? is the one expansion: the status of the command before, in decimal, unquoted
or in double quotes ('$?' and \$? are the two characters). There are no variables
and no wildcards; any other $ and every * are themselves. The whole line is read
first (parse_check), so a mistake anywhere in it stops all of it; then each
command is read again just before it runs (parse_next), so in "ls x; echo $?" the
echo sees the status of ls. An empty command (";;", a leading ";") is skipped.
*/

#include "surfsh.h"

#define BLANK(c) ((c) == ' ' || (c) == '\t')
#define ENDS_WORD(c) (BLANK(c) || (c) == ';' || (c) == '&' || (c) == '|' || (c) == '<' || (c) == '>')
#define STATUS_AT(s) ((s)[0] == '$' && (s)[1] == '?')

/* a word that the parser would split or change: it needs quotes to stay as it is */
int needs_quotes(const char *s) {
    if(!*s || *s == '#') return 1;
    for(; *s; s++) {
        if(ENDS_WORD(*s) || *s == '"' || *s == '\'' || *s == '\\' || *s == '$') return 1;
    }
    return 0;
}

static int error(char *err, int size, const char *msg) {
    snprintf(err, (size_t)size, "%s", msg);
    return -1;
}

/* one byte more of the command's words, while there is room */
#define PUT(ch) do {                                                    \
        if(w >= wend) return error(err, size, "the line is too long");  \
        *w++ = (ch);                                                    \
    } while(0)

int parse_next(const char **line, int status, struct command *c, char *err, int size) {
    const char *s = *line, *op = "";
    char *w = c->words, *wend = c->words + sizeof(c->words) - 1, num[16];
    char **target = NULL;           /* < or > was seen: the next word is a file name */
    int numlen = snprintf(num, sizeof(num), "%d", status), i;

    c->argc = 0;
    c->in = c->out = NULL;
    c->append = c->bg = 0;
    for(;;) {
        char *start;
        while(BLANK(*s)) s++;
        if(!*s || *s == '#' || *s == ';' || *s == '&' || *s == '|') {   /* the command ends */
            int end = !*s || *s == '#';
            if(*s == '|') return error(err, size, "there are no pipes (|); quote it to use it as text");
            if(s[0] == '&' && s[1] == '&') return error(err, size, "there is no && ; separate the commands with ;");
            if(target) return snprintf(err, (size_t)size, "a file name must follow %s", op), -1;
            if(*s == '&') c->bg = 1;
            if(!c->argc) {
                if(c->in || c->out) return error(err, size, "a redirection needs a command before it");
                if(c->bg) return error(err, size, "& needs a command before it");
                if(end) {
                    *line = s;
                    return 0;
                }
                s++;                                /* an empty command: skipped */
                continue;
            }
            if(!end) s++;
            c->argv[c->argc] = NULL;
            *line = s;
            return 1;
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
            if(*s == '\\') {
                s++;
                if(*s) PUT(*s++);
                else PUT('\\');                     /* at the end of the line: itself */
            } else if(*s == '\'') {
                for(s++; *s && *s != '\''; s++) PUT(*s);
                if(!*s) return error(err, size, "a ' is not closed");
                s++;
            } else if(*s == '"') {
                for(s++; *s && *s != '"'; s++) {
                    if(*s == '\\' && (s[1] == '"' || s[1] == '\\' || s[1] == '$')) PUT(*++s);
                    else if(STATUS_AT(s)) {
                        for(i = 0; i < numlen; i++) PUT(num[i]);
                        s++;
                    } else PUT(*s);
                }
                if(!*s) return error(err, size, "a \" is not closed");
                s++;
            } else if(STATUS_AT(s)) {
                for(i = 0; i < numlen; i++) PUT(num[i]);
                s += 2;
            } else {
                PUT(*s++);
            }
        }
        PUT(0);
        if(target) {
            *target = start;
            target = NULL;
        } else {
            if(c->argc == MAX_WORDS) return snprintf(err, (size_t)size, "too many words in a command (at most %d)", MAX_WORDS), -1;
            c->argv[c->argc++] = start;
        }
    }
}

int parse_check(const char *line, char *err, int size) {
    static struct command c;
    int r;
    while((r = parse_next(&line, 0, &c, err, size)) > 0);
    return r;
}
