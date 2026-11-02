/*
SurfOS user program: patch
--------------------
File: patch.c   Date: 11/01/26
--------------------
patch [-p n] [-R] [-i patchfile] [-o outfile] [-d dir] [-N] [-f] [--dry-run] [file [patchfile]]: applies
a diff to the files it names. The patch is unified, context or normal, read from patchfile, or
standard input without one; text around it is passed over. file says what to patch, whatever the
patch names; a normal diff names nothing, so it needs file, or the diff line of diff -r before it.

  -p n       take n leading parts off the names in the patch. Without -p only a name's last part
             is used. /dev/null is never touched by this: see below
  -R         the patch was made the other way round: undo it
  -i file    the patch is in this file
  -o file    the results go there (- for standard output), one after the other, and the files stay.
             The file is written only if all of the patch went in and there is something to write;
             otherwise it stays as it was, and a new one is not made
  -d dir     go to dir first, before anything else
  -N         a patch that looks reversed or already applied is skipped: its hunks go into the .rej,
             and the status is 1. patch never asks, so this is what happens without -N too
  -f         no such look: each hunk simply goes in, or fails
  --dry-run  read and check everything, and change nothing. A file named twice is checked both times as
             it is now, while a real run works on the first result, so the two can differ

Long names too: --strip=n --reverse --input=f --output=f --directory=d --forward --force.

A hunk that is not where it says is looked for nearby (the offset is said), and if need be with up to
two of its context lines at each end not compared (the fuzz is said). The file as it was is then
kept as <file>.orig, unless a file has that name already: that one stays, and it is said. One that
fits nowhere goes into <file>.rej, in unified form, and the others still go in. The .rej adds up
over a run; one that was there before stays, and the hunks go to <file>.1.rej and so on, as is
said. A file whose old name is /dev/null is made, and one whose new name is /dev/null is deleted once
nothing is left of it. Never any question: where others ask, the safe answer is taken and said.

Status 0 all went in; 1 some hunks did not, a file was not there or would be made over one that has
text in it, or something is left in a file the patch would have deleted; 2 trouble: a patch that
is damaged, a name refused, a file that cannot be read or written.

Names from a patch are not trusted: one that is absolute or holds a .. is refused, after -p has
done its work, and the file is left alone. What is taken from a patch is cleaned before it is printed.
A file that exists is only ever replaced through saveutil.h, whole, once its new contents are safely
on the disk, so a failure at any point leaves it as it was. A new file is made with O_EXCL and
deleted again on a failure. A malformed hunk stops everything, and drops what was under way on that
file. Files patched before it stay patched. On FAT a file with a descriptor open cannot be replaced
(EBUSY), so a patch that names its own file leaves both as they were, with status 2.
*/

#include "fileutil.h"
#include "saveutil.h"
#include "../diff/cli.h"
#include "../diff/patch.h"

#define OUT_BLOCK 32768

/* a file being written: through saveutil, new, standard output, or nowhere (--dry-run) */
struct output {
    int open_now;
    int saved;                          /* through saveutil */
    int fd;
    int err;                            /* a write failed: errno */
    size_t used;
    unsigned long long bytes;
    char *buf;
    const char *path;
    int appending;                      /* a .rej this run began: added to, and never deleted */
    struct su_save save;
};

/* a .rej this run made, so that another section of its file adds to it */
struct rej_note {
    char *target, *name;
    struct rej_note *next;
};

static int strip = -1, reverse, no_check, dry_run;
static const char *output_name;
static FILE *talk;                      /* where "patching file" goes: not into the result of -o - */
static const char *input_name = "standard input";
static struct output out_file, rej_file, shared_out;
static struct rej_note *rej_notes;

static int usage(void) {
    fprintf(stderr, "usage: patch [-p n] [-R] [-i patchfile] [-o outfile] [-d dir] [-N] [-f] [--dry-run] [file [patchfile]]\n");
    return 2;
}

