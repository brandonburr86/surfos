/*
SurfOS user library: options
--------------------
File: getopt.c  Date: 11/01/26
--------------------
POSIX getopt(): the options end at the first word that is not one, or after --, and nothing
is moved around. "a:b" in optstring: -a takes an argument, in the same word or the next.
A leading : asks for silence, and for ':' instead of '?' when an argument is missing; a
leading + is what happens anyway, and is skipped, for programs written with GNU's in mind.
Messages go on standard error as tool: what: why, the tool being the last part of argv[0].
A caller that wants to scan again sets optind to 0, as with glibc. 1 does the same, but for a
scan that stopped inside a word of letters such as -ab at argv[1]: there it is still where it was,
and getopt() goes on with b.
*/

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

char *optarg;
int optind = 1, opterr = 1, optopt;

static const char *current;              /* the word whose letters are being read */
static int place;                       /* the next of them */

static void say(char *const argv[], const char *optstring, int c, const char *why) {
    const char *name = strrchr(argv[0], '/');
    if(opterr && optstring[0] != ':') dprintf(STDERR_FILENO, "%s: -%c: %s\n", name ? name + 1 : argv[0], c, why);
}

/* on to the next word */
static void word_done(void) {
    optind++;
    current = NULL;
}

int getopt(int argc, char *const argv[], const char *optstring) {
    const char *word, *spec;
    int c;
    if(!optind) {
        optind = 1;
        current = NULL;
    }
    optarg = NULL;
    if(optstring[0] == '+') optstring++;
    if(optind >= argc || !argv[optind]) return -1;
    word = argv[optind];
    if(word != current) {               /* a new word, or a caller that moved optind */
        if(word[0] != '-' || !word[1]) return -1;
        if(word[1] == '-' && !word[2]) {
            optind++;
            return -1;
        }
        current = word;
        place = 1;
    }
    c = (unsigned char)word[place++];
    spec = c == ':' ? NULL : strchr(optstring, c);
    if(!spec) {
        optopt = c;
        say(argv, optstring, c, "unknown option");
        if(!word[place]) word_done();
        return '?';
    }
    if(spec[1] != ':') {
        if(!word[place]) word_done();
        return c;
    }
    if(word[place]) optarg = (char *)word + place;
    else if(optind + 1 < argc) optarg = argv[++optind];
    word_done();
    if(optarg) return c;
    optopt = c;
    say(argv, optstring, c, "needs an argument");
    return optstring[0] == ':' ? ':' : '?';
}
