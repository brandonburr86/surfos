/*
SurfOS user program: diff
--------------------
File: diff.c    Date: 11/01/26
--------------------
diff [-u [n] | -U n | -c | -C n] [-q] [-r] [-i] [-b] [-w] [-B] file1 file2: what differs between two
files, line by line, as an edit that patch can apply. Status 0 the same, 1 different, 2 trouble.
- is standard input. Without a style the changes come in the normal form, a 2c2 and the lines.

  -u, -U n   unified, with n lines of context (3); -u n is taken too, when two files still follow
  -c, -C n   context, likewise
  -q         only say that they differ
  -r         go down through directories
  -i         upper and lower case count the same (ASCII)
  -b         any run of white space counts the same as any other, and none at a line's end
  -w         white space does not count at all
  -B         changes that only add or remove blank lines do not count

file1 and file2 may be directories: the names in both are compared, and what is only in one is said,
one level down without -r. A directory against a file compares the file with the name of it
in the directory. Names pair up without regard to case, as on FAT. A file with a NUL in its first block
is binary: only that the two differ is said.

The comparison itself is in user/diff/ (diff.h tells how it works and why it needs next to no
memory for the files' bytes). Directories are read whole and closed before going into them.
*/

#include "fileutil.h"
#include "../diff/cli.h"
#include "../diff/diff.h"
#include <time.h>

static struct dopts opts = { 'n', 3, 0, 0, 0, 0 };
static int recursive, any_trouble, any_difference;
static char command[256];               /* the options as given, for the line before a pair of a directory's files */

static void compare_dirs(const char *d1, const char *d2);

static int usage(void) {
    fprintf(stderr, "usage: diff [-u [n] | -U n | -c | -C n] [-q] [-r] [-i] [-b] [-w] [-B] file1 file2\n");
    return 2;
}

/* name and time as the file lines have them: no time for standard input, or a file that has none */
static void make_label(char *out, size_t n, const char *name) {
    struct stat st;
    struct tm tm;
    char when[32];
    time_t t;
    if(strcmp(name, "-") && !stat(name, &st) && st.st_mtime) {
        t = (time_t)st.st_mtime;
        localtime_r(&t, &tm);
        if(strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tm)) {
            snprintf(out, n, "%s\t%s", name, when);
            return;
        }
    }
    snprintf(out, n, "%s", name);
}

/* two files. in_dir: they were found in directories, and a line says which pair it is */
static void compare_files(const char *n1, const char *n2, int in_dir) {
    struct dsrc a, b;
    struct dscript s;
    int r;
    if(dsrc_open(&a, n1) < 0) {
        fprintf(stderr, "diff: %s: %s\n", n1, strerror(errno));
        any_trouble = 1;
        return;
    }
    if(!strcmp(n1, "-") && !strcmp(n2, "-")) {  /* the one input against itself */
        dsrc_close(&a);
        return;
    }
    if(dsrc_open(&b, n2) < 0) {
        fprintf(stderr, "diff: %s: %s\n", n2, strerror(errno));
        any_trouble = 1;
        dsrc_close(&a);
        return;
    }
    r = diff_compare(&a, &b, &opts, &s);
    if(r == DIFF_FAILED) {
        fprintf(stderr, "diff: %s: %s\n", s.failed_side < 0 ? "out of memory" : s.failed_side ? n2 : n1,
                s.failed_side < 0 ? "the files have too many lines" : strerror(errno));
        any_trouble = 1;
    } else if(r == DIFF_BINARY) {
        printf(opts.brief ? "Files %s and %s differ\n" : "Binary files %s and %s differ\n", n1, n2);
        any_difference = 1;
    } else if(r == DIFF_DIFFER) {
        any_difference = 1;
        if(opts.brief) printf("Files %s and %s differ\n", n1, n2);
        else {
            static char label1[PATH_MAX + 32], label2[PATH_MAX + 32];
            if(in_dir) printf("diff%s %s %s\n", command, n1, n2);
            make_label(label1, sizeof(label1), n1);
            make_label(label2, sizeof(label2), n2);
            if(diff_print(&s, &opts, label1, label2, stdout)) {
                fprintf(stderr, "diff: %s: %s\n", s.failed_side ? n2 : n1, strerror(errno));
                any_trouble = 1;
            }
        }
    }
    diff_free(&s);
    dsrc_close(&a);
    dsrc_close(&b);
}

static int by_name(const void *a, const void *b) {
    const char *x = *(char *const *)a, *y = *(char *const *)b;
    int c = strcasecmp(x, y);
    return c ? c : strcmp(x, y);
}

/* a directory's names in order, without . and .. (the host's readdir has them) */
static char **sorted_names(const char *dir, int *count) {
    char **v = list_names(dir, count);
    int i, k = 0;
    if(!v) {
        fprintf(stderr, "diff: %s: %s\n", dir, strerror(errno));
        any_trouble = 1;
        return NULL;
    }
    for(i = 0; i < *count; i++) {
        if(!strcmp(v[i], ".") || !strcmp(v[i], "..")) free(v[i]);
        else v[k++] = v[i];
    }
    *count = k;
    qsort(v, (size_t)k, sizeof(*v), by_name);
    return v;
}

static const char *kind_of(const struct stat *st) {
    return S_ISDIR(st->st_mode) ? "directory" : st->st_size ? "regular file" : "regular empty file";
}

