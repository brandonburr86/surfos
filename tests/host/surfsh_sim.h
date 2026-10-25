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

#endif
