/* Host-side tests of surfsh's parser and expansions (10/24/26): user/surfsh/parse.c, unparse.c, expand.c,
   vars.c, arith.c and glob.c as they are (tests/host/surfsh_bridge.c). A text is parsed into its first complete command,
   which unparse.c writes back: the words as typed, lists, && and ||, !, pipelines, groups, subshells,
   redirections with their descriptors, here-documents (<< <<- and a quoted end line), comments and
   empty commands; text that ends inside a command (PARSE_MORE, with what is open) and mistakes
   (PARSE_ERROR); the limits on nesting and on a pipeline's length; if, while, until, for, case and
   functions' definitions (10/25/26). Then words become fields: quotes,
   backslashes, $?, a here-document's text; variables, splitting at IFS, the positional parameters and
   "$@", every ${ } form, $(( )) (arith.c, also directly) and $( ) (exec.c's side stands in), and
   wildcards over a tree of names (glob.c); test and [ (test.c) and printf (printf.c), 10/25/26. Run by
   `make unittest`. */
#include <stdio.h>
#include <string.h>
#include "surfsh_sim.h"

static int checks, failures;

static void check(int ok, const char *what) {
    checks++;
    if(!ok) {
        failures++;
        printf("FAIL: %s\n", what);
    }
}

static char out[8192];

/* text parses (SH_OK) into want */
static void parses(const char *text, const char *want, const char *what) {
    int r = sh_parse(text, out, sizeof(out), NULL);
    checks++;
    if(r != SH_OK || strcmp(out, want)) {
        failures++;
        printf("FAIL: %s\n  text \"%s\"\n  got  %d \"%s\"\n  want \"%s\"\n", what, text, r, out, want);
    }
}

/* text gives r (SH_MORE, SH_ERROR) with a reason that has part in it */
static void fails(const char *text, int r, const char *part, const char *what) {
    int got = sh_parse(text, out, sizeof(out), NULL);
    checks++;
    if(got != r || !strstr(out, part)) {
        failures++;
        printf("FAIL: %s\n  text \"%s\"\n  got  %d \"%s\"\n  want %d with \"%s\"\n", what, text, got, out, r, part);
    }
}

static void expands(const char *word, const char *want, const char *what) {
    int r = sh_expand(word, out, sizeof(out));
    checks++;
    if(r < 0 || strcmp(out, want)) {
        failures++;
        printf("FAIL: %s\n  word \"%s\"\n  got  \"%s\"\n  want \"%s\"\n", what, word, r < 0 ? "(error)" : out, want);
    }
}

static void test_lists(void) {
    parses("echo hi\n", "echo hi\n", "a simple command");
    parses("echo   hi\tthere  \n", "echo hi there\n", "blanks between words");
    parses("echo 'a b' \"c $? d\" e\\ f\n", "echo 'a b' \"c $? d\" e\\ f\n", "words keep their quotes");
    parses("a | b | c\n", "a | b | c\n", "a pipeline");
    parses("a && b || c\n", "a && b || c\n", "&& and ||, left to right");
    parses("! a | b\n", "! a | b\n", "! before a pipeline");
    parses("a; b & c\n", "a; b & c\n", "; and &");
    parses("a &\n", "a &\n", "a job in the background");
    parses("a & b &\n", "a & b &\n", "two jobs");
    parses("{ a; b; } > f\n", "{ a; b; } >f\n", "a group with a redirection");
    parses("{ a & }\n", "{ a & }\n", "a group ending in &");
    parses("( cd x; ls ) | wc\n", "( cd x; ls ) | wc\n", "a subshell in a pipeline");
    parses("{\na\nb\n}\n", "{ a; b; }\n", "a group over lines");
    parses("a |\nb\n", "a | b\n", "a pipeline over two lines");
    parses("a &&\n\nb\n", "a && b\n", "&& and blank lines");
    parses("a; ; b\n", "a; b\n", "an empty command between two");
    parses("; a\n", "a\n", "a leading ;");
    parses("a;; b\n", "a; b\n", ";; outside a case");
    parses("a # b c\n", "a\n", "a comment");
    parses("a#b c\n", "a#b c\n", "# inside a word");
    parses("x=1 y=2 cmd a=b\n", "x=1 y=2 cmd a=b\n", "assignments before the command");
    parses("\"x\"=1 cmd\n", "\"x\"=1 cmd\n", "a quoted name is no assignment");
    parses("echo a\\\nb\n", "echo a\\\nb\n", "a \\ and a newline inside a word stay for the expansion");
    parses("echo a \\\n b\n", "echo a b\n", "a \\ and a newline between words join the lines");
    parses("echo $(echo ')' \"(\") x\n", "echo $(echo ')' \"(\") x\n", "a $( ) with quoted parentheses");
    parses("echo $( (a) ) $((1 + (2 * 3))) ${x:-y} `b`\n", "echo $( (a) ) $((1 + (2 * 3))) ${x:-y} `b`\n",
           "$( ), $(( )), ${ } and ` ` are words");
    parses("echo $(a\nb)\n", "echo $(a\nb)\n", "a $( ) over two lines");
    parses("a\nb\n", "a\n", "the first of two lines");
    check(sh_parse("\n\n# only a comment\n", out, sizeof(out), NULL) == SH_EOF, "blank lines and a comment: the end");
    check(sh_parse("", out, sizeof(out), NULL) == SH_EOF, "no text: the end");
    {
        int used = 0;
        sh_parse("first x\nsecond\n", out, sizeof(out), &used);
        check(used == 8 && !strcmp(out, "first x\n"), "the first command ends after its newline");
    }
}

