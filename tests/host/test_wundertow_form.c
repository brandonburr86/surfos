/*
SurfOS Undertow's window: tests of forms
----------------------
File: test_wundertow_form.c  Date: 11/1/26
----------------------
user/wundertow/form.c with the rest of the window's modules on the host: the page model of forms (controls, what belongs
to which form, selects, text areas, what is disabled, the limits, every allocation failing); what is sent, to the byte, as
the standard's encoding has it, and everything that is not sent and why; and the fields used through browser.c as a
person uses them, by keys and by the pointer. No page submits anything: a request exists only after a person's key or
click, and that is checked. A password shows as stars, goes nowhere, and is gone when its page is left.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "undertow.h"
#include "alloc.h"
#include "url.h"
#include "http.h"
#include "html_scan.h"
#include "doc.h"
#include "form.h"
#include "font.h"
#include "layout.h"
#include "paint.h"
#include "browser.h"

static int failures, checks;

static void check(int ok, const char *what) {
    checks++;
    if(ok) return;
    failures++;
    printf("FAIL %s\n", what);
}

unsigned int uptime(void) { return 0; }
time_t ut_sim_time(time_t *t) { if(t) *t = 0; return 0; }

#define PAGE "https://forms.test/dir/page.html?old=1"

static int build_as(const char *url, const char *html, const char *charset, struct wd_document *d) {
    struct ut_response r;
    char err[UT_ERR_MAX];
    size_t n = strlen(html);
    int rc;
    memset(&r, 0, sizeof(r));
    if(ut_url_parse(url, &r.url, err, sizeof(err))) return -1;
    r.body = (unsigned char *)malloc(n + 1);
    memcpy(r.body, html, n + 1);
    r.len = n;
    r.status = 200;
    snprintf(r.content_type, sizeof(r.content_type), "text/html");
    snprintf(r.charset, sizeof(r.charset), "%s", charset);
    rc = wd_build(&r, d, err, sizeof(err));
    free(r.body);
    return rc;
}

static int build(const char *html, struct wd_document *d) { return build_as(PAGE, html, "utf-8", d); }

/* What form 0 of a page sends, with no button: "" and why in the status when nothing */
static char sent[WB_ADDR_MAX + 64];
static const char *why;

static const char *send_of(const char *html) {
    struct wd_document d;
    if(build(html, &d)) {
        why = "the page did not build";
        return "";
    }
    why = fm_submit(&d, 0, -1, 0, 0, sent, sizeof(sent));
    wd_free(&d);
    return why ? "" : sent;
}

static void sends(const char *html, const char *url, const char *what) {
    char s[400];
    const char *got = send_of(html);
    snprintf(s, sizeof(s), "%s: %s (got %s%s%s)", what, url, got, why ? ": " : "", why ? why : "");
    check(!strcmp(got, url), s);
}

static void refuses(const char *html, const char *word, const char *what) {
    char s[400];
    const char *got = send_of(html);
    snprintf(s, sizeof(s), "%s: refused, saying %s (got %s, %s)", what, word, got, why ? why : "nothing");
    check(!*got && why && strstr(why, word) && strstr(why, "nothing was sent"), s);
}

/**** the model ****/

