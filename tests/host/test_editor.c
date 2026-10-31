/* Host-side tests of surfsh's line editor (10/25/26, track A, A5): user/surfsh/editor.c and complete.c as they
   are (tests/host/editor_bridge.c), with keys given a read at a time and the screen read back through a small
   VT100. The history keeps 1,000 lines and numbers them as history does; history expansion: !! !$ !n !-n !str,
   what stays as typed (quotes, a backslash, $! ${! [!, != and !(, a ! before a blank or at the end, a comment),
   a reference no line answers, a line grown too long. Typing and the old keys; words: Ctrl+Left and Ctrl+Right
   as the console sends them, Alt+B, Alt+F, Alt+D and Alt+Backspace as ESC and the key in one read, Shift and
   Ctrl on other keys; an ESC alone is Esc, and ESC then [ within 250 ms a sequence split in two; Alt+. and
   Alt+. again. Ctrl+R: the newest line that holds the text, the cursor where it begins and the row under the
   line, Ctrl+R again an older line (never the same line twice), Backspace, a text no line holds, Esc and ^G
   putting the line back, Enter, another key ending the search where it stands, Up from the line found, Ctrl+R
   twice taking the last text, a narrow terminal, the foot of the screen, colours. Tab for commands and paths,
   the line limit, ^C and ^D, a paste after Enter. 10/25/26 (the programs' review of track A): a quote opened on
   a command's line before (U6); a line from the history longer than the limit refusing Enter, from Up and from
   Ctrl+R (U7). Run by `make unittest`. */
#include <stdio.h>
#include <string.h>
#include "editor_sim.h"

static int checks, failures, added;             /* added: lines given to the history, as history numbers them */

static void check(int ok, const char *what) {
    checks++;
    if(!ok) {
        failures++;
        printf("FAIL: %s\n", what);
    }
}

static char buf[1100], out[1100];

static void add(const char *line) {
    ed_history_add(line);
    added++;
}

/* the keys, one read each, then Enter in a read of its own: what edit_line() returns, the line in buf */
static int line_of(const char *k1, const char *k2, const char *k3) {
    if(k1) ed_keys(k1);
    if(k2) ed_keys(k2);
    if(k3) ed_keys(k3);
    ed_keys("\r");
    return ed_line(buf, sizeof(buf), 0);
}

static int expands(const char *in, int want, const char *result) {
    int r = ed_expand(in, out, sizeof(out));
    return r == want && (!result || !strcmp(out, result));
}

