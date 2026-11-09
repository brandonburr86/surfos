/*
SurfOS user program: wsettings
--------------------
File: sys.c     Date: 11/05/26 (the desktop applications, track settings)
--------------------
What Settings asks of the machine besides its window: SYS_netctl (include/surfos/netctl.h), made
here with int 0x80 as libsurf makes its calls (user/libsurf/syscalls.c), since libsurf has no wrapper
for it: the kernel's answer as it comes, -errno included, which is what the model takes. And the About
page's figures: the memory and the time since the start (sysinfo()), the build (the first line of
/initrd/etc/build-id, which the initrd's maker writes) and the processor's name, which CPUID gives in
ring 3 as it gives it to the kernel (Intel's Software Developer's Manual, volume 2A, CPUID: the
extended leaves 0x80000002 to 0x80000004 hold the brand string; leaf 0 the vendor's).
*/
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <surf.h>
#include <surfos/proc.h>
#include "settings.h"
#include "sys.h"

/* the model counts on the kernel's numbers (settings.c's E_*), which are this library's (ECANCELED, 125, it lacks) */
typedef char errors_are_the_kernels[EPERM == 1 && ENOENT == 2 && ENOMEM == 12 && EACCES == 13 && EBUSY == 16 && ENODEV == 19 &&
                                    EINVAL == 22 && ENOSYS == 38 && ETIMEDOUT == 110 && ECONNREFUSED == 111 &&
                                    ECONNRESET == 104 ? 1 : -1];

int ws_sys_netctl(void *ctx, int op, void *arg, unsigned size) {
    long r;
    (void)ctx;
    asm volatile("int $0x80" : "=a"(r) : "a"(NETCTL_SYSCALL), "b"(op), "c"(arg), "d"(size) : "memory");
    return (int)r;
}

static void cpuid(unsigned leaf, unsigned r[4]) {
    asm volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(leaf), "c"(0));
}

/* EFLAGS.ID can be changed only where CPUID exists (an i486 without it never lets it) */
static int have_cpuid(void) {
    unsigned before, after;
    asm volatile("pushfl\n\tpopl %0\n\tmovl %0, %1\n\txorl $0x200000, %1\n\tpushl %1\n\tpopfl\n\t"
                 "pushfl\n\tpopl %1\n\tpushl %0\n\tpopfl" : "=&r"(before), "=&r"(after) : : "cc");
    return ((before ^ after) & 0x200000) != 0;
}

static void processor(char *out, int n) {
    unsigned r[4], brand[12];
    char name[49], *s;
    int i;
    out[0] = 0;
    if(!have_cpuid()) return;
    cpuid(0x80000000, r);
    if((r[0] & 0x80000000) && r[0] >= 0x80000004) {
        for(i = 0; i < 3; i++) cpuid(0x80000002 + (unsigned)i, &brand[4 * i]);
        memcpy(name, brand, 48);
        name[48] = 0;
        for(s = name; *s == ' '; s++) ;                 /* some pad it on the left */
        for(i = (int)strlen(s); i > 0 && s[i - 1] == ' '; i--) s[i - 1] = 0;
        if(*s) {
            snprintf(out, (size_t)n, "%s", s);
            return;
        }
    }
    cpuid(0, r);
    memcpy(name, &r[1], 4);                             /* ebx, edx, ecx: GenuineIntel */
    memcpy(name + 4, &r[3], 4);
    memcpy(name + 8, &r[2], 4);
    name[12] = 0;
    snprintf(out, (size_t)n, "%s", name);
}

static void build_id(char *out, int n) {
    char buf[96];
    int fd = open("/initrd/etc/build-id", O_RDONLY), r, i;
    out[0] = 0;
    if(fd < 0) return;
    r = (int)read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if(r <= 0) return;
    buf[r] = 0;
    for(i = 0; buf[i] && buf[i] != '\n' && buf[i] != '\r'; i++) if((unsigned char)buf[i] < 32) buf[i] = '?';
    buf[i] = 0;
    snprintf(out, (size_t)n, "%s", buf);
}

void ws_sys_about(struct ws_about *ab, int all) {
    struct sysinfo si;
    if(all) {
        build_id(ab->build, sizeof(ab->build));
        processor(ab->cpu, sizeof(ab->cpu));
    }
    ab->have_mem = 0;
    if(sysinfo(&si) == 0 && si.page_size && si.hz) {
        unsigned kb = si.page_size / 1024;
        ab->have_mem = 1;
        ab->mem_kb = si.total_pages * kb;
        ab->free_kb = si.free_pages * kb;
        ab->programs_kb = si.program_pages * kb;
        ab->heap_kb = si.heap_used / 1024;
        ab->uptime_s = si.ticks / si.hz;
    }
}