static void test_redirections(void) {
    parses("cmd 2>&1 >out <in 3>>app\n", "cmd 2>&1 >out <in 3>>app\n", "descriptors and operators");
    parses("cmd 2> err >|clob <>rw <&- >&2\n", "cmd 2>err >|clob <>rw <&- >&2\n", "the other operators");
    parses("echo a2>f\n", "echo a2 >f\n", "digits inside a word are no descriptor");
    parses("echo 123>f\n", "echo 123 >f\n", "three digits are no descriptor");
    parses(">f\n", ">f\n", "a redirection alone");
    parses("<f cmd >g\n", "cmd <f >g\n", "redirections anywhere among the words");
    parses("cat <<EOF\nline 1\n$? x\nEOF\n", "cat <<EOF\nline 1\n$? x\nEOF\n", "a here-document");
    parses("cat <<-E\n\tone\n\t\ttwo\n\tE\n", "cat <<-E\none\ntwo\nE\n", "<<- takes the tabs off");
    parses("cat <<'E' | wc\n$x\nE\n", "cat <<'E' | wc\n$x\nE\n", "a quoted end line, in a pipeline");
    parses("a <<A; b <<B\n1\nA\n2\nB\n", "a <<A; b <<B\n1\nA\n2\nB\n", "two here-documents, in order");
    parses("cat <<E\nEOF\n E\nE\n", "cat <<E\nEOF\n E\nE\n", "only the whole line ends a here-document");
    fails("cmd >\n", SH_ERROR, "a file name must follow >", "> without a file");
    fails("cmd 2>&\n", SH_ERROR, "a file name must follow >&", ">& without a descriptor");
    fails("cat <<\n", SH_ERROR, "an end line must follow <<", "<< without an end line");
}

static void test_more(void) {
    fails("echo 'abc\n", SH_MORE, "a ' is not closed", "an open '");
    fails("echo \"abc\n", SH_MORE, "a \" is not closed", "an open \"");
    fails("echo `abc\n", SH_MORE, "a ` is not closed", "an open `");
    fails("echo $(abc\n", SH_MORE, "a $( is not closed", "an open $(");
    fails("echo ${abc\n", SH_MORE, "a ${ is not closed", "an open ${");
    fails("echo $((1 +\n", SH_MORE, "a $(( is not closed", "an open $((");
    fails("a |\n", SH_MORE, "a | needs a command after it", "a | at the end");
    fails("a &&\n", SH_MORE, "a && needs a command after it", "a && at the end");
    fails("a ||\n\n", SH_MORE, "a || needs a command after it", "a || and a blank line");
    fails("{ a\n", SH_MORE, "a { group is not closed with }", "an open group");
    fails("( a\n", SH_MORE, "a ( is not closed", "an open (");
    fails("echo a\\\n", SH_MORE, "the line ends in a \\", "a \\ at the end of the line");
    fails("cat <<E\nx\n", SH_MORE, "a here-document has no end line", "a here-document without its end");
    fails("cat <<E", SH_MORE, "here-document", "the end line not reached yet");
    fails("! \n", SH_ERROR, "a ! needs a command after it", "! and the end of the line");
    fails("!", SH_MORE, "a ! needs a command after it", "! at the end of the text");
    fails("echo $(cat <<E\nx\n", SH_MORE, "", "a here-document inside $( ) not ended");
}

static void test_errors(void) {
    char deep[512] = "", pipe[512] = "a";
    int i;
    fails("echo (\n", SH_ERROR, "unexpected (", "( after a word");
    fails("| a\n", SH_ERROR, "| needs a command before it", "| first");
    fails("&& a\n", SH_ERROR, "&& needs a command before it", "&& first");
    fails("& a\n", SH_ERROR, "& needs a command before it", "& first");
    fails("}\n", SH_ERROR, "} without a { before it", "} alone");
    fails("{ }\n", SH_ERROR, "a { } group needs a command in it", "an empty group");
    fails("( )\n", SH_ERROR, "a ( ) needs a command in it", "an empty ( )");
    fails("{ ; }\n", SH_ERROR, "a command is missing", "a group of an empty command");
    fails("a )\n", SH_ERROR, "unexpected )", "a ) alone");
    fails("echo $(a ; ) b)\n", SH_ERROR, "", "a ) inside $( ) that ends nothing");
    check(sh_parse("a\nb (\n", out, sizeof(out), NULL) == SH_OK && !strcmp(out, "a\n"), "only the first command is read");
    for(i = 0; i < 70; i++) strcat(deep, "{ ");
    strcat(deep, "a\n");
    fails(deep, SH_ERROR, "commands inside each other: at most 64", "groups too deep");
    for(i = 0; i < 16; i++) strcat(pipe, " | a");
    strcat(pipe, "\n");
    fails(pipe, SH_ERROR, "commands in a pipeline: at most 16", "a pipeline too long");
    check(sh_parse("a\nb\n{ c\n", out, sizeof(out), NULL) == SH_OK, "a mistake later in the text is for later");
    sh_parse("a |\n\n\nb (\n", out, sizeof(out), NULL);
    check(sh_err_line() == 4, "the line of a mistake");
}

