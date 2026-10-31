/*
SurfOS shell: surfsh
--------------------
File: main.c    Date: 10/6/26, 10/24/26
--------------------
surfsh [-l]: the SurfOS shell. init starts it with -l on every console (F1 to F4)
and again when it ends; typed at a prompt, it starts a shell inside the shell.

The settings come from /initrd/etc/surfsh.conf when it exists: "key = value"
lines, # comments, the keys of the set command. Then -l prints the banner as the
kernel's do_banner() does (include/surfos/version.h): "Brandon Burr's SurfOS" and
"Version 1.0 on an x86, 2004-2026" in light red, the version and the years in
orange (no colours with prompt.color off). The shell starts in the directory it
was started in ("/" when init starts it).

On the console the line is typed in the line editor (editor.c); with any other
input (surfsh < file) the lines are read and run without a prompt. Either way
the shell ends with exit, at the end of the input or with Ctrl-D on an empty line.
Before each prompt (each line of a file) the jobs in the background that ended are
reported. The exit status is kept in 0-255, as on Unix: init takes -1 for a shell
that was killed.

10/24/26: surfsh -c TEXT [NAME [ARGS]] runs TEXT ($0 NAME, $1... ARGS), and surfsh FILE [ARGS]
the lines of FILE ($0 FILE), without a prompt. The environment becomes exported variables.
10/25/26: . files and eval run their text inside a command (run_source() within run_source());
an interactive shell asks to be told of Ctrl+C itself (TTY_INTR 1): when it stopped commands of
surfsh's own and nobody said so, ^C is written before the next prompt.
Lines are read until they make a complete command (parse.c), which then runs (exec.c):
at the prompt a line that is not complete yet is continued on the next one, after the
continuation prompt (… ≋>); Ctrl-C there drops the command. A file that ends inside a
command, and a mistake in a line of one, end it with status 2. Input that is not the
console is read a byte at a time from a pipe, and a line at a time from a file (put back
where the command's own reading begins), so that a command reads what follows it. The
pager pages each command typed at the prompt as a whole. surfsh --sub STATUS PID is a new
surfsh that another one started (exec.c): it reads what to run from its descriptor 3, with
$? and $$ as they were there. An interactive surfsh lives on when it writes to a pipe nobody
reads (broken_pipe_exit(0)); one that runs a script or a subshell ends, as other programs.
10/25/26: an interactive surfsh draws its prompt and line and writes its reports on a copy of
its terminal (term_fd, among the shell's own descriptors 10-15), so exec > file or
{ ...; } > file takes only what the commands write. A script's file is read on such a
descriptor too, out of the way of its exec 3< and the like; a terminal that is no
interactive shell's input (surfsh > log) is read a line at a time. A . file or eval's text
in which no command runs answers 0, and $? there is the status before it.

10/25/26 (track A, A10): an interactive surfsh runs the owner's startup file after surfsh.conf:
surfos-config/surfshrc on the stick SurfOS was started from (/dev/config/surfshrc, fs/devfs.c),
as a . file runs. It loads the history kept there (/dev/config/history, there only while the
history setting is on: settings history on) and saves it again when it ends, on history -w and
after every HIST_SAVE_EVERY lines typed: what the file holds by then (another session's lines
too), then the lines typed here since the last save, the last HIST_MAX of them in at most
HIST_FILE_MAX bytes. A line that starts with a space was never kept. And it sets HOSTNAME, not
exported, from /dev/config/hostname (the hostname setting), which a telnet session's prompt
shows (≋@>, shell/prompt.c), when the environment has none and the kernel has one.
The reviews' fixes (10/25/26): a save that another shell's came between (close says EAGAIN:
the kernel refuses a file older than the stick's) reads the file again and merges, 8 times at
most; after a periodic save that failed the next comes HIST_SAVE_EVERY lines later, and only
the first failure is said. An exit in the startup file leaves the file, as return does, and a
Ctrl+C stops the rest of it; a startup file that cannot be read is said; a CR before a line's
end (a file written on Windows) is dropped there and in the history.
*/

#include "surfsh.h"
#include <surfos/version.h>

struct settings settings = { 1, 1, 5 };
int last_status;
int exit_requested, exit_code;
int interactive;
int shell_pid;
int term_fd = 1;

/**** the settings ****/

static int caps = -1;

