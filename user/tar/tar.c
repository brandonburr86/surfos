/*
SurfOS user program: tar
--------------------
File: tar.c     Date: 11/01/26
--------------------
tar c|x|t [z] [v] [f archive] [-C dir] [--exclude=pattern] [name...]: makes an archive of the names,
or lists or unpacks one. The letters may have a - before them or not, and go together or apart: tar
czf a.tgz dir, tar -x -f a.tar. f - or no f at all is standard input or output, so a pipe will do;
nothing here seeks. z is gzip, in this program. v says each name, on standard error when the
archive itself goes to standard output. -C dir goes to dir first (for c, before the names after
it). --exclude leaves out what matches (arpath.h says how), and for x and t the names say which
members to take: a name takes a folder's contents too.

c writes ustar: file times, no owners, and each file's mode as a Unix expects it, 0644, since FAT
has none. A leading / or anything up to a .. is left out of the names in the archive, with a
message, because x would refuse them. A name that will not go into ustar's two fields is left out,
with a message. The archive is not put into itself. A file that shrinks while it is read is filled
out with zeros, so the archive stays sound, and said. An archive that exists is replaced only by
one that went right: with anything said above but the / and the .., the old one stays.

x reads what tarfile.h lists. A name from an archive becomes a path only through ar_safe_path():
what is absolute or has a .. is refused, and so is what FAT cannot hold. Links, devices, sparse
files and anything else FAT cannot hold are skipped with a message, as is a member that would land
on the archive itself. A file that exists is replaced through saveutil.h, a new one is made with
O_EXCL and deleted again if it cannot be finished. Times cannot be set on SurfOS, nor modes: every
file is as new.

Status 0, or 2 when anything was skipped or went wrong; x and t go on after anything that does not
stop the archive being read.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/stat.h>
#include "tarfile.h"
#include "arpath.h"
#include "../bin/fileutil.h"
#include "../gzip/outfile.h"

#define BUF_SIZE  32768
#define SHOW_SIZE 1024                  /* a name as printed */
#define GZ_LEVEL  6
#define PAX_BAD   "the pax header before it could not be read, so this may not be its name: skipped"
#define FOLDER_DATA "a folder that holds data: the data is skipped"

struct item { const char *chdir_to, *name; };   /* -C and names, in the order given */

static const char usage_text[] = "usage: tar c|x|t [zv] [f archive] [-C dir] [--exclude=pattern] [name...]\n";

static struct tar_reader *reader;
static struct gz_reader *gz_in;
static struct gz_writer *gz_out;
static unsigned char *buf;
static char **excludes;
static int nexcludes;
static struct item *items;
static char *found;                     /* for x and t: this item's name was met */
static int nitems;
static const char *archive = "-";
static int gzipped, verbose, trouble;
static int fd = -1;                     /* the archive */
static struct stat archive_st;          /* x: what it is, for a member of the same name */
static unsigned int in_record;          /* bytes written into the current record */
static char self[PATH_MAX], self_tmp[PATH_MAX];     /* c: the archive's absolute name, and its new file's */
static FILE *names_to;                  /* where v's names go */

static void member_say(const char *name, const char *why) {
    char show[SHOW_SIZE];
    ar_show_name(name, show, sizeof(show));
    fprintf(stderr, "tar: %s: %s\n", show, why);
    trouble = 1;
}

static void member_errno(const char *name) {
    member_say(name, strerror(errno));
}

/* the archive as bytes */

static int fd_source(void *ctx, void *p, unsigned int n) {
    (void)ctx;
    return read(fd, p, n);
}

static int archive_read(void *ctx, void *p, unsigned int n) {
    return gzipped ? gzr_read(gz_in, p, n) : fd_source(ctx, p, n);
}

static int fd_sink(void *ctx, const void *p, unsigned int n) {
    (void)ctx;
    return write_all(fd, p, n);
}

static int archive_write(const void *p, unsigned int n) {
    in_record = (in_record + n) % TAR_RECORD;
    return gzipped ? gzw_write(gz_out, p, n) : fd_sink(NULL, p, n);
}

/* Reading stopped: why, once. The source failed is either gzip's reason or errno's. */
static void archive_trouble(void) {
    const char *why = reader->why;
    if(reader->lost_entry) {
        member_say(reader->e.name, why);
        return;
    }
    if(!why && gzipped) why = gz_in->why;
    fprintf(stderr, "tar: %s: %s\n", archive, why ? why : strerror(errno));
    trouble = 1;
}

/**** c ****/

