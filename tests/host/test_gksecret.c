/*
SurfOS host tests: what a secret leaves in freed memory
--------------------
File: test_gksecret.c Date: 11/09/26
--------------------
The window toolkit (gk.h; the desktop applications, track kit) built with every free() of libgui's and the
toolkit's going through gks_free() (the Makefile's -Dfree=gks_free for this test's objects), which looks
for a marker in the whole block before it goes. A secret typed into a field, its history, a draft, a
password field, a box's question: whatever path it takes, no block freed with it in it (the review of
11/09/26, F1: a chat line's draft and history were freed unwiped). Built and run by `make unittest`.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gk.h>
#include "gk_fake.h"
#ifdef __APPLE__
#include <malloc/malloc.h>
#define usable(p) malloc_size(p)
#else
#include <malloc.h>
#define usable(p) malloc_usable_size(p)
#endif

static int checks, failed, leaks;
static const char *marker = "hunter2";
#define CHECK(x) do { checks++; if(!(x)) { failed++; printf("FAIL %s line %d: %s\n", __FILE__, __LINE__, #x); } } while(0)

/* every free() of the toolkit's: the block looked through for the marker first */
void gks_free(void *p) {
    size_t n, k, m = strlen(marker);
    if(p) {
        n = usable(p);
        for(k = 0; k + m <= n; k++)
            if(!memcmp((char *)p + k, marker, m)) {
                leaks++;
                break;
            }
    }
    free(p);
}

static void run(struct gk *ui) {
    struct gk_event ev;
    int i;
    for(i = 0; i < 10000 && gk_next(ui, &ev, 0) > 0; i++) ;
}

static struct gk *window(void) {
    struct gk *ui;
    gkf_reset(0, 0);
    ui = gk_open("Secret", 500, 300, WIN_RESIZABLE);
    gkf_focus(1);
    return ui;
}

int main(void) {
    struct gk *ui;
    struct gk_widget *f, *p;
    setvbuf(stdout, NULL, _IOLBF, 0);
    /* a chat line: a line with a password typed, Up, Down, Up over it; Down back to it, and the program's text */
    ui = window();
    f = gk_field(gk_root(ui), 1, 512, 0);
    gk_field_history(f, 10);
    gk_focus(f);
    gkf_type("hello");
    run(ui);
    gk_field_remember(f);
    gk_set_text(f, "");
    gkf_type("/msg NickServ IDENTIFY hunter2");
    gkf_key(WIN_KEY_UP, 0);
    gkf_key(WIN_KEY_DOWN, 0);
    gkf_key(WIN_KEY_UP, 0);
    run(ui);
    CHECK(leaks == 0 && !strcmp(gk_text(f), "hello"));
    gkf_key(WIN_KEY_DOWN, 0);                           /* the line back, its copy gone */
    run(ui);
    gk_set_text(f, "");
    CHECK(leaks == 0 && !strcmp(gk_text(f), ""));
    gkf_type("/msg NickServ IDENTIFY hunter2");
    gkf_key(WIN_KEY_UP, 0);                             /* a recalled line sent: the program's text over the draft */
    run(ui);
    gk_set_text(f, "");
    gkf_type("again hunter2");
    gkf_key(WIN_KEY_UP, 0);
    run(ui);
    gk_field_wipe(f);                                   /* a wipe takes the draft too */
    gkf_key(WIN_KEY_UP, 0);
    gkf_key(WIN_KEY_DOWN, 0);
    run(ui);
    CHECK(leaks == 0);
    gk_close(ui);
    CHECK(leaks == 0);
    /* a history full, and one made shorter: the lines that go are wiped */
    ui = window();
    f = gk_field(gk_root(ui), 1, 512, 0);
    gk_field_history(f, 2);
    gk_set_text(f, "/oper admin hunter2");
    gk_field_remember(f);
    gk_set_text(f, "two");
    gk_field_remember(f);
    gk_set_text(f, "three");
    gk_field_remember(f);
    CHECK(leaks == 0);
    gk_set_text(f, "/oper admin hunter2");
    gk_field_remember(f);
    gk_field_history(f, 0);
    CHECK(leaks == 0);
    gk_close(ui);
    /* a password field: typed, selected and typed over, copy and cut refused, pasted, set, wiped, closed */
    ui = window();
    p = gk_field(gk_root(ui), 1, 64, GK_PASSWORD);
    gk_focus(p);
    gkf_type("hunter2");
    gkf_key(1, WIN_MOD_CTRL);
    gkf_key(3, WIN_MOD_CTRL);
    gkf_key(24, WIN_MOD_CTRL);
    gkf_set_paste("xx");
    gkf_key(22, WIN_MOD_CTRL);
    gkf_key(1, WIN_MOD_CTRL);
    gkf_type("zz");
    run(ui);
    gk_set_text(p, "hunter2");
    gk_set_text(p, "a");
    gkf_type("hunter2");
    run(ui);
    gk_field_wipe(p);
    gkf_type("hunter2");
    run(ui);
    gk_close(ui);
    CHECK(leaks == 0);
    /* a box asking for a password: its answer given, then gone; one answered by a box over it */
    ui = window();
    gk_label(gk_root(ui), "x");
    run(ui);
    gk_ask(ui, 5, "Join", "Password:", "", GK_PASSWORD);
    run(ui);
    gkf_type("hunter2\n");
    run(ui);
    gk_ask(ui, 6, "Join", "Password:", "hunter2", GK_PASSWORD);
    run(ui);
    gk_message(ui, 7, "Over", "", "");
    run(ui);
    gk_close(ui);
    CHECK(leaks == 0);
    gkf_reset(0, 0);
    printf("test_gksecret: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
