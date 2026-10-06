/* Host-side unit tests for shell/prompt.c, the SurfOS prompt: breadcrumbs and the root, the
   five states and their priority, shortening by depth and by width, the ASCII fallback, the
   colours, names with UTF-8 or control characters in them, and buffers that are too small.
   Built like the kernel objects and run by `make unittest`. */
#include <stdio.h>
#include <string.h>
#include <surfos/prompt.h>

void exit(int status);

static int failures, checks;

static void check(int ok, const char *what) {
    checks++;
    if(!ok) { failures++; printf("FAIL: %s\n", what); }
}

static struct prompt_ctx at(const char *cwd) {
    struct prompt_ctx c;
    memset(&c, 0, sizeof(c));
    c.cwd = cwd;
    c.unicode = 1;
    c.width = 120;
    return c;
}

static const char *render(const struct prompt_ctx *c) {
    static char out[PROMPT_MAX];
    if(prompt_render(c, out, sizeof(out)) < 0) return "(did not fit)";
    return out;
}

static void expect(const struct prompt_ctx *c, const char *want, const char *what) {
    const char *got = render(c);
    checks++;
    if(strcmp(got, want)) {
        failures++;
        printf("FAIL: %s\n  got  \"%s\"\n  want \"%s\"\n", what, got, want);
    }
}

static void test_breadcrumbs(void) {
    struct prompt_ctx c = at("/");
    expect(&c, "SURF ≋> ", "the root is SURF");
    c.cwd = NULL;
    expect(&c, "SURF ≋> ", "no directory is the root");
    c.cwd = "/initrd/bin";
    expect(&c, "initrd ▸ bin ≋> ", "names with ▸ between them, no leading /");
    c.cwd = "//initrd//bin/";
    expect(&c, "initrd ▸ bin ≋> ", "empty names are skipped");
    c.cwd = "/initrd/./bin";
    expect(&c, "initrd ▸ bin ≋> ", ". is skipped");
    c.cwd = "/data";
    expect(&c, "data ≋> ", "one name");
    c.cwd = "/a/b/c/d/e";
    expect(&c, "a ▸ b ▸ c ▸ d ▸ e ≋> ", "five names fit the default depth");
}

static void test_states(void) {
    struct prompt_ctx c = at("/initrd/bin");
    c.last_status = 1;
    expect(&c, "initrd ▸ bin ≋!> ", "the previous command failed");
    c.last_status = -1;
    expect(&c, "initrd ▸ bin ≋!> ", "any status but 0 is a failure");
    check(prompt_state(&c) == PROMPT_FAILED, "prompt_state: failed");
    c = at("/");
    c.elevated = 1;
    expect(&c, "SURF ≋#> ", "elevated");
    c.last_status = 3;
    expect(&c, "SURF ≋#> ", "elevated wins over failed");
    c.diagnostic = 1;
    expect(&c, "SURF ≋?> ", "diagnostic wins over elevated");
    check(prompt_state(&c) == PROMPT_DIAGNOSTIC, "prompt_state: diagnostic");
    c = at("/home/brandon");
    c.remote = 1;
    c.hostname = "atlas";
    expect(&c, "atlas ▸ home ▸ brandon ≋@> ", "remote: the host name first");
    c.last_status = 1;
    expect(&c, "atlas ▸ home ▸ brandon ≋@> ", "remote wins over failed");
    c.elevated = 1;
    expect(&c, "atlas ▸ home ▸ brandon ≋#> ", "elevated wins over remote");
    check(prompt_state(&c) == PROMPT_ELEVATED, "prompt_state: elevated");
    c = at("/");
    c.remote = 1;
    c.hostname = "apollo";
    expect(&c, "apollo ▸ SURF ≋@> ", "remote root");
    c.hostname = "";
    expect(&c, "SURF ≋@> ", "remote without a host name");
    c = at("/");
    check(prompt_state(&c) == PROMPT_NORMAL, "prompt_state: normal");
}

#define DEEP "/home/brandon/projects/operating-systems/surfos/src/kernel/memory"

static void test_depth(void) {
    struct prompt_ctx c = at(DEEP);
    c.width = 200;
    expect(&c, "… ▸ surfos ▸ src ▸ kernel ▸ memory ≋> ", "eight names: … and the last four (depth 5)");
    c.max_depth = 3;
    expect(&c, "… ▸ kernel ▸ memory ≋> ", "depth 3");
    c.max_depth = 1;
    expect(&c, "… ▸ memory ≋> ", "depth below 2 counts as 2");
    c.max_depth = 0;
    c.cwd = "/a/b/c/d/e/f";
    expect(&c, "… ▸ c ▸ d ▸ e ▸ f ≋> ", "six names");
    c.remote = 1;
    c.hostname = "atlas";
    expect(&c, "atlas ▸ … ▸ c ▸ d ▸ e ▸ f ≋@> ", "the host name stays");
}