/* a name from a patch, for a message: one at a time */
static const char *show(const char *name) {
    static char buf[2 * PATH_MAX];
    return cli_clean(name, buf, sizeof(buf));
}

/* su_say(), with the names made fit to print */
static void say_save(const struct su_save *s, int r) {
    static struct su_save c;
    int e = errno;
    cli_clean(s->path, c.path, sizeof(c.path));
    cli_clean(s->tmp, c.tmp, sizeof(c.tmp));
    cli_clean(s->twin, c.twin, sizeof(c.twin));
    errno = e;
    su_say("patch", &c, r);
}

/**** writing ****/

static int out_flush(struct output *o) {
    size_t done = 0;
    if(dry_run) {
        o->used = 0;
        return 0;
    }
    while(done < o->used) {
        long w = write(o->fd, o->buf + done, o->used - done);
        if(w <= 0) {
            o->err = w ? errno : ENOSPC;
            return -1;
        }
        done += (size_t)w;
    }
    o->used = 0;
    return 0;
}

/* pa_put */
static int out_put(void *ctx, const void *p, unsigned n) {
    struct output *o = ctx;
    const char *s = p;
    o->bytes += n;
    while(n) {
        size_t k = OUT_BLOCK - o->used;
        if(k > n) k = n;
        memcpy(o->buf + o->used, s, k);
        o->used += k;
        s += k;
        n -= (unsigned)k;
        if(o->used == OUT_BLOCK && out_flush(o) < 0) return -1;
    }
    return 0;
}

/* the directories a new file's name goes through */
static void make_parents(const char *path) {
    char part[PATH_MAX];
    const char *p = path;
    while((p = strchr(p, '/'))) {
        size_t n = (size_t)(p - path);
        p++;
        if(!n || n >= sizeof(part)) continue;
        memcpy(part, path, n);
        part[n] = 0;
        mkdir(part, 0777);                      /* one that is there says so; what cannot be made fails at the open */
    }
}

static int out_ready(struct output *o, const char *path) {
    if(!o->buf && !(o->buf = malloc(OUT_BLOCK))) {
        fprintf(stderr, "patch: out of memory\n");
        return -1;
    }
    o->path = path;
    o->used = 0;
    o->err = 0;
    o->bytes = 0;
    o->saved = 0;
    o->appending = 0;
    o->open_now = 1;
    return 0;
}

/* path, to replace or to make: 0, or -1 said */
static int out_open(struct output *o, const char *path) {
    struct stat st;
    if(out_ready(o, path) < 0) return -1;
    if(dry_run) return 0;
    if(!strcmp(path, "-") && o == &shared_out) {
        o->fd = 1;
        return 0;
    }
    if(!stat(path, &st)) {
        if(S_ISDIR(st.st_mode)) {
            fprintf(stderr, "patch: %s: %s\n", show(path), strerror(EISDIR));
            o->open_now = 0;
            return -1;
        }
        if(su_begin(&o->save, path) < 0) {
            say_save(&o->save, -1);
            o->open_now = 0;
            return -1;
        }
        o->saved = 1;
        o->fd = o->save.fd;
        return 0;
    }
    make_parents(path);
    o->fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0666);
    if(o->fd < 0) {
        fprintf(stderr, "patch: %s: %s\n", show(path), strerror(errno));
        o->open_now = 0;
        return -1;
    }
    return 0;
}

/* the .rej for path: the one this run made, or a new name: a .rej that was there before is the user's, whatever it holds.
   name, of size bytes, is the one used. 0, or -1 said */
