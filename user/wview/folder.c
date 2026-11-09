/*
SurfOS user program: wview
--------------------
File: folder.c  Date: 11/05/26
--------------------
The folder's pictures (view.h): which names are pictures, kept in the caller's buffer within the
limits, and their order, in which N and P step. A name is a picture by its ending, which is cheap to
know for a whole folder; whether it really is one is the decoder's to say when it is opened. The
order is by name with case not counted, as FAT keeps names, and with the digits in a name taken as
the number they make, so that pic9 comes before pic10 as a person counts; names that are still the
same (pic01 and pic1, Pic and pic) are ordered byte by byte, so the order is total. The sort is a
merge sort, in O(n log n) for a folder of FOLDER_MAX, with no recursion.
*/

#include <string.h>
#include "view.h"

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int digit(int c) { return c >= '0' && c <= '9'; }

int folder_is_picture(const char *name) {
    static const char *const ends[] = { ".bmp", ".png", ".gif", ".jpg", ".jpeg" };
    size_t n = strlen(name), e, i, k;
    for(i = 0; i < sizeof(ends) / sizeof(ends[0]); i++) {
        e = strlen(ends[i]);
        if(n <= e) continue;
        for(k = 0; k < e && lower((unsigned char)name[n - e + k]) == ends[i][k]; k++) ;
        if(k == e) return 1;
    }
    return 0;
}

void folder_start(struct folder *f, char *names) {
    f->names = names;
    f->count = 0;
    f->dropped = 0;
    f->used = 0;
}

int folder_add(struct folder *f, const char *name) {
    size_t n = strlen(name);
    if(!folder_is_picture(name)) return -1;
    if(f->count >= FOLDER_MAX || n + 1 > FOLDER_BYTES - f->used) {
        f->dropped++;
        return -1;
    }
    memcpy(f->names + f->used, name, n + 1);
    f->at[f->count++] = f->used;
    f->used += (unsigned)n + 1;
    return 0;
}

const char *folder_name(const struct folder *f, int i) {
    return i >= 0 && i < f->count ? f->names + f->at[i] : "";
}

/* a before b: <0, 0 (the same name), >0. Digits are compared as the number they make (by their count once the leading
   zeros are passed, then digit by digit), the rest with case not counted, and the names that are still the same byte
   by byte */
static int compare(const char *a, const char *b) {
    const unsigned char *p = (const unsigned char *)a, *q = (const unsigned char *)b;
    while(*p && *q) {
        if(digit(*p) && digit(*q)) {
            const unsigned char *s = p, *t = q;
            size_t m, n;
            while(*s == '0') s++;
            while(*t == '0') t++;
            for(m = 0; digit(s[m]); m++) ;
            for(n = 0; digit(t[n]); n++) ;
            if(m != n) return m < n ? -1 : 1;
            for(; m; m--, s++, t++) if(*s != *t) return *s < *t ? -1 : 1;
            p = s;
            q = t;
            continue;
        }
        if(lower(*p) != lower(*q)) return lower(*p) < lower(*q) ? -1 : 1;
        p++;
        q++;
    }
    if(*p || *q) return *p ? 1 : -1;
    return strcmp(a, b);
}

void folder_sort(struct folder *f) {
    static unsigned tmp[FOLDER_MAX];
    unsigned *from = f->at, *to = tmp, *t;
    int width, i, n = f->count;
    for(width = 1; width < n; width *= 2) {             /* runs of width merged in pairs, from one buffer to the other */
        for(i = 0; i < n; i += 2 * width) {
            int a = i, am = i + width < n ? i + width : n, b = am, bm = i + 2 * width < n ? i + 2 * width : n, o = i;
            while(a < am && b < bm) to[o++] = compare(f->names + from[a], f->names + from[b]) <= 0 ? from[a++] : from[b++];
            while(a < am) to[o++] = from[a++];
            while(b < bm) to[o++] = from[b++];
        }
        t = from;
        from = to;
        to = t;
    }
    if(from != f->at) memcpy(f->at, from, (size_t)n * sizeof(f->at[0]));
}

int folder_find(const struct folder *f, const char *name) {
    int i;
    for(i = 0; i < f->count; i++) if(!strcmp(folder_name(f, i), name)) return i;
    for(i = 0; i < f->count; i++) {                     /* FAT's names do not count case: the path may not have it */
        const char *s = folder_name(f, i), *t = name;
        while(*s && lower((unsigned char)*s) == lower((unsigned char)*t)) {
            s++;
            t++;
        }
        if(!*s && !*t) return i;
    }
    return -1;
}

int folder_step(const struct folder *f, int i, int step) {
    if(f->count <= 0) return -1;
    if(i < 0 || i >= f->count) return step >= 0 ? 0 : f->count - 1;
    return ((i + step) % f->count + f->count) % f->count;
}