/* 10/25/26: compound commands and functions, parsed and written back */
static void test_compound(void) {
    static char deep[1024];
    int k;
    parses("if a; then b; fi\n", "if a; then b; fi\n", "an if");
    parses("if a\nthen\n  b\nelif c; then d\nelse e\nfi\n", "if a; then b; elif c; then d; else e; fi\n",
           "an if over lines, elif and else");
    parses("if a; then b; else if c; then d; fi; fi\n", "if a; then b; elif c; then d; fi\n", "else if ... fi is elif");
    parses("if a; then b; else if c; then d; fi > f; fi\n", "if a; then b; else if c; then d; fi >f; fi\n",
           "... but not one with a redirection");
    parses("if a; b; then c; d; fi > out 2>&1\n", "if a; b; then c; d; fi >out 2>&1\n", "lists in an if, its redirections");
    parses("if a & then b & fi\n", "if a & then b & fi\n", "lists that end in &");
    parses("while a; do b; done\n", "while a; do b; done\n", "a while");
    parses("until a\ndo\n  b\ndone\n", "until a; do b; done\n", "an until over lines");
    parses("for x in a 'b c' $d; do echo $x; done\n", "for x in a 'b c' $d; do echo $x; done\n", "a for");
    parses("for x\ndo echo $x\ndone\n", "for x; do echo $x; done\n", "a for without in");
    parses("for x; do a; done\n", "for x; do a; done\n", "for x; do");
    parses("for x in; do a; done\n", "for x in; do a; done\n", "a for with in and no words");
    parses("for x\nin a b\ndo c; done\n", "for x in a b; do c; done\n", "a newline before in");
    parses("for i in 1 2; do for j in a b; do echo $i$j; done; done\n",
           "for i in 1 2; do for j in a b; do echo $i$j; done; done\n", "loops inside each other");
    parses("case $x in a|b) echo ab;; (c) echo c;; *) ;; esac\n",
           "case $x in (a|b) echo ab ;; (c) echo c ;; (*) ;; esac\n", "a case");
    parses("case x in\na)\n  echo a\n  ;;\nb) echo b\nesac\n", "case x in (a) echo a ;; (b) echo b ;; esac\n",
           "a case over lines, its last ;; left out");
    parses("case x in esac\n", "case x in esac\n", "a case with nothing in it");
    parses("case x in (esac) a;; esac\n", "case x in (esac) a ;; esac\n", "esac as a pattern after (");
    parses("case \"$1\" in -*) a; b & ;; esac\n", "case \"$1\" in (-*) a; b & ;; esac\n", "a case item's list");
    parses("f() { echo hi; }\n", "f() { echo hi; }\n", "a function");
    parses("f ( )\n{\n  echo hi\n}\n", "f() { echo hi; }\n", "a function over lines");
    parses("f() ( cd /; ls ) > out\n", "f() ( cd /; ls ) >out\n", "a function of a ( ) with a redirection");
    parses("f() if a; then b; fi\n", "f() if a; then b; fi\n", "a function of an if");
    parses("f() { g() { x; }; }\n", "f() { g() { x; }; }\n", "a function that defines one");
    parses("while a; do if b; then break; fi; done | c\n", "while a; do if b; then break; fi; done | c\n",
           "a loop in a pipeline");
    parses("echo if then fi do done esac\n", "echo if then fi do done esac\n", "reserved words as arguments");
    parses("{ if a; then b; fi; }\n", "{ if a; then b; fi; }\n", "an if in a group");
    parses("x=$(case a in a) echo yes;; esac)\n", "x=$(case a in a) echo yes;; esac)\n",
           "a case inside $( ): its ) is the pattern's");
    parses("if a; then cat <<E; fi\nline\nE\n", "if a; then cat <<E; fi\nline\nE\n", "a here-document inside an if");
    fails("if a; then b\n", SH_MORE, "an if is not closed with fi", "an if open");
    fails("if a\n", SH_MORE, "an if needs a then", "an if without its then yet");
    fails("if a; fi\n", SH_ERROR, "unexpected fi", "fi before then");
    fails("if a; then b; done\n", SH_ERROR, "unexpected done", "done in an if");
    fails("while a; do b; fi\n", SH_ERROR, "unexpected fi", "fi in a loop");
    fails("for x in a; b; done\n", SH_ERROR, "unexpected b where do was to come", "a for without its do");
    fails("if then b; fi\n", SH_ERROR, "a command is missing before then", "an if with no condition");
    fails("then\n", SH_ERROR, "unexpected then", "then alone");
    fails("a; fi\n", SH_ERROR, "unexpected fi", "fi alone");
    fails("while a; do b\n", SH_MORE, "a do is not closed with done", "a while open");
    fails("while a\n", SH_MORE, "a while needs a do", "a while without its do yet");
    fails("while a; do done\n", SH_ERROR, "a command is missing before done", "an empty loop");
    fails("for 1x in a; do b; done\n", SH_ERROR, "a for needs a variable's name, not 1x", "a for of a number");
    fails("for x in a b\n", SH_MORE, "a for needs a do", "a for open");
    fails("for x in a > f; do b; done\n", SH_ERROR, "unexpected > in a for's words", "a redirection in a for's words");
    fails("case x in a) b\n", SH_MORE, "a case is not closed with esac", "a case open");
    fails("case x in\n", SH_MORE, "a case is not closed with esac", "a case open after in");
    fails("case x in a b) c;; esac\n", SH_ERROR, "unexpected b in a case's patterns", "two patterns without |");
    fails("case x in a) b; c) d;; esac\n", SH_ERROR, "unexpected )", "an item without its ;;");
    fails("case x a) b;; esac\n", SH_ERROR, "unexpected a where in was to come", "a case without in");
    fails("f() echo hi\n", SH_ERROR, "a function's body is a { } group or another compound command",
          "a function of a simple command");
    fails("f()\n", SH_MORE, "a function needs a body", "a function, its body to come");
    for(k = 0; k < 70; k++) memcpy(deep + k * 11, "if a; then ", 11);
    strcpy(deep + 770, "b\n");
    fails(deep, SH_ERROR, "commands inside each other: at most 64", "ifs inside each other: too deep");
}

