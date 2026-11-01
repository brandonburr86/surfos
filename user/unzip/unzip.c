/*
SurfOS user program: unzip
--------------------
File: unzip.c   Date: 11/01/26
--------------------
unzip [-l|-t|-p] [-o|-n] [-q] [-d dir] file.zip [name...]: unpacks a zip into the current folder or
dir (made if need be), or with -l lists it, with -t checks it, with -p writes its files to standard
output. The options may come after the file's name, as in unzip a.zip -d out. The names are * and ?
patterns (arpath.h), and without them everything is taken. Without -o or -n a file that exists is
left alone and counts as trouble; -o replaces it, through saveutil.h; -n leaves it alone quietly.
-q says only what went wrong.

The zip's central directory says what is in it: the end record is found from the file's end, and
the directory, at most 16 MB, is read whole and checked before anything else happens
(zipfile.h). Each file's local header must agree with it. Stored and deflated entries are taken,
and their CRCs checked; an encrypted one or any other method is skipped and said. A file is written
only as far as its claimed size: more data than that, or less, is the end of it, and it is deleted.
A name becomes a path only through ar_safe_path(), as in tar. Links and devices are skipped. Times
and modes are not kept.

lseek stops at 2 GB here, and a read goes on past it. So an archive larger than that works, by
reading forward to what is wanted, slowly: a member past 2 GB costs a read of all between.

Status 0, or 2 when anything was skipped or went wrong; the other files are still done.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <inflate.h>
#include <crc32.h>
#include "zipfile.h"
#include "../tar/arpath.h"
#include "../bin/fileutil.h"
#include "../gzip/outfile.h"

#define BUF_SIZE  32768
#define NAME_SIZE 1024                  /* a name as UTF-8: longer ones are refused, and listed cut */
#define SEEK_MAX  0x7fffffffu           /* what lseek's 32 signed bits reach */

static const char usage_text[] = "usage: unzip [-l|-t|-p] [-o|-n] [-q] [-d dir] file.zip [name...]\n";

static int fd, trouble;
static uint32_t at;                     /* where fd is */
static uint32_t file_size;
static const char *archive;
static unsigned char *in_buf, *out_buf, *central;
static struct inflate *z;
static struct zip_end end;
static int list_only, test_only, to_stdout, overwrite, never, quiet;
static char **members;
static char *member_found;
static int nmembers;

static void say(const char *name, const char *why) {
    char show[NAME_SIZE];
    ar_show_name(name, show, sizeof(show));
    fprintf(stderr, "unzip: %s: %s\n", show, why);
    trouble = 1;
}

/* the descriptor to position to: 0, or -1 and errno. Past lseek's reach it reads forward; see the header */
static int seek_to(uint32_t to) {
    if(to != at && (to < at || at < SEEK_MAX)) {
        uint32_t near = to < SEEK_MAX ? to : SEEK_MAX;
        if(near != at) {
            if(lseek(fd, (off_t)near, SEEK_SET) < 0) return -1;
            at = near;
        }
    }
    while(at < to) {
        uint32_t want = to - at < BUF_SIZE ? to - at : BUF_SIZE;
        ssize_t n = read(fd, in_buf, want);
        if(n <= 0) {
            if(!n) errno = EIO;
            return -1;
        }
        at += (uint32_t)n;
    }
    return 0;
}

/* n bytes at pos: 0, or -1 and errno */
static int read_at(uint32_t pos, unsigned char *p, uint32_t n) {
    if(seek_to(pos) < 0) return -1;
    while(n) {
        ssize_t r = read(fd, p, n);
        if(r <= 0) {
            if(!r) errno = EIO;
            return -1;
        }
        p += r;
        n -= (uint32_t)r;
        at += (uint32_t)r;
    }
    return 0;
}