static void model(void) {
    struct wd_document d;
    const struct wd_control *c;
    check(!build("<form id=f action=/s method=GET name=n><input name=a value=1><input type=hidden name=h value=2>"
                 "<input type=checkbox name=c checked><input type=radio name=r value=x><input type=radio name=r value=y checked>"
                 "<input type=password name=p><input type=submit name=go value=Go><button name=b value=v>B</button>"
                 "<input type=reset><input type=button value=X><input type=image name=i alt=I><input type=file name=up>"
                 "<textarea name=t rows=4 cols=30>\nhi &amp; bye\nthere</textarea></form>", &d), "a form");
    check(d.form_count == 1 && d.control_count == 13, "its controls");
    check(!strcmp(d.forms[0].action, "https://forms.test/s") && d.forms[0].method == WM_GET && d.forms[0].password && !d.forms[0].too_much, "its address, method, a password");
    c = d.controls;
    check(c[0].kind == WF_TEXT && !strcmp(c[0].name, "a") && !strcmp(c[0].value, "1") && !strcmp(c[0].dflt, "1") && c[0].form == 0 && c[0].run >= 0 && d.runs[c[0].run].control == 0, "a text field and its box");
    check(c[1].kind == WC_HIDDEN && c[1].run == -1 && !strcmp(c[1].value, "2"), "a hidden one has no box");
    check(c[2].kind == WF_CHECKBOX && c[2].checked, "a checkbox");
    check(c[3].kind == WF_RADIO && !c[3].checked && c[4].checked, "radio buttons");
    check(c[5].kind == WF_PASSWORD, "a password");
    check(c[6].kind == WF_BUTTON && c[6].role == WC_SUBMIT && c[7].role == WC_SUBMIT && c[8].role == WC_RESET && c[9].role == WC_PUSH, "buttons by what they do");
    check(c[10].image && c[10].role == WC_SUBMIT, "an image button");
    check(c[11].kind == WF_FILE, "a file field");
    check(c[12].kind == WF_AREA && !strcmp(c[12].value, "hi & bye\nthere") && c[12].rows == 4 && c[12].cols == 30, "a text area: its first line feed goes, references decoded");
    wd_free(&d);

    check(!build("<form action=a><input name=x><form action=b><input name=y></form><input name=z></form>", &d), "a form in a form");
    check(d.form_count == 1 && d.controls[1].form == 0 && d.controls[2].form == -1, "the standard: one form, which the first </form> ends");
    wd_free(&d);
    check(!build("<input name=before><form id=f></form><input name=in form=f><form id=g><input name=there form=f><input name=nowhere form=nope></form>", &d), "form= attributes");
    check(d.controls[0].form == -1 && d.controls[1].form == 0 && d.controls[2].form == 0 && d.controls[3].form == -1, "a control's form by id; of no such id, none");
    wd_free(&d);
    check(!build("<form><input name=a disabled><fieldset disabled><input name=b><select name=s></select></fieldset><input name=c><input name=d readonly></form>", &d), "disabled");
    check(d.controls[0].disabled && d.controls[1].disabled && d.controls[2].disabled && !d.controls[3].disabled && d.controls[4].readonly, "a disabled fieldset's, to its end");
    wd_free(&d);
    check(!build("<form><input type=radio name=r checked><input type=radio name=r checked><input type=radio name=q checked></form>", &d), "two checked");
    check(!d.controls[0].checked && d.controls[1].checked && d.controls[2].checked, "of a group, the last checked");
    wd_free(&d);
    /* selects */
    check(!build("<form><select name=s><option>One<option value=2 selected>Two &amp; a half<optgroup disabled label=g><option>Three</optgroup><option disabled>Four<option label=L value=5>Five</select>"
                 "<select name=m multiple><option selected>A<option>B<option selected>C</select><select name=e></select><select name=f><option disabled>No<option>Yes</select></form>", &d), "selects");
    c = d.controls;
    check(d.control_count == 4 && c[0].kind == WF_SELECT && c[0].noptions == 5, "its options");
    check(!strcmp(d.options[0].label, "One") && !strcmp(d.options[0].value, "One") && !strcmp(d.options[1].value, "2") && !strcmp(d.options[1].label, "Two & a half"), "values and labels");
    check(d.options[2].disabled && d.options[3].disabled && !d.options[4].disabled, "disabled, by its group too");
    check(fm_chosen(&d, 0) == 1, "the selected one");
    check(c[1].multiple && d.options[c[1].first_option].on && !d.options[c[1].first_option + 1].on && d.options[c[1].first_option + 2].on, "of a multiple, each");
    check(c[2].noptions == 0 && fm_chosen(&d, 2) == -1, "no options");
    check(fm_chosen(&d, 3) == 1, "the first that can be chosen");
    wd_free(&d);
    /* a select's label as milestone 1 showed it stays */
    check(!build("<form action='' method=post><input name=a></form>", &d) && d.forms[0].method == WM_POST && !strcmp(d.forms[0].action, PAGE), "an empty action: the page's own address");
    wd_free(&d);
    check(!build("<base href=https://other.test/x/><form action=y><button formaction=z formmethod=post></form>", &d), "a base");
    check(!strcmp(d.forms[0].action, "https://other.test/x/y") && !strcmp(d.controls[0].formaction, "https://other.test/x/z") && d.controls[0].formmethod == WM_POST + 1, "addresses against the base");
    wd_free(&d);
    check(!build("<form action=mailto:a@b.test></form><form action=file:///etc/passwd></form><form action=ftp://a.test/></form>", &d), "addresses of other schemes");
    check(!d.forms[0].action && !d.forms[1].action && !d.forms[2].action, "are no addresses");
    wd_free(&d);
    /* limits */
    {
        static char big[400000];
        char *e = big;
        int i;
        e += snprintf(e, 32, "<form>");
        for(i = 0; i < (int)WD_CONTROLS_MAX + 3; i++) e += snprintf(e, 32, "<input name=a>");
        check(!build(big, &d) && d.control_count == WD_CONTROLS_MAX && d.forms[0].too_much, "past the controls kept: the form is not sent");
        wd_free(&d);
        e = big;
        for(i = 0; i < (int)WD_FORMS_MAX + 3; i++) e += snprintf(e, 32, "<form></form>");
        check(!build(big, &d) && d.form_count == WD_FORMS_MAX, "past the forms kept");
        wd_free(&d);
        e = big;
        e += snprintf(e, 32, "<form><select name=s>");
        for(i = 0; i < 17000; i++) e += snprintf(e, 16, "<option>a");
        check(!build(big, &d) && d.option_count == WD_OPTIONS_MAX && d.forms[0].too_much, "past the options kept");
        wd_free(&d);
    }
    {
        static char big[1600000];
        char *e = big, name[1040];
        int i;
        memset(name, 'n', 1000);
        name[1000] = 0;
        e += snprintf(e, 32, "<form>");
        for(i = 0; i < 700; i++) e += snprintf(e, 2100, "<input name=%s value=%s>", name, name);
        check(!build(big, &d) && d.forms[0].too_much && d.control_count < 700, "past the bytes kept");
        check(d.form_bytes <= 2 * WD_FORM_BYTES, "and no more kept than that");
        wd_free(&d);
        /* a value longer than an attribute keeps, and a text area past its limit */
        e = big;
        e += snprintf(e, 40, "<form><input name=a value=\"");
        memset(e, 'v', 2000);
        e += 2000;
        snprintf(e, 16, "\"></form>");
        check(!build(big, &d) && d.forms[0].too_much, "a value cut: the form is not sent");
        wd_free(&d);
        e = big;
        e += snprintf(e, 40, "<form><textarea name=a>");
        memset(e, 't', 70000);
        e += 70000;
        snprintf(e, 32, "</textarea></form>");
        check(!build(big, &d) && d.forms[0].too_much && d.controls[0].len == WD_VALUE_MAX, "a text area cut: the form is not sent");
        wd_free(&d);
    }
    /* a page cut at its limit: nothing can be known of its forms */
    {
        static char big[700000];
        memcpy(big, "<form><input name=a></form><p>", 30);
        memset(big + 30, 'x', 600000);
        check(!build(big, &d) && d.forms[0].too_much, "a page that stops at a limit: its forms are not sent");
        wd_free(&d);
    }
    /* without memory */
    {
        static const char html[] = "<form id=f action=/a><input name=a value=b><select name=s><option value=1>One</select><textarea name=t>x</textarea><button formaction=/z>B</button></form>";
        long n, fails = 0;
        int rc = -1;
        for(n = 0; n < 400 && rc; n++) {
            long before = ut_alloc_live;
            ut_alloc_fail_after = n;
            rc = build(html, &d);
            ut_alloc_fail_after = -1;
            if(rc) {
                fails++;
                if(ut_alloc_live != before) {
                    check(0, "without memory: nothing held");
                    break;
                }
            } else wd_free(&d);
        }
        check(!rc && fails > 5 && ut_alloc_live == 0, "every allocation failing in turn");
    }
}