static void test_history(void) {
    char name[16], big[700];
    int i;
    for(i = 1; i <= 1005; i++) {                 /* before anything else, so the numbers are known */
        snprintf(name, sizeof(name), "cmd %d", i);
        add(name);
    }
    check(expands("!1005", 1, "cmd 1005") && expands("!6", 1, "cmd 6"), "!n: line n, as history numbers it");
    check(expands("!5", -1, "!5") && expands("!-1001", -1, "!-1001"), "1,000 lines kept: the oldest five are gone");
    check(expands("!-1000", 1, "cmd 6") && expands("!-1", 1, "cmd 1005"), "!-n: the n-th line back");
    check(expands("!0 x", -1, "!0") && expands("!1006", -1, "!1006") && expands("!-0", -1, "!-0"),
          "!0, a line not yet typed, !-0: no such line");

    add("echo alpha beta");
    add("ls -l /tmp");
    add("grep 'a b' \"c d\" x\\ y");
    check(expands("!!", 1, "grep 'a b' \"c d\" x\\ y"), "!!: the line before");
    check(expands("echo !$", 1, "echo x\\ y"), "!$: its last word, with its backslash");
    check(expands("!ls", 1, "ls -l /tmp") && expands("!ls -a", 1, "ls -l /tmp -a") && expands("!ec;!l", 1,
          "echo alpha beta;ls -l /tmp"), "!str: the newest line that starts with str (up to a blank or ;)");
    check(expands("!! && !-3", 1, "grep 'a b' \"c d\" x\\ y && echo alpha beta"), "two references in a line");
    check(expands("!$x", 1, "x\\ yx") && expands("!!z", 1, "grep 'a b' \"c d\" x\\ yz") &&
          expands("!1007a", 1, "ls -l /tmpa"), "what follows a reference stays after it");
    add("cat notes > 'my file'");
    check(expands("!$", 1, "'my file'"), "!$: a quoted word, quotes and all");
    add("grep x f|sort");
    check(expands("!$", 1, "sort"), "!$: | parts words");
    add("sleep 1 &");
    check(expands("!$", 1, "&"), "!$: & is a word of its own, as in bash");

    check(expands("echo '!!' \"!!\" \\!\\! end", 0, NULL), "inside quotes (double ones too) and after a backslash: as typed");
    check(expands("echo $! ${!x} [!a]* yes!", 0, NULL), "$! ${! [! and a ! at the end: as typed");
    check(expands("[ a != b ] && ! true; f !( x )", 0, NULL), "!= ! and !(: as typed");
    check(expands("echo !) !; !'q' !\"q\"", 0, NULL), "a ! before ) ; or a quote: as typed");
    check(expands("echo hi # !! too", 0, NULL) && expands("#!/bin/sh", 0, NULL), "a comment: as typed");
    check(expands("echo a#!-2", 1, "echo a#grep x f|sort"), "a # inside a word starts no comment");
    check(expands("echo \"#!/bin/sh\" '!x'", 0, NULL), "\"#!/bin/sh\": as typed (bash would refuse it)");
    check(expands("echo !nosuch here", -1, "!nosuch"), "a reference no line answers: refused, and named");
    for(i = 0; i < 600; i++) big[i] = 'x';
    big[600] = 0;
    add(big);
    check(ed_expand("!! !!", out, 1024) == -2 && ed_expand("!!", out, 1024) == 1, "a line expanded beyond 1,023 bytes: refused");
    strcpy(buf, "say !-2");
    check(ed_expand(buf, buf, sizeof(buf)) == 1 && !strcmp(buf, "say sleep 1 &"), "expanded in place");
}

/* 10/25/26 (the programs' review, U6): a command's quote goes over its lines */
static void test_quote_lines(void) {
    int q = 0;
    add("echo carried");
    check(ed_expand_in("echo 'Thanks,", out, sizeof(out), &q) == 0 && q == '\'' &&
          ed_expand_in("great job!!'", out, sizeof(out), &q) == 0 && q == 0,
          "a quote opened on the line before: a !! inside it stays as typed, and the quote ends there");
    q = 0;
    check(ed_expand_in("echo \"one", out, sizeof(out), &q) == 0 && q == '"' &&
          ed_expand_in("two !!", out, sizeof(out), &q) == 0 && q == '"' &&
          ed_expand_in("three\" !!", out, sizeof(out), &q) == 1 && q == 0 && !strcmp(out, "three\" echo carried"),
          "double quotes over three lines; a !! after the closing one expands");
    q = 0;
    check(ed_expand_in("echo hi # it's", out, sizeof(out), &q) == 0 && q == 0 && expands("!!", 1, "echo carried"),
          "a quote in a comment opens nothing; history_expand() starts with none");
}

/* 10/25/26 (the programs' review, U7): a line from the history longer than the limit is cut to it and refused */
static void test_long_history(void) {
    char longer[40];
    ed_reset(80, 25, 0);
    memset(longer, 'y', 30);
    longer[30] = 0;
    memcpy(longer, "echo ", 5);
    add(longer);
    ed_keys("\033[A");
    ed_keys("\r");
    check(ed_line(buf, 21, 0) == -2 && !strncmp(buf, longer, 20), "Up to a line longer than the limit: Enter refuses it");
    ed_reset(80, 25, 0);
    ed_keys("\022");
    ed_keys("yyy");
    ed_keys("\r");
    check(ed_line(buf, 21, 0) == -2, "Ctrl+R finding a line longer than the limit: Enter refuses it");
    ed_reset(80, 25, 0);
    ed_keys("\033[A");
    ed_keys("\177");
    ed_keys("\r");
    check(ed_line(buf, 21, 0) == 19 && !strncmp(buf, longer, 19), "once something is deleted, the line shown runs");
    ed_reset(80, 25, 0);
    line_of("\033[A", NULL, NULL);
    check(!strcmp(buf, longer), "with room for it: the whole line");
}

