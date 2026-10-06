/*
SurfOS user program: fstest
--------------------
File: fstest.c  Date: 10/5/26
--------------------
fstest <dir>: a self-checking test of the file API as a program sees it, run
in <dir> (made if missing, emptied and removed at the end when fstest made it):
descriptors (open read write lseek fstat ftruncate fsync dup2 close), names
(rename unlink mkdir rmdir chdir), directories (opendir readdir), statfs, stdio
(fopen fprintf fgets fread fwrite fseek ftell append), a 1 MB pattern written
and read back, a long file name, and the errno of the failures (ENOENT EEXIST
ENOTEMPTY EISDIR ENOTDIR EBADF). One line per check, then FSTEST PASS or
FSTEST FAIL. A call that answers ENOSYS (rename on a file system without it)
or a statfs without sizes is a "skip", not a failure.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <surf.h>

#define BIG       (1024 * 1024)
#define BIG_CHUNK 4096
#define LONG_NAME "A file with a rather long name, mixed Case.text"

static int checks, failures, skips, made;
static char dir[PATH_MAX];

static void check(int ok, const char *what) {
    checks++;
    if(!ok) failures++;
    printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
}

/* ok, or a skip when the call said ENOSYS */
static void check_or_skip(int ok, int err, const char *what) {
    if(!ok && err == ENOSYS) {
        skips++;
        printf("  skip  %s (%s)\n", what, strerror(ENOSYS));
        return;
    }
    check(ok, what);
}

static const char *in_dir(const char *name) {
    static char paths[4][PATH_MAX];
    static int next;
    char *p = paths[next++ & 3];
    snprintf(p, PATH_MAX, "%s/%s", dir, name);
    return p;
}

static unsigned char pattern(unsigned long i) {
    return (unsigned char)((i * 7) ^ (i >> 9) ^ 0x5a);
}

static void test_descriptors(void) {
    const char *a = in_dir("a.txt");
    char buf[64];
    struct stat st;
    int fd, fd2, r;

    fd = open(a, O_WRONLY | O_CREAT | O_EXCL);
    check(fd >= 0, "open(O_CREAT | O_EXCL) makes a new file");
    r = open(a, O_WRONLY | O_CREAT | O_EXCL);
    check(r < 0 && errno == EEXIST, "open(O_CREAT | O_EXCL) of an existing file is EEXIST");
    check(write(fd, "hello world\n", 12) == 12, "write 12 bytes");
    check(lseek(fd, 0, SEEK_CUR) == 12, "lseek(SEEK_CUR) is 12 after the write");
    check(fstat(fd, &st) == 0 && S_ISREG(st.type) && st.size == 12, "fstat: a regular file of 12 bytes");
    check(fsync(fd) == 0, "fsync");
    check(close(fd) == 0, "close");
    check(close(fd) < 0 && errno == EBADF, "close again is EBADF");
    check(write(fd, "x", 1) < 0 && errno == EBADF, "write to a closed descriptor is EBADF");

    fd = open(a, O_RDONLY);
    memset(buf, 0, sizeof(buf));
    check(fd >= 0 && read(fd, buf, sizeof(buf)) == 12 && !memcmp(buf, "hello world\n", 12), "read it back");
    check(read(fd, buf, sizeof(buf)) == 0, "read at the end returns 0");
    check(lseek(fd, 6, SEEK_SET) == 6 && read(fd, buf, 5) == 5 && !memcmp(buf, "world", 5), "lseek(SEEK_SET) then read");
    check(lseek(fd, -6, SEEK_END) == 6, "lseek(SEEK_END)");
    check(write(fd, "x", 1) < 0 && errno == EBADF, "write to a read-only descriptor is EBADF");
    check(ftruncate(fd, 1) < 0 && errno == EBADF, "ftruncate of a read-only descriptor is EBADF");

    fd2 = dup2(fd, 10);
    check(fd2 == 10 && lseek(10, 0, SEEK_SET) == 0 && read(fd, buf, 5) == 5 && !memcmp(buf, "hello", 5),
          "dup2: both descriptors share one position");
    check(close(10) == 0 && read(fd, buf, 1) == 1, "the original survives closing the copy");
    close(fd);

    fd = open(a, O_RDWR);
    check(fd >= 0 && ftruncate(fd, 5) == 0 && fstat(fd, &st) == 0 && st.size == 5, "ftruncate to 5 bytes");
    memset(buf, 0, sizeof(buf));
    check(lseek(fd, 0, SEEK_SET) == 0 && read(fd, buf, sizeof(buf)) == 5 && !strcmp(buf, "hello"), "the first 5 bytes are left");
    check(lseek(fd, 0, SEEK_END) == 5 && write(fd, "!", 1) == 1 && fstat(fd, &st) == 0 && st.size == 6, "append after it");
    check(ftruncate(fd, 0) == 0 && fstat(fd, &st) == 0 && st.size == 0, "ftruncate to 0");
    check(close(fd) == 0 && stat(a, &st) == 0 && st.size == 0, "stat agrees after close");

    fd = open(a, O_WRONLY | O_APPEND);
    check(fd >= 0 && write(fd, "one\n", 4) == 4 && lseek(fd, 0, SEEK_SET) == 0 && write(fd, "two\n", 4) == 4, "O_APPEND writes");
    close(fd);
    fd = open(a, O_RDONLY);
    memset(buf, 0, sizeof(buf));
    check(fd >= 0 && read(fd, buf, sizeof(buf)) == 8 && !strcmp(buf, "one\ntwo\n"), "O_APPEND always wrote at the end");
    close(fd);
    fd = open(a, O_WRONLY | O_TRUNC);
    check(fd >= 0 && fstat(fd, &st) == 0 && st.size == 0, "O_TRUNC empties the file");
    close(fd);
}

