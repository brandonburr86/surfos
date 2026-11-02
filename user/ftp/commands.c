/*
SurfOS file transfer client: ftp
--------------------
File: commands.c  Date: 11/01/26
--------------------
The commands of ftp (ftpsession.h has what they do): a typed line, as the prompt or a script gives
it, split into words and run. Blank lines and lines starting with # are nothing. Each command
comes back with its status, and the worst one so far is ftp's own (main.c).

Wildcards: mget lists the folder of its pattern with NLST and matches the names here, so the
server's own idea of wildcards does not count and a name that does not match what was typed is
not taken, whatever the server lists. Each name is the last part of what the listing says, and
must pass ftpc_safe_name() to become a local file: it is the server's word, while get's name is
the user's. mget never replaces a file unasked: at a terminal it asks, in a script it leaves the
file and says so. mput matches in the local folder, ignoring case as FAT does.
*/

#include <surf.h>
#include "ftpclient.h"
#include "ftpsession.h"
#include "ftpcommands.h"

#define WORDS_MAX  32                   /* words on a line */

static int quitting;

static int need_connection(void) {
    if(fs_connected()) return 0;
    fprintf(stderr, "ftp: not connected (open host)\n");
    return 1;
}

int ftp_open(const char *host, const char *port_word) {
    int port = 21, r;
    if(port_word) {
        char *end;
        long v = strtol(port_word, &end, 10);
        if(*end || v < 1 || v > 65535) {
            fprintf(stderr, "ftp: %s is no port (1-65535)\n", port_word);
            return FS_TROUBLE;
        }
        port = (int)v;
    }
    r = fs_open(host, port);
    if(r != FS_OK) return r;
    return ftp_auto_login ? fs_login(NULL) : FS_OK;
}

static int cmd_open(int argc, char **argv) {
    return ftp_open(argv[1], argc > 2 ? argv[2] : NULL);
}

static int cmd_user(int argc, char **argv) {
    if(need_connection()) return FS_TROUBLE;
    if(argc > 2) {
        fprintf(stderr, "ftp: user takes a name alone: a password is asked for, never typed on a line\n");
        return FS_TROUBLE;
    }
    return fs_login(argv[1]);
}

static int cmd_ls(int argc, char **argv) {
    if(need_connection()) return FS_TROUBLE;
    return fs_list(strcmp(argv[0], "dir") ? "NLST" : "LIST", argc > 1 ? argv[1] : NULL);
}

static int cmd_cd(int argc, char **argv) {
    (void)argc;
    return need_connection() ? FS_TROUBLE : fs_cd(argv[1]);
}

static int cmd_pwd(int argc, char **argv) {
    (void)argc;
    (void)argv;
    return need_connection() ? FS_TROUBLE : fs_pwd();
}

/* the local name of a remote one: the second word, or its last name */
static const char *local_of(int argc, char **argv) {
    const char *name = argc > 2 ? argv[2] : ftpc_last_name(argv[1]);
    if(*name) return name;
    fprintf(stderr, "ftp: %s has no last name: say what the local file is called\n", argv[1]);
    return NULL;
}

static int cmd_get(int argc, char **argv) {
    const char *local;
    if(need_connection()) return FS_TROUBLE;
    if(!(local = local_of(argc, argv))) return FS_TROUBLE;
    return fs_get(argv[1], strcmp(local, "-") ? local : NULL);
}

static int cmd_reget(int argc, char **argv) {
    const char *local;
    if(need_connection()) return FS_TROUBLE;
    if(!(local = local_of(argc, argv))) return FS_TROUBLE;
    if(!strcmp(local, "-")) {
        fprintf(stderr, "ftp: reget goes on with a file: - is none\n");
        return FS_TROUBLE;
    }
    return fs_reget(argv[1], local);
}