static void test_typing(void) {
    int r;
    ed_reset(80, 25, 0);
    r = line_of("echo hi", NULL, NULL);
    check(r == 7 && !strcmp(buf, "echo hi"), "a line: its length and its text");
    check(!strcmp(ed_row(0), "$ echo hi") && ed_cursor() == 1000, "the prompt and the line, then the next row");
    ed_reset(80, 25, 0);
    r = line_of("helo", "\033[D\033[Dl", "\033[F!");
    check(r == 6 && !strcmp(buf, "hello!"), "Left, typing in the middle, End");
    ed_reset(80, 25, 0);
    r = line_of("abc\003", NULL, NULL);
    check(r == 0 && !strcmp(ed_row(0), "$ abc^C") && !strcmp(ed_row(1), "$"), "^C: the line dropped, a new prompt");
    ed_reset(80, 25, 0);
    ed_keys("\004");
    check(ed_line(buf, sizeof(buf), 0) == -1 && !strcmp(ed_row(0), "$ exit"), "^D on an empty line: the end");
    ed_reset(80, 25, 0);
    ed_keys("abcdefghij\r");
    check(ed_line(buf, 8, 0) == -2 && !strcmp(buf, "abcdefg"), "a line longer than the buffer: refused");
    ed_reset(80, 25, 0);
    ed_keys("one\rtwo\r");
    check(ed_line(buf, sizeof(buf), 0) == 3 && !strcmp(buf, "one") && ed_skips() == 1,
          "keys after Enter in the same read: the rest of the paste skipped");
    ed_reset(80, 25, 0);
    ed_keys("x");
    ed_keys("\r");
    check(ed_line(buf, sizeof(buf), 1) == 1 && !strcmp(ed_row(0), "> x"), "the continuation prompt");
}

static void test_words(void) {
    ed_reset(80, 25, 0);
    line_of("one two three", "\033[1;5D\033[1;5DX", "\033[1;5CY");
    check(!strcmp(buf, "one XtwoY three"), "Ctrl+Left and Ctrl+Right (ESC[1;5D, ESC[1;5C) move by words");
    ed_reset(80, 25, 0);
    line_of("one two", "\033[1;3D_", "\033[1;3C\033[1;3C.");
    check(!strcmp(buf, "one _two."), "Alt+Left and Alt+Right (ESC[1;3D, ESC[1;3C) too");
    ed_reset(80, 25, 0);
    ed_keys("cp /usr/local/bin x");
    ed_keys("\033b");
    ed_keys("\033b");
    ed_keys("\033d");
    line_of("lib", NULL, NULL);
    check(!strcmp(buf, "cp /usr/local/lib x"), "Alt+B (ESC b in one read) twice, Alt+D: a word deleted");
    ed_reset(80, 25, 0);
    line_of("a-b-c", "\033[H\033f\033f!", NULL);
    check(!strcmp(buf, "a-b!-c"), "Alt+F: to the end of the next word (letters and digits)");
    ed_reset(80, 25, 0);
    ed_keys("cd /usr/local/bin");
    ed_keysn("\033\177", 2, -1);
    line_of(NULL, NULL, NULL);
    check(!strcmp(buf, "cd /usr/local/"), "Alt+Backspace: back to the start of a word");
    ed_reset(80, 25, 0);
    line_of("cd /usr/local/bin", "\027", NULL);
    check(!strcmp(buf, "cd "), "^W still takes everything back to a blank");
    ed_reset(80, 25, 0);
    line_of("caf\xc3\xa9 au lait", "\033[1;5D\033[1;5D\033[1;5D", "\033dX");
    check(!strcmp(buf, "X au lait"), "a word with a character beyond ASCII is one word");
    ed_reset(80, 25, 0);
    line_of("ab", "\033[1;2DX", "\033[1;6D\033[3;5~");
    check(!strcmp(buf, "Xb"), "Shift+Left is Left; Ctrl+Shift+Left a word; Ctrl+Delete is Delete");
    ed_reset(80, 25, 0);
    add("from up");
    line_of("\033[1;5A", NULL, NULL);
    check(!strcmp(buf, "from up"), "Ctrl+Up is Up");
}

