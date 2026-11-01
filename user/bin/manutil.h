/*
SurfOS user programs: what man knows without a file system
----------------------
File: manutil.h  Date: 11/01/26
----------------------
Which page a name has, what a page's name may be, and whether a page's first line has a word.
No system call is made here (man.c makes them), so tests/host/test_manutil.c runs this as it
is, and tests/test_man.py reads the table below to see that every name has its page.

A page is rootfs/man/NAME.txt, which the initrd holds as /initrd/man/NAME.txt. The table is for
the names that share one: the test programs, groups of like commands, and the names a file could
not have ('.', ':', '['). A page of a name's own comes first, so a table entry is never in the
way of a new page.
*/

#ifndef _MANUTIL_H
#define _MANUTIL_H

#include <string.h>

#define MAN_DIR     "/initrd/man"
#define MAN_SUFFIX  ".txt"
#define MAN_NAME_MAX 64                 /* no command's name comes near this */

struct man_alias {
    const char *name, *page;
};

static const struct man_alias man_aliases[] = {
    /* surfsh's commands */
    { ".", "source" }, { ":", "true" }, { "[", "test" }, { "false", "true" },
    { "continue", "break" }, { "return", "break" },
    { "readonly", "export" }, { "bg", "jobs" }, { "fg", "jobs" }, { "wait", "jobs" },
    /* the kernel's commands */
    { "tick", "uptime" }, { "acpidump", "acpi" }, { "acpinvs", "acpi" }, { "himem", "memstat" },
    { "heaptest", "selftest" }, { "wifiinfo", "wifi" }, { "wlansim", "wifi" },
    { "pointerinfo", "touchpad" }, { "gfxtest", "gfxinfo" }, { "rawkeys", "ansi" },
    { "umount", "mount" }, { "spawn", "run" }, { "poweroff", "reboot" }, { "arp", "netstat" },
    { "beep", "demo" }, { "funky", "demo" }, { "hanoi", "demo" },
    { "die", "crash" }, { "crashdiv", "crash" }, { "crashfs", "crash" }, { "crashgp", "crash" },
    { "crashint", "crash" }, { "crashlock", "crash" }, { "crashnull", "crash" }, { "crashstack", "crash" },
    /* one tool under three names (track archive's): a page of its own, if it has one, comes first */
    { "gunzip", "gzip" }, { "zcat", "gzip" },
    /* the test programs */
    { "count", "tests" }, { "fputest", "tests" }, { "fstest", "tests" }, { "hello", "tests" },
    { "kcmdtest", "tests" }, { "memhog", "tests" }, { "nettest", "tests" }, { "pagetest", "tests" },
    { "pipetest", "tests" }, { "ptytest", "tests" }, { "toobig", "tests" }, { "wdemo", "tests" },
    { "wintest", "tests" },
};

/* the page that the table gives a name, or NULL */
static inline const char *man_alias_of(const char *name) {
    unsigned int i;
    for(i = 0; i < sizeof(man_aliases) / sizeof(man_aliases[0]); i++)
        if(!strcmp(man_aliases[i].name, name)) return man_aliases[i].page;
    return NULL;
}

/* may this be the name of a page? Letters, digits, and - _ + inside it: nothing that leads out of the manual */
static inline int man_name_ok(const char *name) {
    size_t i, n = strlen(name);
    if(!n || n > MAN_NAME_MAX) return 0;
    for(i = 0; i < n; i++) {
        char c = name[i];
        if((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) continue;
        if(i && (c == '-' || c == '_' || c == '+')) continue;
        return 0;
    }
    return 1;
}

/* does this directory entry hold a page? Its length without the suffix, or 0 */
static inline size_t man_page_file(const char *file) {
    size_t n = strlen(file), s = strlen(MAN_SUFFIX);
    return n > s && !strcmp(file + n - s, MAN_SUFFIX) ? n - s : 0;
}

static inline char man_lower(char c) {
    return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
}

/* is the word somewhere in the line? Plain text, without case in the ASCII letters; other bytes must be the same */
static inline int man_has_word(const char *line, const char *word) {
    size_t n = strlen(word);
    for(; *line; line++) {
        size_t k;
        for(k = 0; k < n && line[k] && man_lower(line[k]) == man_lower(word[k]); k++);
        if(k == n) return 1;
    }
    return !n;
}

#endif