static int cmd_put(int argc, char **argv) {
    const char *remote;
    if(need_connection()) return FS_TROUBLE;
    remote = argc > 2 ? argv[2] : ftpc_last_name(argv[1]);
    if(!*remote) {
        fprintf(stderr, "ftp: %s has no last name\n", argv[1]);
        return FS_TROUBLE;
    }
    return fs_put(argv[1], remote);
}

static int file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

/* mget's question for a name that is there: 1 replace it, 0 leave it, -1 stop. *all: yes to the rest */
static int may_replace(const char *name, int *all) {
    char shown[FTPC_NAME_MAX + 8], answer[16];
    ftpc_text(shown, sizeof(shown), name);
    if(*all) return 1;
    if(!isatty(0)) {
        fprintf(stderr, "ftp: %s is there already: left as it is\n", shown);
        return 0;
    }
    printf("replace %s? (y, n, a for all, q to stop) ", shown);
    fflush(stdout);
    if(readline(answer, sizeof(answer)) < 0 || answer[0] == 'q') return -1;
    if(answer[0] == 'a') {
        *all = 1;
        return 1;
    }
    return answer[0] == 'y';
}

/* one pattern of mget; *stop: Ctrl+C came, nothing more of it is to run */
static int mget_one(const char *pattern, int *all, int *stop) {
    struct fs_names names;
    char dir[FTP_PATH_MAX], path[2 * FTP_PATH_MAX];
    const char *glob;
    int i, worst = FS_OK, taken = 0;
    if(ftpc_split(pattern, dir, sizeof(dir), &glob) < 0 || !*glob) {
        fprintf(stderr, "ftp: %s: no name to match\n", pattern);
        return FS_TROUBLE;
    }
    worst = fs_names(dir[0] ? dir : NULL, &names);
    if(fs_stopped()) {                  /* a listing cut short is no list to go by */
        ftp_note_stopped();
        *stop = 1;
        fs_names_free(&names);
        return worst;
    }
    for(i = 0; i < names.count && fs_connected(); i++) {
        const char *name = names.name[i];
        int r;
        if(!ftpc_glob(glob, name, 0)) continue;
        taken++;
        if(!ftpc_safe_name(name)) {
            char shown[FTPC_NAME_MAX + 8];
            ftpc_text(shown, sizeof(shown), name);
            fprintf(stderr, "ftp: the server lists a name that is not safe here: left out: %s\n", shown);
            if(worst < FS_NO) worst = FS_NO;
            continue;
        }
        if(file_exists(name)) {
            r = may_replace(name, all);
            if(r < 0) break;
            if(!r) {
                if(worst < FS_NO) worst = FS_NO;
                continue;
            }
        }
        if(ftpc_join(path, sizeof(path), dir, name) < 0) {
            fprintf(stderr, "ftp: %s: too long\n", pattern);
            worst = FS_TROUBLE;
            continue;
        }
        r = fs_get(path, name);
        if(r > worst) worst = r;
        if(fs_stopped()) {
            ftp_note_stopped();
            *stop = 1;
            break;
        }
    }
    if(names.cut) {
        fprintf(stderr, "ftp: %s: the listing is too long: only the start of it was looked at\n", pattern);
        worst = FS_TROUBLE;
    }
    if(!taken && worst == FS_OK) {
        fprintf(stderr, "ftp: %s: no name matches\n", pattern);
        worst = FS_NO;
    }
    fs_names_free(&names);
    return worst;
}

static int cmd_mget(int argc, char **argv) {
    int i, worst = FS_OK, all = 0, stop = 0;
    if(need_connection()) return FS_TROUBLE;
    for(i = 1; i < argc && fs_connected() && !stop; i++) {
        int r = mget_one(argv[i], &all, &stop);
        if(r > worst) worst = r;
    }
    return worst;
}