/**** what is sent ****/

static void encoding(void) {
    sends("<form action=/s><input name=q value=word></form>", "https://forms.test/s?q=word", "one field");
    sends("<form action=/s><input name=q value='two words'></form>", "https://forms.test/s?q=two+words", "a space is a plus");
    sends("<form action=/s><input name=q value='a&amp;b=c+d%e?f#g/h'></form>", "https://forms.test/s?q=a%26b%3Dc%2Bd%25e%3Ff%23g%2Fh", "what the query needs escaped");
    sends("<form action=/s><input name=q value='*-._~!'></form>", "https://forms.test/s?q=*-._%7E%21", "what stays, and what does not");
    sends("<form action=/s><input name=q value='caf\xc3\xa9 \xe2\x82\xac'></form>", "https://forms.test/s?q=caf%C3%A9+%E2%82%AC", "UTF-8");
    sends("<form action=/s><input name='a b&amp;' value=1></form>", "https://forms.test/s?a+b%26=1", "a name likewise");
    sends("<form action=/s><input name=a value=1><input name=b value=2><input name=c></form>", "https://forms.test/s?a=1&b=2&c=", "several, and one empty");
    sends("<form action=/s><input value=1><input name='' value=2></form>", "https://forms.test/s?", "no name: nothing; the address has its ? all the same");
    sends("<form action=/s?old=1#frag><input name=a value=1></form>", "https://forms.test/s?a=1#frag", "a query is replaced, a fragment stays");
    sends("<form><input name=a value=1></form>", "https://forms.test/dir/page.html?a=1", "no action: the page itself");
    sends("<form action=''><input name=a value=1></form>", "https://forms.test/dir/page.html?a=1", "an empty action too");
    sends("<form action=http://plain.test/x><input name=a value=1></form>", "http://plain.test/x?a=1", "an address of its own (the browser judges it: below)");
    sends("<form method=get action=/s><input name=a value=1></form>", "https://forms.test/s?a=1", "method=get");
    sends("<form method=GeT action=/s><input name=a value=1></form>", "https://forms.test/s?a=1", "in any case");
    sends("<form method=put action=/s><input name=a value=1></form>", "https://forms.test/s?a=1", "a method that is none: GET, as the standard says");
    sends("<form action=/s><input type=checkbox name=a><input type=checkbox name=b checked><input type=checkbox name=c value=v checked></form>", "https://forms.test/s?b=on&c=v", "checkboxes");
    sends("<form action=/s><input type=radio name=r value=1><input type=radio name=r value=2 checked></form>", "https://forms.test/s?r=2", "radio buttons");
    sends("<form action=/s><select name=s><option>One<option selected>Two</select><select name=t><option value=9>Nine</select></form>", "https://forms.test/s?s=Two&t=9", "selects");
    sends("<form action=/s><select name=m multiple><option selected>A<option>B<option selected>C</select></form>", "https://forms.test/s?m=A&m=C", "a multiple select");
    sends("<form action=/s><select name=m multiple><option>A</select><select name=e></select></form>", "https://forms.test/s?", "nothing chosen: nothing");
    sends("<form action=/s><select name=d><option selected disabled>A<option>B</select></form>", "https://forms.test/s?", "a disabled option is not sent, chosen or not");
    sends("<form action=/s><input type=hidden name=h value=secret><input type=hidden name=_charset_></form>", "https://forms.test/s?h=secret&_charset_=UTF-8", "hidden fields, and _charset_");
    sends("<form action=/s><input name=a disabled value=1><fieldset disabled><input name=b value=2></fieldset><input name=c readonly value=3></form>", "https://forms.test/s?c=3", "disabled is not sent, read-only is");
    sends("<form action=/s><textarea name=t>one\ntwo\r\nthree\rfour</textarea></form>", "https://forms.test/s?t=one%0D%0Atwo%0D%0Athree%0D%0Afour", "a text area's lines end in CR LF");
    sends("<form action=/s><input type=file name=f></form>", "https://forms.test/s?f=", "a file field: empty, the standard's for no file");
    sends("<form action=/s><input type=submit name=go value=1><input name=a value=2><button name=b value=3></form>", "https://forms.test/s?a=2", "with no button: no button's value");
    /* on a page in windows-1252 */
    {
        struct wd_document d;
        const char *html = "<form action=/s><input name=a value='caf\xe9 \x80'><input name=b value='&#937;'></form>";
        check(!build_as(PAGE, html, "windows-1252", &d) && d.form_charset == UT_CS_1252, "a page in windows-1252");
        check(!fm_submit(&d, 0, -1, 0, 0, sent, sizeof(sent)) && !strcmp(sent, "https://forms.test/s?a=caf%E9+%80&b=%26%23937%3B"), "its bytes, and a reference for what it cannot say");
        wd_free(&d);
    }
    /* the button that submitted */
    {
        struct wd_document d;
        build("<form action=/s method=post><input name=a value=1><input type=submit name=x value=X><button name=y value=Y formaction=/other formmethod=get>B</button><button name=z formmethod=post>P</button>"
              "<input type=image name=i alt=I><input type=image alt=J></form>", &d);
        check(!fm_submit(&d, 0, 2, 0, 0, sent, sizeof(sent)) && !strcmp(sent, "https://forms.test/other?a=1&y=Y"), "its own address and method: a POST form's GET button");
        check(fm_submit(&d, 0, 1, 0, 0, sent, sizeof(sent)) != NULL, "the form's own method: POST");
        check(fm_submit(&d, 0, 3, 0, 0, sent, sizeof(sent)) != NULL, "formmethod=post");
        wd_free(&d);
        build("<form action=/s><input type=submit name=x value=X><button name=z formmethod=post>P</button><input type=image name=i alt=I><input type=image alt=J></form>", &d);
        check(!fm_submit(&d, 0, 0, 0, 0, sent, sizeof(sent)) && !strcmp(sent, "https://forms.test/s?x=X"), "a button's name and value");
        why = fm_submit(&d, 0, 1, 0, 0, sent, sizeof(sent));
        check(why && strstr(why, "POST"), "a GET form's POST button is not sent");
        check(!fm_submit(&d, 0, 2, 3, 4, sent, sizeof(sent)) && !strcmp(sent, "https://forms.test/s?i.x=3&i.y=4"), "an image button: where it was pressed");
        check(!fm_submit(&d, 0, 3, 5, 6, sent, sizeof(sent)) && !strcmp(sent, "https://forms.test/s?x=5&y=6"), "one with no name");
        check(fm_default_button(&d, 0) == 0, "the default button");
        wd_free(&d);
    }
    /* what is not sent */
    refuses("<form method=post action=/s><input name=a value=1></form>", "POST", "a POST form");
    refuses("<form method=PoSt action=/s><input name=a value=1></form>", "POST", "in any case");
    refuses("<form method=dialog><input name=a value=1></form>", "dialog", "method=dialog");
    refuses("<form action=/login><input name=u value=me><input type=password name=p value=secret></form>", "password", "a password field, over https");
    refuses("<form action=/login><input type=password disabled></form>", "password", "a disabled one too");
    refuses("<form id=f action=/s></form><input type=password form=f>", "password", "one that belongs to it from outside");
    refuses("<form action=mailto:a@b.test><input name=a></form>", "address", "an address of another scheme");
    {
        static char big[3000];
        char *e = big;
        e += snprintf(e, 40, "<form action=/s><input name=a value=\"");
        memset(e, 'v', 2000);
        e += 2000;
        snprintf(e, 16, "\"></form>");
        refuses(big, "more than", "a value longer than was kept");
    }
    {
        static char big[16000];
        int i;
        char *e = big;
        e += snprintf(e, 32, "<form action=/s>");
        for(i = 0; i < 300; i++) e += snprintf(e, 64, "<input name=aaaaa%d value=bbbbbbb>", i);
        refuses(big, "do not fit", "more than an address holds");
    }
    {
        struct wd_document d;
        build("<input name=a><button>x</button>", &d);
        check(fm_submit(&d, -1, 0, 0, 0, sent, sizeof(sent)) != NULL, "no form");
        check(fm_submit(&d, 5, -1, 0, 0, sent, sizeof(sent)) != NULL, "a form that is none");
        wd_free(&d);
        /* the buffer's end */
        build("<form action=/s><input name=a value=bbbbbbbbbbbbbbbbbbbb></form>", &d);
        check(fm_submit(&d, 0, -1, 0, 0, sent, 24) != NULL, "a short buffer: refused, not cut");
        check(fm_submit(&d, 0, -1, 0, 0, sent, 8) != NULL, "shorter than the address");
        wd_free(&d);
    }
}