static void test_names(void) {
    const char *a = in_dir("a.txt"), *b = in_dir("b.txt"), *sub = in_dir("sub"), *inner = in_dir("sub/f");
    char cwd[PATH_MAX], back[PATH_MAX];
    struct stat st;
    int fd, r;

    r = rename(a, b);
    check_or_skip(r == 0 && stat(b, &st) == 0 && stat(a, &st) < 0 && errno == ENOENT, r < 0 ? errno : 0, "rename a.txt to b.txt");
    if(r == 0) {
        fd = open(a, O_WRONLY | O_CREAT);
        if(fd >= 0) { write(fd, "new", 3); close(fd); }
        r = rename(a, b);
        check(r == 0 && stat(b, &st) == 0 && st.size == 3 && stat(a, &st) < 0, "rename over an existing file replaces it");
        r = rename(b, a);
        check(r == 0, "rename it back");
        check(rename(in_dir("missing"), b) < 0 && errno == ENOENT, "rename of a missing file is ENOENT");
    }

    check(unlink(a) == 0, "unlink");
    check(stat(a, &st) < 0 && errno == ENOENT, "stat of the removed file is ENOENT");
    check(unlink(a) < 0 && errno == ENOENT, "unlink again is ENOENT");
    check(open(a, O_RDONLY) < 0 && errno == ENOENT, "open of a missing file is ENOENT");

    check(mkdir(sub, 0777) == 0 && stat(sub, &st) == 0 && S_ISDIR(st.type), "mkdir");
    check(mkdir(sub, 0777) < 0 && errno == EEXIST, "mkdir of an existing directory is EEXIST");
    fd = open(inner, O_WRONLY | O_CREAT);
    check(fd >= 0 && close(fd) == 0, "a file inside it");
    check(rmdir(sub) < 0 && errno == ENOTEMPTY, "rmdir of a directory with a file is ENOTEMPTY");
    check(open(sub, O_WRONLY) < 0 && errno == EISDIR, "open of a directory for writing is EISDIR");
    check(unlink(sub) < 0 && errno == EISDIR, "unlink of a directory is EISDIR");
    check(rmdir(inner) < 0 && errno == ENOTDIR, "rmdir of a file is ENOTDIR");
    check(open(inner, O_RDONLY | O_DIRECTORY) < 0 && errno == ENOTDIR, "O_DIRECTORY on a file is ENOTDIR");
    fd = open(sub, O_RDONLY);
    check(fd >= 0 && read(fd, cwd, 1) < 0 && errno == EISDIR, "read of a directory is EISDIR");
    if(fd >= 0) close(fd);

    check(getcwd(back, sizeof(back)) != NULL && chdir(sub) == 0 && getcwd(cwd, sizeof(cwd)) && !strcmp(cwd, sub),
          "chdir and getcwd");
    check(stat("f", &st) == 0 && S_ISREG(st.type), "a relative path finds the file");
    check(chdir(back) == 0, "chdir back");
    check(unlink(inner) == 0 && rmdir(sub) == 0 && stat(sub, &st) < 0, "unlink and rmdir clean up");
}

