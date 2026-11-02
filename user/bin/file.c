/*
SurfOS user program: file
--------------------
File: file.c    Date: 11/01/26
--------------------
file name...: what each file is, by what is in it and never by its name: "name: what". Known are
ELF programs, BMP, PNG, JPEG and GIF images, gzip and zip archives, tar archives (the header's
checksum is checked), PDF, a FAT12, FAT16 or FAT32 volume's image, scripts (#!), and ASCII or
UTF-8 text with the line ends it has; an empty file, a directory, and data for all else. Only
the start of a file is read: the signatures in its first block, and for text the first
megabyte, so a long text with a binary tail is called text. Status 2 when a file could not be
read; the others are still said.
*/

#include "../diff/cli.h"
#include <fcntl.h>
#include <sys/stat.h>

#define HEAD 4096                       /* holds every signature here */
#define TEXT_LIMIT (1024 * 1024)        /* as much as a text is looked at */

static unsigned char head[HEAD];

static unsigned le16(const unsigned char *p) {
    return (unsigned)p[0] | (unsigned)p[1] << 8;
}

static unsigned le32(const unsigned char *p) {
    return (unsigned)p[0] | (unsigned)p[1] << 8 | (unsigned)p[2] << 16 | (unsigned)p[3] << 24;
}

static unsigned be32(const unsigned char *p) {
    return (unsigned)p[0] << 24 | (unsigned)p[1] << 16 | (unsigned)p[2] << 8 | (unsigned)p[3];
}

/**** what has a signature: 1 and the words in out, or 0 ****/

static int is_elf(const unsigned char *b, size_t n, char *out, size_t size) {
    const char *type, *machine = NULL;
    unsigned t, m;
    int big;
    static char other[32];
    if(n < 20 || memcmp(b, "\177ELF", 4) || (b[4] != 1 && b[4] != 2) || (b[5] != 1 && b[5] != 2)) return 0;
    big = b[5] == 2;
    t = big ? (unsigned)b[16] << 8 | b[17] : le16(b + 16);
    m = big ? (unsigned)b[18] << 8 | b[19] : le16(b + 18);
    type = t == 1 ? "relocatable" : t == 2 ? "executable" : t == 3 ? "shared object" : t == 4 ? "core file" : "file";
    machine = m == 3 ? "Intel 80386" : m == 8 ? "MIPS" : m == 20 ? "PowerPC" : m == 40 ? "ARM" : m == 62 ? "x86-64"
            : m == 183 ? "ARM aarch64" : m == 243 ? "RISC-V" : NULL;
    if(!machine) {
        snprintf(other, sizeof(other), "machine %u", m);
        machine = other;
    }
    snprintf(out, size, "ELF %d-bit %s %s, %s", b[4] == 1 ? 32 : 64, big ? "MSB" : "LSB", type, machine);
    return 1;
}

static int is_bmp(const unsigned char *b, size_t n, char *out, size_t size) {
    unsigned dib;
    if(n < 30 || b[0] != 'B' || b[1] != 'M') return 0;
    dib = le32(b + 14);
    if(dib == 12) snprintf(out, size, "PC bitmap, %u x %u x %u", le16(b + 18), le16(b + 20), le16(b + 24));
    else if(dib == 40 || dib == 52 || dib == 56 || dib == 108 || dib == 124) {
        int h = (int)le32(b + 22);              /* a negative height: rows from the top */
        snprintf(out, size, "PC bitmap, %d x %d x %u", (int)le32(b + 18), h < 0 && h != INT_MIN ? -h : h, le16(b + 28));
    } else return 0;
    return 1;
}

static int is_png(const unsigned char *b, size_t n, char *out, size_t size) {
    if(n < 8 || memcmp(b, "\211PNG\r\n\032\n", 8)) return 0;
    if(n >= 24 && !memcmp(b + 12, "IHDR", 4)) snprintf(out, size, "PNG image data, %u x %u", be32(b + 16), be32(b + 20));
    else snprintf(out, size, "PNG image data");
    return 1;
}

static int is_gif(const unsigned char *b, size_t n, char *out, size_t size) {
    if(n < 10 || (memcmp(b, "GIF87a", 6) && memcmp(b, "GIF89a", 6))) return 0;
    snprintf(out, size, "GIF image data, version %.3s, %u x %u", (const char *)b + 3, le16(b + 6), le16(b + 8));
    return 1;
}

static int is_pdf(const unsigned char *b, size_t n, char *out, size_t size) {
    size_t k = 5;
    if(n < 8 || memcmp(b, "%PDF-", 5)) return 0;
    while(k < n && k < 12 && ((b[k] >= '0' && b[k] <= '9') || b[k] == '.')) k++;
    snprintf(out, size, "PDF document, version %.*s", (int)(k - 5), (const char *)b + 5);
    return 1;
}