static int console_caps(void) {
    if(caps < 0) {
        caps = ttyctl(TTY_CAPS, 0);
        if(caps < 0) caps = 0;          /* a kernel without TTY_CAPS: plain ASCII, no colours */
    }
    return caps;
}

int use_unicode(void) {
    return settings.unicode && (console_caps() & TTY_CAP_UTF8);
}

int use_color(void) {
    return settings.color && (console_caps() & TTY_CAP_COLOR);
}

int display_cols(const char *s, int n) {
    int i, c = 0;
    for(i = 0; i < n; i++) if(((unsigned char)s[i] & 0xC0) != 0x80) c++;
    return c;
}

static int on_off(const char *value, int *out) {
    if(!strcmp(value, "on")) *out = 1;
    else if(!strcmp(value, "off")) *out = 0;
    else return -1;
    return 0;
}

int settings_apply(const char *key, const char *value, char *why, int size) {
    if(!strcmp(key, "prompt.unicode") || !strcmp(key, "prompt.color")) {
        int v;
        if(on_off(value, &v)) return snprintf(why, (size_t)size, "%s is on or off, not %s", key, value), -1;
        if(key[7] == 'u') settings.unicode = v;
        else settings.color = v;
        return 0;
    }
    if(!strcmp(key, "prompt.max_depth")) {
        char *end;
        long v = strtol(value, &end, 10);
        if(!*value || *end || v < 2 || v > 16)
            return snprintf(why, (size_t)size, "%s is a number from 2 to 16, not %s", key, value), -1;
        settings.max_depth = (int)v;
        return 0;
    }
    snprintf(why, (size_t)size, "no setting called %s (set alone lists them)", key);
    return -1;
}

int settings_show(const char *key) {
    int all = !key, shown = 0;
    if(all || !strcmp(key, "prompt.unicode")) {
        printf("prompt.unicode   = %s%s\n", settings.unicode ? "on" : "off",
               settings.unicode && !(console_caps() & TTY_CAP_UTF8) ? "   (this console draws no UTF-8: the prompt is ASCII)" : "");
        shown++;
    }
    if(all || !strcmp(key, "prompt.color")) {
        printf("prompt.color     = %s%s\n", settings.color ? "on" : "off",
               settings.color && !(console_caps() & TTY_CAP_COLOR) ? "   (this console draws no colours)" : "");
        shown++;
    }
    if(all || !strcmp(key, "prompt.max_depth")) {
        printf("prompt.max_depth = %d\n", settings.max_depth);
        shown++;
    }
    return shown ? 0 : -1;
}

/* /initrd/etc/surfsh.conf: "key = value" lines and # comments; a bad line is reported and skipped */
static void read_conf(const char *path) {
    char buf[256], why[120];
    FILE *f = fopen(path, "r");
    int n = 0;
    if(!f) return;                      /* none: the defaults */
    while(fgets(buf, sizeof(buf), f)) {
        char *key, *value, *p;
        size_t len = strlen(buf);
        n++;
        if(len && buf[len - 1] != '\n' && !feof(f)) {
            int c;
            while((c = fgetc(f)) != EOF && c != '\n');
            fprintf(stderr, "surfsh: %s:%d: the line is too long\n", path, n);
            continue;
        }
        if((p = strchr(buf, '#'))) *p = 0;
        for(key = buf; *key == ' ' || *key == '\t'; key++);
        for(p = key + strlen(key); p > key && isspace((unsigned char)p[-1]); p--);
        *p = 0;
        if(!*key) continue;
        value = strchr(key, '=');
        if(!value || value == key) {
            fprintf(stderr, "surfsh: %s:%d: not key = value: %s\n", path, n, key);
            continue;
        }
        for(p = value; p > key && (p[-1] == ' ' || p[-1] == '\t'); p--);
        *p = 0;
        for(value++; *value == ' ' || *value == '\t'; value++);
        if(settings_apply(key, value, why, sizeof(why))) fprintf(stderr, "surfsh: %s:%d: %s\n", path, n, why);
    }
    fclose(f);
}

/**** 10/25/26 (track A, A10): the startup file and the history on the stick ****/

static int hist_saved;              /* history_count() when the file was last read or written: the lines after are new */
static int hist_kept;               /* this shell keeps its history there (an interactive one) */
static int hist_failed, hist_retry; /* 10/25/26 (the review, U4): the last periodic save failed; the count for the next */

