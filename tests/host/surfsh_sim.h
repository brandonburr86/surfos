/* tests/host/test_surfsh.c and its bridge, tests/host/surfsh_bridge.c (10/24/26): what the test asks of
   surfsh's parser and expansions. Plain C, for the host's headers and SurfOS's alike. */
#ifndef SURFSH_SIM_H
#define SURFSH_SIM_H

enum { SH_OK, SH_EOF, SH_MORE, SH_ERROR };      /* parse.c's PARSE_* */

/* the first complete command of text: SH_OK with the tree as unparse.c writes it in out; otherwise
   the parser's reason (SH_MORE, SH_ERROR) or "" (SH_EOF). *used: the bytes of text it read */
int sh_parse(const char *text, char *out, int size, int *used);
int sh_err_line(void);                          /* the line of the last reason */
/* the word's fields as expand_words() makes them, each followed by '|'; -1 on an error */
int sh_expand(const char *word, char *out, int size);
int sh_expand_doc(const char *body, char *out, int size);   /* a here-document's text */
void sh_set_status(int status);                 /* $? */
/* 10/24/26: variables, the positional parameters, IFS (NULL unsets it), $(( )) */
void sh_set_var(const char *name, const char *value);   /* value NULL: unset */
const char *sh_get_var(const char *name);
void sh_readonly(const char *name);
void sh_set_args(int n, const char *const *args);
int sh_arith(const char *text, long *result, char *err, int errsize);
/* the names a pattern may match (a trailing / makes a directory; relative names are below "."), given
   to the stand-ins for opendir() and readdir() and to sim_fs.c's stat() */
void sh_tree(const char *const *names);
/* one word as one string (expand_one), and a here-document's text: -1 on an error */
int sh_expand_one(const char *word, char *out, int size);
/* 10/25/26: test's arguments (0 true, 1 false, 2 a mistake), and printf's output: its status */
int sh_test(int n, char **v);
int sh_printf(int n, const char *const *args, char *out, int size);

#endif