/* a tar header's checksum: the sum of its 512 bytes, the field itself counted as spaces */
static int tar_sum_ok(const unsigned char *b) {
    unsigned sum = 0, stored = 0;
    int i, digits = 0;
    for(i = 0; i < 512; i++) sum += i >= 148 && i < 156 ? ' ' : b[i];
    for(i = 148; i < 156; i++) {
        if(b[i] >= '0' && b[i] <= '7') {
            stored = stored * 8 + (unsigned)(b[i] - '0');
            digits++;
        } else if(b[i] && b[i] != ' ') return 0;
        else if(digits) break;
    }
    return digits && stored == sum;
}

static int is_tar(const unsigned char *b, size_t n, char *out, size_t size) {
    if(n < 512 || !b[0] || !tar_sum_ok(b)) return 0;
    if(!memcmp(b + 257, "ustar\0", 6)) snprintf(out, size, "POSIX tar archive");
    else if(!memcmp(b + 257, "ustar ", 6)) snprintf(out, size, "GNU tar archive");
    else snprintf(out, size, "tar archive");
    return 1;
}

static int power_of_two(unsigned v) {
    return v && !(v & (v - 1));
}

/* the rules of the BPB as fs/fat.c and mkfs read them; the jump, the signature and all of these together are not
   chance. Which FAT it is goes by the count of clusters, as Microsoft's rule says and fat_mkfs.c makes them. */
static int is_fat(const unsigned char *b, size_t n, char *out, size_t size) {
    unsigned sector, cluster, reserved, fats, roots, media, total, fat_size, root_sectors, clusters, system;
    const unsigned char *label;
    int bits, len;
    if(n < 512 || b[510] != 0x55 || b[511] != 0xAA || (b[0] != 0xEB && b[0] != 0xE9)) return 0;
    sector = le16(b + 11);
    cluster = b[13];
    reserved = le16(b + 14);
    fats = b[16];
    roots = le16(b + 17);
    media = b[21];
    if(!power_of_two(sector) || sector < 512 || sector > 4096 || !power_of_two(cluster) || !reserved || !fats || fats > 2) return 0;
    if(media != 0xF0 && media < 0xF8) return 0;
    total = le16(b + 19) ? le16(b + 19) : le32(b + 32);
    fat_size = le16(b + 22) ? le16(b + 22) : le32(b + 36);
    if(!total || !fat_size) return 0;
    root_sectors = (roots * 32 + sector - 1) / sector;
    system = reserved + root_sectors;
    if(fat_size > (total - (system < total ? system : total)) / fats) return 0;        /* the FATs must fit */
    system += fats * fat_size;
    if(system >= total) return 0;
    clusters = (total - system) / cluster;
    bits = clusters < 4085 ? 12 : clusters < 65525 ? 16 : 32;
    label = b + (bits == 32 ? 71 : 43);
    len = 11;
    while(len && (label[len - 1] == ' ' || !label[len - 1])) len--;
    if(len) {
        char raw[12], shown[12 * 4];
        memcpy(raw, label, (size_t)len);
        raw[len] = 0;
        snprintf(out, size, "FAT%d file system image, label %s", bits, cli_clean(raw, shown, sizeof(shown)));
    } else snprintf(out, size, "FAT%d file system image", bits);
    return 1;
}

static int by_signature(const unsigned char *b, size_t n, char *out, size_t size) {
    if(is_elf(b, n, out, size) || is_bmp(b, n, out, size) || is_png(b, n, out, size) || is_gif(b, n, out, size) ||
       is_pdf(b, n, out, size) || is_tar(b, n, out, size) || is_fat(b, n, out, size)) return 1;
    if(n >= 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF) {
        snprintf(out, size, "JPEG image data");
        return 1;
    }
    if(n >= 3 && b[0] == 0x1F && b[1] == 0x8B && b[2] == 8) {
        snprintf(out, size, "gzip compressed data");
        return 1;
    }
    if(n >= 4 && (!memcmp(b, "PK\003\004", 4) || !memcmp(b, "PK\005\006", 4))) {
        snprintf(out, size, "Zip archive data");
        return 1;
    }
    return 0;
}

/**** text ****/

struct text {
    int binary, high, cr_waiting;
    unsigned long lf, crlf, cr;
};

