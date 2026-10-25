/* Host-side tests of surfsh's parser and expansions (10/24/26): user/surfsh/parse.c, unparse.c and
   expand.c as they are (tests/host/surfsh_bridge.c). A text is parsed into its first complete command,
   which unparse.c writes back: the words as typed, lists, && and ||, !, pipelines, groups, subshells,
   redirections with their descriptors, here-documents (<< <<- and a quoted end line), comments and
   empty commands; text that ends inside a command (PARSE_MORE, with what is open) and mistakes
   (PARSE_ERROR); the limits on nesting and on a pipeline's length. Then words become fields: quotes,
   backslashes, $?, a here-document's text. Run by `make unittest`. */
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
    expands("$x*", "$x*|", "no variables and no wildcards yet");
    expands("end\\", "end\\|", "a \\ at the very end is itself");
    check(sh_expand_doc("a $? \\$ \\\\ \\x \"q\" 'q'\nb\\\nc\n", out, sizeof(out)) == 0 &&
          !strcmp(out, "a 7 $ \\ \\x \"q\" 'q'\nbc\n"), "a here-document's text: $?, \\ before $ \\ and a newline");
}

int main(void) {
    test_lists();
    test_redirections();
    test_more();
    test_errors();
    sh_set_status(7);
    test_expand();
    printf("SURFSH UNIT TESTS %s: %d checks, %d failures\n", failures ? "FAILED" : "PASSED", checks, failures);
    return failures ? 1 : 0;
}