/**** by hand ****/

#define W 640
#define H 400

static struct wb b;
static unsigned int screen[W * H];

static struct wb_page *page_at(const char *url, const char *html, int file) {
    struct ut_response r;
    char err[UT_ERR_MAX], path[UT_URL_MAX + 1];
    struct wb_page *p;
    memset(&r, 0, sizeof(r));
    if(file) {
        wb_file_path(url, path, sizeof(path));
        snprintf(err, sizeof(err), "https://" WB_FILE_HOST "%s", path);
        ut_url_parse(err, &r.url, err, sizeof(err));
    } else ut_url_parse(url, &r.url, err, sizeof(err));
    r.body = (unsigned char *)strdup(html);
    r.len = strlen(html);
    r.status = 200;
    r.verified = !strncmp(url, "https:", 6);
    snprintf(r.content_type, sizeof(r.content_type), "text/html");
    p = wb_page_new(&r, file, err, sizeof(err));
    free(r.body);
    return p;
}

static struct wb_page *open_page(const char *url, const char *html) {
    struct wb_request q;
    wb_go(&b, url);
    wb_take(&b, &q);
    wb_loaded(&b, page_at(q.url, html, !strncmp(q.url, "file:", 5)), 0);
    wb_cancel(&b);
    wb_paint(&b, screen, W);
    return wb_current(&b);
}

