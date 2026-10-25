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
*/

#include "surfsh.h"
#include <surfos/version.h>

struct settings settings = { 1, 1, 5 };
int last_status;
int exit_requested, exit_code;
int interactive;
int shell_pid;

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

/**** the sources of lines ****/

static int blank(const char *s) {
    while(*s == ' ' || *s == '\t') s++;
    return !*s;
}

/* the line editor: a line typed at the prompt (the jobs that ended reported first) */
static int editor_get(struct source *s, struct sbuf *out, int more) {
    static char line[LINE_LIMIT + 1];
    int n;
    if(!more) jobs_collect(1);
    n = edit_line(line, sizeof(line), more);
    if(n == -1) return -1;                      /* Ctrl-D, or no more input */
    if(n == -3) return -2;                      /* Ctrl-C on a line that continued a command */
    if(n == -2) {
        fprintf(stderr, "surfsh: the line is longer than %d bytes: it was not run\n", LINE_LIMIT);
        last_status = STATUS_USAGE;
        return -2;
    }
    if(!blank(line)) history_add(line);
    s->lines++;
    sb_put(out, line, n);
    sb_putc(out, '\n');
    return 0;
}

/* Input that is not the console: a pipe a byte at a time (what follows the line is not taken from
   it), a file a block at a time, with what was read past the line given back (fd_give_back())
   before a command runs. Read in blocks: rd[rpos .. rlen). */
static char rd[4096];
static int rpos, rlen, seekable = -1;

static int fd_get(struct source *s, struct sbuf *out, int more) {
    int got = 0;
    (void)more;
    if(seekable < 0) seekable = !isatty(s->fd) && lseek(s->fd, 0, SEEK_CUR) >= 0;
    for(;;) {
        char c;
        if(rpos < rlen) c = rd[rpos++];
        else {
            ssize_t n = seekable ? read(s->fd, rd, sizeof(rd)) : read(s->fd, &c, 1);
            if(n <= 0) break;
            if(seekable) {
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
    int used = 0, more = 0, ended = 0, first = 1, r, top = interactive && !source_depth;
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
        last_status = exec_tree(n, top ? X_TOP : 0);
        if(top) {
            page(0);
            if(interrupted && !interrupt_said) {    /* Ctrl+C stopped surfsh's own commands: say so */
                edit_fresh_row();
                printf("^C\n");
            }
            if(interrupted) last_status = STATUS_INTERRUPTED;
            if(suspended) last_status = STATUS_STOPPED;     /* 10/25/26: a job stopped (jobs.c) */
            interrupted = interrupt_said = suspended = 0;
        }
        arena_free(&mem);
        fflush(stdout);
        if(s->get == fd_get) jobs_collect(0);   /* after each command of a file, as surfsh did after each line */
    }
    source_depth--;
    sb_free(&buf);
    return last_status;
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
        s.fd = open(file, O_RDONLY);
        if(s.fd < 0) {
            fprintf(stderr, "surfsh: %s: %s\n", file, strerror(errno));
            return STATUS_NOTFOUND;
        }
        s.get = fd_get;
        s.name = file;
        run_source(&s);
        close(s.fd);
    } else if(isatty(0) && isatty(1)) {
        interactive = 1;
        broken_pipe_exit(0);
        s.get = editor_get;
        run_source(&s);
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