static void test_expand(void) {
    expands("plain", "plain|", "a word as it is");
    expands("'a b'", "a b|", "single quotes");
    expands("'$? \\ \"'", "$? \\ \"|", "single quotes keep everything");
    expands("\"x$?y\"", "x7y|", "$? in double quotes");
    expands("$?", "7|", "$?");
    expands("a\\ b", "a b|", "a \\ keeps a blank");
    expands("\\$?", "$?|", "\\$? is the two characters");
    expands("\"\\$?\"", "$?|", "\\$ in double quotes");
    expands("\"a\\\"b\\\\c\\x\"", "a\"b\\c\\x|", "\\ in double quotes keeps only \" \\ $ `");
    expands("ab\\\ncd", "abcd|", "a \\ and a newline go");
    expands("\"ab\\\ncd\"", "abcd|", "... in double quotes too");
    expands("''", "|", "an empty word in quotes is a field");
    expands("\"\"", "|", "\"\" too");
    expands("a'b'\"c\"d", "abcd|", "quotes inside a word");
    expands("$", "$|", "a $ alone is itself");
    expands("a$%b", "a$%b|", "a $ before what is no parameter is itself");
    expands("end\\", "end\\|", "a \\ at the very end is itself");
    check(sh_expand_doc("a $? \\$ \\\\ \\x \"q\" 'q'\nb\\\nc\n", out, sizeof(out)) == 0 &&
          !strcmp(out, "a 7 $ \\ \\x \"q\" 'q'\nbc\n"), "a here-document's text: $?, \\ before $ \\ and a newline");
}

