/*
SurfOS user programs: names from archives
--------------------
File: arpath.c  Date: 11/01/26
--------------------
What arpath.h says. ar_safe_path() takes both / and \ as separators when it looks for "..", though
only / is one here: a \ is refused in any case, but a name from Windows that climbs out with
..\..\ should say so. The kernel checks the same things where it can (fs/fat.c's validate_name(), a
".." by vfs_normalize()), but that would be too late for the message, and on another file system
may not hold; here they hold before any system call has seen the name.
*/

#include <string.h>
#include <stdio.h>
#include "arpath.h"

#define NAME_LIMIT 255                  /* one component (a FAT long name) */
#define PATH_LIMIT 256                  /* a path with its terminator: limits.h's PATH_MAX */

static int lower(int c) {
    return c >= 'A' && c <= 'Z' ? c + 'a' - 'A' : c;
}

/* The length of the well-formed UTF-8 sequence at s, 0 for a byte that does not start one: the rules of fs/fat.c's
   utf8_next(): no stray continuation byte, no overlong form, no surrogate, nothing past U+10FFFF. */
static int utf8_len(const unsigned char *s) {
    unsigned int c = s[0], cp, need, i, least;
    if(c < 0x80) return 1;
    if(c >= 0xC2 && c <= 0xDF) { need = 1; cp = c & 0x1F; least = 0x80; }
    else if(c >= 0xE0 && c <= 0xEF) { need = 2; cp = c & 0x0F; least = 0x800; }
    else if(c >= 0xF0 && c <= 0xF4) { need = 3; cp = c & 0x07; least = 0x10000; }
    else return 0;
    for(i = 1; i <= need; i++) {
        if((s[i] & 0xC0) != 0x80) return 0;
        cp = (cp << 6) | (s[i] & 0x3Fu);
    }
    if(cp < least || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
    return (int)need + 1;
}

int ar_utf8_valid(const char *s, size_t n) {
    char copy[8];
    size_t i = 0;
    while(i < n) {
        size_t k = n - i < sizeof(copy) - 1 ? n - i : sizeof(copy) - 1;
        int len;
        memcpy(copy, s + i, k);                         /* utf8_len() may look past n: never past this */
        copy[k] = 0;
        len = utf8_len((const unsigned char *)copy);
        if(!len) return 0;
        i += (size_t)len;
    }
    return 1;
}

const char *ar_safe_path(const char *name, char *out, size_t size) {
    const char *p = name, *start;
    size_t n = 0;
    if(*p == '/' || *p == '\\') return "the name is absolute";
    for(start = p;; p++) {
        if(!*p || *p == '/' || *p == '\\') {
            if(p - start == 2 && start[0] == '.' && start[1] == '.') return "the name leaves the folder";
            if(!*p) break;
            start = p + 1;
        }
    }
    for(p = name; *p;) {
        int k = utf8_len((const unsigned char *)p);
        unsigned char c = (unsigned char)*p;
        if(!k) return "the name is not valid UTF-8";
        if(c < 0x20 || c == 0x7f || strchr(":*?\"<>|\\", c)) return "the name has a character FAT cannot hold";
        p += k;
    }
    for(p = name; *p;) {
        size_t len;
        start = p;
        while(*p && *p != '/') p++;
        len = (size_t)(p - start);
        if(*p) p++;
        if(!len || (len == 1 && start[0] == '.')) continue;
        if(len > NAME_LIMIT) return "the name is too long";
        if(start[len - 1] == ' ' || start[len - 1] == '.') return "a name that ends in a space or a dot cannot be held on FAT";
        if(n + !!n + len >= size || n + !!n + len >= PATH_LIMIT) return "the name is too long";
        if(n) out[n++] = '/';
        memcpy(out + n, start, len);
        n += len;
    }
    out[n] = 0;
    return NULL;
}

void ar_show_name(const char *name, char *out, size_t size) {
    const unsigned char *p = (const unsigned char *)name;
    size_t n = 0;
    if(size < 4) {
        if(size) out[0] = 0;
        return;
    }
    while(*p) {
        int k = utf8_len(p);
        int control = !k || *p < 0x20 || *p == 0x7f || (*p == 0xC2 && p[1] < 0xA0);     /* C1, too: 0x9b starts an escape */
        size_t need = control ? 4 : (size_t)k;
        if(n + need + 4 > size) {                       /* room for what is cut to say so */
            strcpy(out + n, "...");
            return;
        }
        if(control) {
            snprintf(out + n, 5, "\\%03o", *p);
            n += 4;
            p++;
        } else {
            memcpy(out + n, p, (size_t)k);
            n += (size_t)k;
            p += k;
        }
    }
    out[n] = 0;
}

/* The next character after s, UTF-8 as one, without leaving the string */
static const char *next_char(const char *s) {
    s++;
    while((*s & 0xC0) == 0x80) s++;
    return s;
}

/* the matching of both: at a / with part set, the pattern may end there. Never recursive, and each star only moves
   the last one's mark: time bounded by the two lengths' product */
static int glob(const char *p, const char *s, int part) {
    const char *star = NULL, *mark = NULL;
    for(;;) {
        if(!*p && (!*s || (part && *s == '/'))) return 1;
        if(!*s) {
            while(*p == '*') p++;
            return !*p;
        }
        if(*p == '*') {
            star = ++p;
            mark = s;
        } else if(*p == '?') {
            p++;
            s = next_char(s);
        } else if(*p && lower((unsigned char)*p) == lower((unsigned char)*s)) {
            p++;
            s++;
        } else if(star) {
            p = star;
            s = mark = next_char(mark);
        } else {
            return 0;
        }
    }
}

int ar_glob(const char *pattern, const char *s) {
    return glob(pattern, s, 0);
}

int ar_glob_part(const char *pattern, const char *s) {
    for(;;) {
        if(glob(pattern, s, 1)) return 1;
        s = strchr(s, '/');
        if(!s) return 0;
        s++;
    }
}

int ar_under(const char *member, const char *path) {
    size_t n;
    while(path[0] == '.' && path[1] == '/') path += 2;
    n = strlen(path);
    while(n && path[n - 1] == '/') n--;
    if(!n) return 1;
    return !strncasecmp(member, path, n) && (!member[n] || member[n] == '/');
}