#define HIST_SAVE_TRIES 8           /* 10/25/26 (the review, 4): saves another shell's came between, read again and merged */

static int text_get(struct source *s, struct sbuf *out, int more);

/* 10/25/26 (the programs' review, U8): a CR before a line's end dropped in place, as netcfg does (a file written on
   Windows): the new length */
static int drop_crs(char *s, int len) {
    int i, j;
    for(i = j = 0; i < len; i++)
        if(s[i] != '\r' || (i + 1 < len && s[i + 1] != '\n')) s[j++] = s[i];
    s[j] = 0;
    return j;
}

/* a line more for lines (n of cap): 0, -1 without memory */
static int add_line(const char ***lines, int *n, int *cap, const char *l) {
    if(*n == *cap) {
        const char **more = realloc(*lines, (size_t)(*cap ? *cap * 2 : 256) * sizeof(**lines));
        if(!more) return -1;
        *lines = more;
        *cap = *cap ? *cap * 2 : 256;
    }
    (*lines)[(*n)++] = l;
    return 0;
}

/* one save: the file's lines (other sessions' too), then the new ones, the last HIST_MAX in at most HIST_FILE_MAX bytes,
   as the file again. The lines it holds now; -1 and errno (10/25/26, the kernel review's 4: EAGAIN when another shell
   saved after this one read the file) */
static int save_once(void) {
    struct sbuf out = { NULL, 0, 0 };
    char *old = NULL;
    const char **lines = NULL;
    int fd, n = 0, cap = 0, i, first, bytes = 0, r = 0, short_of = 0, e;
    ssize_t got, len = 0;
    if((fd = open(HISTORY_FILE, O_RDWR)) < 0) return -1;
    old = malloc(HIST_FILE_MAX + 1);
    while(old && len < HIST_FILE_MAX && (got = read(fd, old + len, HIST_FILE_MAX - len)) > 0) len += got;
    if(old) len = drop_crs(old, (int)len);         /* 10/25/26 (the review, U8) */
    for(i = 0; old && i < len && !short_of; ) {     /* the file's lines */
        char *nl = memchr(old + i, '\n', (size_t)(len - i));
        if(nl) *nl = 0;
        if(old[i] && add_line(&lines, &n, &cap, old + i)) short_of = 1;
        i = nl ? (int)(nl - old) + 1 : (int)len;
    }
    for(i = hist_saved; i < history_count() && !short_of; i++)     /* then the new ones still kept */
        if(history_line(i) && add_line(&lines, &n, &cap, history_line(i))) short_of = 1;
    first = n > HIST_MAX ? n - HIST_MAX : 0;
    for(i = n - 1; i >= first; i--) {               /* as many of the last as fit */
        if(bytes + (int)strlen(lines[i]) + 1 > HIST_FILE_MAX) {
            first = i + 1;
            break;
        }
        bytes += (int)strlen(lines[i]) + 1;
    }
    for(i = first; i < n; i++) {
        sb_puts(&out, lines[i]);
        sb_putc(&out, '\n');
    }
    if(!old || short_of) {
        r = -1;
        errno = ENOMEM;
    } else if(lseek(fd, 0, SEEK_SET) < 0 || (out.len && write(fd, sb_str(&out), (size_t)out.len) != out.len) ||
              ftruncate(fd, out.len) < 0) {
        r = -1;
    }
    if(close(fd) < 0 && !r) r = -1;                 /* the stick written: EIO when not (dmesg says why), EAGAIN */
    e = errno;
    free(lines);
    free(old);
    sb_free(&out);
    errno = e;
    return r ? -1 : n - first;
}

/* the history on the stick, again and merged while other shells' saves come between (8 tries). 0; -1, said when say:
   1 history -w (its outcome said), 2 a failure said (10/25/26, U4: the first periodic save that fails); a save not asked
   for when nothing new needs one is none */