static void test_directories(void) {
    static const char *const names[] = { "x1", "x2", "x3" };
    struct dirent *de;
    DIR *d;
    int seen = 0, other = 0, i, fd;

    for(i = 0; i < 3; i++) {
        fd = open(in_dir(names[i]), O_WRONLY | O_CREAT | O_TRUNC);
        if(fd >= 0) { write(fd, names[i], 2); close(fd); }
    }
    d = opendir(dir);
    check(d != NULL, "opendir");
    while(d && (de = readdir(d))) {
        for(i = 0; i < 3; i++) if(!strcmp(de->d_name, names[i]) && de->d_type == DT_REG && de->size == 2) break;
        if(i < 3) seen |= 1 << i;
        else other++;
    }
    if(d) closedir(d);
    check(seen == 7 && (!other || !made), "readdir lists the three files, with type and size");
    check(opendir(in_dir("nothing")) == NULL && errno == ENOENT, "opendir of a missing directory is ENOENT");
    for(i = 0; i < 3; i++) unlink(in_dir(names[i]));
}

static void test_long_name(void) {
    const char *p = in_dir(LONG_NAME);
    struct dirent *de;
    struct stat st;
    DIR *d;
    int fd, found = 0;
    fd = open(p, O_WRONLY | O_CREAT | O_TRUNC);
    check(fd >= 0 && write(fd, "long", 4) == 4 && close(fd) == 0, "create \"" LONG_NAME "\"");
    check(stat(p, &st) == 0 && st.size == 4, "stat finds it under its long name");
    d = opendir(dir);
    while(d && (de = readdir(d))) if(!strcmp(de->d_name, LONG_NAME)) found = 1;
    if(d) closedir(d);
    check(found, "readdir returns the long name exactly");
    check(unlink(p) == 0, "unlink it");
}

static void test_stdio(void) {
    const char *s = in_dir("stdio.txt");
    char line[64], block[300], back[300];
    FILE *f;
    long size;
    int i, ok;

    f = fopen(s, "w");
    check(f != NULL, "fopen(\"w\")");
    if(!f) return;
    for(i = 1; i <= 3; i++) fprintf(f, "line %d of %s\n", i, "three");
    check(fclose(f) == 0, "fprintf and fclose");

    f = fopen(s, "r");
    ok = f != NULL;
    for(i = 1; ok && i <= 3; i++) {
        char want[64];
        snprintf(want, sizeof(want), "line %d of three\n", i);
        ok = fgets(line, sizeof(line), f) && !strcmp(line, want);
    }
    check(ok && !fgets(line, sizeof(line), f) && feof(f), "fgets reads the three lines, then end of file");
    if(f) fclose(f);

    f = fopen(s, "a");
    check(f && fputs("line 4 appended\n", f) >= 0 && fclose(f) == 0, "fopen(\"a\") appends");
    f = fopen(s, "r");
    ok = 0;
    if(f) {
        for(i = 0; fgets(line, sizeof(line), f); i++) ok = (i == 3 && !strcmp(line, "line 4 appended\n"));
        fclose(f);
    }
    check(ok, "the appended line is the fourth");

    for(i = 0; i < (int)sizeof(block); i++) block[i] = (char)(i * 13);
    f = fopen(s, "w+");
    check(f && fwrite(block, 1, sizeof(block), f) == sizeof(block), "fwrite 300 bytes (\"w+\")");
    if(!f) return;
    check(fseek(f, 0, SEEK_END) == 0 && (size = ftell(f)) == (long)sizeof(block), "fseek(SEEK_END) and ftell give the size");
    check(fseek(f, 100, SEEK_SET) == 0 && fread(back, 1, 50, f) == 50 && !memcmp(back, block + 100, 50) && ftell(f) == 150,
          "fseek, fread and ftell in the middle");
    check(fgetc(f) == (unsigned char)block[150] && ungetc('Q', f) == 'Q' && fgetc(f) == 'Q' && fgetc(f) == (unsigned char)block[151],
          "fgetc and ungetc");
    rewind(f);
    check(fread(back, 1, sizeof(back), f) == sizeof(block) && !memcmp(back, block, sizeof(block)) && fread(back, 1, 1, f) == 0 && feof(f),
          "rewind and read everything back");
    check(fseek(f, 10, SEEK_SET) == 0 && fwrite("XY", 1, 2, f) == 2 && fseek(f, 9, SEEK_SET) == 0 && fread(back, 1, 4, f) == 4 &&
          back[0] == block[9] && back[1] == 'X' && back[2] == 'Y' && back[3] == block[12], "write in the middle after a seek (\"r+\" style)");
    check(fclose(f) == 0, "fclose");
    check(fopen(in_dir("nothing/x"), "r") == NULL && errno == ENOENT, "fopen of a missing file is ENOENT");
    check(remove(s) == 0, "remove");
}