static void type(const char *s) { while(*s) wb_key(&b, (unsigned char)*s++); }

/* the middle of a control's box on the window */
static void spot(struct wb_page *p, int control, int *x, int *y) {
    long i = wl_control_item(&p->lay, control);
    *x = *y = 0;
    if(i < 0) return;
    *x = p->lay.items[i].x + p->lay.items[i].w / 2;
    *y = p->lay.items[i].y + p->lay.items[i].h / 2 + WB_TOOLBAR - p->scroll;
}

static void click(int x, int y) {
    wb_pointer(&b, x, y, 0);
    wb_pointer(&b, x, y, 1);
    wb_pointer(&b, x, y, 0);
}

static void click_control(struct wb_page *p, int control) {
    int x, y;
    spot(p, control, &x, &y);
    click(x, y);
}

static int asks(const char *url) {
    struct wb_request q;
    if(!b.want || strcmp(b.req.url, url)) {
        if(b.want) printf("  asks for %s\n", b.req.url);
        return 0;
    }
    wb_take(&b, &q);
    wb_stop(&b);
    wb_cancel(&b);
    return 1;
}

static int has_color(unsigned int c, int x0, int y0, int w, int h) {
    int i, j;
    for(j = y0; j < y0 + h; j++)
        for(i = x0; i < x0 + w; i++) if(screen[j * W + i] == c) return 1;
    return 0;
}