int history_save(int say) {
    int kept, tries = 0;
    if(!hist_kept || (say != 1 && hist_saved >= history_count())) return 0;
    do kept = save_once();                          /* EAGAIN: read again, merged again */
    while(kept < 0 && errno == EAGAIN && ++tries < HIST_SAVE_TRIES);
    if(kept < 0) {
        if(say && errno == ENOENT) fprintf(stderr, "surfsh: history: not kept on the stick: the history setting is off "
                                           "(settings history on), or SurfOS was not started from one\n");
        else if(say) fprintf(stderr, "surfsh: history: not saved on the stick: %s%s%s\n",
                             errno == EAGAIN ? "other shells saved it each time it was merged" : strerror(errno),
                             errno == EIO ? " (dmesg says why)" : "",
                             say == 2 ? "; tried again after more lines" : "");
        return -1;
    }
    hist_saved = history_count();
    if(say == 1) printf("history: %d line%s in surfos-config/history on the stick\n", kept, kept == 1 ? "" : "s");
    return 0;
}

/* the history the stick keeps, as lines typed before this shell's */
static void history_load(void) {
    char *buf;
    ssize_t got, len = 0;
    int fd, i;
    hist_kept = 1;
    if((fd = open(HISTORY_FILE, O_RDONLY)) < 0) return;
    buf = malloc(HIST_FILE_MAX + 1);
    while(buf && len < HIST_FILE_MAX && (got = read(fd, buf + len, HIST_FILE_MAX - len)) > 0) len += got;
    close(fd);
    if(!buf) return;
    len = drop_crs(buf, (int)len);                  /* 10/25/26 (the review, U8) */
    for(i = 0; i < len; ) {
        char *nl = memchr(buf + i, '\n', (size_t)(len - i));
        if(nl) *nl = 0;
        if(buf[i]) history_add(buf + i);
        i = nl ? (int)(nl - buf) + 1 : (int)len;
    }
    free(buf);
    hist_saved = history_count();
}

/* HOSTNAME from the kernel's host name, for a telnet session's prompt */
static void hostname_load(void) {
    char name[80];
    int fd, n;
    if(var_get("HOSTNAME") || (fd = open("/dev/config/hostname", O_RDONLY)) < 0) return;
    n = (int)read(fd, name, sizeof(name) - 1);
    close(fd);
    while(n > 0 && (name[n - 1] == '\n' || name[n - 1] == '\r')) n--;
    if(n <= 0) return;
    name[n] = 0;
    var_set("HOSTNAME", name, 0);
}

/* the owner's startup file, run as a . file is (return leaves it). 10/25/26 (the programs' review): an exit there leaves
   the file, as return does, and is said, for it would end every interactive shell as it starts (a console's shell
   started again and again, a telnet session closed at once), and the first Ctrl+C stops the rest of it (U5); a file
   that is there but cannot be read is said, the reason in the kernel log, and a CR before a line's end is dropped
   (U8). It is read whole (16 KB at most, the kernel's limit), so no descriptor of it is left for its commands */
static void run_rc(void) {
    struct sbuf text = { NULL, 0, 0 };
    struct source s;
    char buf[1024];
    ssize_t got;
    int fd = open(SURFSHRC, O_RDONLY);
    if(fd < 0) {                                /* none, or no stick to read it from (ENODEV), are no mistake: the */
        if(errno != ENOENT && errno != ENODEV)  /* kernel says it on this console when a USB stick was not ready */
            fprintf(stderr, "surfsh: %s: %s: not run (dmesg says why)\n", SURFSHRC, strerror(errno));
        return;
    }
    while((got = read(fd, buf, sizeof(buf))) > 0) sb_put(&text, buf, (int)got);
    close(fd);
    if(text.len) text.len = drop_crs(text.s, text.len);
    memset(&s, 0, sizeof(s));
    s.get = text_get;
    s.text = sb_str(&text);
    s.name = SURFSHRC;
    s.rc = 1;
    dot_depth++;
    run_source(&s);
    dot_depth--;
    returning = 0;
    if(exit_requested) {
        exit_requested = 0;
        fprintf(stderr, "surfsh: %s: exit leaves the startup file, as return does; the shell goes on\n", SURFSHRC);
    } else if(s.rc == 2) {
        fprintf(stderr, "surfsh: %s: the rest of the startup file was skipped (Ctrl+C)\n", SURFSHRC);
    }
    sb_free(&text);
}

/**** the sources of lines ****/

static int blank(const char *s) {
    while(*s == ' ' || *s == '\t') s++;
    return !*s;
}

