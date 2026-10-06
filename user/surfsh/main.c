/*
SurfOS shell: surfsh
--------------------
File: main.c    Date: 10/6/26
--------------------
surfsh [-l]: the SurfOS shell. init starts it with -l on every console (F1 to F4)
and again when it ends; typed at a prompt, it starts a shell inside the shell.

-l prints the banner as the kernel's do_banner() does (include/surfos/version.h):
"Brandon Burr's SurfOS" and "Version 1.0 on an x86, 2004-2026" in light red, the
version and the years in orange. Then the settings come from /initrd/etc/surfsh.conf
when it exists: "key = value" lines, # comments, the keys of the set command. The
shell starts in the directory it was started in ("/" when init starts it).

On the console the line is typed in the line editor (editor.c); with any other
input (surfsh < file) the lines are read and run without a prompt. Either way
the shell ends with exit, at the end of the input or with Ctrl-D on an empty line.
Before each prompt (each line of a file) the programs that ran in the background
and ended are reported.
*/

#include "surfsh.h"
#include <surfos/version.h>

struct settings settings = { 1, 1, 5 };
int last_status;
int exit_requested, exit_code;

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

/**** the start ****/

/* the kernel's banner (kernel/sys.c, do_banner) with the same strings, light red and orange */
static void banner(void) {
    int colour = console_caps() & TTY_CAP_COLOR;
    printf("\n%s%s's %s\nVersion %s%s%s on an x86, %s%s\n%s",
           colour ? SGR_LRED : "", SURFOS_AUTHOR, SURFOS_NAME,
           colour ? SGR_ORANGE : "", SURFOS_VERSION, colour ? SGR_LRED : "",
           colour ? SGR_ORANGE : "", SURFOS_YEARS, colour ? SGR_RESET : "");
    fflush(stdout);
}

static int blank(const char *s) {
    while(*s == ' ' || *s == '\t') s++;
    return !*s;
}

/* lines from a file or another program, without a prompt or an editor */
static void read_lines(void) {
    char buf[LINE_LIMIT + 2];
    while(!exit_requested && fgets(buf, sizeof(buf), stdin)) {
        size_t n = strlen(buf);
        if(n && buf[n - 1] == '\n') buf[--n] = 0;
        else if(n == sizeof(buf) - 1) {             /* no line end: longer than the editor takes */
            int c;
            while((c = fgetc(stdin)) != EOF && c != '\n');
            fprintf(stderr, "surfsh: the line is longer than %d bytes: it was not run\n", LINE_LIMIT);
            last_status = STATUS_USAGE;
            continue;
        }
        if(n && buf[n - 1] == '\r') buf[--n] = 0;
        last_status = run_line(buf);
        run_collect(0);
    }
}

int main(int argc, char **argv) {
    static char line[LINE_LIMIT + 1];
    int i, login = 0;
    for(i = 1; i < argc; i++) {
        if(!strcmp(argv[i], "-l")) login = 1;
        else {
            fprintf(stderr, "usage: surfsh [-l]   (-l: the banner, as init starts it)\n");
            return STATUS_USAGE;
        }
    }
    if(login) banner();
    read_conf(SURFSH_CONF);
    run_init();
    if(!isatty(0) || !isatty(1)) {
        read_lines();
        return exit_requested ? exit_code : last_status;
    }
    while(!exit_requested) {
        int n;
        run_collect(1);                             /* the background programs that ended */
        n = edit_line(line, sizeof(line));
        if(n == -1) break;                          /* Ctrl-D, or no more input */
        if(n == -2) {
            fprintf(stderr, "surfsh: the line is longer than %d bytes: it was not run\n", LINE_LIMIT);
            last_status = STATUS_USAGE;
            continue;
        }
        if(blank(line)) continue;
        history_add(line);
        last_status = run_line(line);
    }
    fflush(stdout);
    return exit_requested ? exit_code : 0;
}