/* the local files a pattern matches, read whole before anything else is done (a program has 16 descriptors) */
static char **local_matches(const char *dir, const char *glob, int *count) {
    DIR *d = opendir(dir);
    struct dirent *de;
    char **v = NULL;
    int n = 0;
    if(!d) return NULL;
    while((de = readdir(d))) {
        char **nv;
        if(de->d_type != DT_REG || !ftpc_glob(glob, de->d_name, 1)) continue;
        if(n >= FS_NAMES_MAX || !(nv = realloc(v, (size_t)(n + 1) * sizeof(char *))) ||
           !(nv[n] = strdup(de->d_name))) {
            closedir(d);
            while(n > 0) free(v[--n]);
            free(v);
            errno = ENOMEM;
            return NULL;
        }
        v = nv;
        n++;
    }
    closedir(d);
    if(!v) v = malloc(sizeof(char *));
    *count = n;
    return v;
}

static int cmd_mput(int argc, char **argv) {
    int a, worst = FS_OK, stop = 0;
    if(need_connection()) return FS_TROUBLE;
    for(a = 1; a < argc && fs_connected() && !stop; a++) {
        char dir[FTP_PATH_MAX], path[2 * FTP_PATH_MAX];
        const char *glob;
        char **names;
        int n = 0, i;
        if(ftpc_split(argv[a], dir, sizeof(dir), &glob) < 0 || !*glob) {
            fprintf(stderr, "ftp: %s: no name to match\n", argv[a]);
            worst = FS_TROUBLE;
            continue;
        }
        if(!(names = local_matches(dir[0] ? dir : ".", glob, &n))) {
            fprintf(stderr, "ftp: %s: %s\n", dir[0] ? dir : ".", strerror(errno));
            worst = FS_TROUBLE;
            continue;
        }
        if(!n) {
            fprintf(stderr, "ftp: %s: no file matches\n", argv[a]);
            if(worst < FS_NO) worst = FS_NO;
        }
        for(i = 0; i < n && fs_connected(); i++) {
            int r = FS_TROUBLE;
            if(ftpc_join(path, sizeof(path), dir, names[i]) == 0) r = fs_put(path, names[i]);
            if(r > worst) worst = r;
            if(fs_stopped()) {
                ftp_note_stopped();
                stop = 1;
                break;
            }
        }
        while(n > 0) free(names[--n]);
        free(names);
    }
    return worst;
}

static int cmd_lcd(int argc, char **argv) {
    char cwd[PATH_MAX];
    if(argc > 1 && chdir(argv[1]) < 0) {
        fprintf(stderr, "ftp: %s: %s\n", argv[1], strerror(errno));
        return FS_TROUBLE;
    }
    if(getcwd(cwd, sizeof(cwd))) printf("The local folder is %s\n", cwd);
    return FS_OK;
}

static int cmd_delete(int argc, char **argv) {
    (void)argc;
    return need_connection() ? FS_TROUBLE : fs_verb("DELE", argv[1]);
}

static int cmd_mkdir(int argc, char **argv) {
    (void)argc;
    return need_connection() ? FS_TROUBLE : fs_verb("MKD", argv[1]);
}

static int cmd_rmdir(int argc, char **argv) {
    (void)argc;
    return need_connection() ? FS_TROUBLE : fs_verb("RMD", argv[1]);
}

static int cmd_rename(int argc, char **argv) {
    (void)argc;
    return need_connection() ? FS_TROUBLE : fs_rename(argv[1], argv[2]);
}

static int cmd_size(int argc, char **argv) {
    (void)argc;
    return need_connection() ? FS_TROUBLE : fs_size(argv[1]);
}

static int cmd_binary(int argc, char **argv) {
    (void)argc;
    (void)argv;
    fs_set_text(0);
    printf("Transfers are binary: the bytes as they are\n");
    return FS_OK;
}

static int cmd_ascii(int argc, char **argv) {
    (void)argc;
    (void)argv;
    fs_set_text(1);
    printf("Transfers are ascii: text, its line ends converted\n");
    return FS_OK;
}

static int cmd_status(int argc, char **argv) {
    (void)argc;
    (void)argv;
    fs_status();
    return FS_OK;
}

static int cmd_quit(int argc, char **argv) {
    (void)argc;
    (void)argv;
    quitting = 1;
    return fs_close();
}

