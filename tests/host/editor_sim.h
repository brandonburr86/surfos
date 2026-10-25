/* tests/host/test_editor.c and its bridge, tests/host/editor_bridge.c (10/25/26): surfsh's line editor on a
   simulated terminal, as the test drives it. Plain C, for the host's headers and SurfOS's alike. */
#ifndef EDITOR_SIM_H
#define EDITOR_SIM_H

/* a fresh terminal of cols x rows (at most 200 x 60): the screen blank, the cursor at the top left, no keys
   waiting, no snapshots; colour: whether the editor colours what is typed (its escape sequences are read
   and dropped) */
void ed_reset(int cols, int rows, int colour);
/* what one read of the terminal answers next (after the reads queued before it): n bytes of s; at_ms, when
   not negative, is the uptime from that read on (for the editor's ESC that came alone) */
void ed_keys(const char *s);
void ed_keysn(const char *s, int n, int at_ms);
/* the screen as it is when the editor next waits for a key after the reads queued so far: snapshot k
   (0, 1, ...; at most 8) */
void ed_snapshot(void);
void ed_print(const char *s);               /* output on the terminal, as a command would leave it */
/* edit_line() on a buffer of size bytes (the continuation prompt when more): what it returns, the line in buf;
   a terminal with no more keys answers the end of input */
int ed_line(char *buf, int size, int more);
const char *ed_row(int r);                  /* row r of the screen now, without the blanks at its end */
const char *ed_snap_row(int k, int r);      /* row r of snapshot k, likewise ("" when there is none) */
int ed_snap_cursor(int k);                  /* the cursor in snapshot k: row * 1000 + column */
int ed_cursor(void);                        /* the cursor now: row * 1000 + column */
const char *ed_output(void);                /* every byte the editor wrote since ed_reset() */
int ed_skips(void);                         /* TTY_SKIP calls since ed_reset() */
int ed_pending(void);                       /* reads queued and not made */
/* the history (editor.c's, kept from test to test): a line added, the history builtin's lines, expansion */
void ed_history_add(const char *line);
int ed_expand(const char *in, char *out, int size);

#endif