static void test_esc(void) {
    ed_reset(80, 25, 0);
    ed_keys("ab");
    ed_keysn("\033", 1, 5000);
    ed_keysn("b", 1, 5050);
    line_of(NULL, NULL, NULL);
    check(!strcmp(buf, "abb"), "an ESC alone is Esc: the next key is itself, even at once");
    ed_reset(80, 25, 0);
    ed_keys("ab");
    ed_keysn("\033", 1, 6000);
    ed_keysn("[D", 2, 6100);
    line_of("X", NULL, NULL);
    check(!strcmp(buf, "aXb"), "ESC, then [D within 250 ms: Left, a sequence that came in two reads");
    ed_reset(80, 25, 0);
    ed_keys("ab");
    ed_keysn("\033", 1, 7000);
    ed_keysn("[D", 2, 7400);
    line_of(NULL, NULL, NULL);
    check(!strcmp(buf, "ab[D"), "ESC, then [D after 250 ms: typed");
    ed_reset(80, 25, 0);
    ed_keysn("\033\033x", 3, -1);
    line_of(NULL, NULL, NULL);
    check(!strcmp(buf, ""), "ESC ESC x: Esc, then Alt+x (nothing)");
}

static void test_last_arg(void) {
    ed_reset(80, 25, 0);
    add("echo alpha beta");
    add("ls -l /initrd");
    line_of("cat ", "\033.", NULL);
    check(!strcmp(buf, "cat /initrd"), "Alt+.: the last word of the line before");
    ed_reset(80, 25, 0);
    line_of("cat ", "\033.", "\033.");
    check(!strcmp(buf, "cat beta"), "Alt+. again at once: the last word of the line before that, in its place");
    ed_reset(80, 25, 0);
    line_of("cat ", "\033.", "\033[D\033[C\033.");
    check(!strcmp(buf, "cat /initrd/initrd"), "Alt+. after another key: the line before again");
    ed_reset(80, 25, 0);
    line_of("x", "\033_", NULL);
    check(!strcmp(buf, "x/initrd"), "Alt+_ as Alt+.");
}