/* 10/24/26: variables, field splitting, the positional parameters, ${ } */
static void test_parameters(void) {
    static const char *const args[] = { "a", "b c", "" };
    sh_set_var("x", "hello");
    sh_set_var("sp", "a  b c");
    sh_set_var("st", "*");
    sh_set_var("e", "");
    sh_set_var("u", NULL);
    sh_set_var("f", "/a/b/c.txt");
    sh_set_var("IFS", NULL);
    expands("$x", "hello|", "$name");
    expands("pre${x}post$x", "prehelloposthello|", "${name} and $name in a word");
    expands("'$x'\\$x\"$x\"", "$x$xhello|", "quoted $ is itself, but not in double quotes");
    expands("$sp", "a|b|c|", "an unquoted value is split at blanks");
    expands("\"$sp\"", "a  b c|", "a quoted one is not");
    expands("x${sp}y", "xa|b|cy|", "split inside a word");
    expands("$e", "", "an empty value unquoted: no field");
    expands("\"$e\"", "|", "quoted: an empty field");
    expands("$u$e", "", "unset and empty: no field");
    expands("$st", "*|", "a value's * with nothing to match it stays");
    expands("${#x}.${#u}.${#e}", "5.0.0|", "${#name}");
    sh_set_var("ue", "\xc3\xa9t\xc3\xa9");
    expands("${#ue}", "3|", "${#name} counts characters, not bytes");
    expands("${u:-d1}${u-d2}${e:-d3}${x:-d4}", "d1d2d3hello|", "${:-} and ${-}");
    expands("\"${e-d5}\"", "|", "${e-...} of an empty but set e is its value");
    expands("${x:+alt}${u:+alt}${e:+alt}${e+set}", "altset|", "${:+} and ${+}");
    expands("${v:=assigned}", "assigned|", "${:=} gives the word");
    check(sh_get_var("v") && !strcmp(sh_get_var("v"), "assigned"), "... and assigns it");
    check(sh_expand("${u:?it is missing}", out, sizeof(out)) < 0, "${:?} is an error");
    check(sh_expand("${e:?}", out, sizeof(out)) < 0, "${:?} of an empty one too");
    expands("${f#*/}|${f##*/}|${f%/*}|${f%%/*}|${f%.txt}", "a/b/c.txt|c.txt|/a/b||/a/b/c|", "# ## % %%");
    expands("${f#\"*\"}", "/a/b/c.txt|", "a quoted pattern is itself");
    expands("${x%l*}${x%%l*}", "helhe|", "% shortest, %% longest");
    expands("${u:-a b}", "a|b|", "the word of ${:-} unquoted is split");
    expands("\"${u:-a b}\"", "a b|", "... quoted not");
    expands("${u:-\"a b\" c}", "a b|c|", "... nor the quoted part of it");
    expands("${x:+$sp}", "a|b|c|", "a value in the word of ${:+}");
    sh_set_var("IFS", ":");
    sh_set_var("c", "a::b:");
    expands("$c", "a||b|", "IFS=: a::b: is three fields, the last : ends one");
    sh_set_var("c", ":lead");
    expands("$c", "|lead|", "a field before a leading :");
    sh_set_var("IFS", " :");
    sh_set_var("c", " a : b  c :");
    expands("$c", "a|b|c|", "blanks and one : around it are one delimiter");
    sh_set_var("IFS", "");
    expands("$sp", "a  b c|", "IFS empty: nothing is split");
    sh_set_var("IFS", NULL);
    sh_set_args(3, args);
    expands("$#.$1.${2}", "3.a.b|c|", "$# $1 ${2}");
    expands("\"$@\"", "a|b c||", "\"$@\": a field each, the empty one too");
    expands("$@", "a|b|c|", "$@ unquoted: split, the empty one gone");
    expands("\"x$@y\"", "xa|b c|y|", "\"x$@y\"");
    expands("\"$*\"", "a b c |", "\"$*\": one field, joined by a blank");
    sh_set_var("IFS", "-");
    expands("\"$*\"", "a-b c-|", "\"$*\" joined by IFS's first character");
    sh_set_var("IFS", NULL);
    expands("${10}${3}x", "x|", "a parameter that is not there");
    sh_set_args(0, args);
    expands("\"$@\"", "", "\"$@\" of none: no field at all");
    expands("\"${@}\"", "", "\"${@}\" of none: no field at all");
    expands("\"x$@\"", "x|", "\"x$@\" of none");
    expands("\"$*\"", "|", "\"$*\" of none: one empty field");
    expands("$#", "0|", "$# of none");
    expands("$$.$!.$0", "1.42.surfsh|", "$$ $! $0");
    sh_set_args(3, args);
    check(sh_expand_doc("<$@> <$*>\n", out, sizeof(out)) == 0 && !strcmp(out, "<a b c > <a b c >\n"),
          "$@ and $* in a here-document: joined by blanks");
    expands("${x%\"$@\"}", "hello|", "\"$@\" in a pattern: joined, and it does not match");
    sh_set_args(0, args);
}