static void test_width(void) {
    struct prompt_ctx c = at(DEEP);
    char path[PROMPT_MAX], want[PROMPT_MAX], name[80];
    const char *got;
    int i;
    c.width = 60;
    expect(&c, "… ▸ src ▸ kernel ▸ memory ≋> ", "60 columns: the prompt takes at most 30");
    check(prompt_columns(render(&c)) <= 30, "and it does");
    c.width = 0;
    expect(&c, "… ▸ surfos ▸ src ▸ kernel ▸ memory ≋> ", "unknown width: 80");
    c.width = 10;
    got = render(&c);
    check(prompt_columns(got) <= 20, "a narrow terminal still gets 20 columns");
    check(strstr(got, "memory ≋> ") != NULL, "and the current directory");

    for(i = 0; i < 60; i++) name[i] = 'x';
    strcpy(name + 60, "yz");
    snprintf(path, sizeof(path), "/%s", name);
    c = at(path);
    c.width = 80;
    got = render(&c);
    check(prompt_columns(got) == 40, "a 62-character name is cut to fit 40 columns");
    snprintf(want, sizeof(want), "…%s ≋> ", name + 62 - 35);
    expect(&c, want, "its last 35 characters after …");
    snprintf(path, sizeof(path), "/data/%s", name);
    c.cwd = path;
    got = render(&c);
    check(!strncmp(got, "…", 3) && strstr(got, "xyz ≋> ") && prompt_columns(got) == 40, "the parent goes, then the name is cut");
}

static void test_names(void) {
    struct prompt_ctx c = at("/café/menü");
    expect(&c, "café ▸ menü ≋> ", "UTF-8 names as they are");
    check(prompt_columns(render(&c)) == 15, "é and ü take a column each");
    c.cwd = "/a\x1b[2Jb";
    expect(&c, "a?[2Jb ≋> ", "an escape in a name is a ?");
    c.cwd = "/tab\there";
    expect(&c, "tab?here ≋> ", "a tab in a name is a ?");
}

static void test_ascii(void) {
    struct prompt_ctx c = at("/initrd/bin");
    c.unicode = 0;
    expect(&c, "initrd > bin ~> ", "ASCII: > and ~");
    c.last_status = 1;
    expect(&c, "initrd > bin ~!> ", "ASCII failed");
    c = at("/");
    c.unicode = 0;
    c.elevated = 1;
    expect(&c, "SURF ~#> ", "ASCII elevated");
    c = at(DEEP);
    c.unicode = 0;
    c.width = 200;
    expect(&c, "... > surfos > src > kernel > memory ~> ", "ASCII: ... for the hidden names");
}

static void test_colour(void) {
    struct prompt_ctx c = at("/initrd");
    char plain[PROMPT_MAX];
    c.color = 1;
    expect(&c, "\033[0;1;34minitrd ≋>\033[0m ", "light blue, reset before the space");
    c.last_status = 1;
    expect(&c, "\033[0;1;34minitrd ≋\033[1;31m!\033[0;1;34m>\033[0m ", "the failure mark in light red");
    c.color = 0;
    strcpy(plain, render(&c));
    c.color = 1;
    check(prompt_columns(render(&c)) == prompt_columns(plain), "colours take no columns");
    c = at("/");
    c.color = 1;
    c.elevated = 1;
    check(strstr(render(&c), "\033[1;33m#") != NULL, "elevated in yellow");
    c.diagnostic = 1;
    check(strstr(render(&c), "\033[0;33m?") != NULL, "diagnostic in orange");
    c = at("/");
    c.color = 1;
    c.remote = 1;
    check(strstr(render(&c), "\033[1;35m@") != NULL, "remote in light magenta");
}

static void test_sizes(void) {
    struct prompt_ctx c = at("/");
    char out[16];
    check(prompt_columns("SURF ≋> ") == 8, "columns of the root prompt");
    check(prompt_columns("") == 0 && prompt_columns(NULL) == 0, "columns of nothing");
    check(prompt_render(&c, out, 11) == 10 && !strcmp(out, "SURF ≋> "), "exactly enough room");
    memset(out, 'Z', sizeof(out));
    check(prompt_render(&c, out, 10) == -1 && strlen(out) < 10, "one byte short: -1, still terminated");
    check(prompt_render(&c, out, 1) == -1 && out[0] == 0, "room for the NUL only");
    check(prompt_render(&c, out, 0) == -1, "no room at all");
    check(prompt_render(&c, NULL, 16) == -1, "no buffer");
}

int main(void) {
    test_breadcrumbs();
    test_states();
    test_depth();
    test_width();
    test_names();
    test_ascii();
    test_colour();
    test_sizes();
    printf("PROMPT UNIT TESTS %s: %d checks, %d failures\n", failures ? "FAILED" : "PASSED", checks, failures);
    exit(failures ? 1 : 0);
    return 0;
}