static void test_search(void) {
    int r;
    ed_reset(80, 25, 0);
    add("make all");
    add("echo one");
    add("ls -l");
    add("echo two");
    ed_keys("\022");
    ed_snapshot();
    ed_keys("ech");
    ed_snapshot();
    ed_keys("\022");
    ed_snapshot();
    r = line_of(NULL, NULL, NULL);
    check(r == 8 && !strcmp(buf, "echo one"), "Ctrl+R, ech, Ctrl+R: an older line that holds ech; Enter returns it");
    check(!strcmp(ed_snap_row(0, 0), "$") && !strcmp(ed_snap_row(0, 1), "search:") && ed_snap_cursor(0) == 2,
          "Ctrl+R: the row under the line says search:, the cursor stays in the line");
    check(!strcmp(ed_snap_row(1, 0), "$ echo two") && !strcmp(ed_snap_row(1, 1), "search: ech") &&
          ed_snap_cursor(1) == 2, "typed text: the newest line that holds it, the cursor where it begins");
    check(!strcmp(ed_snap_row(2, 0), "$ echo one") && !strcmp(ed_snap_row(2, 1), "search: ech"),
          "Ctrl+R again: the next older line");
    check(!strcmp(ed_row(1), "") && ed_cursor() == 1000, "Enter: the search's row empty again, output starts there");

    ed_reset(80, 25, 0);
    add("echo one");
    add("echo two");
    add("echo one");
    line_of("\022one", "\022", NULL);
    check(!strcmp(buf, "echo one"), "Ctrl+R again skips a line the same as the one shown (none older: it stays)");
    ed_reset(80, 25, 0);
    ed_keys("\022one\022");
    ed_snapshot();
    line_of(NULL, NULL, NULL);
    check(!strcmp(ed_snap_row(0, 1), "search: one (not found)") && !strcmp(buf, "echo one"),
          "no other line holds it: said, and the line found stays");

    ed_reset(80, 25, 0);
    ed_keys("draft");
    ed_keys("\022echo");
    ed_keysn("\033", 1, -1);
    ed_snapshot();
    line_of(" text", NULL, NULL);
    check(!strcmp(buf, "draft text") && !strcmp(ed_snap_row(0, 0), "$ draft") && !strcmp(ed_snap_row(0, 1), "") &&
          ed_snap_cursor(0) == 7, "Esc: the line as it was, the cursor where it was, the row empty");
    ed_reset(80, 25, 0);
    line_of("draft\033[H\033[C", "\022make\007", "!");
    check(!strcmp(buf, "d!raft"), "^G likewise");
    ed_reset(80, 25, 0);
    ed_keys("\022zzz");
    ed_snapshot();
    ed_keys("\177\177\177");
    ed_snapshot();
    line_of("\007", NULL, NULL);
    check(!strcmp(ed_snap_row(0, 0), "$") && !strcmp(ed_snap_row(0, 1), "search: zzz (not found)") &&
          !strcmp(ed_snap_row(1, 1), "search:") && !strcmp(buf, ""), "a text no line holds; Backspace takes it back");
    ed_reset(80, 25, 0);
    line_of("\022echo t", "\177o", NULL);
    check(!strcmp(buf, "echo one"), "Backspace, then more text: from the newest line again");

    ed_reset(80, 25, 0);
    line_of("\022all", "\033[DX", NULL);
    check(!strcmp(buf, "makeX all"), "another key ends the search where it stands and is done (Left, then typing)");
    ed_reset(80, 25, 0);
    line_of("\022\022", NULL, NULL);
    check(!strcmp(buf, "make all"), "Ctrl+R twice: the last search's text");
    ed_reset(80, 25, 0);
    add("first");
    add("second");
    add("third");
    line_of("\022sec", "\033[A", NULL);
    check(!strcmp(buf, "first"), "Up after a search: the line before the one found");
    ed_reset(80, 25, 0);
    line_of("typed", "\022thi", "\033[B");
    check(!strcmp(buf, "typed"), "Down from the newest line found: what was typed");
    ed_reset(80, 25, 0);
    ed_keys("\022caf\xc3\xa9");
    ed_snapshot();
    ed_keys("\177");
    ed_snapshot();
    line_of("\007", NULL, NULL);
    check(!strcmp(ed_snap_row(0, 1), "search: caf\xc3\xa9 (not found)") && !strcmp(ed_snap_row(1, 1), "search: caf (not found)"),
          "Backspace takes a whole character from the text (UTF-8)");

    ed_reset(20, 10, 0);
    add("echo abcdefghijklmnopqrstuvwxyz");
    ed_keys("\022xyz");
    ed_snapshot();
    line_of(NULL, NULL, NULL);
    check(!strcmp(ed_snap_row(0, 0), "$ echo abcdefghijklm") && !strcmp(ed_snap_row(0, 1), "nopqrstuvwxyz") &&
          !strcmp(ed_snap_row(0, 2), "search: xyz") && ed_snap_cursor(0) == 1010,
          "a line over two rows: the search's row under its last one");
    check(!strcmp(ed_row(2), "") && ed_cursor() == 2000, "and emptied after Enter");
    ed_reset(20, 3, 0);
    ed_print("a\r\nb\r\n");                  /* the prompt on the last row */
    add("abc");
    ed_keys("\022abc");
    ed_snapshot();
    line_of(NULL, NULL, NULL);
    check(!strcmp(ed_snap_row(0, 0), "b") && !strcmp(ed_snap_row(0, 1), "$ abc") && !strcmp(ed_snap_row(0, 2), "search: abc") &&
          ed_snap_cursor(0) == 1002, "at the foot of the screen the search's row scrolls it up a row");
    check(!strcmp(ed_row(1), "$ abc") && !strcmp(ed_row(2), "") && ed_cursor() == 2000 && !strcmp(buf, "abc"),
          "and the line runs from there");
    ed_reset(16, 10, 0);
    ed_keys("\022%%%%%%%%%%%%%%%%%");
    ed_snapshot();
    line_of("\007", NULL, NULL);
    check(!strcmp(ed_snap_row(0, 1), "%%% (not found)"), "a long text: the row shows its end, never the last column");
}