static int rej_open(const char *path, char *name, size_t size) {
    struct rej_note *n;
    int k;
    for(n = rej_notes; n; n = n->next)
        if(!strcmp(n->target, path)) break;
    if(n) {
        if(snprintf(name, size, "%s", n->name) >= (int)size || out_ready(&rej_file, name) < 0) return -1;
        rej_file.appending = 1;
        rej_file.fd = open(name, O_WRONLY | O_APPEND);
        if(rej_file.fd < 0) {
            fprintf(stderr, "patch: %s: %s\n", show(name), strerror(errno));
            rej_file.open_now = 0;
            return -1;
        }
        return 0;
    }
    make_parents(path);
    for(k = 0; k < 10; k++) {
        if(snprintf(name, size, k ? "%s.%d.rej" : "%s.rej", path, k) >= (int)size) {
            errno = ENAMETOOLONG;
            break;
        }
        if(out_ready(&rej_file, name) < 0) return -1;
        rej_file.fd = open(name, O_WRONLY | O_CREAT | O_EXCL, 0666);
        if(rej_file.fd >= 0) break;
        rej_file.open_now = 0;
        if(errno != EEXIST) break;
    }
    if(!rej_file.open_now) {
        fprintf(stderr, "patch: %s.rej: %s\n", show(path), strerror(errno));
        return -1;
    }
    n = malloc(sizeof(*n));
    if(n) {
        n->target = strdup(path);
        n->name = strdup(name);
        n->next = rej_notes;
        if(n->target && n->name) rej_notes = n;
    }
    if(k) {
        char other[PATH_MAX + 16];
        fprintf(stderr, "patch: %s.rej is there already, and stays: the rejects go to ", show(path));
        fprintf(stderr, "%s\n", cli_clean(name, other, sizeof(other)));
    }
    return 0;
}

static void rej_forget(const char *name) {
    struct rej_note **pp;
    for(pp = &rej_notes; *pp; pp = &(*pp)->next) {
        if(!strcmp((*pp)->name, name)) {
            *pp = (*pp)->next;
            return;
        }
    }
}

/* the old file stays, and what was new goes */
static void out_abort(struct output *o) {
    if(!o->open_now) return;
    o->open_now = 0;
    if(dry_run || (o == &shared_out && !strcmp(o->path, "-"))) return;
    if(o->saved) su_abort(&o->save);
    else {
        close(o->fd);
        if(!o->appending) {
            unlink(o->path);
            if(o == &rej_file) rej_forget(o->path);
        }
    }
}

/* it is all written: into place. 0, or -1 said */
static int out_commit(struct output *o) {
    int r;
    if(!o->open_now) return 0;
    if(dry_run || (o == &shared_out && !strcmp(o->path, "-"))) {
        if(out_flush(o) < 0) {
            fprintf(stderr, "patch: %s: %s\n", show(o->path), strerror(o->err));
            o->open_now = 0;
            return -1;
        }
        o->open_now = 0;
        return 0;
    }
    if(o->err || out_flush(o) < 0) {
        fprintf(stderr, "patch: %s: %s\n", show(o->path), strerror(o->err));
        out_abort(o);
        return -1;
    }
    o->open_now = 0;
    if(o->saved) {
        r = su_commit(&o->save);
        if(r) {
            say_save(&o->save, r);
            return -1;
        }
        return 0;
    }
    r = fsync(o->fd) < 0 ? errno : 0;
    if(close(o->fd) < 0 && !r) r = errno;
    if(r) {
        fprintf(stderr, "patch: %s: %s\n", show(o->path), strerror(r));
        if(!o->appending) {
            unlink(o->path);
            if(o == &rej_file) rej_forget(o->path);
        }
        return -1;
    }
    return 0;
}

/* the file as it was, in <path>.orig, before it is replaced: when a hunk did not go where it said, that old file may be
   wanted. O_EXCL: one that is there stays, and it is said. 0, or -1 said: the file is then not to be replaced, as what
   was to be kept could not be */