static void compare_entry(const char *dir1, const char *n1, const char *dir2, const char *n2) {
    char p1[PATH_MAX], p2[PATH_MAX];
    struct stat s1, s2;
    if(path_join(p1, sizeof(p1), dir1, n1) < 0 || path_join(p2, sizeof(p2), dir2, n2) < 0) {
        fprintf(stderr, "diff: %s/%s: %s\n", dir1, n1, strerror(errno));
        any_trouble = 1;
        return;
    }
    if(stat(p1, &s1) < 0 || stat(p2, &s2) < 0) {
        fprintf(stderr, "diff: %s: %s\n", p1, strerror(errno));
        any_trouble = 1;
        return;
    }
    if(S_ISDIR(s1.st_mode) && S_ISDIR(s2.st_mode)) {
        if(recursive) compare_dirs(p1, p2);
        else printf("Common subdirectories: %s and %s\n", p1, p2);
    } else if(S_ISDIR(s1.st_mode) || S_ISDIR(s2.st_mode)) {
        printf("File %s is a %s while file %s is a %s\n", p1, kind_of(&s1), p2, kind_of(&s2));
        any_difference = 1;
    } else compare_files(p1, p2, 1);
}

/* the depth is bounded by PATH_MAX, and nothing stays open going down */
static void compare_dirs(const char *d1, const char *d2) {
    char **v1, **v2 = NULL;
    int n1, n2 = 0, i = 0, k = 0;
    if(!(v1 = sorted_names(d1, &n1)) || !(v2 = sorted_names(d2, &n2))) {
        if(v1) free_names(v1, n1);
        return;
    }
    while(i < n1 || k < n2) {
        int c = i == n1 ? 1 : k == n2 ? -1 : strcasecmp(v1[i], v2[k]);
        if(c < 0) {
            printf("Only in %s: %s\n", d1, v1[i++]);
            any_difference = 1;
        } else if(c > 0) {
            printf("Only in %s: %s\n", d2, v2[k++]);
            any_difference = 1;
        } else compare_entry(d1, v1[i++], d2, v2[k++]);
    }
    free_names(v1, n1);
    free_names(v2, n2);
}

static int is_dir(const char *name) {
    struct stat st;
    return strcmp(name, "-") && !stat(name, &st) && S_ISDIR(st.st_mode);
}

/* the last part of a path, for a file against a directory */
static const char *last_part(char *path) {
    strip_slashes(path);
    return path_base(path);
}

static int all_digits(const char *s) {
    if(!*s) return 0;
    for(; *s; s++)
        if(*s < '0' || *s > '9') return 0;
    return 1;
}

int main(int argc, char **argv) {
    char *f1, *f2;
    int i = 1, d1, d2;
    size_t used = 0;
    lu_prog = fu_prog = "diff";
    for(; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        const char *o;
        unsigned long long n;
        int from = i, k;
        if(!strcmp(argv[i], "--")) {
            i++;
            break;
        }
        for(o = argv[i] + 1; *o; o++) {
            if(*o == 'q') opts.brief = 1;
            else if(*o == 'r') recursive = 1;
            else if(*o == 'i') opts.icase = 1;
            else if(*o == 'b') { if(opts.space != 'w') opts.space = 'b'; }
            else if(*o == 'w') opts.space = 'w';
            else if(*o == 'B') opts.blanks = 1;
            else if(*o == 'u' || *o == 'c' || *o == 'U' || *o == 'C') {
                const char *arg = NULL;
                int style = *o == 'u' || *o == 'U' ? 'u' : 'c';
                opts.style = style;
                if(o[1] >= '0' && o[1] <= '9') arg = o + 1;
                else if(*o == 'U' || *o == 'C') {
                    if(o[1]) arg = o + 1;
                    else if(++i < argc) arg = argv[i];
                    else return usage();
                } else if(o[1]) continue;
                /* -u 5 a b: a number here is a count only while two files still follow it */
                else if(i + 3 < argc && all_digits(argv[i + 1])) arg = argv[++i];
                if(arg) {
                    if(cli_number(arg, &n) < 0 || n > 0xFFFFFFFFULL) {
                        fprintf(stderr, "diff: %s: not a number of lines\n", arg);
                        return 2;
                    }
                    opts.context = (unsigned)n;
                }
                break;
            } else return usage();
        }
        for(k = from; k <= i; k++) {
            int w = snprintf(command + used, sizeof(command) - used, " %s", argv[k]);
            if(w > 0 && (size_t)w < sizeof(command) - used) used += (size_t)w;
            else command[used] = 0;
        }
    }
    if(argc - i != 2) return usage();
    f1 = argv[i];
    f2 = argv[i + 1];
    d1 = is_dir(f1);
    d2 = is_dir(f2);
    if(d1 && d2) compare_dirs(f1, f2);
    else if((d1 && !strcmp(f2, "-")) || (d2 && !strcmp(f1, "-"))) {
        fprintf(stderr, "diff: standard input cannot be compared with a directory\n");
        return 2;
    } else if(d1 || d2) {
        char joined[PATH_MAX];
        const char *dir = d1 ? f1 : f2;
        char *file = d1 ? f2 : f1;
        if(path_join(joined, sizeof(joined), dir, last_part(file)) < 0) {
            fprintf(stderr, "diff: %s: %s\n", dir, strerror(errno));
            return 2;
        }
        compare_files(d1 ? joined : f1, d1 ? f2 : joined, 0);
    } else compare_files(f1, f2, 0);
    if(cli_flush("diff") || any_trouble) return 2;
    return any_difference;
}