/* the end record and the central directory: NULL, or why not */
static const char *open_archive(void) {
    unsigned char *tail;
    struct stat st;
    uint32_t n;
    const char *why;
    if(fstat(fd, &st) < 0) return strerror(errno);
    if(S_ISDIR(st.st_mode)) return strerror(EISDIR);
    if(lseek(fd, 0, SEEK_SET) < 0) {
        if(errno == ESPIPE) return "a zip is read from its end: it cannot come through a pipe";
        return strerror(errno);
    }
    file_size = st.st_size;
    n = file_size < ZIP_TAIL_MAX ? file_size : ZIP_TAIL_MAX;
    tail = malloc(n ? n : 1);
    if(!tail) return "out of memory";
    if(read_at(file_size - n, tail, n) < 0) {
        free(tail);
        return strerror(errno);
    }
    why = zip_find_end(tail, n, file_size, &end);
    free(tail);
    if(why) return why;
    central = malloc(end.cd_size ? end.cd_size : 1);
    if(!central) return "out of memory";
    if(read_at(end.cd_offset, central, end.cd_size) < 0) return strerror(errno);
    return zip_check_directory(central, &end);
}

/* The entry's data into descriptor out (-1: nowhere), from the archive at data: NULL, or why it is not what it says.
   The reader never asks for more than csize bytes, and out never gets more than usize. */
static const char *copy_data(const struct zip_entry *e, uint32_t data, int out) {
    uint64_t done = 0;
    uint32_t left = e->csize, crc = 0;
    if(seek_to(data) < 0) return strerror(errno);
    if(e->method == ZIP_STORED) {
        if(e->csize != e->usize) return "stored, but its two sizes differ";
        while(left) {
            ssize_t n = read(fd, in_buf, left < BUF_SIZE ? left : BUF_SIZE);
            if(n <= 0) return n ? strerror(errno) : "the archive ends early";
            at += (uint32_t)n;
            left -= (uint32_t)n;
            crc = crc32(crc, in_buf, (size_t)n);
            if(out >= 0 && write_all(out, (const char *)in_buf, (size_t)n) < 0) return strerror(errno);
        }
        done = e->usize;
    } else {
        inflate_init(z);
        z->in_n = 0;
        for(;;) {
            uint64_t room = e->usize - done;
            size_t made;
            int r;
            if(!z->in_n && left) {
                ssize_t n = read(fd, in_buf, left < BUF_SIZE ? left : BUF_SIZE);
                if(n <= 0) return n ? strerror(errno) : "the archive ends early";
                at += (uint32_t)n;
                left -= (uint32_t)n;
                z->in = in_buf;
                z->in_n = (size_t)n;
            }
            if(room > BUF_SIZE) room = BUF_SIZE;
            if(!room) room = 1;                         /* one byte too many is enough to know */
            z->out = out_buf;
            z->out_n = (size_t)room;
            r = inflate_run(z);
            made = (size_t)room - z->out_n;
            done += made;
            if(done > e->usize) return "it holds more than it says";
            crc = crc32(crc, out_buf, made);
            if(made && out >= 0 && write_all(out, (const char *)out_buf, made) < 0) return strerror(errno);
            if(r == INFLATE_BAD) return z->msg;
            if(r == INFLATE_END) break;
            if(!left && !z->in_n && z->out_n) return "its data ends too early";
        }
        if(left || z->in_n) return "its data ends before its compressed size says";
    }
    if(done != e->usize) return "it holds less than it says";
    if(crc != e->crc) return "its CRC is wrong";
    return NULL;
}

/* Where the entry's data starts: its local header is read and held against the central directory. NULL, or why not */
static const char *find_data(const struct zip_entry *e, uint32_t *data) {
    unsigned char head[ZIP_LOCAL_SIZE];
    uint32_t nlen, xlen;
    uint64_t start;
    const char *why;
    if(read_at(e->offset, head, ZIP_LOCAL_SIZE) < 0) return strerror(errno);
    if((why = zip_local(head, e, &nlen, &xlen))) return why;
    if(nlen != e->name_len || nlen > BUF_SIZE) return "its two headers give different names";
    if(nlen) {
        if(read_at(e->offset + ZIP_LOCAL_SIZE, out_buf, nlen) < 0) return strerror(errno);
        if(memcmp(out_buf, e->name, nlen)) return "its two headers give different names";
    }
    start = (uint64_t)e->offset + ZIP_LOCAL_SIZE + nlen + xlen;
    if(start + e->csize > end.cd_offset) return "it lies outside the archive";
    *data = (uint32_t)start;
    return NULL;
}