static void by_hand(void) {
    struct wb_page *p;
    int x, y;
    check(!wb_init(&b, W, H), "a browser");

    /* no page sends anything by itself */
    p = open_page("https://forms.test/a", "<form action=/s><input name=q autofocus value=x><input type=submit></form><script>form.submit()</script>");
    check(!b.want && !b.running && !wb_busy(&b) && b.field == -1, "a page's arrival sends nothing and takes no focus");

    /* a text field by the pointer and the keys, Enter submits */
    click_control(p, 0);
    check(b.focus == WB_FOCUS_FIELD && b.field == 0, "a click on a field: its keys");
    wb_key(&b, 0x105);
    type("yz w");
    check(!strcmp(p->doc.controls[0].value, "xyz w"), "typing");
    wb_paint(&b, screen, W);
    wb_key(&b, 127);
    wb_key(&b, 0x102);
    wb_key(&b, 0x108);
    check(!strcmp(p->doc.controls[0].value, "xyz"), "Backspace, Left, Delete");
    wb_key(&b, 0x104);
    wb_key(&b, 'a');
    check(!strcmp(p->doc.controls[0].value, "axyz") && p->doc.controls[0].caret == 1, "Home");
    wb_key(&b, 0x101);
    wb_key(&b, ' ');
    check(!strcmp(p->doc.controls[0].value, "a xyz"), "Space types in a text");
    check(!b.want, "typing sends nothing");
    wb_key(&b, 13);
    check(asks("https://forms.test/s?q=a+xyz"), "Enter: its form goes, by its button, its address in the field");
    check(!strcmp(b.addr, "https://forms.test/s?q=a+xyz"), "and the address field says so");
    check(b.focus == WB_FOCUS_PAGE && b.field == -1, "the keys are the page's");

    /* Escape, and the page's keys */
    click_control(p, 0);
    wb_key(&b, 27);
    check(b.focus == WB_FOCUS_PAGE && b.field == -1, "Escape: the keys are the page's");
    click_control(p, 0);
    wb_key(&b, 12);
    check(b.focus == WB_FOCUS_ADDR, "Ctrl+L still goes to the address");
    wb_key(&b, 27);

    /* a click on the button */
    click_control(p, 1);
    check(asks("https://forms.test/s?q=a+xyz"), "a click on its button");
    /* a press on it, and let go elsewhere */
    spot(p, 1, &x, &y);
    wb_pointer(&b, x, y, 0);
    wb_pointer(&b, x, y, 1);
    wb_pointer(&b, x + 200, y + 200, 1);
    wb_pointer(&b, x + 200, y + 200, 0);
    check(!b.want, "let go elsewhere: nothing");

    /* the other fields, by Tab */
    p = open_page("https://forms.test/b", "<p><a href=x.html>a link</a><form action=/f><input name=t value=v><input type=checkbox name=c><input type=radio name=r value=1>"
                                           "<input type=radio name=r value=2><select name=s><option>One<option>Two<option disabled>No<option>Four</select>"
                                           "<textarea name=a rows=3 cols=10></textarea><input disabled name=d><button type=button>Nothing</button><input type=reset><input type=submit name=go value=Go></form>");
    wb_key(&b, 9);
    check(b.tab_link == 0, "Tab: the link");
    wb_key(&b, 9);
    check(b.focus == WB_FOCUS_FIELD && b.field == 0 && b.tab_link == -1, "the field after it");
    wb_key(&b, 9);
    check(b.field == 1, "the checkbox");
    wb_paint(&b, screen, W);
    wb_key(&b, ' ');
    check(p->doc.controls[1].checked, "Space ticks it");
    wb_key(&b, ' ');
    check(!p->doc.controls[1].checked, "and not");
    wb_key(&b, ' ');
    wb_key(&b, 9);
    wb_key(&b, ' ');
    check(p->doc.controls[2].checked, "a radio button");
    wb_key(&b, 9);
    wb_key(&b, ' ');
    check(p->doc.controls[3].checked && !p->doc.controls[2].checked, "the other of its group");
    wb_key(&b, ' ');
    check(p->doc.controls[3].checked, "chosen stays chosen");
    wb_key(&b, 9);
    check(b.field == 4 && p->doc.controls[4].kind == WF_SELECT, "the select");
    wb_key(&b, 0x101);
    check(fm_chosen(&p->doc, 4) == 1, "Down: the next option");
    wb_key(&b, 0x101);
    check(fm_chosen(&p->doc, 4) == 3, "past one that is disabled");
    wb_key(&b, 0x101);
    check(fm_chosen(&p->doc, 4) == 3, "the last");
    wb_key(&b, 0x100);
    check(fm_chosen(&p->doc, 4) == 1, "Up");
    wb_key(&b, ' ');
    check(b.list == 4 && b.list_at == 1, "Space: its list");
    wb_paint(&b, screen, W);
    wb_key(&b, 0x101);
    wb_key(&b, 27);
    check(b.list == -1 && fm_chosen(&p->doc, 4) == 1, "Escape: as it was");
    wb_key(&b, ' ');
    wb_key(&b, 0x101);
    wb_key(&b, 0x101);
    wb_key(&b, 13);
    check(b.list == -1 && fm_chosen(&p->doc, 4) == 3, "Enter: that one");
    wb_key(&b, 9);
    check(b.field == 5, "the text area");
    type("one");
    wb_key(&b, 13);
    type("two");
    check(!strcmp(p->doc.controls[5].value, "one\ntwo") && !b.want, "Enter in a text area: a new line, nothing sent");
    wb_key(&b, 0x100);
    wb_key(&b, 'X');
    check(!strcmp(p->doc.controls[5].value, "oneX\ntwo"), "Up: the line before, the same column");
    wb_paint(&b, screen, W);
    wb_key(&b, 9);
    check(b.field == 7, "past the disabled field: the next that can be used");
    wb_key(&b, ' ');
    check(!b.want, "a plain button does nothing");
    wb_key(&b, 9);
    check(b.field == 8, "Reset");
    wb_key(&b, WB_KEY_BACKTAB);
    check(b.field == 7, "Shift+Tab");
    wb_key(&b, 9);
    wb_key(&b, 9);
    check(b.field == 9, "the submit button");
    wb_key(&b, ' ');
    check(asks("https://forms.test/f?t=v&c=on&r=2&s=Four&a=oneX%0D%0Atwo&go=Go"), "Space on it: all of it, the fields as they stand");
    /* Reset puts it all back, and sends nothing */
    click_control(p, 8);
    check(!b.want && !p->doc.controls[1].checked && !p->doc.controls[3].checked && fm_chosen(&p->doc, 4) == 0 && p->doc.controls[5].len == 0, "Reset: as the page had it");

    /* a select by the pointer */
    click_control(p, 4);
    check(b.list == 4, "a click on a select: its list");
    {
        int lx = 20, ly;
        long i = wl_control_item(&p->lay, 4);
        ly = p->lay.items[i].y + p->lay.items[i].h + WB_TOOLBAR - p->scroll + 1 + 1 * (16 + 2) + 4;
        lx = p->lay.items[i].x + 8;
        click(lx, ly);
        check(b.list == -1 && fm_chosen(&p->doc, 4) == 1, "a click on an option");
    }
    click_control(p, 4);
    click(600, 380);
    check(b.list == -1 && fm_chosen(&p->doc, 4) == 1, "a click elsewhere closes it");
    /* a multiple select, and many options */
    p = open_page("https://forms.test/c", "<form action=/m><select name=m multiple><option>A<option>B<option>C</select></form>");
    wb_key(&b, 9);
    wb_key(&b, ' ');
    wb_key(&b, ' ');
    wb_key(&b, 0x101);
    wb_key(&b, ' ');
    wb_key(&b, 13);
    wb_key(&b, 13);
    check(asks("https://forms.test/m?m=A&m=B"), "a multiple select: each by Space");
    {
        static char many[3000];
        char *e = many;
        int i;
        e += snprintf(e, 32, "<form><select name=s>");
        for(i = 0; i < 40; i++) e += snprintf(e, 32, "<option>Option %d", i);
        p = open_page("https://forms.test/d", many);
        wb_key(&b, 9);
        wb_key(&b, ' ');
        for(i = 0; i < 39; i++) wb_key(&b, 0x101);
        check(b.list_at == 39 && b.list_top == 30, "a long list follows");
        wb_paint(&b, screen, W);
        wb_wheel(&b, -5);
        check(b.list_top == 25, "the wheel over it");
        wb_wheel(&b, 500);
        check(b.list_top == 30, "and its end");
        wb_key(&b, 27);
    }

    /* what is not sent says so, and nothing goes */
    p = open_page("https://forms.test/e", "<form method=post action=/login><input name=a value=1><input type=submit></form>");
    click_control(p, 1);
    check(!b.want && strstr(b.status, "POST") && strstr(b.status, "nothing was sent"), "a POST form says it was not sent");
    p = open_page("https://forms.test/f", "<form action=/login><input name=u value=me><input type=password name=pw value=preset><input type=submit></form>");
    click_control(p, 1);
    wb_key(&b, 0x105);
    type("hunter2");
    wb_paint(&b, screen, W);
    check(!strstr(b.status, "hunter2"), "a password does not show in the status bar");
    wb_key(&b, 13);
    check(!b.want && strstr(b.status, "password") && strstr(b.status, "nothing was sent"), "a password form: not over GET");
    check(!strstr(b.addr, "hunter2") && !strstr(b.addr, "preset"), "and nothing of it in the address");
    click_control(p, 2);
    check(!b.want, "by its button neither");
    {
        char stars[64];
        size_t n;
        const char *s = fm_shown(&p->doc, &p->doc.controls[1], stars, &n);
        check(n == 13 && !memcmp(s, "*************", 13), "it shows as stars");
        check(p->doc.controls[1].len == 13, "typed: 6 + 7");
    }
    /* left: gone from the history */
    {
        struct wb_page *q = p;
        open_page("https://forms.test/g", "<p>Next");
        check(q->doc.controls[1].len == 0 && q->doc.controls[1].value[0] == 0 && !q->doc.controls[1].caret, "a page left keeps no password");
        check(strlen(q->doc.controls[0].value) == 2, "its other fields stay");
        wb_key(&b, 127);
        wb_cancel(&b);
        wb_paint(&b, screen, W);
        check(wb_current(&b) == q && q->doc.controls[1].len == 0, "back: an empty field");
        q->doc.controls[1].first = 0;
        click_control(q, 1);
        type("again");
        check(q->doc.controls[1].len == 5, "typed again");
        wb_key(&b, 27);
        /* left by Back, the same: the page after it, then back to it */
        open_page("https://forms.test/g", "<form><input type=password name=p></form>");
        p = wb_current(&b);
        click_control(p, 0);
        type("secret");
        check(p->doc.controls[0].len == 6, "typed");
        wb_key(&b, 27);
        wb_key(&b, 127);
        wb_cancel(&b);
        check(!p->doc.controls[0].len && !p->doc.controls[0].value[0], "a page left by Back keeps no password either");
    }

    /* https to http, and the rest of the rules */
    p = open_page("https://forms.test/h", "<form action=http://plain.test/s><input name=a value=1><input type=submit></form>");
    click_control(p, 1);
    check(!b.want && strstr(b.status, "unencrypted") && strstr(b.status, "nothing was sent"), "an https page's form over http is not sent");
    p = open_page("http://plain.test/h", "<form action=https://forms.test/s><input name=a value=1><input type=submit></form>");
    click_control(p, 1);
    check(asks("https://forms.test/s?a=1"), "an http page's over https goes");
    p = open_page("http://plain.test/i", "<form action=https://" WB_FILE_HOST "/etc/passwd><input name=a value=1><input type=submit></form>");
    click_control(p, 1);
    check(!b.want && strstr(b.status, "nothing was sent"), "a web page's form never reaches a file");
    p = open_page("https://forms.test/h2", "<form action='https://forms.test/a b'><input name=a value=1><input type=submit></form>");
    click_control(p, 1);
    check(asks("https://forms.test/a%20b?a=1"), "an address with a space, as links have it");
    p = open_page("/stick/form.html", "<form action=other.html><input name=a value=1><input type=submit></form>");
    click_control(p, 1);
    check(asks("file:///stick/other.html?a=1"), "a file page's form: the file beside it");
    p = open_page("/stick/form.html", "<form action=http://10.0.2.2:8000/search><input name=a value=1><input type=submit></form>");
    click_control(p, 1);
    check(asks("http://10.0.2.2:8000/search?a=1"), "or the web");

    /* Enter with no button: as the standard has it */
    p = open_page("https://forms.test/j", "<form action=/one><input name=a value=1></form>");
    click_control(p, 0);
    wb_key(&b, 13);
    check(asks("https://forms.test/one?a=1"), "one field and no button: Enter sends it");
    p = open_page("https://forms.test/k", "<form action=/two><input name=a><input name=b></form>");
    click_control(p, 0);
    wb_key(&b, 13);
    check(!b.want, "two fields and no button: it does not");
    p = open_page("https://forms.test/l", "<form action=/three><input name=a><input type=submit disabled></form>");
    click_control(p, 0);
    wb_key(&b, 13);
    check(!b.want, "a disabled first button: it does not");
    p = open_page("https://forms.test/m", "<form action=/four><input name=a><input type=checkbox name=c></form>");
    click_control(p, 1);
    wb_key(&b, 13);
    check(asks("https://forms.test/four?a=&c=on"), "Enter on a checkbox: its form");
    p = open_page("https://forms.test/n", "<input name=alone><button>Alone</button>");
    click_control(p, 0);
    wb_key(&b, 13);
    check(!b.want, "a field in no form");
    click_control(p, 1);
    check(!b.want && strstr(b.status, "no form"), "a button in no form");

    /* what cannot be used */
    p = open_page("https://forms.test/o", "<form action=/d><input name=a disabled value=1><input name=b readonly value=2 maxlength=4><input name=c maxlength=2><input type=file name=f></form>");
    click_control(p, 0);
    check(b.field == -1, "a disabled field takes no keys");
    click_control(p, 1);
    wb_key(&b, 0x105);
    type("x");
    wb_key(&b, 127);
    check(!strcmp(p->doc.controls[1].value, "2"), "a read-only one takes no typing");
    click_control(p, 2);
    type("abcd");
    check(!strcmp(p->doc.controls[2].value, "ab"), "maxlength");
    click_control(p, 3);
    check(strstr(b.status, "file") != NULL && !b.want, "a file field says what it cannot do");

    /* long texts stay in view; a field goes on to its limit */
    p = open_page("https://forms.test/p", "<form><input name=a size=10><textarea name=t rows=2 cols=10></textarea></form>");
    click_control(p, 0);
    {
        int i;
        for(i = 0; i < 40; i++) wb_key(&b, 'a' + i % 26);
        wb_paint(&b, screen, W);
        check(p->doc.controls[0].first > 20, "the end of a long text in view");
        wb_key(&b, 0x104);
        wb_paint(&b, screen, W);
        check(p->doc.controls[0].first == 0, "Home: its start");
        wb_key(&b, 9);
        for(i = 0; i < 60; i++) wb_key(&b, 'a' + i % 26);
        wb_paint(&b, screen, W);
        check(p->doc.controls[1].top > 0, "an area's lines in view");
    }
    /* the page shows what was typed, in the field's box */
    p = open_page("https://forms.test/q", "<form><input name=a placeholder=Words></form>");
    click_control(p, 0);
    type("Mm");
    wb_key(&b, 27);
    wb_paint(&b, screen, W);
    {
        long i = wl_control_item(&p->lay, 0);
        const struct wl_item *it = &p->lay.items[i];
        check(has_color(b.theme.field_fg, it->x + 3, it->y + WB_TOOLBAR, 24, it->h), "typed text, in the field's colour");
        check(!has_color(b.theme.placeholder, it->x + 3, it->y + WB_TOOLBAR + 2, it->w - 6, it->h - 4), "not the placeholder");
    }
    wb_free(&b);
}

int main(void) {
    model();
    encoding();
    by_hand();
    check(ut_alloc_live == 0, "nothing held at the end");
    if(failures) {
        printf("WUNDERTOW FORM TESTS FAILED: %d of %d checks\n", failures, checks);
        return 1;
    }
    printf("WUNDERTOW FORM TESTS PASSED: %d checks, 0 failures\n", checks);
    return 0;
}