static int cmd_help(int argc, char **argv);

static const struct {
    const char *name;
    int min, max;                       /* words after the name */
    int (*run)(int argc, char **argv);
    const char *usage;
} cmds[] = {
    { "open",   1, 2,  cmd_open,   "open host [port]        connect, as anonymous unless ftp -n" },
    { "user",   1, 99, cmd_user,   "user name               log in; the password is asked for" },
    { "ls",     0, 1,  cmd_ls,     "ls [path]               the names in a folder" },
    { "dir",    0, 1,  cmd_ls,     "dir [path]              a folder, in full" },
    { "cd",     1, 1,  cmd_cd,     "cd path                 change the server's folder" },
    { "pwd",    0, 0,  cmd_pwd,    "pwd                     the server's folder" },
    { "get",    1, 2,  cmd_get,    "get remote [local]      fetch a file; - for standard output" },
    { "put",    1, 2,  cmd_put,    "put local [remote]      send a file" },
    { "mget",   1, 31, cmd_mget,   "mget pattern...         fetch what the patterns match" },
    { "mput",   1, 31, cmd_mput,   "mput pattern...         send the local files they match" },
    { "reget",  1, 2,  cmd_reget,  "reget remote [local]    go on with a file cut short" },
    { "lcd",    0, 1,  cmd_lcd,    "lcd [folder]            the local folder" },
    { "delete", 1, 1,  cmd_delete, "delete name             delete a file on the server" },
    { "mkdir",  1, 1,  cmd_mkdir,  "mkdir name              make a folder on the server" },
    { "rmdir",  1, 1,  cmd_rmdir,  "rmdir name              remove a folder on the server" },
    { "rename", 2, 2,  cmd_rename, "rename from to          rename on the server" },
    { "size",   1, 1,  cmd_size,   "size name               a file's size on the server" },
    { "binary", 0, 0,  cmd_binary, "binary                  transfer the bytes as they are (the default)" },
    { "ascii",  0, 0,  cmd_ascii,  "ascii                   transfer text, converting its line ends" },
    { "status", 0, 0,  cmd_status, "status                  how things stand" },
    { "quit",   0, 0,  cmd_quit,   "quit                    leave" },
    { "help",   0, 1,  cmd_help,   "help [command]          this list" },
};

#define NCMDS ((int)(sizeof(cmds) / sizeof(cmds[0])))

static int cmd_help(int argc, char **argv) {
    int i;
    for(i = 0; i < NCMDS; i++) {
        if(argc > 1 && strcmp(argv[1], cmds[i].name)) continue;
        printf("%s\n", cmds[i].usage);
        if(argc > 1) return FS_OK;
    }
    if(argc > 1) {
        fprintf(stderr, "ftp: no command %s\n", argv[1]);
        return FS_TROUBLE;
    }
    return FS_OK;
}

int ftp_run_line(char *line) {
    char *word[WORDS_MAX];
    int n = ftpc_words(line, word, WORDS_MAX), i;
    if(n == -1) {
        fprintf(stderr, "ftp: too many words on a line\n");
        return FS_TROUBLE;
    }
    if(n == -2) {
        fprintf(stderr, "ftp: a quote is not closed\n");
        return FS_TROUBLE;
    }
    if(!n || word[0][0] == '#') return FS_OK;
    for(i = 0; i < NCMDS; i++) {
        if(strcmp(word[0], cmds[i].name)) continue;
        if(n - 1 < cmds[i].min || n - 1 > cmds[i].max) {
            fprintf(stderr, "usage: %s\n", cmds[i].usage);
            return FS_TROUBLE;
        }
        return cmds[i].run(n, word);
    }
    fprintf(stderr, "ftp: no command %s (help lists them)\n", word[0]);
    return FS_TROUBLE;
}

int ftp_quitting(void) {
    return quitting;
}

void ftp_commands_init(void) {
    quitting = 0;
}
