/*
SurfOS chat client: wchat
--------------------
File: link.c    Date: 11/05/26
--------------------
The frames between the window and its helper (link.h), read as hostile on the window's side.
No system calls: host-tested as it is.
*/

#include <stdio.h>
#include <string.h>
#include "link.h"

/* the most a kind's frame may hold, -1 for a kind not known */
static int most(unsigned int kind) {
    switch(kind) {
    case LINK_UP: return LINK_TLS_MAX;
    case LINK_DATA: return LINK_DATA_MAX;
    case LINK_DOWN: return LINK_TEXT_MAX;
    }
    return -1;
}

int link_read(struct link_reader *r, const char *in, int n, int *used, int *kind, int *len) {
    int i = 0, k;
    while(i < n) {
        if(r->have < (int)sizeof(r->head)) {        /* the head, a byte at a time as it may come */
            r->head[r->have++] = (unsigned char)in[i++];
            if(r->have < (int)sizeof(r->head)) continue;
            memcpy(&r->h, r->head, sizeof(r->h));
            if(r->h.magic != LINK_MAGIC || most(r->h.kind) < 0 || r->h.len > (unsigned int)most(r->h.kind) ||
               (r->h.kind == LINK_DATA && (!r->h.len || !r->up)) || (r->h.kind == LINK_UP && r->up) || r->down)
                return -1;                          /* nothing after DOWN; UP once, before any DATA */
            r->got = 0;
        }
        k = (int)r->h.len - r->got;                 /* the body */
        if(k > n - i) k = n - i;
        memcpy(r->body + r->got, in + i, (size_t)k);
        r->got += k;
        i += k;
        if(r->got < (int)r->h.len) continue;
        r->body[r->got] = 0;
        r->have = 0;
        if(r->h.kind == LINK_UP) r->up = 1;
        if(r->h.kind == LINK_DOWN) r->down = 1;
        *used = i;
        *kind = (int)r->h.kind;
        *len = r->got;
        return 1;
    }
    *used = n;
    return 0;
}

int link_frame(char *out, int size, int kind, const void *data, int len) {
    struct link_head h;
    if(len < 0 || most((unsigned int)kind) < len || len + (int)sizeof(h) > size) return -1;
    h.magic = LINK_MAGIC;
    h.kind = (unsigned int)kind;
    h.len = (unsigned int)len;
    memcpy(out, &h, sizeof(h));
    if(len) memcpy(out + sizeof(h), data, (size_t)len);
    return (int)sizeof(h) + len;
}

void link_words(char *out, int size, const char *why) {
    static const char *const swap[][2] = {
        { "HTTPS", "TLS" },
        { "; http:// addresses still work", "" },
        { "; if that clock is wrong, set it with clock YYYY-MM-DD HH:MM:SS (UTC)", "" },
        { "Set the time (UTC) with clock YYYY-MM-DD HH:MM:SS, or correct", "Correct the PC's clock, or" },
    };
    int o = 0, i;
    if(size < 1) return;
    while(*why && o < size - 1) {
        for(i = 0; i < (int)(sizeof(swap) / sizeof(swap[0])); i++) {
            size_t n = strlen(swap[i][0]);
            if(!strncmp(why, swap[i][0], n)) {
                o += snprintf(out + o, (size_t)(size - o), "%s", swap[i][1]);
                if(o > size - 1) o = size - 1;
                why += n;
                break;
            }
        }
        if(i == (int)(sizeof(swap) / sizeof(swap[0]))) out[o++] = *why++;
    }
    out[o] = 0;
}