/* the line editor: a line typed at the prompt (the jobs that ended reported first) */
static int editor_get(struct source *s, struct sbuf *out, int more) {
    static char line[LINE_LIMIT + 1];
    static int open_quote;
    int n;
    if(!more) jobs_collect(1);
    n = edit_line(line, sizeof(line), more);
    if(n == -1) return -1;                      /* Ctrl-D, or no more input */
    if(n == -3) return -2;                      /* Ctrl-C on a line that continued a command */
    if(!more) open_quote = 0;                   /* 10/25/26 (the review, U6): a command's quote goes over its lines */
    if(n >= 0 && !blank(line)) {                /* 10/25/26: !! and the like (editor.c) */
        int r = history_expand_in(line, line, sizeof(line), &open_quote);
        if(r == -1) {
            fprintf(stderr, "surfsh: %s: no such line in the history\n", line);
            last_status = STATUS_USAGE;
            return -2;
        }
        if(r == -2) {
            n = -2;
        } else if(r == 1) {                     /* the line as it runs, shown as bash shows it */
            n = (int)strlen(line);
            fprintf(stderr, "%s\n", line);
        }
    }
    if(n == -2) {
        fprintf(stderr, "surfsh: the line is longer than %d bytes: it was not run\n", LINE_LIMIT);
        last_status = STATUS_USAGE;
        return -2;
    }
    if(!blank(line) && line[0] != ' ') {        /* 10/25/26: not one that starts with a space */
        history_add(line);
        /* (A10) a save every HIST_SAVE_EVERY lines; 10/25/26 (the programs' review, U4): after one that failed, the next
           try HIST_SAVE_EVERY lines later, not at every line, and only the first failure said */
        if(hist_kept && history_count() - hist_saved >= HIST_SAVE_EVERY && history_count() >= hist_retry) {
            hist_failed = history_save(hist_failed ? 0 : 2) < 0;
            if(hist_failed) hist_retry = history_count() + HIST_SAVE_EVERY;
        }
    }
    s->lines++;
    sb_put(out, line, n);
    sb_putc(out, '\n');
    return 0;
}

/* Input that is not the console: a pipe a byte at a time (what follows the line is not taken from
   it), a file a block at a time, with what was read past the line given back (fd_give_back())
   before a command runs. Read in blocks: rd[rpos .. rlen). 10/25/26: a terminal a line at a time
   (a read of it answers one line, cut to what is asked: a byte would be all of each line) */
static char rd[4096];
static int rpos, rlen, seekable = -1, terminal;

static int fd_get(struct source *s, struct sbuf *out, int more) {
    int got = 0;
    (void)more;
    if(seekable < 0) {
        terminal = isatty(s->fd);
        seekable = !terminal && lseek(s->fd, 0, SEEK_CUR) >= 0;
    }
    for(;;) {
        char c;
        if(rpos < rlen) c = rd[rpos++];
        else {
            ssize_t n = seekable || terminal ? read(s->fd, rd, sizeof(rd)) : read(s->fd, &c, 1);
            if(n <= 0) break;
            if(seekable || terminal) {
                rpos = 1;
                rlen = (int)n;
                c = rd[0];
            }
        }
        if(out->len >= TEXT_LIMIT) {
            fprintf(stderr, "surfsh: %s: more than %d bytes in a command\n", s->name ? s->name : "the input", TEXT_LIMIT);
            return -1;
        }
        sb_putc(out, c);
        got = 1;
        if(c == '\n') break;
    }
    if(!got) return -1;
    if(out->s[out->len - 1] != '\n') sb_putc(out, '\n');
    s->lines++;
    return 0;
}

/* a file's descriptor where the next line begins: what the commands read comes after it */
static void fd_give_back(struct source *s) {
    if(seekable > 0 && rpos < rlen) lseek(s->fd, -(off_t)(rlen - rpos), SEEK_CUR);
    rpos = rlen = 0;
}

/* text in memory: all of what is left at once */
static int text_get(struct source *s, struct sbuf *out, int more) {
    size_t n = strlen(s->text);
    (void)more;
    if(!n) return -1;
    sb_put(out, s->text, (int)n);
    if(s->text[n - 1] != '\n') sb_putc(out, '\n');
    s->text += n;
    return 0;
}

/**** running what the lines say ****/

static int count_lines(const char *s, int n) {
    int i, k = 0;
    for(i = 0; i < n; i++) if(s[i] == '\n') k++;
    return k;
}