static int excluded(const char *member) {
    int i;
    for(i = 0; i < nexcludes; i++) if(ar_glob_part(excludes[i], member)) return 1;
    return 0;
}

static int is_archive(const char *path) {
    char a[PATH_MAX];
    if(!self[0] || abs_path(path, a, sizeof(a)) < 0) return 0;
    return !strcasecmp(a, self) || (self_tmp[0] && !strcasecmp(a, self_tmp));   /* FAT's names ignore case */
}

static void say_name(const char *name) {
    char show[SHOW_SIZE];
    if(!verbose) return;
    ar_show_name(name, show, sizeof(show));
    fprintf(names_to, "%s\n", show);
}

/* the rest of a block after n bytes of data */
static int write_padding(unsigned int n) {
    static const unsigned char zeros[TAR_BLOCK];
    n &= TAR_BLOCK - 1;
    return n ? archive_write(zeros, TAR_BLOCK - n) : 0;
}

/* One file. -1 only when the archive cannot be written: all else is said and passed over. */
static int add_file(const char *path, const char *member) {
    unsigned char header[TAR_BLOCK];
    struct stat st;
    unsigned int left;
    int in = open(path, O_RDONLY);
    if(in < 0 || fstat(in, &st) < 0) {
        member_errno(path);
        if(in >= 0) close(in);
        return 0;
    }
    if(tar_make_header(header, member, TAR_FILE, st.st_size, st.st_mtime) < 0) {
        member_say(path, "the name does not fit a ustar header: left out");
        close(in);
        return 0;
    }
    say_name(member);
    if(archive_write(header, TAR_BLOCK) < 0) goto bad_write;
    left = st.st_size;
    while(left) {
        unsigned int want = left < BUF_SIZE ? left : BUF_SIZE;
        ssize_t n = read(in, buf, want);
        if(n <= 0) {                                    /* the header has promised the size: it is kept */
            if(n < 0) member_errno(path);
            else member_say(path, "the file shrank while it was read: filled out with zeros");
            memset(buf, 0, BUF_SIZE);
            while(left) {
                want = left < BUF_SIZE ? left : BUF_SIZE;
                if(archive_write(buf, want) < 0) goto bad_write;
                left -= want;
            }
            break;
        }
        if(archive_write(buf, (unsigned int)n) < 0) goto bad_write;
        left -= (unsigned int)n;
    }
    close(in);
    return write_padding(st.st_size);
bad_write:
    close(in);
    return -1;
}

/* A path and all under it; the archive's name for it is path + skip. A path is at most 255 bytes, so this goes at
   most 127 folders deep. Each folder is read whole and closed first: a program has 16 descriptors. */
static int add_path(const char *path, size_t skip) {
    const char *member = path + skip;
    struct stat st;
    while(*member == '/') member++;                     /* after a path that ended in .. */
    if(stat(path, &st) < 0) {
        member_errno(path);
        return 0;
    }
    if(*member && excluded(member)) return 0;
    if(is_archive(path)) {
        member_say(path, "this is the archive: left out");
        return 0;
    }
    if(!strcmp(st.fs, "devfs")) {
        member_say(path, "a device: left out");
        return 0;
    }
    if(!S_ISDIR(st.st_mode)) return add_file(path, member);
    if(*member) {
        char dir[PATH_MAX + 1];
        unsigned char header[TAR_BLOCK];
        snprintf(dir, sizeof(dir), "%s/", member);
        if(tar_make_header(header, dir, TAR_DIR, 0, st.st_mtime) < 0) {
            /* What is in it may still fit, and x makes the folders above a file. */
            member_say(path, "the folder's name does not fit a ustar header: its entry is left out");
        } else {
            say_name(dir);
            if(archive_write(header, TAR_BLOCK) < 0) return -1;
        }
    }
    {
        char child[PATH_MAX];
        int n, i, r = 0;
        char **names = list_names(path, &n);
        if(!names) {
            member_errno(path);
            return 0;
        }
        for(i = 0; i < n && !r; i++) {
            if(path_join(child, sizeof(child), path, names[i]) < 0) member_errno(names[i]);
            else r = add_path(child, skip);
        }
        free_names(names, n);
        return r;
    }
}

/* where the archive's name starts in a path given to c: past a leading / and past the last .., which x would refuse */
static size_t name_start(const char *path) {
    const char *p = path, *start = path;
    size_t skip;
    while(*p) {
        const char *s = p;
        while(*p && *p != '/') p++;
        if(p - s == 2 && s[0] == '.' && s[1] == '.') start = *p ? p + 1 : p;
        while(*p == '/') p++;
    }
    while(*start == '/') start++;
    skip = (size_t)(start - path);
    if(skip) fprintf(stderr, "tar: %s: leaving %.*s out of the names\n", path, (int)skip, path);
    return skip;
}

