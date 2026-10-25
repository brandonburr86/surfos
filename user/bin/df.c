/*
SurfOS user program: df
--------------------
File: df.c      Date: 10/5/26
--------------------
df [path...]: size and free space of the mounted file systems (statfs), in KB.
Without paths: / and every mount point under it (the root directory lists them).
A file system that does not count its space shows "-".

10/25/26: surfsh runs this program for df now (a program on PATH comes before a kernel
command of the same name), so it shows what the kernel's df showed (shell/fscmd.c): Device,
Type, Size, Used, Avail, Use%, Mounted on and Label, the sizes for people (16 MB, 1.9 GB, in
powers of 1024). A volume mounted read-only says so after its label. The status is 1 when a
file system could not say its size, as the kernel's.
*/

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <sys/statfs.h>

#define ROW "%-7s %-7s %8s %8s %8s %5s  %-13s %s%s\n"

/* a size for people, from a count of 512-byte blocks, as the kernel's df says it: 64 MB, 1.9 GB (no
   64-bit division: user programs have no libgcc) */
static void human(unsigned long long blocks, char *out, size_t n) {
    unsigned int tenths;
    const char *unit;
    if(blocks >= 2097152) { tenths = (unsigned int)((blocks * 10) >> 21); unit = "GB"; }
    else if(blocks >= 2048) { tenths = (unsigned int)((blocks * 10) >> 11); unit = "MB"; }
    else { tenths = (unsigned int)blocks * 5; unit = "KB"; }
    if(tenths % 10 == 0 || tenths >= 1000) snprintf(out, n, "%u %s", tenths / 10, unit);
    else snprintf(out, n, "%u.%u %s", tenths / 10, tenths % 10, unit);
}

static int show(const char *path) {
    struct statfs st;
    char size[16], used[16], avail[16], pct[8];
    const char *dev, *ro;
    unsigned int inuse, u, s;
    if(statfs(path, &st) < 0) {
        fprintf(stderr, "df: %s: %s\n", path, strerror(errno));
        return 1;
    }
    dev = st.dev[0] ? st.dev : "-";
    ro = !(st.flags & ST_RDONLY) ? "" : st.label[0] ? "  (read-only)" : "(read-only)";
    if(!st.units || !st.unit) {
        printf(ROW, dev, st.fs, "-", "-", "-", "-", st.path, st.label, ro);
        return 0;
    }
    inuse = st.units - st.free_units;
    human((unsigned long long)st.units * st.unit >> 9, size, sizeof(size));
    human((unsigned long long)inuse * st.unit >> 9, used, sizeof(used));
    human((unsigned long long)st.free_units * st.unit >> 9, avail, sizeof(avail));
    u = inuse;
    s = st.units;
    while(s > 0xFFFFFF) { u >>= 4; s >>= 4; }      /* the percentage in 32-bit arithmetic */
    snprintf(pct, sizeof(pct), "%u%%", s ? (u * 100 + s - 1) / s : 0);
    printf(ROW, dev, st.fs, size, used, avail, pct, st.path, st.label, ro);
    return 0;
}

int main(int argc, char **argv) {
    int i, rc = 0;
    printf(ROW, "Device", "Type", "Size", "Used", "Avail", "Use%", "Mounted on", "Label", "");
    if(argc > 1) {
        for(i = 1; i < argc; i++) rc |= show(argv[i]);
    } else {
        DIR *d = opendir("/");
        struct dirent *de;
        char path[300];
        rc |= show("/");
        while(d && (de = readdir(d))) {
            snprintf(path, sizeof(path), "/%s", de->d_name);
            rc |= show(path);
        }
        if(d) closedir(d);
    }
    return rc;
}
