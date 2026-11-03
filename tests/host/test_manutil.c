/* Host tests for what man knows without a file system (user/bin/manutil.h), built as it is with the host's C library: the
   table of names that share a page, what a page's name may be (nothing that leads out of the manual), which directory
   entries are pages, and the word search of man -k. tests/test_man.py checks that the table's pages exist, and that
   every name has one. man itself runs in the smoke test. Built and run by `make unittest`. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "manutil.h"

static int checks, failed;
#define CHECK(x) do { checks++; if(!(x)) { failed++; printf("FAIL %s line %d: %s\n", __FILE__, __LINE__, #x); } } while(0)

static void table(void) {
    unsigned int i, j, n = sizeof(man_aliases) / sizeof(man_aliases[0]);
    CHECK(!strcmp(man_alias_of("."), "source"));
    CHECK(!strcmp(man_alias_of(":"), "true"));
    CHECK(!strcmp(man_alias_of("["), "test"));
    CHECK(!strcmp(man_alias_of("crashdiv"), "crash"));
    CHECK(!man_alias_of("ls") && !man_alias_of("") && !man_alias_of("crash"));
    for(i = 0; i < n; i++) {
        CHECK(man_name_ok(man_aliases[i].page));            /* a page's file can be found */
        CHECK(!man_alias_of(man_aliases[i].page));          /* one step: a name's page is never a name for another */
        for(j = 0; j < i; j++) CHECK(strcmp(man_aliases[i].name, man_aliases[j].name));
    }
}

static void names(void) {
    static const char *const good[] = { "ls", "sha256sum", "a", "Z9", "a-b", "a_b", "g++" };
    static const char *const bad[] = { "", ".", "..", "/", "a/b", "../etc/motd", "/initrd/motd", ".hidden", "-k", "-", "_a", "+",
                                       "a b", "a.txt", "a\n", "a\\b", "\xc3\xa9", "\x1b[2J", "*", "a:b" };
    char big[MAN_NAME_MAX + 2];
    unsigned int i;
    for(i = 0; i < sizeof(good) / sizeof(good[0]); i++) CHECK(man_name_ok(good[i]));
    for(i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) CHECK(!man_name_ok(bad[i]));
    memset(big, 'x', sizeof(big));
    big[MAN_NAME_MAX] = 0;
    CHECK(man_name_ok(big));
    big[MAN_NAME_MAX] = 'x';
    big[MAN_NAME_MAX + 1] = 0;
    CHECK(!man_name_ok(big));
}

static void files(void) {
    CHECK(man_page_file("ls.txt") == 2);
    CHECK(man_page_file("a.txt") == 1);
    CHECK(man_page_file(".txt") == 0);
    CHECK(man_page_file("txt") == 0);
    CHECK(man_page_file("ls") == 0);
    CHECK(man_page_file("ls.txt.bak") == 0);
    CHECK(man_page_file("") == 0);
    CHECK(man_page_file("ls.TXT") == 0);
}

static void words(void) {
    CHECK(man_has_word("sed - a stream editor", "stream"));
    CHECK(man_has_word("sed - a stream editor", "sed"));
    CHECK(man_has_word("sed - a stream editor", "editor"));
    CHECK(man_has_word("sed - a stream editor", "STREAM"));
    CHECK(man_has_word("SED - A STREAM EDITOR", "stream"));
    CHECK(man_has_word("sed - a stream editor", "m e"));
    CHECK(!man_has_word("sed - a stream editor", "streams"));
    CHECK(!man_has_word("sed - a stream editor", "awk"));
    CHECK(!man_has_word("", "a"));
    CHECK(man_has_word("caf\xc3\xa9 - a place", "caf\xc3\xa9"));
    CHECK(!man_has_word("caf\xc3\x89 - a place", "caf\xc3\xa9"));  /* only the ASCII letters have no case here */
    CHECK(!man_has_word("ab", "abc"));
    CHECK(man_has_word("aab", "ab"));
}

int main(void) {
    table();
    names();
    files();
    words();
    printf("test_manutil: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