/* $(( )) and $( ) */
static void test_arith(void) {
    char err[120];
    long v = 0;
    static const struct { const char *text; long want; } ok[] = {
        { "1 + 2 * 3", 7 }, { "(1 + 2) * 3", 9 }, { "7 / 2", 3 }, { "-7 / 2", -3 }, { "7 % 3", 1 }, { "-7 % 3", -1 },
        { "1 << 4", 16 }, { "256 >> 2", 64 }, { "0x10 + 010", 24 }, { "~0", -1 }, { "!5", 0 }, { "!0", 1 },
        { "3 > 2", 1 }, { "3 <= 2", 0 }, { "2 == 2 && 3 != 4", 1 }, { "0 || 0", 0 }, { "6 & 3", 2 }, { "6 | 3", 7 },
        { "6 ^ 3", 5 }, { "1 ? 2 : 3", 2 }, { "0 ? 2 : 0 ? 4 : 5", 5 }, { "-(3)", -3 }, { "+4", 4 }, { "", 0 },
        { "2147483647 + 1", -2147483647L - 1 }, { "n", 5 }, { "n * n", 25 }, { "undefined + 1", 1 },
    };
    unsigned i;
    sh_set_var("n", "5");
    for(i = 0; i < sizeof(ok) / sizeof(ok[0]); i++) {
        int r = sh_arith(ok[i].text, &v, err, sizeof(err));
        checks++;
        if(r < 0 || v != ok[i].want) {
            failures++;
            printf("FAIL: $((%s)) is %ld (%s), not %ld\n", ok[i].text, v, r < 0 ? err : "ok", ok[i].want);
        }
    }
    check(sh_arith("m = 4", &v, err, sizeof(err)) == 0 && v == 4 && !strcmp(sh_get_var("m"), "4"), "an assignment");
    check(sh_arith("m += 3", &v, err, sizeof(err)) == 0 && v == 7 && !strcmp(sh_get_var("m"), "7"), "+=");
    check(sh_arith("m <<= 1", &v, err, sizeof(err)) == 0 && v == 14, "<<=");
    check(sh_arith("a = b = 2", &v, err, sizeof(err)) == 0 && v == 2 && !strcmp(sh_get_var("a"), "2") &&
          !strcmp(sh_get_var("b"), "2"), "assignments to the right");
    check(sh_arith("m++", &v, err, sizeof(err)) == 0 && v == 14 && !strcmp(sh_get_var("m"), "15"), "m++ gives the old value");
    check(sh_arith("++m", &v, err, sizeof(err)) == 0 && v == 16 && !strcmp(sh_get_var("m"), "16"), "++m the new one");
    check(sh_arith("--m", &v, err, sizeof(err)) == 0 && v == 15, "--m");
    check(sh_arith("0 && (m = 99)", &v, err, sizeof(err)) == 0 && v == 0 && !strcmp(sh_get_var("m"), "15"),
          "&& does not work out the side it does not take");
    check(sh_arith("1 || 1 / 0", &v, err, sizeof(err)) == 0 && v == 1, "|| neither (no division by 0 there)");
    check(sh_arith("1 ? 3 : (m = 1)", &v, err, sizeof(err)) == 0 && v == 3 && !strcmp(sh_get_var("m"), "15"), "?: neither");
    check(sh_arith("1 / 0", &v, err, sizeof(err)) < 0 && strstr(err, "division by 0"), "division by 0");
    check(sh_arith("1 % 0", &v, err, sizeof(err)) < 0, "% by 0");
    check(sh_arith("-2147483648 / -1", &v, err, sizeof(err)) < 0, "the one division the processor refuses");
    check(sh_arith("1 +", &v, err, sizeof(err)) < 0, "1 +");
    check(sh_arith("(1", &v, err, sizeof(err)) < 0 && strstr(err, "not closed"), "an open (");
    check(sh_arith("1 2", &v, err, sizeof(err)) < 0, "two numbers");
    check(sh_arith("08", &v, err, sizeof(err)) < 0, "8 is no octal digit");
    sh_set_var("w", "abc");
    check(sh_arith("w + 1", &v, err, sizeof(err)) < 0 && strstr(err, "not a number"), "a variable that is no number");
    check(sh_arith("0 && w", &v, err, sizeof(err)) == 0 && v == 0, "... on the side not taken: no error");
    sh_set_var("ro", "1");
    sh_readonly("ro");
    check(sh_arith("ro = 2", &v, err, sizeof(err)) < 0 && strstr(err, "ro is read-only") && !strcmp(sh_get_var("ro"), "1"),
          "an assignment to a read-only variable");
    {
        char deep[400];
        int k;
        for(k = 0; k < 90; k++) memcpy(deep + 4 * k, "1?1:", 4);
        strcpy(deep + 360, "1");
        check(sh_arith(deep, &v, err, sizeof(err)) < 0 && strstr(err, "too deep"), "?: nested too deep");
        for(k = 0; k < 90; k++) deep[k] = '(';
        deep[90] = '1';
        for(k = 0; k < 90; k++) deep[91 + k] = ')';
        deep[181] = 0;
        check(sh_arith(deep, &v, err, sizeof(err)) < 0 && strstr(err, "too deep"), "( ) nested too deep");
        for(k = 0; k < 30; k++) memcpy(deep + 2 * k, "1?", 2);
        strcpy(deep + 60, "7");
        for(k = 0; k < 30; k++) memcpy(deep + 61 + 2 * k, ":0", 2);
        deep[121] = 0;
        check(sh_arith(deep, &v, err, sizeof(err)) == 0 && v == 7, "?: 30 deep is fine");
    }
    expands("$((n + 1))", "6|", "$(( )) in a word");
    expands("$(( $n * 2 ))x", "10x|", "$n inside $(( ))");
    check(sh_expand("$((1 / 0))", out, sizeof(out)) < 0, "$(( )) that fails is an error");
    expands("$(echo a b)", "[echo|a|b]|", "$( ) unquoted is split");
    expands("\"$(echo a  b)\"", "[echo a  b]|", "quoted not, and its newlines at the end go");
    expands("`echo \\$n`", "[echo|$n]|", "` ` with \\$");
    expands("\"`echo \\\"q\\\"`\"", "[echo \"q\"]|", "` ` in double quotes with \\\"");
    expands("$(echo $(inner))", "[echo|$(inner)]|", "$( ) inside $( ) is the inner command's");
    check(sh_expand_doc("x=$x $(c) $((1+1)) \\$x \"q\" '$x'\n", out, sizeof(out)) == 0 &&
          !strcmp(out, "x=hello [c] 2 $x \"q\" 'hello'\n"), "a here-document's expansions");
    check(sh_expand_one("a  $sp $st", out, sizeof(out)) == 0 && !strcmp(out, "a  a  b c *"),
          "one word for a file's name: nothing split nor matched");
}