static int keep_old(const char *path) {
    static char name[PATH_MAX + 8];
    char *block;
    int in, out, r = 0;
    long n;
    if(snprintf(name, sizeof(name), "%s.orig", path) >= (int)sizeof(name)) {
        fprintf(stderr, "patch: %s.orig: %s\n", show(path), strerror(ENAMETOOLONG));
        return -1;
    }
    out = open(name, O_WRONLY | O_CREAT | O_EXCL, 0666);
    if(out < 0) {
        if(errno == EEXIST) {
            fprintf(stderr, "patch: %s is there already, and stays: the old file is not kept\n", show(name));
            return 0;
        }
        fprintf(stderr, "patch: %s: %s\n", show(name), strerror(errno));
        return -1;
    }
    in = open(path, O_RDONLY);
    block = malloc(OUT_BLOCK);
    if(in < 0 || !block) r = in < 0 ? errno : ENOMEM;
    while(!r && (n = read(in, block, OUT_BLOCK)) != 0) {
        long done = 0;
        if(n < 0) r = errno;
        while(!r && done < n) {
            long w = write(out, block + done, (size_t)(n - done));
            if(w <= 0) r = w ? errno : ENOSPC;
            else done += w;
        }
    }
    free(block);
    if(in >= 0) close(in);
    if(!r && fsync(out) < 0) r = errno;
    if(close(out) < 0 && !r) r = errno;
    if(r) {
        unlink(name);
        fprintf(stderr, "patch: %s: %s\n", show(name), strerror(r));
        return -1;
    }
    return 0;
}

/**** names ****/

/* name after -p: NULL when it has too few parts */
static const char *stripped(const char *name) {
    const char *p = name;
    int k;
    if(strip < 0) {
        const char *last = strrchr(name, '/');
        return last ? (last[1] ? last + 1 : NULL) : name;
    }
    for(k = 0; k < strip; k++) {
        if(!(p = strchr(p, '/'))) return NULL;
        while(*p == '/') p++;
    }
    return *p ? p : NULL;
}

static int safe_name(const char *s) {
    const char *p = s;
    if(*s == '/') return 0;
    while(*p) {
        const char *e = strchr(p, '/');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if(n == 2 && p[0] == '.' && p[1] == '.') return 0;
        p += n;
        while(*p == '/') p++;
    }
    return 1;
}

static int exists(const char *path) {
    struct stat st;
    return !stat(path, &st);
}

/* the file a section patches: 1 and *path, 0 no name will do (said), -1 a name was refused (said). The first that
   exists of the old name, the new and Index:'s; for a file to be made, the new name */
static int choose_file(const struct psection *s, const char *given, int creating, const char **path) {
    const char *names[3], *pick = NULL;
    const char *cands[3] = { s->old_name, s->new_name, s->index_name };
    int i, n = 0, new_at = -1;
    if(given) {
        *path = given;
        return 1;
    }
    for(i = 0; i < 3; i++) {
        const char *name;
        if(!cands[i][0] || !strcmp(cands[i], "/dev/null")) continue;
        name = stripped(cands[i]);
        if(!name) continue;
        if(!safe_name(name)) {
            fprintf(stderr, "patch: %s: refused: a name in a patch may not be absolute or hold ..\n", show(name));
            return -1;
        }
        if(i == 1) new_at = n;
        names[n++] = name;
    }
    if(!n) {
        fprintf(stderr, "patch: %s: no file name for this part of the patch\n", input_name);
        return 0;
    }
    for(i = 0; i < n && !pick; i++)
        if(exists(names[i])) pick = names[i];
    if(!pick && creating) pick = names[new_at >= 0 ? new_at : 0];
    if(!pick) {
        fprintf(stderr, "patch: %s: %s\n", show(names[0]), strerror(ENOENT));
        return 0;
    }
    *path = pick;
    return 1;
}

/**** a section ****/

/* a hunk out to <path>.rej */
static int reject(const struct psection *s, const struct phunk *h, const char *path, char *rej_name, size_t size) {
    if(dry_run) return 0;
    if(!rej_file.open_now) {
        char head[2 * PATCH_NAME_MAX + 16];
        if(rej_open(path, rej_name, size) < 0) return -1;
        snprintf(head, sizeof(head), "--- %s\n+++ %s\n", s->old_name[0] ? s->old_name : path, s->new_name[0] ? s->new_name : path);
        if(out_put(&rej_file, head, (unsigned)strlen(head)) < 0) return -1;
    }
    return pr_write(h, out_put, &rej_file);
}