static void parse_failed(struct source *s, struct parser *p, int first, int at_end) {
    const char *head = at_end ? (s->get == editor_get ? "the input ended inside a command: " :
                                 "the text ends inside a command: ") : "";
    if(s->name) fprintf(stderr, "surfsh: %s: line %d: %s%s\n", s->name, first + p->err_line - 1, head, p->err);
    else fprintf(stderr, "surfsh: %s%s\n", head, p->err);
}

/* 10/25/26: sources inside each other (. files and eval run theirs within a command): only the
   outermost of an interactive shell is what was typed at the prompt */
static int source_depth;

int run_source(struct source *s) {
    struct sbuf buf = { NULL, 0, 0 };
    struct arena mem = { NULL };
    struct parser p;
    struct node *n;
    int used = 0, more = 0, ended = 0, first = 1, r, top = interactive && !source_depth, ran = 0;
    /* 10/25/26: the startup file (run_rc()) runs as the prompt's commands do, each its own (top): paged, Ctrl+C reaching
       surfsh's own commands */
    source_depth++;
    while(!exit_requested && (top || !stopping())) {
        if(used >= buf.len && !more) {          /* all of it ran: start again empty */
            buf.len = used = 0;
            if(buf.s) buf.s[0] = 0;
            first = s->lines + 1;
            if(ended) break;
        }
        if(!ended && (more || used >= buf.len)) {
            r = s->get(s, &buf, more);
            if(r == -2) {                       /* the line refused: so is the command it was part of */
                buf.len = used = more = 0;
                if(buf.s) buf.s[0] = 0;
                continue;
            }
            if(r < 0) {
                ended = 1;
                if(used >= buf.len) break;
            }
        }
        parser_init(&p, buf.s + used, &mem);
        r = parse_command(&p, &n);
        if(r == PARSE_MORE && !ended) {
            arena_free(&mem);
            more = 1;
            continue;
        }
        more = 0;
        if(r == PARSE_MORE || r == PARSE_ERROR) {
            parse_failed(s, &p, first, r == PARSE_MORE);
            arena_free(&mem);
            last_status = STATUS_USAGE;
            ran = 1;
            if(!top) break;                     /* a script with a mistake goes no further */
            buf.len = used = 0;
            continue;
        }
        if(r == PARSE_EOF) {
            arena_free(&mem);
            used = buf.len;
            continue;
        }
        first += count_lines(buf.s + used, p.pos);
        used += p.pos;
        if(s->get == fd_get) fd_give_back(s);
        if(top) {
            page(1);
            ttyctl(TTY_INTR, 1);                /* 10/25/26: a Ctrl+C counts from here (exec.c) */
        }
        last_status = exec_tree(n, top ? X_TOP : cond_flag);    /* 10/25/26: eval's and .'s text as their command */
        ran = 1;
        if(top) {
            page(0);
            if(interrupted && !interrupt_said) {    /* Ctrl+C stopped surfsh's own commands: say so */
                edit_fresh_row();
                fflush(stdout);
                dprintf(term_fd, "^C\n");
            }
            if(interrupted) last_status = STATUS_INTERRUPTED;
            if(suspended) last_status = STATUS_STOPPED;     /* 10/25/26: a job stopped (jobs.c) */
            if(interrupted && s->rc) s->rc = 2;     /* 10/25/26 (the review, U5): the startup file goes no further */
            interrupted = interrupt_said = suspended = 0;
        }
        arena_free(&mem);
        fflush(stdout);
        if(s->get == fd_get) jobs_collect(0);   /* after each command of a file, as surfsh did after each line */
        if(s->rc == 2 || (s->rc && returning)) break;   /* 10/25/26 (U5): a Ctrl+C or return in the startup file */
    }
    source_depth--;
    sb_free(&buf);
    return ran ? last_status : 0;                   /* 10/25/26: no command, as for a . file of comments */
}

int run_text(const char *text, const char *name) {
    struct source s;
    memset(&s, 0, sizeof(s));
    s.get = text_get;
    s.text = text;
    s.name = name;
    return run_source(&s);
}

/**** the start ****/

