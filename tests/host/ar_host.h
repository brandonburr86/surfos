/* What tests/host/test_gztool.c, test_tar.c and test_unzip.c share (11/01/26): their checks, and the host's real tools
   reached through files in a temporary folder. Plain C, for the host's headers. */
#ifndef AR_HOST_H
#define AR_HOST_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int ar_checks, ar_failures;

static void check(int ok, const char *what) {
    ar_checks++;
    if(!ok) {
        ar_failures++;
        printf("FAIL: %s\n", what);
    }
}

static int report(const char *name) {
    printf("%s: %d checks, %d failures\n", name, ar_checks, ar_failures);
    return ar_failures ? 1 : 0;
}

/* the tool's name when the host has it, or NULL */
static __attribute__((unused)) const char *host_tool(const char *name) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "command -v %s >/dev/null 2>&1", name);
    return system(cmd) == 0 ? name : NULL;
}

static __attribute__((unused)) void host_tmpdir(char *dir) {
    const char *base = getenv("TMPDIR");
    snprintf(dir, 256, "%s/ar-test-XXXXXX", base && *base ? base : "/tmp");
    if(!mkdtemp(dir)) {
        printf("no temporary folder\n");
        exit(2);
    }
}

static __attribute__((unused)) void host_rmdir(const char *dir) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
    if(system(cmd)) printf("could not remove %s\n", dir);
}

static __attribute__((unused)) void host_write(const char *dir, const char *name, const void *p, size_t n) {
    char path[512];
    FILE *f;
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    f = fopen(path, "wb");
    if(!f || (n && fwrite(p, 1, n, f) != n) || fclose(f)) {
        printf("cannot write %s\n", path);
        exit(2);
    }
}

/* a whole file in new memory, one byte more than *n so that an empty one has an address */
static __attribute__((unused)) unsigned char *host_read(const char *dir, const char *name, size_t *n) {
    char path[512];
    FILE *f;
    unsigned char *p = NULL;
    size_t have = 0, cap = 0, k;
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    f = fopen(path, "rb");
    if(!f) {
        printf("cannot read %s\n", path);
        exit(2);
    }
    do {
        if(have + 65536 + 1 > cap) {
            cap = (have + 65536 + 1) * 2;
            p = realloc(p, cap);
            if(!p) exit(2);
        }
        k = fread(p + have, 1, 65536, f);
        have += k;
    } while(k);
    fclose(f);
    *n = have;
    return p;
}

#endif