static void say_hunk(const struct phunk *h, long long line, unsigned fuzz, long long off) {
    if(!off && !fuzz) return;
    fprintf(talk, "Hunk #%u succeeded at %lld", h->number, line);
    if(fuzz) fprintf(talk, " with fuzz %u", fuzz);
    if(off) fprintf(talk, " (offset %lld line%s)", off, off == 1 || off == -1 ? "" : "s");
    fputs(".\n", talk);
}

/* the section's hunks after a decision not to use them; -1: one was malformed */
static int skip_hunks(struct pinput *in, struct psection *s, struct phunk *h, int have, unsigned *count) {
    int r = have;
    while(r > 0) {
        ++*count;
        r = pp_next_hunk(in, s, h);
    }
    return r;
}

/* 0, 1 or 2 as the status; 3: stop, the input cannot be read on */
static int one_section(struct pinput *in, struct psection *s, const char *given, struct phunk *h) {
    static char rej_name[PATH_MAX + 16];
    struct output *o = output_name ? &shared_out : &out_file;
    struct ptarget t;
    struct papply a;
    struct dsrc src;
    const char *path = NULL;
    unsigned total = 0, failed = 0, applied = 0;
    int r, rc = 0, have_src = 0, old_null, new_null, creating, skipping = 0, gone, moved = 0;
    if(reverse) ps_reverse(s);
    old_null = !strcmp(s->old_name, "/dev/null");
    new_null = !strcmp(s->new_name, "/dev/null");
    if((r = pp_next_hunk(in, s, h)) < 0) goto malformed;
    if(!r) return 0;
    if(reverse) ph_reverse(h);
    creating = old_null || (h->old_count == 0 && h->old_start <= 1);
    r = choose_file(s, given, creating, &path);
    if(r <= 0) {
        int bad_name = r < 0;
        if(skip_hunks(in, s, h, 1, &total) < 0) goto malformed;
        fprintf(talk, "%u hunk%s skipped\n", total, total == 1 ? "" : "s");
        return bad_name ? 2 : 1;
    }
    fprintf(talk, "%s file %s\n", dry_run ? "checking" : "patching", show(path));
    if(exists(path)) {
        struct stat st;
        if(old_null && !stat(path, &st) && st.st_size > 0) {
            fprintf(stderr, "patch: %s: already there, and the patch would make it\n", show(path));
            if(skip_hunks(in, s, h, 1, &total) < 0) goto malformed;
            fprintf(talk, "%u hunk%s skipped\n", total, total == 1 ? "" : "s");
            return 1;
        }
        if(dsrc_open(&src, path) < 0) {
            fprintf(stderr, "patch: %s: %s\n", show(path), strerror(errno));
            if(skip_hunks(in, s, h, 1, &total) < 0) goto malformed;
            return 2;
        }
        have_src = 1;
    } else if(!creating) {
        fprintf(stderr, "patch: %s: %s\n", show(path), strerror(ENOENT));
        if(skip_hunks(in, s, h, 1, &total) < 0) goto malformed;
        fprintf(talk, "%u hunk%s skipped\n", total, total == 1 ? "" : "s");
        return 1;
    }
    if(pt_load(&t, have_src ? &src : NULL) < 0) {
        fprintf(stderr, "patch: %s: %s\n", show(path), strerror(errno));
        if(have_src) dsrc_close(&src);
        if(skip_hunks(in, s, h, 1, &total) < 0) goto malformed;
        return 2;
    }
    pa_start(&a, &t, out_put, o);
    for(r = 1; r > 0; ) {
        unsigned at = 0, fuzz = 0;
        int found = 0;
        total++;
        if(!skipping) {
            int back = 0;
            found = pa_locate(&a, h, &at, &fuzz, h->number == 1 && !no_check ? &back : NULL);
            if(found < 0) goto read_failed;
            if(back) {
                fprintf(talk, "Reversed (or previously applied) patch detected!  Skipping patch.\n");
                skipping = 1;
            }
        }
        if(found) {
            if(!o->open_now) {                  /* only now: nothing is touched for a patch that does not go in */
                if(out_open(o, output_name ? output_name : path) < 0) {
                    rc = 2;
                    skipping = 1;
                    found = 0;
                }
            }
        }
        if(found) {
            if(pa_apply(&a, h, at, fuzz) < 0) {
                if(a.failed) goto read_failed;
                goto write_failed;
            }
            applied++;
            if(fuzz || a.last_offset) moved = 1;
            say_hunk(h, a.last_line, fuzz, a.last_offset);
        } else {
            if(!skipping) fprintf(talk, "Hunk #%u FAILED at %lld.\n", h->number, (long long)h->old_start + a.added);
            failed++;
            if(reject(s, h, path, rej_name, sizeof(rej_name)) < 0) rc = 2;
        }
        r = pp_next_hunk(in, s, h);
        if(r > 0 && reverse) ph_reverse(h);
    }
    if(r < 0) {
        out_abort(o);
        out_abort(&rej_file);
        pt_free(&t);
        if(have_src) dsrc_close(&src);
        goto malformed;
    }
    if(applied) {
        if(pa_finish(&a) < 0) {
            if(a.failed) goto read_failed;
            goto write_failed;
        }
    }
    gone = applied && new_null && !output_name && !failed && !o->bytes;
    pt_free(&t);
    if(have_src) dsrc_close(&src);
    if(!output_name) {
        if(have_src && applied && (moved || failed) && !dry_run && keep_old(path) < 0) {
            out_abort(o);
            rc = 2;
        } else if(gone) {
            out_abort(o);
            if(!dry_run && unlink(path) < 0) {
                fprintf(stderr, "patch: %s: %s\n", show(path), strerror(errno));
                rc = 2;
            }
        } else {
            if(applied && new_null && !failed) {
                fprintf(talk, "Not deleting %s: there is more in it than the patch had\n", show(path));
                if(!rc) rc = 1;
            }
            if(out_commit(o) < 0) rc = 2;
        }
    }
    if(failed) {
        const char *word = skipping ? "ignored" : "FAILED";
        if(rej_file.open_now) fprintf(talk, "%u out of %u hunk%s %s -- saving rejects to file %s\n", failed, total,
                                      total == 1 ? "" : "s", word, show(rej_name));
        else fprintf(talk, "%u out of %u hunk%s %s\n", failed, total, total == 1 ? "" : "s", word);
    }
    if(out_commit(&rej_file) < 0) rc = 2;
    if(failed && !rc) rc = 1;
    return rc;
read_failed:
    fprintf(stderr, "patch: %s: %s\n", show(path), strerror(a.failed));
    out_abort(o);
    out_abort(&rej_file);
    pt_free(&t);
    if(have_src) dsrc_close(&src);
    return 3;
write_failed:
    fprintf(stderr, "patch: %s: %s\n", show(o->path), strerror(o->err));
    out_abort(o);
    out_abort(&rej_file);
    pt_free(&t);
    if(have_src) dsrc_close(&src);
    return 3;
malformed:
    fprintf(stderr, "patch: %s: %s\n", input_name, in->why);
    return 3;
}

