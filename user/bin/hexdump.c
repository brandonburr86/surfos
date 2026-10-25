/*
SurfOS user program: hexdump
--------------------
File: hexdump.c Date: 10/5/26
--------------------
hexdump [file]: the bytes of a file (or standard input) as hex and ASCII, 16 to
a line, in the layout of hexdump -C. (The ring-0 shell's hexdump shows blocks
of a block device instead.)

10/25/26: surfsh runs this program for hexdump now (a program on PATH comes before a kernel
command of the same name), so what it does not take goes to the kernel's hexdump (kcmd(),
shell/main.c): a name that is no file here, or more than one argument. So hexdump usb0 0
[count] still shows the blocks of a block device, and hexdump <file> <offset> 512 bytes of a
file from there.
*/

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <surf.h>

static unsigned char line[16];

static void print_line(unsigned long off, int n) {
    char out[80];
    int i, k;
    k = sprintf(out, "%08lx ", off);
    for(i = 0; i < 16; i++) {
        if(i == 8) out[k++] = ' ';
        if(i < n) k += sprintf(out + k, " %02x", line[i]);
        else { strcpy(out + k, "   "); k += 3; }
    }
    k += sprintf(out + k, "  |");
    for(i = 0; i < n; i++) out[k++] = (line[i] >= 32 && line[i] < 127) ? (char)line[i] : '.';
    strcpy(out + k, "|");
    puts(out);
}

/* 10/25/26: the kernel's hexdump with these arguments: its status (it says what is wrong); -1 when this
   kernel has no hexdump for programs (no bridge) */
static int kernel_hexdump(char **argv) {
    int r;
    fflush(stdout);
    r = kcmd(argv);
    if(r >= 0) return r;
    if(errno == ENOSYS || errno == ENOENT) return -1;
    fprintf(stderr, "hexdump: %s\n", strerror(errno));
    return 1;
}

int main(int argc, char **argv) {
    unsigned char buf[4096];
    unsigned long off = 0;
    int fd = 0, have = 0;
    ssize_t n, i;
    if(argc > 2 || (argc == 2 && (fd = open(argv[1], O_RDONLY)) < 0)) {
        int e = errno, r;
        if((r = kernel_hexdump(argv)) >= 0) return r;   /* a block device, or a file and an offset */
        if(argc > 2) { fprintf(stderr, "usage: hexdump [file]\n"); return 2; }
        fprintf(stderr, "hexdump: %s: %s\n", argv[1], strerror(e));
        return 1;
    }
    while((n = read(fd, buf, sizeof(buf))) > 0) {
        for(i = 0; i < n; i++) {
            line[have++] = buf[i];
            if(have == 16) { print_line(off, 16); off += 16; have = 0; }
        }
    }
    if(n < 0) { fprintf(stderr, "hexdump: %s\n", strerror(errno)); return 1; }
    if(have) { print_line(off, have); off += (unsigned long)have; }
    printf("%08lx\n", off);
    return 0;
}