/* wildcards over the names of sh_tree() */
static void test_glob(void) {
    static const char *const names[] = {
        "/g/", "/g/a.txt", "/g/b.txt", "/g/c.c", "/g/.hid", "/g/sub/", "/g/sub/x.txt", "/g/sub/y.c",
        "/g/\xc3\xa9.txt", "/g/[x]", "rel/", "rel/one", "rel/two", NULL
    };
    sh_tree(names);
    expands("/g/*.txt", "/g/a.txt|/g/b.txt|/g/\xc3\xa9.txt|", "*.txt, sorted, é after b");
    expands("/g/?.txt", "/g/a.txt|/g/b.txt|/g/\xc3\xa9.txt|", "? is one character, é too");
    expands("/g/?.c", "/g/c.c|", "?.c");
    expands("/g/*", "/g/[x]|/g/a.txt|/g/b.txt|/g/c.c|/g/sub|/g/\xc3\xa9.txt|", "* leaves the dot names out");
    expands("/g/.*", "/g/.hid|", "only a . finds them");
    expands("/g/*/*.c", "/g/sub/y.c|", "a pattern in each part");
    expands("/g/[ab].txt", "/g/a.txt|/g/b.txt|", "[ab]");
    expands("/g/[!a]*.txt", "/g/b.txt|/g/\xc3\xa9.txt|", "[!a]");
    expands("/g/[a-b].txt", "/g/a.txt|/g/b.txt|", "[a-b]");
    expands("/g/sub/*", "/g/sub/x.txt|/g/sub/y.c|", "below a plain part");
    expands("/g/\\*.txt", "/g/*.txt|", "\\* is a star");
    expands("'/g/*.txt'", "/g/*.txt|", "and so is a quoted one");
    expands("\"/g/\"*.c", "/g/c.c|", "a quoted part with a pattern after it");
    expands("/g/nothing*", "/g/nothing*|", "nothing matches: the word stays");
    expands("/g/[x]", "/g/[x]|", "[x] matches x, which is not there: the word stays");
    expands("/g/[x", "/g/[x|", "a [ not closed is itself");
    expands("rel/*", "rel/one|rel/two|", "a relative pattern");
    expands("/g/*/x.*", "/g/sub/x.txt|", "a file below a matched directory");
    expands("/g/a.txt/*", "/g/a.txt/*|", "a file is no directory to go into");
    sh_set_var("pat", "/g/*.c");
    expands("$pat", "/g/c.c|", "a pattern in a value matches");
    expands("\"$pat\"", "/g/*.c|", "quoted it does not");
}

/* 10/25/26: test and [ (test.c), on the tree of names test_glob() made */
static int t(const char *a, const char *b, const char *c, const char *d, const char *e, const char *f) {
    char *v[6];
    int n = 0;
    if(a) v[n++] = (char *)a;
    if(b) v[n++] = (char *)b;
    if(c) v[n++] = (char *)c;
    if(d) v[n++] = (char *)d;
    if(e) v[n++] = (char *)e;
    if(f) v[n++] = (char *)f;
    return sh_test(n, v);
}

static void test_test(void) {
    check(t(0, 0, 0, 0, 0, 0) == 1, "test alone: false");
    check(t("x", 0, 0, 0, 0, 0) == 0 && t("", 0, 0, 0, 0, 0) == 1, "one argument: not empty");
    check(t("-n", 0, 0, 0, 0, 0) == 0 && t("-z", 0, 0, 0, 0, 0) == 0, "-n alone is a string");
    check(t("!", 0, 0, 0, 0, 0) == 0, "! alone is a string");
    check(t("-n", "a", 0, 0, 0, 0) == 0 && t("-n", "", 0, 0, 0, 0) == 1 && t("-z", "", 0, 0, 0, 0) == 0, "-n -z");
    check(t("!", "", 0, 0, 0, 0) == 0 && t("!", "x", 0, 0, 0, 0) == 1, "! s");
    check(t("-e", "/g/a.txt", 0, 0, 0, 0) == 0 && t("-e", "/g/none", 0, 0, 0, 0) == 1, "-e");
    check(t("-f", "/g/a.txt", 0, 0, 0, 0) == 0 && t("-f", "/g/sub", 0, 0, 0, 0) == 1, "-f");
    check(t("-d", "/g/sub", 0, 0, 0, 0) == 0 && t("-d", "/g/a.txt", 0, 0, 0, 0) == 1, "-d");
    check(t("-s", "/g/a.txt", 0, 0, 0, 0) == 0, "-s of a file with a byte in it");
    check(t("-x", "/g/sub", 0, 0, 0, 0) == 0 && t("-h", "/g/a.txt", 0, 0, 0, 0) == 1, "-x, -h");
    check(t("a", "=", "a", 0, 0, 0) == 0 && t("a", "=", "b", 0, 0, 0) == 1 && t("a", "!=", "b", 0, 0, 0) == 0 &&
          t("a", "==", "a", 0, 0, 0) == 0, "= != ==");
    check(t("a", "<", "b", 0, 0, 0) == 0 && t("b", ">", "a", 0, 0, 0) == 0 && t("b", "<", "a", 0, 0, 0) == 1, "< >");
    check(t("-n", "=", "-n", 0, 0, 0) == 0, "three arguments: the binary operator first");
    check(t("!", "=", "!", 0, 0, 0) == 0, "\"!\" = \"!\" compares");
    check(t("10", "-eq", "10", 0, 0, 0) == 0 && t("9", "-lt", "10", 0, 0, 0) == 0 && t("-3", "-le", "-3", 0, 0, 0) == 0 &&
          t("2", "-gt", "10", 0, 0, 0) == 1 && t("2", "-ne", "2", 0, 0, 0) == 1 && t("7", "-ge", "8", 0, 0, 0) == 1,
          "-eq -lt -le -gt -ne -ge as numbers");
    check(t("abc", "-eq", "1", 0, 0, 0) == 2, "a word that is no number: 2");
    check(t("!", "a", "=", "b", 0, 0) == 0, "four arguments: ! and three");
    check(t("(", "-n", "x", ")", 0, 0) == 0, "( two )");
    check(t("(", "x", ")", 0, 0, 0) == 0, "( one )");
    check(t("-f", "/g/a.txt", "-a", "-d", "/g/sub", 0) == 0 && t("-f", "/g/a.txt", "-a", "-d", "/g/a.txt", 0) == 1,
          "-a");
    check(t("-f", "/g/none", "-o", "-d", "/g/sub", 0) == 0, "-o");
    check(t("!", "-f", "/g/none", "-a", "x", 0) == 0, "! binds tighter than -a");
    check(t("", "-o", "x", "-a", "", 0) == 1, "-a binds tighter than -o");
    check(t("(", "", "-o", "x", ")", 0) == 0, "( a -o b ) in five arguments");
    check(t("(", "", "-o", "x", ")", "-a") == 2, "an -a with nothing after it: 2");
    check(t("x", "-q", "y", 0, 0, 0) == 2, "an operator that is none: 2");
    check(t("-e", "/g/a.txt", "/g/b.txt", 0, 0, 0) == 2, "a word too many: 2");
    check(t("/g/a.txt", "-ef", "/g/a.txt", 0, 0, 0) == 0 && t("/g/a.txt", "-ef", "/g/b.txt", 0, 0, 0) == 1, "-ef");
    check(t("/g/a.txt", "-nt", "/g/none", 0, 0, 0) == 0 && t("/g/none", "-ot", "/g/a.txt", 0, 0, 0) == 0, "-nt -ot");
}