/* the end of -o: it takes its place only if all of the patch went in, and there is something to put there. What standard
   output has had, it has had */
static int finish_output(int rc) {
    if(!output_name) return rc;
    if(!strcmp(output_name, "-")) {
        if(out_commit(&shared_out) < 0) rc = 2;
    } else if(rc || !shared_out.bytes) {
        out_abort(&shared_out);
        if(rc && !dry_run) fprintf(stderr, "patch: %s: not written: not all of the patch went in\n", show(output_name));
    } else if(out_commit(&shared_out) < 0) rc = 2;
    return rc;
}

/* -x n, -xn and --name=n: the option's value, or NULL for another option */
static const char *value_of(int argc, char **argv, int *i, const char *shortname, const char *longname) {
    size_t n = strlen(longname);
    if(!strncmp(argv[*i], longname, n) && argv[*i][n] == '=') return argv[*i] + n + 1;
    if(strncmp(argv[*i], shortname, 2)) return NULL;
    if(argv[*i][2]) return argv[*i] + 2;
    if(*i + 1 >= argc) return NULL;
    return argv[++*i];
}

int main(int argc, char **argv) {
    static struct dsrc src;
    static struct pinput in;
    static struct psection sec;
    static struct phunk hunk;
    const char *patch_file = NULL, *dir = NULL, *given = NULL, *operand[2];
    int i = 1, rc = 0, r, operands = 0;
    lu_prog = fu_prog = "patch";
    for(; i < argc; i++) {                      /* options and files in any order, as elsewhere */
        const char *v;
        if(!strcmp(argv[i], "--")) {
            for(i++; i < argc; i++) {
                if(operands == 2) return usage();
                operand[operands++] = argv[i];
            }
            break;
        }
        if(argv[i][0] != '-' || !argv[i][1]) {
            if(operands == 2) return usage();
            operand[operands++] = argv[i];
        } else if(!strcmp(argv[i], "-R") || !strcmp(argv[i], "--reverse")) reverse = 1;
        else if(!strcmp(argv[i], "-N") || !strcmp(argv[i], "--forward")) continue;     /* never asked: see above */
        else if(!strcmp(argv[i], "-f") || !strcmp(argv[i], "--force")) no_check = 1;
        else if(!strcmp(argv[i], "--dry-run")) dry_run = 1;
        else if((v = value_of(argc, argv, &i, "-p", "--strip"))) {
            if(cli_int(v, &strip) < 0) {
                fprintf(stderr, "patch: %s: not a number of parts\n", v);
                return 2;
            }
        } else if((v = value_of(argc, argv, &i, "-i", "--input"))) patch_file = v;
        else if((v = value_of(argc, argv, &i, "-o", "--output"))) output_name = v;
        else if((v = value_of(argc, argv, &i, "-d", "--directory"))) dir = v;
        else return usage();
    }
    if(operands > 0) given = operand[0];
    if(operands > 1) {
        if(patch_file) return usage();
        patch_file = operand[1];
    }
    if(dir && chdir(dir) < 0) {
        fprintf(stderr, "patch: %s: %s\n", dir, strerror(errno));
        return 2;
    }
    talk = output_name && !strcmp(output_name, "-") ? stderr : stdout;
    if(patch_file && strcmp(patch_file, "-")) {
        input_name = patch_file;
        if(dsrc_open(&src, patch_file) < 0) {
            fprintf(stderr, "patch: %s: %s\n", patch_file, strerror(errno));
            return 2;
        }
    } else dsrc_stream(&src, 0);
    if(pi_start(&in, &src) < 0) {
        fprintf(stderr, "patch: out of memory\n");
        return 2;
    }
    while((r = pp_next_section(&in, &sec)) > 0) {
        int one = one_section(&in, &sec, given, &hunk);
        if(in.stray) {
            fprintf(stderr, "patch: %s: line %lu: past its hunk's counts, and passed over\n", input_name, in.stray);
            in.stray = 0;
        }
        if(one == 3) {
            rc = finish_output(2);
            cli_flush("patch");
            return rc;
        }
        if(one > rc) rc = one;
    }
    if(r < 0) {
        fprintf(stderr, "patch: %s: %s\n", input_name, in.why);
        rc = 2;
    } else if(!in.started && in.lineno) {
        fprintf(stderr, "patch: %s: only garbage was found in it\n", input_name);
        rc = 2;
    }
    rc = finish_output(rc);
    if(cli_flush("patch")) rc = 2;
    ph_free(&hunk);
    pi_stop(&in);
    return rc;
}