/* The archive's file, open for writing: 0, or -1 said with nothing made. By its absolute name from here on: -C moves
   this program, and what is made must be found again to finish it. */
static int open_new_archive(struct out_file *f) {
    if(!strcmp(archive, "-")) {
        if(isatty(1)) {
            fprintf(stderr, "tar: an archive is not written to a terminal\n");
            return -1;
        }
        fd = 1;
        names_to = stderr;
        return 0;
    }
    names_to = stdout;
    if(abs_path(archive, self, sizeof(self)) < 0) {
        of_say("tar", archive);
        return -1;
    }
    if(of_open(f, "tar", self, 1) < 0) return -1;
    if(f->replacing && abs_path(f->save.tmp, self_tmp, sizeof(self_tmp)) < 0) self_tmp[0] = 0;
    fd = f->fd;
    return 0;
}

static int finish_archive(void) {
    static const unsigned char zeros[TAR_BLOCK];
    if(archive_write(zeros, TAR_BLOCK) < 0 || archive_write(zeros, TAR_BLOCK) < 0) return -1;
    while(in_record) {
        unsigned int n = TAR_RECORD - in_record;
        if(archive_write(zeros, n < TAR_BLOCK ? n : TAR_BLOCK) < 0) return -1;
    }
    return gzipped ? gzw_finish(gz_out) : 0;
}

static int do_create(void) {
    struct out_file out;
    int i, any = 0, r = 0, is_file = strcmp(archive, "-");
    for(i = 0; i < nitems; i++) any |= items[i].name != NULL;
    if(!any) {
        fprintf(stderr, "tar: nothing to put in the archive\n");
        return 2;
    }
    gz_out = gzipped ? malloc(sizeof(*gz_out)) : NULL;
    if(gzipped && !gz_out) {
        fprintf(stderr, "tar: out of memory\n");
        return 2;
    }
    if(open_new_archive(&out) < 0) return 2;
    if(gzipped && gzw_start(gz_out, fd_sink, NULL, GZ_LEVEL, NULL, 0) < 0) r = -1;
    for(i = 0; i < nitems && !r; i++) {
        char path[PATH_MAX];
        if(items[i].chdir_to) {
            if(chdir(items[i].chdir_to) < 0) {
                member_errno(items[i].chdir_to);
                r = -2;
            }
        } else if(snprintf(path, sizeof(path), "%s", items[i].name) >= (int)sizeof(path)) {
            errno = ENAMETOOLONG;
            member_errno(items[i].name);
        } else {
            strip_slashes(path);
            r = add_path(path, name_start(path));
        }
    }
    if(!r && finish_archive() < 0) r = -1;
    if(r == -1) fprintf(stderr, "tar: %s: %s\n", archive, strerror(errno));
    if(r) {
        if(is_file) of_drop(&out);
        return 2;
    }
    /* The old archive may be the only copy of what this one could not read: it goes only for one that went right. */
    if(is_file && out.replacing && trouble) {
        of_drop(&out);
        fprintf(stderr, "tar: %s: not everything went in, so the archive that was there stays as it was\n", archive);
        return 2;
    }
    if(is_file && of_commit(&out, "tar", 1) < 0) trouble = 1;
    return trouble ? 2 : 0;
}

/**** x and t ****/

/* the name to compare, given what the safety check made of it */
static int taken(const char *clean, const char *raw) {
    const char *name = clean ? clean : raw;
    int i, any = 0, hit = 0;
    while(name[0] == '.' && name[1] == '/') name += 2;
    if(strlen(name) < PATH_MAX && excluded(name)) return 0;     /* a longer one is no path anyone could have meant */
    for(i = 0; i < nitems; i++) {
        if(!items[i].name) continue;
        any = 1;
        if(ar_under(name, items[i].name)) {
            found[i] = 1;
            hit = 1;
        }
    }
    return !any || hit;
}

