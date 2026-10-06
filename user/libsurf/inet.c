/*
SurfOS user library: IPv4 addresses as text
--------------------
File: inet.c    Date: 10/6/26
--------------------
ipstr() and ipparse(): dotted quads to and from the network byte order that the
network calls (surfos/sock.h) use.
*/

#include <stdio.h>
#include <surf.h>

char *ipstr(unsigned int ip, char buf[16]) {
    const unsigned char *b = (const unsigned char *)&ip;  /* network byte order: the first octet first */
    snprintf(buf, 16, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    return buf;
}

int ipparse(const char *s, unsigned int *ip) {
    unsigned char b[4];
    int i;
    if(!s) return -1;
    for(i = 0; i < 4; i++) {
        unsigned int v = 0, digits = 0;
        while(*s >= '0' && *s <= '9' && digits < 4) { v = v * 10 + (unsigned int)(*s++ - '0'); digits++; }
        if(!digits || v > 255) return -1;
        b[i] = (unsigned char)v;
        if(i < 3 && *s++ != '.') return -1;
    }
    if(*s) return -1;
    if(ip) *ip = (unsigned int)b[0] | (unsigned int)b[1] << 8 | (unsigned int)b[2] << 16 | (unsigned int)b[3] << 24;
    return 0;
}
