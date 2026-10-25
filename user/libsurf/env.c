/*
SurfOS user library: the environment
--------------------
File: env.c     Date: 10/24/26
--------------------
environ is what the kernel put on the stack after argv (SYS_spawnx, surfos/spawn.h); crt0
sets it before main(). setenv(), unsetenv() and putenv() change it: the first change copies
the array into memory of this library's own, so that it can grow. A string setenv() makes is
never freed, also when a later setenv() replaces it, so a pointer getenv() handed out stays
good (POSIX allows either). spawn() and spawnfd() pass environ on to the programs they start.
*/

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

char **environ;
static int own;                         /* environ is this library's array: it may be realloc'ed */

static int count(void) {
    int n = 0;
    if(environ) while(environ[n]) n++;
    return n;
}

/* the entry of name (n bytes, no '='), or -1 */
static int find(const char *name, size_t n) {
    int i;
    for(i = 0; environ && environ[i]; i++)
        if(!strncmp(environ[i], name, n) && environ[i][n] == '=') return i;
    return -1;
}

/* room for extra more entries in an array of this library's; 0, or -1 (ENOMEM) */
static int room(int extra) {
    int n = count(), i;
    char **v;
    if(own) v = realloc(environ, (size_t)(n + extra + 1) * sizeof(char *));
    else if((v = malloc((size_t)(n + extra + 1) * sizeof(char *)))) {
        for(i = 0; i < n; i++) v[i] = environ[i];
        v[n] = NULL;
    }
    if(!v) {
        errno = ENOMEM;
        return -1;
    }
    environ = v;
    own = 1;
    return 0;
}

static int bad_name(const char *name) {
    if(name && *name && !strchr(name, '=')) return 0;
    errno = EINVAL;
    return 1;
}

char *getenv(const char *name) {
    int i;
    if(!name || !*name || strchr(name, '=')) return NULL;
    i = find(name, strlen(name));
    return i < 0 ? NULL : environ[i] + strlen(name) + 1;
}

/* s is "name=value" and becomes the entry itself (POSIX's putenv: s is not copied) */
int putenv(char *s) {
    char *eq = s ? strchr(s, '=') : NULL;
    int i;
    if(!eq || eq == s) {
        errno = EINVAL;
        return -1;
    }
    i = find(s, (size_t)(eq - s));
    if(room(i < 0)) return -1;
    if(i < 0) {
        i = count();
        environ[i + 1] = NULL;
    }
    environ[i] = s;
    return 0;
}

int setenv(const char *name, const char *value, int overwrite) {
    size_t n, v;
    char *s;
    if(bad_name(name)) return -1;
    n = strlen(name);
    if(find(name, n) >= 0 && !overwrite) return 0;
    v = strlen(value ? value : "");
    if(!(s = malloc(n + v + 2))) {
        errno = ENOMEM;
        return -1;
    }
    memcpy(s, name, n);
    s[n] = '=';
    memcpy(s + n + 1, value ? value : "", v + 1);
    if(putenv(s) < 0) {
        free(s);
        return -1;
    }
    return 0;
}

int unsetenv(const char *name) {
    size_t n;
    int i, j;
    if(bad_name(name)) return -1;
    n = strlen(name);
    while((i = find(name, n)) >= 0) {
        if(room(0)) return -1;
        for(j = i; environ[j]; j++) environ[j] = environ[j + 1];
    }
    return 0;
}