/* will this entry's data be read at all? Said when not */
static int readable(const struct zip_entry *e, const char *name) {
    if(e->flags & ZIP_FLAG_ENCRYPTED) {
        say(name, "encrypted: skipped");
        return 0;
    }
    if(e->method != ZIP_STORED && e->method != ZIP_DEFLATED) {
        say(name, "a compression method unzip does not know: skipped");
        return 0;
    }
    return 1;
}

static int is_folder(const char *name) {
    size_t n = strlen(name);
    return n && name[n - 1] == '/';
}

static void make_folder(char *path, const char *show) {
    struct stat st;
    if(of_parents(path) < 0) {
        say(path, strerror(errno));
        return;
    }
    if(!quiet) printf("   creating: %s\n", show);
    if(mkdir(path, 0777) < 0) {
        if(errno != EEXIST) say(path, strerror(errno));
        else if(stat(path, &st) < 0 || !S_ISDIR(st.st_mode)) say(path, "a file is in the way");
    }
}

/* show is the name as it may be printed. Nothing is made for an entry until all that can be told about it without
   reading its data is known to be right. */
static void extract_one(const struct zip_entry *e, const char *name, const char *show) {
    struct out_file f;
    char path[PATH_MAX];
    uint32_t data;
    const char *why = ar_safe_path(name, path, sizeof(path));
    int r;
    if(why) {
        say(name, why);
        return;
    }
    if(!path[0]) return;
    if(zip_is_special(e)) {
        say(name, "a link or a device: skipped, FAT cannot hold one");
        return;
    }
    if(is_folder(name)) {
        make_folder(path, show);
        return;
    }
    if(!readable(e, name)) return;
    if((why = find_data(e, &data))) {
        say(name, why);
        return;
    }
    if(of_parents(path) < 0) {
        say(path, strerror(errno));
        return;
    }
    r = of_open(&f, "unzip", path, overwrite);
    if(r == OF_EXISTS) {
        if(!never) say(path, "it already exists: -o replaces it, -n leaves it quietly");
        return;
    }
    if(r) {
        trouble = 1;
        return;
    }
    if(!quiet) printf("%s: %s\n", e->method == ZIP_STORED ? " extracting" : "  inflating", show);
    if((why = copy_data(e, data, f.fd))) {
        say(path, why);
        of_drop(&f);
        return;
    }
    if(of_commit(&f, "unzip", 0) < 0) trouble = 1;
}

static void test_one(const struct zip_entry *e, const char *name) {
    uint32_t data;
    const char *why;
    if(is_folder(name) || !readable(e, name)) return;
    why = find_data(e, &data);
    if(!why) why = copy_data(e, data, -1);
    if(why) say(name, why);
    else if(!quiet) printf("    testing: %s  OK\n", name);
}

static void pipe_one(const struct zip_entry *e, const char *name) {
    uint32_t data;
    const char *why;
    if(is_folder(name) || !readable(e, name)) return;
    why = find_data(e, &data);
    if(!why) why = copy_data(e, data, 1);
    if(why) say(name, why);
}

static void list_one(const struct zip_entry *e, const char *show) {
    printf("%10u  %04u-%02u-%02u %02u:%02u   %s\n", e->usize, (e->date >> 9) + 1980, (e->date >> 5) & 15, e->date & 31,
           e->time >> 11, (e->time >> 5) & 63, show);
}

static int wanted(const char *name) {
    int i, hit = 0;
    if(!nmembers) return 1;
    for(i = 0; i < nmembers; i++) {
        if(ar_glob(members[i], name)) {
            member_found[i] = 1;
            hit = 1;
        }
    }
    return hit;
}

