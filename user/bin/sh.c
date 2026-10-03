/* sh: a small user-mode shell. Runs programs from /initrd/bin or by path, waits for them. */
#include <surf.h>

#define MAXARGS 16

static int split(char *line, char **argv) {
    int argc = 0;
    char *p = line;
    while(*p && argc < MAXARGS - 1) {
        while(*p == ' ' || *p == '\t') p++;
        if(!*p) break;
        argv[argc++] = p;
        while(*p && *p != ' ' && *p != '\t') p++;
        if(*p) *p++ = 0;
    }
    argv[argc] = 0;
    return argc;
}

static void run(int argc, char **argv) {
    char path[VFS_PATH_MAX];
    struct stat st;
    int pid, status = 0, bg = 0;
    if(argc > 1 && !strcmp(argv[argc - 1], "&")) { bg = 1; argv[--argc] = 0; }
    if(strchr(argv[0], '/')) snprintf(path, sizeof(path), "%s", argv[0]);
    else snprintf(path, sizeof(path), "/initrd/bin/%s", argv[0]);
    if(stat(path, &st) < 0) { printf("sh: %s: command not found\n", argv[0]); return; }
    pid = spawn(path, argv);
    if(pid < 0) { printf("sh: %s: %s\n", argv[0], strerror(pid)); return; }
    if(bg) { printf("[%d] started\n", pid); return; }
    if(wait(pid, &status) == pid && status) printf("[exit status %d]\n", status);
}

int main(int argc, char **argv) {
    char line[256], cwd[VFS_PATH_MAX], *args[MAXARGS];
    int n;
    printf("SurfOS user shell (pid %d). Programs live in /initrd/bin; 'exit' leaves.\n", getpid());
    for(;;) {
        if(getcwd(cwd, sizeof(cwd)) < 0) strcpy(cwd, "?");
        printf("%s $ ", cwd);
        if(readline(line, sizeof(line)) < 0) break;
        n = split(line, args);
        if(!n) continue;
        if(!strcmp(args[0], "exit")) return n > 1 ? atoi(args[1]) : 0;
        if(!strcmp(args[0], "cd")) {
            int r = chdir(n > 1 ? args[1] : "/");
            if(r < 0) printf("cd: %s\n", strerror(r));
            continue;
        }
        run(n, args);
    }
    return 0;
}