/* the last colour sequence (ESC [ ... m) in o before end: where it starts, or NULL */
static const char *last_sgr(const char *o, const char *end) {
    const char *p, *found = NULL;
    for(p = o; p + 1 < end; p++) {
        const char *q = p + 2;
        if(p[0] != '\033' || p[1] != '[') continue;
        while(q < end && ((*q >= '0' && *q <= '9') || *q == ';')) q++;
        if(q < end && *q == 'm') found = p;
    }
    return found;
}

static void test_colour(void) {
    const char *o, *q, *sgr;
    ed_reset(80, 25, 1);
    add("colour test");
    line_of("\022colo", "\033[FQ", NULL);
    o = ed_output();
    q = strrchr(o, 'Q');
    sgr = q ? last_sgr(o, q) : NULL;
    check(strstr(o, "\033[0msearch: colo\033[K\033[0;36m") != NULL, "the search's row in the plain colour, teal after it");
    check(!strcmp(buf, "colour testQ") && sgr && !strncmp(sgr, "\033[0;36m", 7), "typed at the line's end after the search: teal");
}

static void test_tab(void) {
    ed_reset(80, 25, 0);
    line_of("ec\t", NULL, NULL);
    check(!strcmp(buf, "echo "), "Tab: a builtin");
    ed_reset(80, 25, 0);
    line_of("ca\t", NULL, NULL);
    check(!strcmp(buf, "cat "), "Tab: a program");
    ed_reset(80, 25, 0);
    ed_keys("ls\t\t");
    ed_snapshot();
    line_of(NULL, NULL, NULL);
    check(!strcmp(buf, "ls") && !strcmp(ed_snap_row(0, 1), "lsblk  lspci") && !strcmp(ed_snap_row(0, 2), "$ ls"),
          "a second Tab lists the kernel's commands, then the prompt and the line again");
    ed_reset(80, 25, 0);
    line_of("cat no\t", NULL, NULL);
    check(!strcmp(buf, "cat notes.txt "), "Tab: a file");
    ed_reset(80, 25, 0);
    line_of("cat s\t", "M\t", NULL);
    check(!strcmp(buf, "cat src/Makefile "), "Tab: a directory, then a file in it");
}

int main(void) {
    test_history();
    test_quote_lines();
    test_long_history();
    test_typing();
    test_words();
    test_esc();
    test_last_arg();
    test_search();
    test_colour();
    test_tab();
    if(failures) {
        printf("EDITOR UNIT TESTS FAILED: %d of %d checks\n", failures, checks);
        return 1;
    }
    printf("EDITOR UNIT TESTS PASSED: %d checks, 0 failures\n", checks);
    return 0;
}