static void list_entry(const struct tar_entry *e) {
    static const char rwx[] = "rwxrwxrwx";
    char show[SHOW_SIZE], line[12], when[32];
    time_t t = (time_t)e->mtime;
    struct tm tm;
    int i;
    ar_show_name(e->name, show, sizeof(show));
    if(!verbose) {
        printf("%s\n", show);
        return;
    }
    line[0] = e->type == TAR_DIR ? 'd' : e->type == TAR_SYMLINK ? 'l' : e->type == TAR_HARDLINK ? 'h' :
              e->type == TAR_CHARDEV ? 'c' : e->type == TAR_BLOCKDEV ? 'b' : e->type == TAR_FIFO ? 'p' :
              e->type == TAR_FILE ? '-' : '?';
    for(i = 0; i < 9; i++) line[1 + i] = e->mode & (0400u >> i) ? rwx[i] : '-';
    line[10] = 0;
    gmtime_r(&t, &tm);
    if(!strftime(when, sizeof(when), "%Y-%m-%d %H:%M", &tm)) when[0] = 0;
    printf("%s %10llu %s %s\n", line, e->size, when, show);
}

static void make_folder(char *path) {
    struct stat st;
    if(of_parents(path) < 0) {
        member_errno(path);
        return;
    }
    if(mkdir(path, 0777) < 0) {
        if(errno != EEXIST) member_errno(path);
        else if(stat(path, &st) < 0 || !S_ISDIR(st.st_mode)) member_say(path, "a file is in the way");
    }
}

/* the data of the entry into path: 0, or -1 when the archive cannot be read on (said by the caller) */
static int extract_file(char *path) {
    struct out_file out;
    int n = 0, failed = 0;
    if(of_same_file(path, &archive_st)) {
        member_say(path, "this is the archive being read: skipped");
        return 0;
    }
    if(of_parents(path) < 0) {
        member_errno(path);
        return 0;
    }
    if(of_open(&out, "tar", path, 1) < 0) {
        trouble = 1;
        return 0;
    }
    while(!failed && (n = tar_data(reader, buf, BUF_SIZE)) > 0) {
        if(write_all(out.fd, (const char *)buf, (size_t)n) < 0) {
            member_errno(path);
            failed = 1;
        }
    }
    if(n < 0 || failed) {
        of_drop(&out);
        return failed ? 0 : -1;
    }
    if(of_commit(&out, "tar", 0) < 0) trouble = 1;
    return 0;
}

static const char *kind_of(const struct tar_entry *e) {
    switch(e->type) {
    case TAR_SYMLINK:  return "a symbolic link: skipped, FAT cannot hold one";
    case TAR_HARDLINK: return "a hard link: skipped, FAT cannot hold one";
    case TAR_CHARDEV:
    case TAR_BLOCKDEV: return "a device: skipped, FAT cannot hold one";
    case TAR_FIFO:     return "a named pipe: skipped, FAT cannot hold one";
    case TAR_SPARSE:   return "a sparse file: skipped, its data is not the file's";
    default:           return "a kind of entry SurfOS does not know: skipped";
    }
}

/* one entry of x: 0, or -1 when the archive cannot be read on */
static int extract_entry(void) {
    const struct tar_entry *e = &reader->e;
    char path[PATH_MAX];
    const char *why = e->name_bad ? "the name is too long, or has a zero byte in it" :
                      e->pax_bad ? PAX_BAD : ar_safe_path(e->name, path, sizeof(path));
    if(!taken(why ? NULL : path, e->name)) return 0;
    if(e->type == TAR_DIR && !why) say_name(e->name);
    if(why) {
        member_say(e->name, why);
        return 0;
    }
    if(e->type == TAR_DIR) {
        if(e->size) member_say(e->name, FOLDER_DATA);
        if(path[0]) make_folder(path);
        return 0;
    }
    if(e->type != TAR_FILE || e->sparse) {
        member_say(e->name, e->sparse ? "a sparse file: skipped, its data is not the file's" : kind_of(e));
        return 0;
    }
    if(!path[0]) {
        member_say(e->name, "a file with no name: skipped");
        return 0;
    }
    if(e->size > 0xffffffffULL) {
        member_say(e->name, "4 GB or more: skipped, FAT cannot hold it");
        return 0;
    }
    say_name(e->name);
    return extract_file(path);
}

/* After the end marker: whatever is left of the input. Through gzip that checks the CRC of all that was read, and a
   writer in a pipeline is not left without a reader. */
static void read_to_end(void) {
    int n;
    do {
        n = archive_read(NULL, buf, BUF_SIZE);
    } while(n > 0);
    if(n < 0) archive_trouble();
}