/* the bytes at p as more of a text; left over at the end: the start of a sequence, for the next block */
static size_t scan_text(struct text *t, const unsigned char *p, size_t n, int last) {
    size_t i = 0;
    while(i < n && !t->binary) {
        unsigned c = p[i];
        int k = 1;
        if(t->cr_waiting) {
            t->cr_waiting = 0;
            if(c == '\n') {
                t->crlf++;
                i++;
                continue;
            }
            t->cr++;
        }
        if(c == '\n') t->lf++;
        else if(c == '\r') t->cr_waiting = 1;
        else if(c >= 0x80) {
            size_t want = c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
            k = cli_seq(p + i, n - i);
            if(k == 1) {
                if(n - i < want && !last && c >= 0xC2 && c <= 0xF4) return n - i;   /* cut by the block's end */
                t->binary = 1;
            }
            t->high = 1;
        } else if(c == 0x7F || (c < 0x20 && !strchr("\a\b\t\f\v\033", (int)c)) || !c) t->binary = 1;
        i += (size_t)k;
    }
    if(last && t->cr_waiting) {
        t->cr++;
        t->cr_waiting = 0;
    }
    return 0;
}

static void text_words(const struct text *t, char *out, size_t size) {
    const char *kind = t->high ? "UTF-8 text" : "ASCII text";
    int kinds = (t->lf != 0) + (t->crlf != 0) + (t->cr != 0);
    if(!kinds) snprintf(out, size, "%s, with no line terminators", kind);
    else if(kinds == 1 && t->lf) snprintf(out, size, "%s", kind);
    else if(kinds == 1) snprintf(out, size, "%s, with %s line terminators", kind, t->crlf ? "CRLF" : "CR");
    else snprintf(out, size, "%s, with %s%s%s%s%s line terminators", kind, t->crlf ? "CRLF" : "", t->crlf && t->cr ? ", " : "",
                  t->cr ? "CR" : "", t->lf ? ", " : "", t->lf ? "LF" : "");
}

/* the interpreter of a #! line, into word */
static int script_of(const unsigned char *b, size_t n, char *word, size_t size) {
    size_t i = 2, k = 0;
    if(n < 3 || b[0] != '#' || b[1] != '!') return 0;
    while(i < n && (b[i] == ' ' || b[i] == '\t')) i++;
    while(i < n && b[i] > ' ' && k + 1 < size) word[k++] = (char)b[i++];
    word[k] = 0;
    return k > 0;
}

/* what the file on fd is; n bytes of its start are in head. 0, or -1 and errno for a read that failed */
static int classify(int fd, size_t n, char *out, size_t size) {
    static unsigned char block[8192];
    struct text t;
    char script[64], words[96];
    size_t total = n, carry;
    if(by_signature(head, n, out, size)) return 0;
    memset(&t, 0, sizeof(t));
    carry = scan_text(&t, head, n, n < HEAD);
    if(carry) memcpy(block, head + n - carry, carry);
    while(n == HEAD && !t.binary && total < TEXT_LIMIT) {       /* n < HEAD: that was the whole file */
        long r = read(fd, block + carry, sizeof(block) - carry);
        size_t len;
        if(r < 0) return -1;
        total += (size_t)r;
        len = carry + (size_t)r;
        carry = scan_text(&t, block, len, !r);
        if(!r) break;
        memmove(block, block + len - carry, carry);
    }
    if(t.binary) {
        snprintf(out, size, "data");
        return 0;
    }
    text_words(&t, words, sizeof(words));
    if(script_of(head, n, script, sizeof(script))) {
        char shown[sizeof(script)];
        snprintf(out, size, "a %s script, %s", cli_clean(script, shown, sizeof(shown)), words);
    }
    else snprintf(out, size, "%s", words);
    return 0;
}

static int one_file(const char *name) {
    struct stat st;
    char what[192];
    size_t n = 0;
    int fd, rc = 0;
    if(!strcmp(name, "-")) fd = 0;
    else if(stat(name, &st) == 0 && S_ISDIR(st.st_mode)) {
        printf("%s: directory\n", name);
        return 0;
    } else if((fd = open(name, O_RDONLY)) < 0) {
        fprintf(stderr, "file: %s: %s\n", name, strerror(errno));
        return 2;
    }
    while(n < HEAD) {                           /* a pipe gives it in pieces */
        long r = read(fd, head + n, HEAD - n);
        if(r < 0) {
            fprintf(stderr, "file: %s: %s\n", name, strerror(errno));
            rc = 2;
            break;
        }
        if(!r) break;
        n += (size_t)r;
    }
    if(!rc) {
        if(!n) printf("%s: empty\n", name);
        else if(classify(fd, n, what, sizeof(what)) < 0) {
            fprintf(stderr, "file: %s: %s\n", name, strerror(errno));
            rc = 2;
        } else printf("%s: %s\n", name, what);
    }
    if(fd) close(fd);
    return rc;
}

int main(int argc, char **argv) {
    int i = 1, rc = 0;
    if(i < argc && !strcmp(argv[i], "--")) i++;
    else if(i < argc && argv[i][0] == '-' && argv[i][1]) i = argc;
    if(i >= argc) {
        fprintf(stderr, "usage: file name...\n");
        return 2;
    }
    for(; i < argc; i++) rc |= one_file(argv[i]);
    return cli_flush("file") ? 2 : rc;
}