/* the kernel's banner (kernel/sys.c, do_banner) with the same strings, light red and orange */
static void banner(void) {
    int colour = use_color();
    printf("\n%s%s's %s\nVersion %s%s%s on an x86, %s%s\n%s",
           colour ? SGR_LRED : "", SURFOS_AUTHOR, SURFOS_NAME,
           colour ? SGR_ORANGE : "", SURFOS_VERSION, colour ? SGR_LRED : "",
           colour ? SGR_ORANGE : "", SURFOS_YEARS, colour ? SGR_RESET : "");
    fflush(stdout);
}

/* A new surfsh (exec.c): from descriptor 3 what makes it the shell it was started from (its
   variables, positional parameters), a NUL, and the text to run, with $?, $$, $! and $0 as they
   were there (its arguments) */
static int run_sub(char **argv) {
    struct sbuf text = { NULL, 0, 0 };
    char buf[1024];
    const char *run;
    ssize_t n;
    while((n = read(3, buf, sizeof(buf))) > 0) {
        if(text.len + n > TEXT_LIMIT) {
            fprintf(stderr, "surfsh: more than %d bytes to run\n", TEXT_LIMIT);
            return STATUS_USAGE;
        }
        sb_put(&text, buf, (int)n);
    }
    close(3);
    run = sb_str(&text);
    if((int)strlen(run) < text.len) {           /* the state first */
        run_text(run, NULL);
        run += strlen(run) + 1;
    }
    last_status = atoi(argv[0]);
    shell_pid = atoi(argv[1]);
    jobs_set_last(atoi(argv[2]));
    arg0 = argv[3];
    run_text(run, NULL);
    sb_free(&text);
    return exit_requested ? exit_code : last_status;
}

static int usage(void) {
    fprintf(stderr, "usage: surfsh [-l] [-c text [name [args]] | file [args]]   (-l: the banner, as init starts it)\n");
    return STATUS_USAGE;
}

int main(int argc, char **argv) {
    struct source s;
    int i, login = 0;
    const char *text = NULL, *file = NULL;
    setvbuf(stdout, NULL, _IOLBF, 0);           /* line by line, wherever descriptor 1 goes later */
    shell_pid = getpid();
    vars_init(environ);
    for(i = 1; i < argc; i++) {
        if(!strcmp(argv[i], "-l")) login = 1;
        else if(!strcmp(argv[i], "--sub") && i + 4 < argc) {
            run_init();
            return run_sub(argv + i + 1) & 0xff;
        } else if(!strcmp(argv[i], "-c") && i + 1 < argc) {
            text = argv[++i];                   /* then $0 and the positional parameters */
            if(i + 1 < argc) arg0 = argv[++i];
            pos_set(argc - i - 1, argv + i + 1);
            break;
        } else if(argv[i][0] == '-') {
            return usage();
        } else {
            file = argv[i];                     /* $0, then the positional parameters */
            arg0 = argv[i];
            pos_set(argc - i - 1, argv + i + 1);
            break;
        }
    }
    read_conf(SURFSH_CONF);
    if(login) banner();
    run_init();
    memset(&s, 0, sizeof(s));
    if(text) {
        run_text(text, "-c");
    } else if(file) {
        int high;
        s.fd = open(file, O_RDONLY);
        if(s.fd < 0) {
            fprintf(stderr, "surfsh: %s: %s\n", file, strerror(errno));
            return STATUS_NOTFOUND;
        }
        if((high = fd_copy_high(s.fd)) >= 0) {     /* 10/25/26: not 3, which the script may redirect */
            close(s.fd);
            s.fd = high;
        }
        s.get = fd_get;
        s.name = file;
        run_source(&s);
        close(s.fd);
    } else if(isatty(0) && isatty(1)) {
        interactive = 1;
        if((term_fd = fd_copy_high(1)) < 0) term_fd = 1;     /* 10/25/26: the prompt's and the reports' */
        broken_pipe_exit(0);
        history_load();                         /* 10/25/26 (A10): the stick's history and startup file */
        hostname_load();
        run_rc();
        if(!exit_requested) {
            s.get = editor_get;
            run_source(&s);
        }
        history_save(0);
        fflush(stdout);
        return exit_requested ? exit_code & 0xff : 0;   /* Ctrl-D: 0, as an exit without a number */
    } else {
        s.get = fd_get;
        s.fd = 0;
        run_source(&s);
    }
    fflush(stdout);
    return (exit_requested ? exit_code : last_status) & 0xff;
}