static void walk(void) {
    unsigned long long total = 0;
    uint32_t pos = 0, i, files = 0;
    if(list_only) {
        printf("Archive:  %s\n      Size  Date       Time    Name\n----------  ---------- -----   ----\n", archive);
    }
    for(i = 0; i < end.entries; i++) {
        struct zip_entry e;
        char name[NAME_SIZE], show[NAME_SIZE];
        const char *why = zip_entry_at(central, end.cd_size, &pos, &e), *bad;
        if(why) {                                       /* cannot be: it was checked */
            say(archive, why);
            return;
        }
        bad = zip_name(&e, name, sizeof(name));
        ar_show_name(name, show, sizeof(show));
        if(!wanted(name)) continue;
        if(list_only) {
            list_one(&e, show);
            total += e.usize;
            files++;
        } else if(bad) {
            say(name, bad);
        } else if(test_only) {
            test_one(&e, show);
        } else if(to_stdout) {
            pipe_one(&e, name);
        } else {
            extract_one(&e, name, show);
        }
    }
    if(list_only) {
        printf("----------                     ----\n%10llu                     %u file%s\n", total, files, files == 1 ? "" : "s");
    } else if(test_only && !trouble && !quiet) {
        printf("No errors in %s\n", archive);
    }
}

int main(int argc, char **argv) {
    const char *dir = NULL, *why;
    int i, only_names = 0;
    fd = -1;
    trouble = 0;
    at = 0;
    archive = NULL;
    central = NULL;
    list_only = test_only = to_stdout = overwrite = never = quiet = nmembers = 0;
    members = calloc((size_t)argc + 1, sizeof(*members));
    member_found = calloc((size_t)argc + 1, 1);
    in_buf = malloc(BUF_SIZE);
    out_buf = malloc(BUF_SIZE + 1);
    z = malloc(sizeof(*z));
    if(!members || !member_found || !in_buf || !out_buf || !z) {
        fprintf(stderr, "unzip: out of memory\n");
        trouble = 1;
        goto end;
    }
    for(i = 1; i < argc; i++) {
        const char *a = argv[i];
        if(only_names || a[0] != '-' || !a[1]) {
            if(!archive) archive = a;
            else members[nmembers++] = argv[i];
            continue;
        }
        if(!strcmp(a, "--")) {
            only_names = 1;
            continue;
        }
        for(a++; *a; a++) {
            if(*a == 'l') list_only = 1;
            else if(*a == 't') test_only = 1;
            else if(*a == 'p') to_stdout = 1;
            else if(*a == 'o') overwrite = 1;
            else if(*a == 'n') never = 1;
            else if(*a == 'q') quiet = 1;
            else if(*a == 'd') {
                if(a[1]) dir = a + 1;
                else if(i + 1 < argc) dir = argv[++i];
                else goto usage;
                break;
            } else {
                goto usage;
            }
        }
    }
    if(!archive || (overwrite && never)) goto usage;
    fd = strcmp(archive, "-") ? open(archive, O_RDONLY) : 0;
    if(fd < 0) {
        fprintf(stderr, "unzip: %s: %s\n", archive, strerror(errno));
        trouble = 1;
        goto end;
    }
    if((why = open_archive())) {
        say(archive, why);
        goto end;
    }
    if(dir && !list_only && !test_only && !to_stdout) {
        char probe[PATH_MAX];
        if(snprintf(probe, sizeof(probe), "%s/x", dir) >= (int)sizeof(probe)) {
            say(dir, strerror(ENAMETOOLONG));
            goto end;
        }
        if(of_parents(probe) < 0 || (mkdir(dir, 0777) < 0 && errno != EEXIST) || chdir(dir) < 0) {
            say(dir, strerror(errno));
            goto end;
        }
    }
    walk();
    for(i = 0; i < nmembers; i++) if(!member_found[i]) say(members[i], "not in the archive");
    if(!list_only && !test_only && !to_stdout) sync();
end:
    if(fd > 0) close(fd);
    free(central);
    free(members);
    free(member_found);
    free(in_buf);
    free(out_buf);
    free(z);
    return trouble ? 2 : 0;
usage:
    fputs(usage_text, stderr);
    trouble = 1;
    goto end;
}