static void test_big(void) {
    const char *p = in_dir("big.bin");
    unsigned char *buf = malloc(BIG_CHUNK);
    unsigned long i, done;
    unsigned int t0, t1, t2;
    struct statfs before, after;
    struct stat st;
    int fd, ok = 1, have_sizes;

    if(!buf) { check(0, "memory for the 1 MB test"); return; }
    have_sizes = statfs(dir, &before) == 0 && before.units;
    t0 = uptime();
    fd = open(p, O_WRONLY | O_CREAT | O_TRUNC);
    for(done = 0; fd >= 0 && ok && done < BIG; done += BIG_CHUNK) {
        for(i = 0; i < BIG_CHUNK; i++) buf[i] = pattern(done + i);
        ok = write(fd, buf, BIG_CHUNK) == BIG_CHUNK;
    }
    check(fd >= 0 && ok && fsync(fd) == 0 && close(fd) == 0, "write 1 MB in 4 KB pieces and fsync");
    t1 = uptime();
    check(stat(p, &st) == 0 && st.size == BIG, "its size is 1048576");
    fd = open(p, O_RDONLY);
    ok = fd >= 0;
    for(done = 0; ok && done < BIG; done += BIG_CHUNK) {
        ok = read(fd, buf, BIG_CHUNK) == BIG_CHUNK;
        for(i = 0; ok && i < BIG_CHUNK; i++) ok = buf[i] == pattern(done + i);
    }
    check(ok && read(fd, buf, 1) == 0, "read back: every byte matches the pattern");
    if(fd >= 0) close(fd);
    t2 = uptime();
    printf("        (write %u ms, read %u ms)\n", t1 - t0, t2 - t1);
    if(have_sizes) {
        check(statfs(dir, &after) == 0 && (unsigned long long)(before.free_units - after.free_units) * after.unit >= BIG,
              "statfs: the free space went down by at least 1 MB");
    }
    check(unlink(p) == 0, "unlink the 1 MB file");
    free(buf);
}

static void test_statfs(void) {
    struct statfs st;
    int r = statfs(dir, &st);
    check(r == 0, "statfs");
    if(r) return;
    if(!st.units) {
        skips++;
        printf("  skip  statfs sizes (%s reports none)\n", st.fs);
        return;
    }
    check(st.unit >= 512 && st.free_units <= st.units, "statfs: unit size and free units make sense");
    check(st.path[0] == '/', "statfs names the mount point");
}

int main(int argc, char **argv) {
    struct stat st;
    size_t n;
    if(argc != 2) { fprintf(stderr, "usage: fstest <dir>   (made if missing, removed again)\n"); return 2; }
    if(argv[1][0] == '/') snprintf(dir, sizeof(dir), "%s", argv[1]);
    else {                                              /* absolute, so getcwd() can be compared with it */
        char cwd[PATH_MAX];
        snprintf(dir, sizeof(dir), "%s/%s", getcwd(cwd, sizeof(cwd)) ? (strcmp(cwd, "/") ? cwd : "") : ".", argv[1]);
    }
    n = strlen(dir);
    while(n > 1 && dir[n - 1] == '/') dir[--n] = 0;
    if(stat(dir, &st) < 0) {
        if(mkdir(dir, 0777) < 0) { fprintf(stderr, "fstest: %s: %s\n", dir, strerror(errno)); return 1; }
        made = 1;
    } else if(!S_ISDIR(st.type)) {
        fprintf(stderr, "fstest: %s: not a directory\n", dir);
        return 1;
    }
    printf("fstest in %s (%s)\n", dir, made ? "made for the test" : "already there");
    test_descriptors();
    test_names();
    test_directories();
    test_long_name();
    test_stdio();
    test_statfs();
    test_big();
    if(made) check(rmdir(dir) == 0, "rmdir the test directory");
    printf("%d checks, %d failed, %d skipped\n", checks, failures, skips);
    puts(failures ? "FSTEST FAIL" : "FSTEST PASS");
    return failures ? 1 : 0;
}