static int do_read(int listing) {
    int i, r;
    names_to = stdout;
    reader = malloc(sizeof(*reader));
    found = calloc((size_t)nitems + 1, 1);
    gz_in = gzipped ? malloc(sizeof(*gz_in)) : NULL;
    if(!reader || !found || (gzipped && !gz_in)) {
        fprintf(stderr, "tar: out of memory\n");
        return 2;
    }
    if(strcmp(archive, "-")) {
        fd = open(archive, O_RDONLY);
        if(fd < 0) {
            fprintf(stderr, "tar: %s: %s\n", archive, strerror(errno));
            return 2;
        }
    } else {
        fd = 0;
    }
    if(fstat(fd, &archive_st) < 0) memset(&archive_st, 0, sizeof(archive_st));
    for(i = 0; i < nitems; i++) {
        if(items[i].chdir_to && chdir(items[i].chdir_to) < 0) {
            member_errno(items[i].chdir_to);
            return 2;
        }
    }
    if(gzipped) gzr_start(gz_in, fd_source, NULL);
    tar_start(reader, archive_read, NULL);
    while((r = tar_next(reader)) > 0) {
        if(listing) {
            char path[PATH_MAX];
            const char *why = reader->e.name_bad ? "bad" : ar_safe_path(reader->e.name, path, sizeof(path));
            if(taken(why ? NULL : path, reader->e.name)) {
                list_entry(&reader->e);
                if(reader->e.pax_bad) member_say(reader->e.name, PAX_BAD);
            }
        } else if(extract_entry() < 0) {
            r = -1;
            break;
        }
    }
    if(r < 0) {
        archive_trouble();
    } else {
        if(reader->no_end) {
            fprintf(stderr, "tar: %s: the archive has no end marker: it may have been cut short\n", archive);
            trouble = 1;
        }
        read_to_end();
    }
    if(reader->pax_stray) {
        fprintf(stderr, "tar: %s: a pax header with no entry after it could not be read\n", archive);
        trouble = 1;
    }
    for(i = 0; i < nitems; i++) {
        if(items[i].name && !found[i]) member_say(items[i].name, "not in the archive");
    }
    if(fd > 0) close(fd);
    if(!listing) sync();
    return trouble ? 2 : 0;
}

/**** the command line ****/

static int mode;

/* a word of letters; *i moves past the arguments its f and C took. 0, or -1 for usage */
static int letters(const char *w, int argc, char **argv, int *i) {
    for(; *w; w++) {
        if(*w == 'c' || *w == 'x' || *w == 't') {
            if(mode && mode != *w) return -1;
            mode = *w;
        } else if(*w == 'z') {
            gzipped = 1;
        } else if(*w == 'v') {
            verbose = 1;
        } else if(*w == 'f' || *w == 'C') {
            if(*i + 1 >= argc) return -1;
            ++*i;
            if(*w == 'f') archive = argv[*i];
            else items[nitems++].chdir_to = argv[*i];
        } else {
            return -1;
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    int i, only_names = 0, status = 2;
    fu_prog = "tar";
    reader = NULL;
    gz_in = NULL;
    gz_out = NULL;
    found = NULL;
    mode = 0;
    gzipped = verbose = trouble = nexcludes = nitems = 0;
    archive = "-";
    in_record = 0;
    self[0] = self_tmp[0] = 0;
    items = calloc((size_t)argc + 1, sizeof(*items));
    excludes = calloc((size_t)argc + 1, sizeof(*excludes));
    buf = malloc(BUF_SIZE);
    if(!items || !excludes || !buf) {
        fprintf(stderr, "tar: out of memory\n");
        goto end;
    }
    for(i = 1; i < argc; i++) {
        char *a = argv[i];
        if(only_names || (a[0] != '-' && i > 1) || !strcmp(a, "-")) {
            items[nitems++].name = a;
        } else if(!strcmp(a, "--")) {
            only_names = 1;
        } else if(!strncmp(a, "--exclude=", 10) || !strcmp(a, "--exclude")) {
            char *p;
            size_t n;
            if(!a[9]) {
                if(i + 1 >= argc) goto usage;
                p = argv[++i];
            } else {
                p = a + 10;
            }
            n = strlen(p);
            while(n > 1 && p[n - 1] == '/') p[--n] = 0;
            excludes[nexcludes++] = p;
        } else if(a[0] == '-' && a[1] == '-') {
            goto usage;
        } else if(letters(a[0] == '-' ? a + 1 : a, argc, argv, &i) < 0) {
            goto usage;
        }
    }
    if(!mode) goto usage;
    status = mode == 'c' ? do_create() : do_read(mode == 't');
    goto end;
usage:
    fputs(usage_text, stderr);
end:
    free(items);
    free(excludes);
    free(buf);
    free(reader);
    free(found);
    free(gz_in);
    free(gz_out);
    return status;
}