/* printf (printf.c) */
static void prints(const char *const *args, const char *want, int status, const char *what) {
    int n = 0, r;
    while(args[n]) n++;
    r = sh_printf(n, args, out, sizeof(out));
    checks++;
    if(r != status || strcmp(out, want)) {
        failures++;
        printf("FAIL: %s\n  got  %d \"%s\"\n  want %d \"%s\"\n", what, r, out, status, want);
    }
}

static void test_printf(void) {
    static const char *const a1[] = { "a\\tb\\n", NULL }, *const a2[] = { "%s-%s|", "x", "y", "z", NULL },
        *const a3[] = { "%5s|%-5s|%.2s|", "ab", "cd", "efg", NULL }, *const a4[] = { "%d %i %x %X %o %u|", "42", "-7", "255", "255", "8", "3", NULL },
        *const a5[] = { "%05d|%+d|% d|%-4d|", "42", "5", "6", "7", NULL }, *const a6[] = { "%c%c|", "hello", "", NULL },
        *const a7[] = { "%b|", "a\\nb\\0101\\c-gone", NULL }, *const a8[] = { "%d|", "0x10", NULL },
        *const a9[] = { "%d|", "'A", NULL }, *const a10[] = { "%d|%s|", NULL }, *const a11[] = { "%d|", "12abc", NULL },
        *const a12[] = { "100%%\\101\\n", NULL }, *const a13[] = { "%*d|%.*s|", "4", "7", "2", "abc", NULL },
        *const a14[] = { "%q", "x", NULL }, *const a15[] = { "no conversions\\n", "extra", NULL },
        *const a16[] = { "%s\\n", "one", "two", NULL };
    prints(a1, "a\tb\n", 0, "\\t \\n in the format");
    prints(a2, "x-y|z-|", 0, "the format again for the arguments left, a missing one empty");
    prints(a3, "   ab|cd   |ef|", 0, "width, -, precision");
    prints(a4, "42 -7 ff FF 10 3|", 0, "%d %i %x %X %o %u");
    prints(a5, "00042|+5| 6|7   |", 0, "0 + space -");
    prints(a6, "h|", 0, "%c: the first character, nothing for an empty one");
    prints(a7, "a\nbA", 0, "%b: \\n, \\0101, and \\c ends all output");
    prints(a8, "16|", 0, "0x10 is a number");
    prints(a9, "65|", 0, "'A is the character's code");
    prints(a10, "0||", 0, "no arguments: 0 and \"\"");
    prints(a11, "12|", 1, "12abc: said, 12, status 1");
    prints(a12, "100%A\n", 0, "%% and \\101");
    prints(a13, "   7|ab|", 0, "* for the width and the precision");
    prints(a14, "", 2, "%q: no conversion, status 2");
    prints(a15, "no conversions\n", 0, "a format with no conversion: once, whatever the arguments");
    prints(a16, "one\ntwo\n", 0, "a line for each argument");
}

int main(void) {
    test_lists();
    test_redirections();
    test_more();
    test_errors();
    test_compound();
    sh_set_status(7);
    test_expand();
    test_parameters();
    test_arith();
    test_glob();
    test_test();
    test_printf();
    printf("SURFSH UNIT TESTS %s: %d checks, %d failures\n", failures ? "FAILED" : "PASSED", checks, failures);
    return failures ? 1 : 0;
}
