/*
SurfOS user program: sha256sum
--------------------
File: sha256sum.c   Date: 10/25/26
--------------------
sha256sum [file...]: the SHA-256 of each file, or of standard input without one (or for -),
as "hash  name" lines (track A, A9). sha256sum -c [file...]: reads such lines from the files
(or standard input) and checks each file named: "name: OK", or "name: FAILED" and status 1;
a file that cannot be read says why, and a line that is not one of these is counted and said
at the end. The hash is Mbed TLS's (third_party/mbedtls, the library Undertow is built with).
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include "mbedtls/sha256.h"

/* the hash of what fd holds, in hex (65 bytes with the terminator); -1 with errno on a read error */
static int hash_fd(int fd, char *hex) {
    static unsigned char buf[32768];
    unsigned char sum[32];
    mbedtls_sha256_context ctx;
    int i, rc = 0;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    for(;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if(n < 0 && errno == EINTR) continue;
        if(n < 0) {
            rc = -1;
            break;
        }
        if(n == 0) break;
        mbedtls_sha256_update(&ctx, buf, (size_t)n);
    }
    if(!rc) {
        mbedtls_sha256_finish(&ctx, sum);
        for(i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", sum[i]);
    }
    mbedtls_sha256_free(&ctx);
    return rc;
}

/* the hash of a file (- standard input); -1 said */
static int hash_file(const char *name, char *hex, int quiet) {
    int fd = strcmp(name, "-") ? open(name, O_RDONLY) : 0, rc, e;
    if(fd < 0) {
        if(!quiet) fprintf(stderr, "sha256sum: %s: %s\n", name, strerror(errno));
        return -1;
    }
    rc = hash_fd(fd, hex);
    e = errno;
    if(fd) close(fd);
    if(rc < 0 && !quiet) fprintf(stderr, "sha256sum: %s: %s\n", name, strerror(e));
    errno = e;
    return rc;
}

/* -c: the lines of f, "hash  name" (or "hash *name"), each name hashed and compared */
static int check(FILE *f, int *bad_lines) {
    char line[600], hex[65];
    int rc = 0, i;
    while(fgets(line, sizeof(line), f)) {
        char *name = line + 66;
        size_t len = strlen(line);
        while(len && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
        for(i = 0; i < 64 && isxdigit((unsigned char)line[i]); i++) line[i] = (char)tolower((unsigned char)line[i]);
        if(i < 64 || line[64] != ' ' || (line[65] != ' ' && line[65] != '*') || !*name) {
            if(len) (*bad_lines)++;
            continue;
        }
        if(hash_file(name, hex, 1) < 0) {
            printf("%s: FAILED to read (%s)\n", name, strerror(errno));
            rc = 1;
        } else if(memcmp(hex, line, 64)) {
            printf("%s: FAILED\n", name);
            rc = 1;
        } else printf("%s: OK\n", name);
    }
    return rc;
}

int main(int argc, char **argv) {
    char hex[65];
    int i = 1, checking = 0, rc = 0, bad_lines = 0;
    if(i < argc && !strcmp(argv[i], "-c")) {
        checking = 1;
        i++;
    }
    if(i < argc && argv[i][0] == '-' && argv[i][1]) {
        fprintf(stderr, "usage: sha256sum [-c] [file...]\n");
        return 2;
    }
    if(checking) {
        do {
            FILE *f = i < argc && strcmp(argv[i], "-") ? fopen(argv[i], "r") : stdin;
            if(!f) {
                fprintf(stderr, "sha256sum: %s: %s\n", argv[i], strerror(errno));
                rc = 1;
                continue;
            }
            if(check(f, &bad_lines)) rc = 1;
            if(f != stdin) fclose(f);
        } while(++i < argc);
        if(bad_lines) {
            fprintf(stderr, "sha256sum: %d line%s not \"hash  name\"\n", bad_lines, bad_lines == 1 ? " is" : "s are");
            rc = 1;
        }
        return rc;
    }
    do {
        const char *name = i < argc ? argv[i] : "-";
        if(hash_file(name, hex, 0) < 0) rc = 1;
        else printf("%s  %s\n", hex, name);
    } while(++i < argc);
    return rc;
}
