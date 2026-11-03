/*
SurfOS user program: base64
--------------------
File: base64.c  Date: 11/01/26
--------------------
base64 [-d] [-w columns] [file]: a file (standard input without one, or for -) as base64 text on
standard output, in lines of 76 columns; -w sets that width, and -w 0 makes one line with no
newline at its end. -d goes the other way: line ends are passed over and anything else outside
the alphabet is refused, with status 2 after what came before it. Both stream: only a block is
ever in memory. --decode and --wrap=N are other names for -d and -w.
*/

#include "../diff/cli.h"

#define BLOCK (3 * 1024)                /* whole groups of 3 */

static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static unsigned long wrap = 76, column;

static void put_text(const char *s, size_t n) {
    while(n) {
        size_t k = n;
        if(wrap && k > wrap - column) k = wrap - column;
        fwrite(s, 1, k, stdout);
        s += k;
        n -= k;
        column += k;
        if(wrap && column == wrap) {
            putchar('\n');
            column = 0;
        }
    }
}

static void encode_group(const unsigned char *in, size_t n, char *out) {
    out[0] = alphabet[in[0] >> 2];
    out[1] = alphabet[((in[0] & 3) << 4) | (n > 1 ? in[1] >> 4 : 0)];
    out[2] = n > 1 ? alphabet[((in[1] & 15) << 2) | (n > 2 ? in[2] >> 6 : 0)] : '=';
    out[3] = n > 2 ? alphabet[in[2] & 63] : '=';
}

static int encode(FILE *f) {
    static unsigned char in[BLOCK];
    static char out[BLOCK / 3 * 4];
    size_t n;
    while((n = fread(in, 1, sizeof(in), f)) > 0) {
        size_t i, k = 0;
        for(i = 0; i < n; i += 3, k += 4) encode_group(in + i, n - i, out + k);
        put_text(out, k);
        if(n < sizeof(in)) break;
    }
    if(wrap && column) putchar('\n');
    return ferror(f) ? -1 : 0;
}

static int value_of(int c) {
    if(c >= 'A' && c <= 'Z') return c - 'A';
    if(c >= 'a' && c <= 'z') return c - 'a' + 26;
    if(c >= '0' && c <= '9') return c - '0' + 52;
    return c == '+' ? 62 : c == '/' ? 63 : -1;
}

/* 0, -1 for a read that failed, -2 for what is no base64. A group of 4 may end early with =; after padding a
   new group may start, as files joined by cat have it. */
static int decode(FILE *f) {
    unsigned long bits = 0;
    int have = 0, pad = 0, c;
    while((c = getc(f)) != EOF) {
        if(c == '\n' || c == '\r') continue;     /* a file from another system: line ends of either kind */
        if(c == '=') {
            if(have < 2 || have + pad >= 4) return -2;
            pad++;
            if(have + pad == 4) {
                have = 0;
                pad = 0;
                bits = 0;
            }
            continue;
        }
        if(pad || (c = value_of(c)) < 0) return -2;
        bits = (bits << 6) | (unsigned long)c;
        have++;
        if(have == 2) putchar((int)((bits >> 4) & 255));
        else if(have == 3) putchar((int)((bits >> 2) & 255));
        else if(have == 4) {
            putchar((int)(bits & 255));
            have = 0;
            bits = 0;
        }
    }
    if(ferror(f)) return -1;
    return have ? -2 : 0;               /* cut off: what it had came out, and it is said */
}

static int usage(void) {
    fprintf(stderr, "usage: base64 [-d] [-w columns] [file]\n");
    return 2;
}

int main(int argc, char **argv) {
    const char *name = "-";
    int i = 1, undo = 0, r;
    FILE *f;
    lu_prog = "base64";
    for(; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        const char *arg = NULL;
        if(!strcmp(argv[i], "--")) {
            i++;
            break;
        }
        if(!strcmp(argv[i], "-d") || !strcmp(argv[i], "--decode")) {
            undo = 1;
            continue;
        }
        if(!strncmp(argv[i], "--wrap=", 7)) arg = argv[i] + 7;
        else if(!strncmp(argv[i], "-w", 2)) arg = argv[i][2] ? argv[i] + 2 : ++i < argc ? argv[i] : NULL;
        else return usage();
        if(!arg) return usage();
        {
            int w;
            if(cli_int(arg, &w) < 0) {
                fprintf(stderr, "base64: %s: not a width\n", arg);
                return 2;
            }
            wrap = (unsigned long)w;
        }
    }
    if(argc - i > 1) return usage();
    if(i < argc) name = argv[i];
    if(!(f = lu_open(name))) return 2;
    r = undo ? decode(f) : encode(f);
    if(r == -1) fprintf(stderr, "base64: %s: %s\n", name, strerror(errno));
    else if(r == -2) fprintf(stderr, "base64: %s: not base64\n", name);
    lu_close(f);
    return cli_flush("base64") ? 2 : r ? 2 : 0;
}
