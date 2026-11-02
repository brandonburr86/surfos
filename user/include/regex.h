/*
SurfOS user library: regular expressions
----------------------
File: regex.h   Date: 11/01/26
----------------------
POSIX regcomp(), regexec(), regerror() and regfree(), in user/libsurf/regex.c; the page
in the manual is man regex. Basic and extended syntax, the longest of the leftmost
matches. A character is a UTF-8 sequence (a byte that is not in one counts as one).

SurfOS adds:
  REG_STARTEND  regexec() looks at string[pmatch[0].rm_so .. pmatch[0].rm_eo), which may
                hold NUL bytes; what comes before it still counts for ^ and \<, and the
                offsets it reports are from string, not from rm_so
  REG_ESIZE     regcomp(): the pattern is too big, or nested too deeply
  REG_ESTEPS    regexec(): a pattern with a back-reference took too many steps on this
                text. Only such a pattern can; without one the time is bounded by
                the states of the program, at most 65000, plus what its subexpressions
                add, at most 260000, per character of text.
*/

#ifndef _REGEX_H
#define _REGEX_H

#include <stddef.h>

#define RE_DUP_MAX 32767                /* the largest count in an interval */

typedef int regoff_t;

typedef struct {
    size_t re_nsub;                     /* the parenthesized subexpressions */
    void *re_g;                         /* the compiled pattern */
} regex_t;

typedef struct {
    regoff_t rm_so;                     /* start, -1 for what took no part in the match */
    regoff_t rm_eo;                     /* end, one past the last byte */
} regmatch_t;

/* regcomp()'s flags */
#define REG_EXTENDED 1                  /* extended syntax */
#define REG_ICASE    2                  /* ignore the case of A-Z */
#define REG_NOSUB    4                  /* only whether it matches */
#define REG_NEWLINE  8                  /* a newline ends lines: . and [^x] do not take it, ^ and $ match beside it */

/* regexec()'s */
#define REG_NOTBOL   1                  /* the string does not start a line */
#define REG_NOTEOL   2                  /* it does not end one */
#define REG_STARTEND 4                  /* above */

/* the errors, 0 for success */
#define REG_NOMATCH  1
#define REG_BADPAT   2
#define REG_ECOLLATE 3
#define REG_ECTYPE   4
#define REG_EESCAPE  5
#define REG_ESUBREG  6
#define REG_EBRACK   7
#define REG_EPAREN   8
#define REG_EBRACE   9
#define REG_BADBR    10
#define REG_ERANGE   11
#define REG_ESPACE   12
#define REG_BADRPT   13
#define REG_ESIZE    14
#define REG_ESTEPS   15

int regcomp(regex_t *preg, const char *pattern, int cflags);
int regexec(const regex_t *preg, const char *string, size_t nmatch, regmatch_t pmatch[], int eflags);
size_t regerror(int err, const regex_t *preg, char *buf, size_t size);
void regfree(regex_t *preg);

#endif
