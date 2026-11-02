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

static const char *content_type = "text/html";

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
    snprintf(r.content_type, sizeof(r.content_type), "%s", content_type);
    snprintf(r.charset, sizeof(r.charset), "%s", charset);
    rc = wd_build(&r, d, err, sizeof(err));
    free(r.body);
    return rc;
}

static int build(const char *html, struct wd_document *d) { return build_as(PAGE, html, "utf-8", d); }

/* The same of n bytes, which may hold a NUL */
static int build_bytes(const char *html, size_t n, struct wd_document *d) {
    struct ut_response r;
    char err[UT_ERR_MAX];
    int rc;
    memset(&r, 0, sizeof(r));
    ut_url_parse(PAGE, &r.url, err, sizeof(err));
    r.body = (unsigned char *)malloc(n + 1);
    memcpy(r.body, html, n);
    r.body[n] = 0;
    r.len = n;
    r.status = 200;
    snprintf(r.content_type, sizeof(r.content_type), "text/html");
    snprintf(r.charset, sizeof(r.charset), "utf-8");
    rc = wd_build(&r, d, err, sizeof(err));
    free(r.body);
    return rc;
}

/* What form 0 of a page sends, with no button: "" and why in the status when nothing */
static char sent[WB_ADDR_MAX + 64];
static const char *why;

/* 11/02/26 (the integration of milestones 2 and 3): what display: none hides has no box but is its form's, so a page's
   forms are the same whatever its CSS hides: every page sent here is built again with a sheet that hides all of it, and
   with one that hides its elements, and must send the same */
static int css_pages, css_differ;

static void same_whatever_css(const char *html, const struct wd_document *d) {
    static const char *const hide[] = { "<style>*{display:none}</style>",
        "<style>input,select,textarea,button,form,fieldset,legend,option,div,p,span,label,b,i,a,ul,li,table,tr,td"
        "{display:none}</style>" };
    static char page[600000], got[WB_ADDR_MAX + 64];
    size_t n = strlen(html), k;
    for(k = 0; k < sizeof(hide) / sizeof(hide[0]); k++) {
        struct wd_document e;
        const char *no;
        if(n + strlen(hide[k]) + 1 > sizeof(page)) continue;
        memcpy(page, html, n);
        strcpy(page + n, hide[k]);
        if(build(page, &e)) continue;
        no = fm_submit(&e, 0, -1, 0, 0, got, sizeof(got));
        css_pages++;
        if(e.control_count != d->control_count || e.option_count != d->option_count || !no != !why || (!no && strcmp(got, sent))) {
            css_differ++;
            printf("  with %s: %.200s\n", hide[k], html);
        }
        wd_free(&e);
    }
}

static const char *send_of(const char *html) {
    struct wd_document d;
    if(build(html, &d)) {
        why = "the page did not build";
        return "";
    }
    why = fm_submit(&d, 0, -1, 0, 0, sent, sizeof(sent));
    same_whatever_css(html, &d);
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
    check(!build("<form action=a><form hidden action=b><input name=y></form><input name=z>", &d), "a form in a form, hidden");
    check(d.form_count == 1 && d.controls[0].form == 0 && d.controls[0].run >= 0 && d.controls[1].form == -1, "it is no element at all: nothing of it counts");
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
    /* what follows a form is outside it, in the page's boxes too */
    check(!build("<form>in</form>out", &d), "text in a form and after it");
    {
        size_t i, in = 0, out = 0;
        for(i = 0; i < d.block_count; i++) {
            unsigned int k;
            for(k = 0; k < d.blocks[i].nruns; k++) {
                const struct wd_run *r = &d.runs[d.blocks[i].run + k];
                if(r->len == 2 && !memcmp(d.text + r->off, "in", 2)) in = i;
                if(r->len == 3 && !memcmp(d.text + r->off, "out", 3)) out = i;
            }
        }
        check(in && out && in != out, "in boxes of their own");
    }
    wd_free(&d);
    /* a table's cell ends a button left open in it */
    check(!build("<table><tr><td><button>x</td><td>after</table>", &d), "a button left open");
    check(d.runs[d.controls[0].run].len == 1, "its label ends with its cell");
    wd_free(&d);
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
        e += snprintf(e, 32, "<form id=f action=/s></form>");
        for(i = 0; i < (int)WD_CONTROLS_MAX; i++) e += snprintf(e, 32, "<input name=a>");
        e += snprintf(e, 32, "<input name=z form=f>");
        check(!build(big, &d) && d.control_count == WD_CONTROLS_MAX && d.forms[0].too_much, "one past them of a form elsewhere: that form");
        wd_free(&d);
        e = big;
        e += snprintf(e, 32, "<form id=f action=/s></form>");
        for(i = 0; i < (int)WD_CONTROLS_MAX; i++) e += snprintf(e, 32, "<input name=a>");
        e += snprintf(e, 32, "<input name=z form=''>");
        check(!build(big, &d) && d.control_count == WD_CONTROLS_MAX && !d.forms[0].too_much, "one of no form at all: no form's");
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
        for(i = 0; i < 360; i++) e += snprintf(e, 2100, "<input type=hidden name=%s value=%s>", name, name);
        check(!build(big, &d) && !d.truncated && d.forms[0].too_much && d.control_count == 349, "past the bytes kept, on a page that did not stop");
        check(d.form_bytes <= WD_FORM_BYTES + d.control_count * sizeof(*d.controls) + d.form_count * sizeof(*d.forms), "and no more kept than that");
        wd_free(&d);
        /* the same page within them */
        e = big;
        e += snprintf(e, 32, "<form>");
        for(i = 0; i < 349; i++) e += snprintf(e, 2100, "<input type=hidden name=%s value=%s>", name, name);
        check(!build(big, &d) && !d.forms[0].too_much && d.control_count == 349, "just within them");
        wd_free(&d);
        /* a name longer than is kept */
        e = big;
        e += snprintf(e, 40, "<form><input value=1 name=\"");
        memset(e, 'n', 2000);
        e += 2000;
        snprintf(e, 16, "\"></form>");
        check(!build(big, &d) && d.forms[0].too_much, "a name cut: the form is not sent");
        wd_free(&d);
        /* an option's value, and an option's text where that is its value */
        e = big;
        e += snprintf(e, 60, "<form><select name=s><option value=\"");
        memset(e, 'v', 2000);
        e += 2000;
        snprintf(e, 40, "\">One</select></form>");
        check(!build(big, &d) && d.forms[0].too_much, "an option's value cut");
        wd_free(&d);
        e = big;
        e += snprintf(e, 60, "<form><select name=s><option>");
        memset(e, 't', 2000);
        e += 2000;
        snprintf(e, 40, "</select></form>");
        check(!build(big, &d) && d.forms[0].too_much, "an option's text cut, which it would send");
        wd_free(&d);
        e = big;
        e += snprintf(e, 60, "<form><select name=s><option value=1>");
        memset(e, 't', 2000);
        e += 2000;
        snprintf(e, 40, "</select></form>");
        check(!build(big, &d) && !d.forms[0].too_much, "with a value, only what it shows is cut");
        wd_free(&d);
        e = big;
        e += snprintf(e, 60, "<form><select name=s><option value=1>");
        memset(e, 't', 2000);
        e += 2000;
        snprintf(e, 40, "<option>Two</select></form>");
        check(!build(big, &d) && !d.forms[0].too_much, "and the next option's own text is whole");
        wd_free(&d);
        /* options past the bytes kept */
        e = big;
        e += snprintf(e, 60, "<form><select name=s>");
        for(i = 0; i < 700; i++) e += snprintf(e, 2100, "<option value=%s>%s", name, name);
        check(!build(big, &d) && !d.truncated && d.forms[0].too_much && d.option_count < 700, "options past the bytes kept");
        wd_free(&d);
        /* the page's own address past them: refused, not outside them */
        {
            char url[900];
            size_t k = (size_t)snprintf(url, sizeof(url), "https://forms.test/");
            memset(url + k, 'p', 700);
            url[k + 700] = 0;
            e = big;
            for(i = 0; i < 349; i++) e += snprintf(e, 2100, "<input type=hidden name=%s value=%s>", name, name);
            snprintf(e, 40, "<form><input type=hidden name=a></form>");
            check(!build_as(url, big, "utf-8", &d) && !d.forms[0].action, "a form of the page's own address, with no room for it");
            why = fm_submit(&d, 0, -1, 0, 0, sent, sizeof(sent));
            check(why && strstr(why, "address"), "says so");
            wd_free(&d);
        }
        e = big;
        e += snprintf(e, 80, "<form id=f></form><select name=s form=f><option value=\"");
        memset(e, 'v', 2000);
        e += 2000;
        snprintf(e, 40, "\">One</select>");
        check(!build(big, &d) && d.forms[0].too_much, "one cut in a select of a form elsewhere: that form");
        wd_free(&d);
        /* a form= too long to be sure of */
        e = big;
        e += snprintf(e, 80, "<form id=f action=/s></form><input name=a form=\"");
        memset(e, 'f', 2000);
        e += 2000;
        snprintf(e, 40, "\">");
        check(!build(big, &d) && d.forms[0].too_much, "a form= cut: it could be any form");
        wd_free(&d);
        /* an id too long to be sure of */
        e = big;
        e += snprintf(e, 40, "<form id=\"");
        memset(e, 'f', 2000);
        e += 2000;
        e += snprintf(e, 40, "\" action=/s></form>");
        check(!build(big, &d) && d.forms[0].too_much, "an id cut: a form= could have meant it");
        wd_free(&d);
        /* a control that came first, naming a form whose id then does not fit */
        e = big;
        e += snprintf(e, 1100, "<input name=a value=1 form=%s>", name);
        for(i = 0; i < 349; i++) e += snprintf(e, 2100, "<input type=hidden name=%s value=%s>", name, name);
        snprintf(e, 1100, "<form id=%s action=/s></form>", name);
        check(!build(big, &d) && d.form_count == 1 && !d.forms[0].id && d.forms[0].too_much, "an id that does not fit: its controls are not known, so it is not sent");
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
    /* deeper than the elements kept */
    {
        static char deep[4000];
        char *e = deep;
        int i;
        for(i = 0; i < 140; i++) e += snprintf(e, 8, "<div>");
        snprintf(e, 200, "<form action=/a id=f><input name=x></form><input name=y>");
        check(!build(deep, &d) && d.controls[0].form == 0 && d.controls[1].form == -1, "a form too deep to keep ends at its </form>");
        wd_free(&d);
        e = deep;
        e += snprintf(e, 30, "<form action=/a>");
        for(i = 0; i < 140; i++) e += snprintf(e, 8, "<div>");
        snprintf(e, 200, "<fieldset disabled><input name=x value=1></fieldset></form>");
        check(!build(deep, &d) && d.forms[0].too_much && !d.controls[0].disabled, "a disabled fieldset too deep to keep: its form is not sent");
        check(d.control_count == 1, "(its field)");
        wd_free(&d);
        e = deep;
        e += snprintf(e, 30, "<form action=/a>");
        for(i = 0; i < 140; i++) e += snprintf(e, 8, "<div>");
        snprintf(e, 200, "<fieldset><input name=x value=1></fieldset></form>");
        check(!build(deep, &d) && !d.forms[0].too_much, "one that is not disabled changes nothing");
        wd_free(&d);
    }
    /* addresses are in the budget when they are made absolute */
    check(!build("<form action=a></form>", &d) && d.form_bytes == sizeof(*d.forms) + strlen("https://forms.test/dir/a") + 1, "an address counts as it ends up, once");
    wd_free(&d);
    {
        static char big[200000], url[2100];
        char *e = big;
        int i;
        size_t n = (size_t)snprintf(url, sizeof(url), "https://forms.test/");
        memset(url + n, 'p', 1800);
        url[n + 1800] = '/';
        url[n + 1801] = 0;
        e += snprintf(e, 32, "<form action=/s>");
        for(i = 0; i < 4000; i++) e += snprintf(e, 32, "<button formaction=z>");
        check(!build_as(url, big, "utf-8", &d) && d.control_count == 4000, "4000 buttons with an address, on a page of a long one");
        check(d.form_bytes <= WD_FORM_BYTES + d.control_count * sizeof(*d.controls) + d.form_count * sizeof(*d.forms), "their addresses stay in the budget");
        check(d.controls[0].formaction && !d.controls[3999].formaction, "the first are kept, the last are not");
        why = fm_submit(&d, 0, 3999, 0, 0, sent, sizeof(sent));
        check(why && strstr(why, "address") && strstr(why, "nothing was sent"), "and a button with none says so, and does not use the form's");
        wd_free(&d);
        e = big;
        for(i = 0; i < (int)WD_FORMS_MAX; i++) e += snprintf(e, 32, "<form></form>");
        for(i = 0; i < 600; i++) e += snprintf(e, 32, "<form action=a></form>");
        check(!build_as(url, big, "utf-8", &d) && d.form_count == WD_FORMS_MAX && d.forms[0].action, "forms of the page's own address");
        check(d.form_bytes <= WD_FORM_BYTES + d.form_count * sizeof(*d.forms), "in the budget too");
        check(!d.forms[WD_FORMS_MAX - 1].action || d.form_bytes > 900000, "(the budget or the forms' limit, whichever comes first)");
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
    /* without memory: a page fails, or holds all of it */
    {
        static const char html[] = "<base href=/b/><form action=x></form>";
        long n;
        int bad = 0, held = 0;
        for(n = 0; n < 200; n++) {
            long before = ut_alloc_live;
            ut_alloc_fail_one = n;
            if(!build(html, &d)) {
                if(!d.forms[0].action || strcmp(d.forms[0].action, "https://forms.test/b/x")) bad = 1;
                wd_free(&d);
            }
            ut_alloc_fail_one = -1;
            if(ut_alloc_live != before) held = 1;
        }
        check(!bad, "a base, whichever allocation fails");
        check(!held, "and nothing held");
    }
    /* without memory */
    {
        static const char html[] = "<form id=f action=/a><input name=a value=b><select name=s><option value=1>One</select><textarea name=t>x</textarea><button formaction=/z>B</button></form><form><button formaction=''>C</button></form>";
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
    sends("<form action=/s><input type=file name=f value=c:/secrets.txt></form>", "https://forms.test/s?f=", "whatever a page says its value is");
    sends("<form action=/s><input type=hidden name=h value='x&#13;&#10;y&#13;z'></form>", "https://forms.test/s?h=x%0D%0Ay%0D%0Az", "a value's CR LF is one line end, a CR alone another");
    sends("<form action=/s><input type=submit name=go value=1><input name=a value=2><button name=b value=3></form>", "https://forms.test/s?a=2", "with no button: no button's value");
    /* what surrounds an address, and what no address means */
    sends("<form action=' /s\t'><input name=a value=1></form>", "https://forms.test/s?a=1", "an action's spaces at its ends go");
    sends("<form action='   '><input name=a value=1></form>", "https://forms.test/dir/page.html?a=1", "only spaces: no action");
    sends("<base href=https://other.test/x/><form action=''><input name=a value=1></form>", "https://forms.test/dir/page.html?a=1", "no action is the page's address, not the base's");
    /* a text field's value is one line, as a browser has it; a hidden one's is as it is */
    sends("<form action=/s><input name=a value='one&#10;two&#13;three'><input type=hidden name=h value='x&#10;y'></form>", "https://forms.test/s?a=onetwothree&h=x%0D%0Ay", "no line ends in a text field");
    /* _charset_: a hidden field of that name, in any case, whatever its value */
    sends("<form action=/s><input type=hidden name=_Charset_ value=zz><input name=_charset_ value=yy></form>", "https://forms.test/s?_Charset_=UTF-8&_charset_=yy", "_charset_: the standard's hidden field");
    /* options */
    sends("<form action=/s><select name=s><option>a&nbsp;b</select></form>", "https://forms.test/s?s=a%C2%A0b", "a no-break space in the text an option sends");
    sends("<form action=/s><select name=s><option>  a \n b  </select></form>", "https://forms.test/s?s=a+b", "ASCII spaces collapse");
    sends("<form action=/s><select name=s size=3><option>A<option>B</select></form>", "https://forms.test/s?", "a list of rows has nothing chosen by itself");
    sends("<form action=/s><select name=s><option label=L>Text</select></form>", "https://forms.test/s?s=Text", "an option sends its text, not its label");
    sends("<form action=/s><select name=s size=1><option>A<option>B</select></form>", "https://forms.test/s?s=A", "one of one row does");
    /* what the hidden attribute hides is still the page's form */
    sends("<form action=/s><div hidden><input type=hidden name=token value=abc></div><input name=a value=1></form>", "https://forms.test/s?token=abc&a=1", "a hidden field under the hidden attribute");
    sends("<form action=/s><div hidden><div><p><input name=b value=2><input type=checkbox name=c checked></div></div><input name=a value=1></form>", "https://forms.test/s?b=2&c=on&a=1", "any field, however deep");
    sends("<form action=/s><div hidden></form></div><input name=a value=1>", "https://forms.test/s?", "a form's end there is its end");
    sends("<form action=/s><div hidden><textarea name=t>words</textarea></div><textarea hidden name=u>more</textarea></form>", "https://forms.test/s?t=words&u=more", "a text area there");
    sends("<form action=/s><div hidden><template><input name=x value=1></template><input name=y value=2></div></form>", "https://forms.test/s?y=2", "a template there holds no fields");
    sends("<form action=/s><div hidden><template><template></template><input name=x></template><input name=y value=2></div></form>", "https://forms.test/s?y=2", "nor one in one");
    sends("<form action=/s><template><input name=x value=1></template><datalist><input name=z value=3></datalist><input name=y value=2></form>", "https://forms.test/s?y=2", "nor anywhere else");
    sends("<form action=/s><svg><input name=x value=1></svg><input name=y value=2></form>", "https://forms.test/s?y=2", "nor svg");
    sends("<div hidden></div><form action=/s><template><input name=x value=1></template><input name=y value=2></form>", "https://forms.test/s?y=2", "and after hidden content that has ended");
    sends("<form action=/s><div hidden><datalist></div><div hidden><input name=y value=2></div></form>", "https://forms.test/s?y=2", "what was open in hidden content is not left over");
    sends("<form action=/s><div hidden><template></div><input name=x value=1></template></div><input name=y value=2></form>", "https://forms.test/s?y=2", "a template holds even what would end what is around it");
    sends("<form action=/s><div hidden><fieldset><input name=a value=1></fieldset></div></form>", "https://forms.test/s?a=1", "a fieldset there that is not disabled");
    sends("<form action=/s><div hidden><template><textarea name=t>x</textarea></template><input name=y value=2></div></form>", "https://forms.test/s?y=2", "a text area in a template there");
    sends("<form action=/s><template><textarea name=t>x</textarea></template><input name=y value=2></form>", "https://forms.test/s?y=2", "or anywhere");
    refuses("<form action=/s><div hidden><select name=s><option>One</select></div></form>", "more than", "a select there is not followed");
    refuses("<form action=/s><select hidden name=s><option>One</select></form>", "more than", "a hidden select");
    refuses("<form action=/s><div hidden><button name=b>B</button></div></form>", "more than", "a button there");
    refuses("<form action=/s><div hidden><fieldset disabled><input name=x value=1></fieldset></div></form>", "more than", "a disabled fieldset there");
    refuses("<form action=/s><fieldset hidden disabled><input name=x value=1></fieldset></form>", "more than", "a disabled fieldset hidden");
    /* a form runs from its tag to its end tag, whatever the elements around them do */
    sends("<form action=/s><input name=q value=1></form><font size=2><input name=t value=2></font>", "https://forms.test/s?q=1", "a control after a form is in no form");
    sends("<form action=/s><input name=q value=1></form><b><select name=s><option>A</select><textarea name=t>x</textarea><button name=c>C</button></b>", "https://forms.test/s?q=1", "of any kind");
    sends("<table><tr><td><form action=/s></td><td><input name=q value=1></form></td><td><input name=t value=2></table>", "https://forms.test/s?q=1", "from the tag to the end tag, over what ended between them");
    sends("<table><form action=/s><tr><td><input name=q value=1></td></tr></form><tr><td><input name=t value=2></table>", "https://forms.test/s?q=1", "as an old table has it");
    sends("<div><form action=/s></div><div><input name=q value=1></div></form><input name=t value=2>", "https://forms.test/s?q=1", "and a div");
    sends("<form action=/s><input name=q value=1></form></form><input name=t value=2>", "https://forms.test/s?q=1", "a second </form> is nothing");
    /* what a browser has in a form by an element left open past its end is not followed: docs/UNDERTOW.md, Not yet */
    sends("<form action=/s><div><input name=q value=1></form><input name=t value=2></div>", "https://forms.test/s?q=1", "a control in a div left open");
    sends("<form action=/s><font><input name=q value=1></form><input type=password name=t></font>", "https://forms.test/s?q=1", "a password there is in no form, and goes nowhere");
    /* </form> where elements are not followed */
    sends("<form action=/s><div hidden></form></div><input name=a value=1>", "https://forms.test/s?", "in hidden content");
    sends("<form action=/s><div hidden></form><input name=a value=1></div>", "https://forms.test/s?", "and a control after it there");
    sends("<form action=/s><object></form><input name=x value=1></object><input name=y value=2>", "https://forms.test/s?", "in an object");
    sends("<form hidden action=/s><form action=/x></form>the rest<input name=b value=2>", "https://forms.test/s?", "a form in a hidden form is nothing, and the </form> is the first's");
    sends("<form action=/s><input name=a value=1><svg></form></svg><input name=b value=2>", "https://forms.test/s?a=1", "in svg");
    sends("<form action=/s><input name=a value=1><datalist></form></datalist><input name=b value=2>", "https://forms.test/s?a=1", "in a datalist");
    sends("<form action=/s><input name=a value=1><template></form></template><input name=b value=2></form>", "https://forms.test/s?a=1&b=2", "not in a template");
    sends("<form action=/s><div hidden><svg></form></svg><input name=b value=2></div>", "https://forms.test/s?", "in svg in hidden content");
    sends("<form action=/s><div hidden><template></form></template><input name=b value=2></div></form>", "https://forms.test/s?b=2", "not in a template there");
    {
        static char deep[4000];
        char *e = deep;
        int i;
        for(i = 0; i < 140; i++) e += snprintf(e, 8, "<div>");
        snprintf(e, 200, "<form action=/s><input name=q value=1></form><input name=x value=1>");
        sends(deep, "https://forms.test/s?q=1", "too deep to keep, a form still ends");
    }
    {
        struct wd_document d;
        build("<form action=/a><b></form><form action=/b><input name=x value=1></form></b>", &d);
        check(d.form_count == 2 && d.controls[0].form == 1 && !d.forms[0].too_much && !d.forms[1].too_much, "a control is the open form's");
        wd_free(&d);
        build("<form id=g action=/g></form><form action=/a><input name=x form=g></form>", &d);
        check(d.controls[0].form == 0, "or the one it names");
        wd_free(&d);
        build("<form hidden action=/s><form action=/x></form><input name=b>", &d);
        check(d.control_count == 1 && d.controls[0].run >= 0, "after a form in a hidden form, the page goes on: it was one element");
        wd_free(&d);
    }
    /* what falls back when it cannot be shown is still the page's form */
    sends("<form action=/s><object data=a.swf><input type=hidden name=token value=t></object><input name=q value=1></form>", "https://forms.test/s?token=t&q=1", "object");
    sends("<form action=/s><applet code=a><input type=hidden name=token value=t></applet><input name=q value=1></form>", "https://forms.test/s?token=t&q=1", "applet");
    sends("<object><form action=/s><input name=q value=1></form></object>", "https://forms.test/s?q=1", "a form in an object");
    sends("<form action=/s><div hidden><object><input name=q value=1></object></div></form>", "https://forms.test/s?q=1", "and under the hidden attribute");
    sends("<form action=/s><object><template><input name=x value=1></template></object><input name=y value=2></form>", "https://forms.test/s?y=2", "a template is none, there too");
    refuses("<form action=/s><object><select name=s><option>One</select></object></form>", "more than", "what takes more than an input there");
    /* in svg and math there are no controls */
    sends("<form action=/s><svg><input name=x value=1></svg><input name=y value=2></form>", "https://forms.test/s?y=2", "svg holds no control");
    sends("<form action=/s><svg><div><input name=a value=1></div></svg></form>", "https://forms.test/s?a=1", "what ends it, as a page's text has it");
    /* <svg/> and an element of no content in hidden content */
    sends("<!DOCTYPE html><form action=/s><div hidden><svg/><input name=y value=2></div></form>", "https://forms.test/s?y=2", "an svg ended by itself under the hidden attribute");
    sends("<form action=/s><div hidden><svg><svg/></svg><input name=y value=2></div></form>", "https://forms.test/s?y=2", "an svg ended by itself in one");
    sends("<form action=/s><div hidden><datalist><input name=x value=1></datalist><input name=y value=2></div></form>", "https://forms.test/s?y=2", "a datalist there");
    sends("<form action=/s><div hidden><svg><input name=x value=1></svg><input name=y value=2></div></form>", "https://forms.test/s?y=2", "svg there");
    sends("<form action=/s><div hidden><template></object><input name=x value=1></template><input name=y value=2></div></form>", "https://forms.test/s?y=2", "an end tag of another element ends no template");
    sends("<form action=/s><div hidden><button type=button name=b>B</button><button type=reset>R</button><input name=y value=2></div></form>", "https://forms.test/s?y=2", "a button that sends nothing, there");
    /* svg closed, the page goes on as before: raw text is raw */
    sends("<form action=/s><svg></svg><textarea name=a><input name=b value=2></textarea></form>", "https://forms.test/s?a=%3Cinput+name%3Db+value%3D2%3E", "a text area after svg");
    sends("<form action=/s><div hidden><svg></div><textarea name=a>x</textarea></form>", "https://forms.test/s?a=x", "after svg left open in hidden content");
    /* what a text area and an option hold */
    sends("<form action=/s><textarea name=t>a&#13;&#10;b&#13;\nc&#13;&#13;d</textarea></form>", "https://forms.test/s?t=a%0D%0Ab%0D%0Ac%0D%0A%0D%0Ad", "a CR and its LF, written any way: one line end");
    sends("<form action=/s><select name=s><option>A<template><textarea></template><option selected>B</textarea></template></select></form>", "https://forms.test/s?s=A", "a template in a select: what is in it is not read");
    sends("<form action=/s><select name=s><style><option selected>A</style></select></form>", "https://forms.test/s?s=A", "<style> in a select is ignored");
    sends("<table><tr><td><form action=/s><select name=s><option>A</th><option selected>B</select></form></table>", "https://forms.test/s?s=B", "the end of a cell that is not open does not end a select");
    sends("<form action=/s><input type=url name=u value='&#1;x&#1;'></form>", "https://forms.test/s?u=%01x%01", "only ASCII whitespace goes from a url's ends");
    {
        struct wd_document d;
        check(!build("<form><input type=url name=u value='  ab  '></form>", &d) && d.runs[d.controls[0].run].len == 2, "and from what its box shows");
        wd_free(&d);
        check(!build_bytes("<form><textarea name=t>a\0b</textarea></form>", 39, &d) && !strcmp(d.controls[0].value, "a\xef\xbf\xbd" "b"), "a NUL in a text area is U+FFFD");
        wd_free(&d);
    }
    refuses("<form action=/s><textarea name=t wrap=hard cols=3>abcdef</textarea></form>", "more than", "wrap=hard: line ends a browser would add");
    sends("<form action=/s><textarea name=t wrap=soft cols=3>abcdef</textarea></form>", "https://forms.test/s?t=abcdef", "wrap=soft adds none");
    sends("<form action=/s><input name=t wrap=hard value=1></form>", "https://forms.test/s?t=1", "and on an input it is nothing");
    refuses("<form action=/s><input type=range name=x value=''></form>", "more than", "a range with an empty value");
    /* a disabled fieldset's first legend is not disabled by it: which one that is takes elements that are not kept */
    refuses("<form action=/s><fieldset disabled><legend><input name=a value=1></legend><input name=b value=2></fieldset></form>", "more than", "a control in a legend of a disabled fieldset");
    refuses("<form action=/s><fieldset disabled><font><legend><input name=a value=1></legend></font></fieldset></form>", "more than", "wherever it is");
    refuses("<form action=/s><fieldset disabled><legend hidden><input name=a value=1></legend></fieldset></form>", "more than", "a hidden one too");
    sends("<form action=/s><fieldset disabled><legend>Words</legend><input name=b value=2></fieldset><input name=c value=3></form>", "https://forms.test/s?c=3", "a legend that is words is nothing");
    sends("<form action=/s><fieldset><legend><input name=a value=1></legend></fieldset></form>", "https://forms.test/s?a=1", "and one of a fieldset that is not disabled");
    sends("<form action=/s><fieldset><legend hidden><input name=a value=1></legend></fieldset></form>", "https://forms.test/s?a=1", "hidden too");
    sends("<form action=/s><fieldset disabled><legend></legend></fieldset><legend><input name=c value=3></legend></form>", "https://forms.test/s?c=3", "nor one after it");
    /* a disabled fieldset where it is not followed: only what it may hold */
    sends("<form action=/s><input name=a value=1></form><div hidden><fieldset disabled><input name=x></fieldset></div>", "https://forms.test/s?a=1", "a form elsewhere goes");
    sends("<div hidden><fieldset disabled></fieldset></div><form action=/s><div hidden><input name=y value=2></div></form>", "https://forms.test/s?y=2", "and what comes in other hidden content");
    sends("<div hidden><fieldset disabled></fieldset></div><form action=/s><input name=y value=2></form>", "https://forms.test/s?y=2", "and what is not hidden");
    refuses("<form action=/a><span hidden><fieldset disabled></span><input name=b value=1></fieldset></form>", "more than", "one that an end tag of what hides it may not end");
    refuses("<form action=/a><span hidden><fieldset disabled><fieldset></fieldset></span><input name=b value=1></fieldset></form>", "more than", "with one inside it that ended");
    sends("<form action=/a><div hidden><fieldset disabled></fieldset><fieldset></fieldset></div><input name=b value=1></form>", "https://forms.test/a?b=1", "one that ended is none");
    sends("<svg></svg><form action=/a><script/><input name=x value=1></script><input name=b value=2></form>", "https://forms.test/a?b=2", "after svg, a script is raw text, ended or not");
    /* references this browser does not know are not what a browser sends */
    refuses("<form action=/s><input type=hidden name=h value='x&sum;y'></form>", "more than", "in a value");
    refuses("<form action=/s><input name='a&beta;' value=1></form>", "more than", "in a name");
    refuses("<form action=/s><select name=s><option value='&hearts;'>A</select></form>", "more than", "in an option's value");
    refuses("<form action=/s><select name=s><option>&spades;</select></form>", "more than", "in the text it sends");
    refuses("<form action=/s><textarea name=t>&check;</textarea></form>", "more than", "in a text area");
    refuses("<form action=/s><input name=h value='&there4;'></form>", "more than", "with a digit in it");
    refuses("<form action=/s><input name=h value='&Alpha;'></form>", "more than", "with a capital");
    refuses("<form action=/s><input name=h value='a & b &sum;'></form>", "more than", "after another ampersand");
    sends("<form action=/s><input name=q value=x><input type=submit value='Find &rArr;'></form>", "https://forms.test/s?q=x", "not in what cannot go: a label with no name");
    sends("<form action=/s><input name=q value=x><input type=reset name=r value='&rArr;'><input type=button name=b value='&rArr;'><button type=reset name=c value='&rArr;'>C</button></form>", "https://forms.test/s?q=x", "a button that sends nothing");
    sends("<form action=/s><input name=q value=x><input name=n value='&rArr;' disabled><input value='&rArr;'><input type=file name=f value='&rArr;'><input type=image name=i value='&rArr;'></form>", "https://forms.test/s?q=x&f=", "what is disabled, has no name, or never sends its value");
    refuses("<form action=/s><input type=file name='f&sum;'></form>", "more than", "a file field's name goes");
    refuses("<form action=/s><input type=image name='i&sum;'></form>", "more than", "an image button's");
    sends("<form action=/s><select name=s><option value='&rArr;' disabled>A<option value=1 selected>B</select></form>", "https://forms.test/s?s=1", "a disabled option");
    refuses("<form action=/s><input type=submit name=go value='Find &rArr;'></form>", "more than", "a button with a name can go");
    refuses("<form action='/a&sum;b'><input name=a value=1></form>", "more than", "in the address");
    refuses("<form action=/s><button formaction='/a&sum;b'>B</button></form>", "more than", "in a button's");
    sends("<form action=/s><select name=s><option value=1>&spades;</select></form>", "https://forms.test/s?s=1", "not in what it only shows");
    sends("<form action=/s><input name=h value='a&b c&d'><input name=i value='&amp;&#38;&'></form>", "https://forms.test/s?h=a%26b+c%26d&i=%26%26%26", "an ampersand is one");
    sends("<form action=/s><input name=h value='&eacute;'></form>", "https://forms.test/s?h=%C3%A9", "one it knows is its character");
    sends("<form action=/s><input name=h value='a&;b'></form>", "https://forms.test/s?h=a%26%3Bb", "an ampersand and a semicolon are two");
    /* email, multiple */
    refuses("<form action=/s><input type=email multiple name=e value='a@x.org, b@x.org'></form>", "more than", "addresses a browser would take apart");
    sends("<form action=/s><input type=email multiple name=e value='a@x.org'></form>", "https://forms.test/s?e=a%40x.org", "one of them");
    sends("<form action=/s><input type=email name=e value='a,b'></form>", "https://forms.test/s?e=a%2Cb", "and not multiple");
    /* a whole text area */
    sends("<form action=/c><math></math><textarea name=t />y</textarea><input name=b value=1></form>", "https://forms.test/c?t=y&b=1", "<textarea /> is a start tag, also after math");
    {
        struct wd_document d;
        content_type = "application/xhtml+xml";
        check(!build("<form action='/a'><textarea name='t'/><input name='b' value='1'/></form>", &d), "a page of XHTML");
        check(d.control_count == 2 && d.controls[0].kind == WF_AREA && d.controls[0].run >= 0 && !d.controls[0].len, "<textarea/> is a text area with nothing in it");
        wd_free(&d);
        check(!build("<form action='/a'><div hidden='hidden'><textarea name='t'/></div></form>", &d) && d.control_count == 1, "under the hidden attribute too");
        wd_free(&d);
        check(!build("<form action='/a'><div hidden='hidden'><template><textarea name='t'/></template></div></form>", &d) && !d.control_count, "not in a template");
        wd_free(&d);
        content_type = "text/html";
    }
    /* what nothing says */
    {
        struct wd_document d;
        check(!build_as(PAGE, "<form action=/a><select name=s><option>Z&uuml;rich</select><input type=hidden name=_charset_></form>", "", &d) && d.form_charset == UT_CS_1252, "ASCII and no word of a charset");
        check(!fm_submit(&d, 0, -1, 0, 0, sent, sizeof(sent)) && !strcmp(sent, "https://forms.test/a?s=Z%FCrich&_charset_=windows-1252"), "a browser's default, in a form's bytes");
        wd_free(&d);
        check(!build_as(PAGE, "<meta charset=utf-8><form action=/a><input name=s value='&uuml;'></form>", "", &d) && d.form_charset == UT_CS_UTF8, "with a meta");
        wd_free(&d);
        check(!build_as(PAGE, "<form action=/a><input name=s value='\xc3\xbc'></form>", "", &d) && d.form_charset == UT_CS_UTF8, "with UTF-8 in it");
        wd_free(&d);
        check(!build_as(PAGE, "\xef\xbb\xbf<form action=/a><input name=s></form>", "", &d) && d.form_charset == UT_CS_UTF8, "with a byte order mark");
        wd_free(&d);
        {
            static char big[3000];
            char *e = big;
            e += snprintf(e, 40, "<html><head><title>");
            memset(e, 't', 1100);
            e += 1100;
            snprintf(e, 100, "</title><meta charset=utf-8></head><form action=/a><input name=s></form>");
            check(!build_as(PAGE, big, "", &d) && d.form_charset == UT_CS_UTF8, "a meta after the first 1024 bytes: a browser's parser sees it");
            wd_free(&d);
        }
        {
            static char big[3000];
            char *e = big;
            e += snprintf(e, 40, "<html><head><title>");
            memset(e, 't', 1100);
            e += 1100;
            snprintf(e, 200, "</title><meta http-equiv=Content-Type content='text/html; charset=iso-8859-1'></head><form action=/a><input name=s></form>");
            check(!build_as(PAGE, big, "", &d) && d.form_charset == UT_CS_1252, "one that says windows-1252's");
            wd_free(&d);
            e = big;
            e += snprintf(e, 40, "<html><head><title>");
            memset(e, 't', 1100);
            e += 1100;
            snprintf(e, 200, "</title><meta charset=shift_jis></head><form action=/a><input name=s value='&eacute;'></form>");
            check(!build_as(PAGE, big, "", &d) && d.form_charset == UT_CS_ASCII && d.forms[0].too_much, "one this browser does not write: its value is not sent");
            wd_free(&d);
        }
        check(!build_as(PAGE, "<?xml version='1.0' encoding='UTF-8'?><form action=/a><input name=s></form>", "", &d) && d.form_charset == UT_CS_UTF8, "an XML declaration");
        wd_free(&d);
        check(!build_as(PAGE, "<?xml version='1.0' encoding='iso-8859-1'?><form action=/a><input name=s></form>", "", &d) && d.form_charset == UT_CS_1252, "of latin-1");
        wd_free(&d);
        check(!build_as(PAGE, "<?xml version='1.0' encoding='iso-8859-1'?><html><head><meta charset=utf-8><form action=/a><input name=s></form>", "", &d) && d.form_charset == UT_CS_UTF8, "a meta says more than it");
        wd_free(&d);
        check(!build_as(PAGE, "<?xml version='>' encoding='utf-8'?><form action=/a><input name=s></form>", "", &d) && d.form_charset == UT_CS_1252, "a declaration ends at its first >, as browsers read it");
        wd_free(&d);
        check(!build_as(PAGE, "<?xml version='1.0' ENCODING='utf-8'?><form action=/a><input name=s></form>", "", &d) && d.form_charset == UT_CS_1252, "the word in lower case");
        wd_free(&d);
        check(!build_as(PAGE, "<?xml version='1.0' x-encoding='utf-8'?><form action=/a><input name=s></form>", "", &d) && d.form_charset == UT_CS_UTF8, "anywhere in it");
        wd_free(&d);
        check(!build_as(PAGE, "<?xml version='1.0' encoding=utf-8 ?><form action=/a><input name=s></form>", "", &d) && d.form_charset == UT_CS_1252, "quoted");
        wd_free(&d);
        check(!build_as(PAGE, "<?xml version='1.0'?><form action=/a><input name=s></form>", "", &d) && d.form_charset == UT_CS_1252, "that says none");
        wd_free(&d);
        content_type = "application/xhtml+xml";
        check(!build_as(PAGE, "<form action='/a'><input name='s'/></form>", "", &d) && d.form_charset == UT_CS_UTF8, "XHTML is UTF-8");
        wd_free(&d);
        content_type = "text/html";
    }
    /* a place on the page, with a base */
    refuses("<base href=https://other.test/d/><form action='#here'><input name=a value=1></form>", "address", "an action of a place only, with a base: links' rule is not a form's");
    sends("<form action='#here'><input name=a value=1></form>", "https://forms.test/dir/page.html?a=1#here", "with none, the page");
    sends("<form action=/s><textarea name=t>a&#13;b&#1;c&#x202e;d</textarea></form>", "https://forms.test/s?t=a%0D%0Ab%01c%E2%80%AEd", "a text area sends what it holds, and a CR is a line end");
    refuses("<form action=/s><select name=s><option>a&shy;b</select></form>", "more than", "an option's text with what it shows without");
    sends("<form action=/s><select name=s><option value=1>a&shy;b</select></form>", "https://forms.test/s?s=1", "with a value of its own it does not matter");
    /* the types of input a browser holds to a shape */
    sends("<form action=/s><input type=url name=u value=' http://a.test/ '><input type=email name=e value=' a@b.test '><input type=text name=t value=' x '></form>", "https://forms.test/s?u=http%3A%2F%2Fa.test%2F&e=a%40b.test&t=+x+", "a url's and an email's spaces go, not a text's");
    refuses("<form action=/s><input type=range name=x></form>", "more than", "a range with no value: a browser's is its middle");
    refuses("<form action=/s><input type=color name=x></form>", "more than", "a colour with none");
    sends("<form action=/s><input type=range name=x value=7><input type=color name=y value=#123456></form>", "https://forms.test/s?x=7&y=%23123456", "with one, as it is");
    refuses("<form action=/s><input name=q dirname=q.dir value=v></form>", "more than", "dirname: an entry it would add");
    /* a select's tags */
    sends("<form action=/s><select name=s><tr><option selected>A</select><input name=b value=2></form>", "https://forms.test/s?s=A&b=2", "a table's tags in a select outside any table");
    sends("<form action=/s><table><tr><td><select name=s><option>A<td><input name=b value=2></table></form>", "https://forms.test/s?s=A&b=2", "inside one they end it");
    sends("<form action=/s><select name=s><option>A<xmp><option selected>B</xmp></select><input name=b value=2></form>", "https://forms.test/s?s=B&b=2", "tags of raw text in a select are ignored");
    /* an address a limit changed */
    {
        static char big[6000];
        char *e = big;
        e += snprintf(e, 40, "<base href=\"https://other.test/");
        memset(e, 'a', 3000);
        e += 3000;
        snprintf(e, 100, "/\"><form action=x><input name=a value=1></form>");
        refuses(big, "more than", "a base too long for an address: where it would go is not known");
        e = big;
        e += snprintf(e, 40, "<base href=\"https://other.test/");
        memset(e, 'a', 5000);
        e += 5000;
        snprintf(e, 100, "/\"><form action=x><input name=a value=1></form>");
        refuses(big, "more than", "one too long to keep");
        refuses("<base href='http://[bad'><form action=x><input name=a value=1></form>", "more than", "one url.c does not take: a browser's rules are wider");
        refuses("<base href='http://[bad'><base href=https://other.test/><form action=x><input name=a value=1></form>", "more than", "the first is the one that counts");
        refuses("<base href=''><base href=https://other.test/><form action=x><input name=a value=1></form>", "more than", "even an empty one: links go on to the next, a browser's forms do not");
        {
            struct wd_document d;
            check(!build("<base href='http://[bad'><base href=/y/><a href=z>z</a>", &d) && !strcmp(d.links[0].url, "https://forms.test/y/z"), "links take the first that resolves, as Undertow's");
            wd_free(&d);
        }
        sends("<base href=https://other.test/d/><base href=https://third.test/><form action=x><input name=a value=1></form>", "https://other.test/d/x?a=1", "a second one is nothing");
        sends("<base href=''><base href='http://[bad'><form action=x><input name=a value=1></form>", "https://forms.test/dir/x?a=1", "an empty first, and one that is no address after it");
        e = big;
        e += snprintf(e, 80, "<base href=/b/><base href=\"https://other.test/");
        memset(e, 'a', 5000);
        e += 5000;
        snprintf(e, 100, "/\"><form action=x><input name=a value=1></form>");
        sends(big, "https://forms.test/b/x?a=1", "one too long after the first");
        {
            struct wd_document d;
            e = big;
            e += snprintf(e, 80, "<base href=\"https://other.test/");
            memset(e, 'a', 5000);
            e += 5000;
            snprintf(e, 100, "/\"><a href=z>z</a>");
            check(!build(big, &d) && !strcmp(d.links[0].url, "https://forms.test/dir/z"), "one too long is no base for links");
            wd_free(&d);
        }
        refuses("<div hidden><base href=https://other.test/></div><form action=x><input name=a value=1></form>", "more than", "one in what is not followed");
        sends("<base href=https://other.test/d/><div hidden><base href=/x/></div><form action=x><input name=a value=1></form>", "https://other.test/d/x?a=1", "not after the first");
        sends("<base href='https://other.test/d/?q'><form action=x><input name=a value=1></form>", "https://other.test/d/x?a=1", "a query in one is nothing");
    }
    /* accept-charset */
    {
        struct wd_document d;
        const char *html = "<form action=/s accept-charset=utf-8><input name=a value='caf\xe9'></form><form action=/s accept-charset='iso-8859-1 utf-8'><input name=a value='caf\xe9'><input type=hidden name=_charset_></form><form action=/s accept-charset=shift_jis><input name=a></form><form action=/s accept-charset=''><input name=a value='caf\xe9'></form>";
        check(!build_as(PAGE, html, "windows-1252", &d), "a page of forms with their own charsets");
        check(!fm_submit(&d, 0, -1, 0, 0, sent, sizeof(sent)) && !strcmp(sent, "https://forms.test/s?a=caf%C3%A9"), "accept-charset=utf-8 on a page in windows-1252");
        check(!fm_submit(&d, 1, -1, 0, 0, sent, sizeof(sent)) && !strcmp(sent, "https://forms.test/s?a=caf%E9&_charset_=windows-1252"), "a label of windows-1252, the first of two");
        why = fm_submit(&d, 2, -1, 0, 0, sent, sizeof(sent));
        check(why && strstr(why, "charset") && strstr(why, "nothing was sent"), "one this browser does not write: nothing goes");
        check(!fm_submit(&d, 3, -1, 0, 0, sent, sizeof(sent)) && !strcmp(sent, "https://forms.test/s?a=caf%E9"), "an empty one: the page's");
        wd_free(&d);
        check(!build_as(PAGE, "<form action=/s accept-charset=' utf-8 '><input name=a value='caf\xe9'></form>", "windows-1252", &d), "a label with spaces around it");
        check(!fm_submit(&d, 0, -1, 0, 0, sent, sizeof(sent)) && !strcmp(sent, "https://forms.test/s?a=caf%C3%A9"), "is that label");
        wd_free(&d);
        check(!build_as(PAGE, "<form action=/s accept-charset=windows-1252><input name=a value='caf&eacute;'></form>", "utf-8", &d), "a page in UTF-8");
        check(!fm_submit(&d, 0, -1, 0, 0, sent, sizeof(sent)) && !strcmp(sent, "https://forms.test/s?a=caf%E9"), "and a form in windows-1252");
        wd_free(&d);
        check(!build_as(PAGE, "<form action=/s accept-charset='iso-8859-1,utf-8'><input name=a value='caf&eacute;'></form><form action=/s accept-charset='UNKNOWN'><input name=a value='caf&eacute;'></form><form action=/s accept-charset='unknown, utf-8'><input name=a value='caf&eacute;'></form><form action=/s accept-charset=UTF-16><input name=a value='caf&eacute;'></form>", "windows-1252", &d), "lists of charsets");
        check(!fm_submit(&d, 0, -1, 0, 0, sent, sizeof(sent)) && !strcmp(sent, "https://forms.test/s?a=caf%E9"), "commas between them");
        check(!fm_submit(&d, 1, -1, 0, 0, sent, sizeof(sent)) && !strcmp(sent, "https://forms.test/s?a=caf%E9"), "HTML 4's UNKNOWN: the page's");
        check(!fm_submit(&d, 2, -1, 0, 0, sent, sizeof(sent)) && !strcmp(sent, "https://forms.test/s?a=caf%C3%A9"), "and on to the next");
        check(!fm_submit(&d, 3, -1, 0, 0, sent, sizeof(sent)) && !strcmp(sent, "https://forms.test/s?a=caf%C3%A9"), "UTF-16 goes as UTF-8");
        wd_free(&d);
        check(!build_as(PAGE, "<form accept-charset=utf-16le><input name=a></form><form accept-charset=UTF-16BE><input name=a></form>", "windows-1252", &d), "each byte order");
        check(d.forms[0].accept == UT_CS_UTF8 + 1 && d.forms[1].accept == UT_CS_UTF8 + 1, "of it");
        wd_free(&d);
        {
            static char big[400];
            char *e = big;
            e += snprintf(e, 40, "<form action=/s accept-charset='");
            memset(e, ' ', 100);
            e += 100;
            snprintf(e, 60, "utf-8'><input name=a></form>");
            check(!build(big, &d) && fm_submit(&d, 0, -1, 0, 0, sent, sizeof(sent)) != NULL, "an accept-charset too long to read: nothing goes");
            wd_free(&d);
        }
    }
    /* a page in a charset this browser does not read: its bytes over 127 are '?' by now */
    {
        struct wd_document d;
        check(!build_as(PAGE, "<form action=/s><input type=hidden name=x value='\x93\xfa\x96\x7b'></form><form action=/s><select name=s><option>\x93\xfa</select></form><form action=/s><input name=a value=plain><input name=b></form>", "shift_jis", &d) && d.form_charset == UT_CS_ASCII, "a page in shift_jis");
        check(d.forms[0].too_much && d.forms[1].too_much, "what it says is not known: not sent");
        check(!d.forms[2].too_much && !fm_submit(&d, 2, -1, 0, 0, sent, sizeof(sent)) && !strcmp(sent, "https://forms.test/s?a=plain&b="), "what is ASCII goes");
        wd_free(&d);
        check(!build_as(PAGE, "<form><input name=x value='&#26085;'></form><form><input name='caf&eacute;'></form><form><select name=s><option value='&#26085;'>A</select></form><form action='/\x93\xfa.cgi'><input name=a></form><form><button formaction='/\x93\xfa.cgi'>B</button></form><form><input type=hidden name=_charset_></form><form><input type=hidden name=_charset_ form=nowhere></form>", "shift_jis", &d), "references on such a page");
        check(d.forms[0].too_much && d.forms[1].too_much && d.forms[2].too_much, "a value, a name, an option's: a browser sends them in that charset");
        check(d.forms[3].too_much && d.forms[4].too_much, "an address that held its bytes, a form's and a button's");
        check(d.forms[5].too_much, "_charset_ would name the charset");
        check(!d.forms[6].too_much, "(a form with nothing in it)");
        wd_free(&d);
        check(!build_as(PAGE, "<base href='/\x93\xfa/'><form action=x><input name=a></form>", "shift_jis", &d) && d.forms[0].too_much, "a base that held them");
        wd_free(&d);
        check(!build_as(PAGE, "<form id='caf\xe9' action=/s></form><input name=secret form='caf\xe8'>", "shift_jis", &d) && d.forms[0].too_much, "a form= that held them: which id it says is not known");
        wd_free(&d);
    }
    /* windows-1252's holes stand for themselves */
    {
        struct wd_document d;
        check(!build_as(PAGE, "<form action=/s><input name=a value='&#129;&#141;'></form>", "windows-1252", &d), "a page in windows-1252, with two of its holes");
        check(!fm_submit(&d, 0, -1, 0, 0, sent, sizeof(sent)) && !strcmp(sent, "https://forms.test/s?a=%81%8D"), "as bytes");
        wd_free(&d);
    }
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
        build("<form action=/s><input name=a><input type=password name=p></form><form><input type=password name=q></form>", &d);
        check(fm_default_button(&d, 0) == -2, "a password field is a field that takes text");
        check(fm_default_button(&d, 1) == -1, "one, and no button");
        wd_free(&d);
        build("<form action=/s><input name=a><input type=range name=r value=2><input type=color name=c value=#000000></form>", &d);
        check(fm_default_button(&d, 0) == -1, "a range and a colour take no text, as a browser has them");
        wd_free(&d);
    }
    {
        struct wd_document d;
        /* a new form inside what is left of the one before: its own */
        build("<form action=/a><div></form><form action=/b><input name=t value=1></form></div>", &d);
        check(d.form_count == 2 && d.controls[0].form == 1, "the form open, not the one around it");
        wd_free(&d);
        build("<form action=/a><div></form><div hidden><form action=/b></div><input name=t value=1></div>", &d);
        check(d.form_count == 2 && d.controls[0].form == 1, "also where elements say the other");
        wd_free(&d);
        /* a CR in a text area's text is a line end there too */
        build("<form><textarea name=t>a&#13;b</textarea></form>", &d);
        check(!strcmp(d.controls[0].value, "a\nb"), "as its value holds it");
        wd_free(&d);
        /* a password's value has no line ends either */
        build("<form><input type=password name=p value='x&#10;y'></form>", &d);
        check(d.controls[0].len == 2 && !strcmp(d.controls[0].value, "xy"), "a password's value is one line");
        wd_free(&d);
        /* a text field's one line is also what Reset gives back */
        build("<form><input name=a value='x&#10;y'></form>", &d);
        check(d.controls[0].len == 2 && !strcmp(d.controls[0].dflt, "xy"), "as the page gave it, one line");
        d.controls[0].value[0] = 'z';
        fm_reset(&d, 0);
        check(!strcmp(d.controls[0].value, "xy") && d.controls[0].len == 2, "Reset");
        wd_free(&d);
        /* form= and formaction= with nothing in them */
        build("<form action=/s id=f><input name=a value=1 form=''><input name=b value=2><button formaction=''>B</button></form>", &d);
        check(d.controls[0].form == -1 && d.controls[1].form == 0, "form='': no form, not the one it is in");
        check(!fm_submit(&d, 0, 2, 0, 0, sent, sizeof(sent)) && !strcmp(sent, "https://forms.test/dir/page.html?b=2"), "formaction='': the page's own address");
        wd_free(&d);
        build("<form action=/s><button formaction=' /z '>B</button><button formaction=mailto:a@b.test>C</button></form>", &d);
        check(!fm_submit(&d, 0, 0, 0, 0, sent, sizeof(sent)) && !strcmp(sent, "https://forms.test/z?"), "a formaction's spaces at its ends go");
        why = fm_submit(&d, 0, 1, 0, 0, sent, sizeof(sent));
        check(why && strstr(why, "address"), "a formaction that cannot be used: nothing goes, nor to the form's address");
        wd_free(&d);
        build("<form method=post action=/s><button formmethod=bogus name=b value=1>B</button></form>", &d);
        check(!fm_submit(&d, 0, 0, 0, 0, sent, sizeof(sent)) && !strcmp(sent, "https://forms.test/s?b=1"), "a formmethod that is none: GET, the standard's");
        wd_free(&d);
        /* only an input is an image button */
        build("<form action=/s><button type=image name=b value=1>B</button></form>", &d);
        check(!d.controls[0].image && !fm_submit(&d, 0, 0, 3, 4, sent, sizeof(sent)) && !strcmp(sent, "https://forms.test/s?b=1"), "a button of type=image is a button");
        wd_free(&d);
        /* a select elsewhere hidden */
        build("<form id=f action=/s></form><form id=g action=/t><div hidden><select form=f name=s><option>One</select></div></form>", &d);
        check(d.forms[0].too_much && d.forms[1].too_much, "what a form= sends elsewhere under the hidden attribute: which it is, is not looked at");
        wd_free(&d);
        /* controls under the hidden attribute have no box */
        build("<form hidden id=f action=/s><input name=a value=1></form><button form=f name=b value=2>Go</button>", &d);
        check(d.form_count == 1 && d.control_count == 2 && d.controls[0].form == 0 && d.controls[0].run == -1 && d.controls[1].run >= 0, "a hidden form, a button of it elsewhere");
        check(!fm_submit(&d, 0, 1, 0, 0, sent, sizeof(sent)) && !strcmp(sent, "https://forms.test/s?a=1&b=2"), "which sends it");
        wd_free(&d);
        /* the label of an option */
        build("<form><select name=s><option label='  Shown  here ' value=1><option label='' value=2>Text<option>Three</select><select name=t><option value=9 selected label='Very long words'></select></form>", &d);
        check(!strcmp(d.options[0].label, "Shown here") && !strcmp(d.options[0].value, "1"), "an option's label attribute is its label");
        check(!strcmp(d.options[1].label, "Text"), "an empty one is none");
        {
            size_t n;
            char stars[64];
            const char *t = fm_shown(&d, &d.controls[1], stars, &n);
            check(n == 15 && !memcmp(t, "Very long words", n), "its select shows it");
            check(d.runs[d.controls[1].run].len == 15, "and is as wide as it");
        }
        wd_free(&d);
        build("<form><select name=s><option label=A value=1><option value=2>Two</select></form>", &d);
        check(!strcmp(d.options[1].label, "Two"), "a label is its own option's");
        check(d.runs[d.controls[0].run].len == 1, "the box's text is that of the option it shows");
        wd_free(&d);
        build("<form><select name=s><option value=1>Shown<option value=2 label=Other>Text</select></form>", &d);
        check(d.runs[d.controls[0].run].len == 5, "not of one after it");
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
static unsigned int screen[W * H], screen2[W * H];

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

static int all_zero(const char *s, size_t n) {
    size_t i;
    for(i = 0; i < n; i++) if(s[i]) return 0;
    return 1;
}

/* A block as free takes it, when it is the one watched */
static const void *watched;
static size_t watched_n;
static int watched_gone, watched_clean;

static void on_free(void *p) {
    if(p != watched) return;
    watched_gone = 1;
    watched_clean = all_zero(p, watched_n);
}

static void watch(const char *block, size_t n) {
    watched = block;
    watched_n = n;
    watched_gone = watched_clean = 0;
    ut_alloc_on_free = on_free;
}

/* Editing, byte by byte: the text of these is of one, two, three and four bytes a character */
#define MIXED "a\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80z"

static void editing(void) {
    struct wb_page *p;
    struct wd_control *c;
    size_t n;
    char stars[64];
    const char *s;
    int line, col, i, x, y;

    /* by the keys */
    p = open_page("https://forms.test/mb", "<form><input name=a size=20 value='" MIXED "'><textarea name=t rows=4 cols=3>" MIXED "</textarea>"
                                           "<input name=r readonly value='" MIXED "'></form>");
    c = &p->doc.controls[0];
    check(c->len == 11 && c->caret == 11, "a text of 11 bytes, the caret at its end");
    click_control(p, 0);
    wb_key(&b, 0x105);
    check(c->caret == 11, "End");
    wb_key(&b, 0x102);
    check(c->caret == 10, "Left: one byte");
    wb_key(&b, 0x102);
    check(c->caret == 6, "four");
    wb_key(&b, 0x102);
    check(c->caret == 3, "three");
    wb_key(&b, 0x102);
    check(c->caret == 1, "two");
    wb_key(&b, 0x102);
    check(c->caret == 0, "one");
    check(!wb_key(&b, 0x102) && c->caret == 0, "the start");
    wb_key(&b, 0x103);
    check(c->caret == 1, "Right: one");
    wb_key(&b, 0x103);
    check(c->caret == 3, "two");
    wb_key(&b, 0x103);
    check(c->caret == 6, "three");
    wb_key(&b, 0x103);
    check(c->caret == 10, "four");
    wb_key(&b, 0x103);
    check(c->caret == 11, "one");
    check(!wb_key(&b, 0x103) && c->caret == 11, "the end");
    wb_key(&b, 0x102);
    wb_key(&b, 127);
    check(c->len == 7 && c->caret == 6 && !strcmp(c->value, "a\xc3\xa9\xe2\x82\xacz"), "Backspace: a character of four bytes");
    wb_key(&b, 127);
    check(c->len == 4 && c->caret == 3 && !strcmp(c->value, "a\xc3\xa9z"), "of three");
    wb_key(&b, 0x104);
    wb_key(&b, 0x108);
    check(c->len == 3 && !c->caret && !strcmp(c->value, "\xc3\xa9z"), "Delete: one");
    wb_key(&b, 0x108);
    check(c->len == 1 && !c->caret && !strcmp(c->value, "z"), "of two");

    /* read-only */
    c = &p->doc.controls[2];
    click_control(p, 2);
    wb_key(&b, 0x104);
    check(!wb_key(&b, 0x108) && c->len == 11, "Delete in a read-only field");
    wb_key(&b, 0x105);
    check(!wb_key(&b, 127) && c->len == 11, "Backspace");
    check(!wb_key(&b, 'x') && c->len == 11 && !strcmp(c->value, MIXED), "typing");
    wb_key(&b, 27);

    /* an area of three columns: a line is three characters, whatever their bytes */
    c = &p->doc.controls[1];
    fm_where(c, 3, &line, &col);
    check(!line && col == 2, "the third character of a line");
    fm_where(c, 6, &line, &col);
    check(line == 1 && !col, "its fourth starts the next line, at its first byte");
    fm_where(c, 10, &line, &col);
    check(line == 1 && col == 1, "past a character of four bytes: one column");
    fm_where(c, 11, &line, &col);
    check(line == 1 && col == 2, "and the text's end");
    check(fm_at(c, 0, 0) == 0 && fm_at(c, 0, 1) == 1 && fm_at(c, 0, 2) == 3, "bytes of a line's columns");
    check(fm_at(c, 0, 1 << 30) == 6 && fm_at(c, 1, 0) == 6 && fm_at(c, 1, 1) == 10, "a line's end, the next one's start, of three bytes before it");
    check(fm_at(c, 5, 0) == 11, "a line past the last: the end");
    click_control(p, 1);
    wb_key(&b, 0x104);
    wb_key(&b, 0x100);
    wb_key(&b, 0x105);
    check(c->caret == 3, "End on a full line stays on it, before its last character");
    fm_where(c, c->caret, &line, &col);
    check(!line && col == 2, "(there)");
    wb_key(&b, 0x101);
    check(c->caret == 11, "Down: the column kept");
    wb_key(&b, 0x100);
    check(c->caret == 3, "Up");
    wb_key(&b, 0x105);
    wb_key(&b, 0x101);
    wb_key(&b, 0x105);
    check(c->caret == 11, "End on the last line: the text's end");
    wb_key(&b, 0x104);
    check(c->caret == 6, "Home on it: its own start");
    wb_key(&b, 27);
    /* a character of several bytes that ends a full line: what comes after all of it says what happens */
    p = open_page("https://forms.test/mb01", "<form><textarea name=t rows=2 cols=2>a\xe2\x82\xac\xc3\xa9</textarea><textarea name=u rows=2 cols=2>a\xe2\x82\xac\nb</textarea><textarea name=v rows=2 cols=2>a\xe2\x82\xac</textarea></form>");
    c = &p->doc.controls[0];
    fm_where(c, 4, &line, &col);
    check(line == 1 && !col, "the third character starts a line");
    c = &p->doc.controls[1];
    fm_where(c, 4, &line, &col);
    check(!line && col == 2, "a line feed after it: the line it ends, no empty one");
    c = &p->doc.controls[2];
    fm_where(c, c->len, &line, &col);
    check(!line && col == 2, "the text's end after it: the same");
    /* Up and Down past a full line's end stay on the line they go to */
    p = open_page("https://forms.test/mb02", "<form><textarea name=t rows=4 cols=3>abc\nabcdef</textarea><textarea name=u rows=4 cols=3>abcdefghi</textarea></form>");
    c = &p->doc.controls[0];
    click_control(p, 0);
    wb_key(&b, 0x100);
    wb_key(&b, 0x100);
    wb_key(&b, 0x100);
    wb_key(&b, 0x105);
    check(c->caret == 3, "End on a full line before a line feed: its end, which is a place of its own");
    {
        size_t before = c->caret;
        wb_key(&b, 0x101);
        fm_where(c, c->caret, &line, &col);
        check(line == 1 && col == 2, "Down: the next line, not the one after");
        (void)before;
    }
    wb_key(&b, 27);
    c = &p->doc.controls[1];
    click_control(p, 1);
    wb_key(&b, 9 * 0 + 0x105);
    for(i = 0; i < 4; i++) wb_key(&b, 0x101);
    wb_key(&b, 0x105);
    check(c->caret == 9, "the end of a text that fills its last line");
    wb_key(&b, 0x100);
    fm_where(c, c->caret, &line, &col);
    check(line == 1 && col == 2, "Up: the line before, not the same one");
    wb_key(&b, 27);
    /* a text that just fills a line is on it */
    p = open_page("https://forms.test/mb1", "<form><textarea name=t rows=2 cols=3>abc</textarea></form>");
    c = &p->doc.controls[0];
    fm_where(c, 3, &line, &col);
    check(!line && col == 3, "a full last line: the caret at its end, not on one more");
    /* lines end at a line feed too, and a full line before it has no empty one after it */
    p = open_page("https://forms.test/mb2", "<form><textarea name=t rows=3 cols=2>ab\ncd\n\nef</textarea></form>");
    c = &p->doc.controls[0];
    fm_where(c, 3, &line, &col);
    check(line == 1 && !col, "a full line and its line feed: one line");
    check(fm_at(c, 2, 0) == 6 && fm_at(c, 3, 0) == 7, "an empty line");
    check(fm_at(c, 0, 1 << 30) == 2 && fm_at(c, 2, 1 << 30) == 6, "ends at the line feed");
    /* a text area with cols and no rows has the box of one with neither: no line is wider than it */
    p = open_page("https://forms.test/mb3", "<form><textarea name=t cols=80>aaaaaaaaaaaaaaaaaaaaaaaaaa</textarea></form>");
    c = &p->doc.controls[0];
    fm_where(c, 20, &line, &col);
    check(line == 1 && !col && p->lay.items[wl_control_item(&p->lay, 0)].w < 40 * 8 + 8, "a text area's lines fit its box");

    /* a click past a full line's end stays on it */
    p = open_page("https://forms.test/mb21", "<form><textarea name=t rows=3 cols=4>abcdefghij</textarea></form>");
    c = &p->doc.controls[0];
    {
        const struct wl_item *it = &p->lay.items[wl_control_item(&p->lay, 0)];
        click(it->x + 4 + 3 * 8 + 6, it->y + WB_TOOLBAR + 4 + 2);
        fm_where(c, c->caret, &line, &col);
        check(!line && col == 3, "at its last character");
    }
    wb_key(&b, 27);
    /* a person's way up an area, and Reset */
    p = open_page("https://forms.test/mb31", "<form><textarea name=t rows=2 cols=4>a\nb\nc\nd\ne\nf</textarea><input name=a size=4 value='a long value'><input type=reset></form>");
    c = &p->doc.controls[0];
    click_control(p, 0);
    wb_key(&b, 0x105);
    for(i = 0; i < 6; i++) wb_key(&b, 0x101);
    wb_paint(&b, screen, W);
    check(c->top == 4, "the last lines in view");
    for(i = 0; i < 5; i++) wb_key(&b, 0x100);
    wb_paint(&b, screen, W);
    check(c->top == 0, "the first again");
    for(i = 0; i < 6; i++) wb_key(&b, 0x101);
    wb_paint(&b, screen, W);
    wb_key(&b, 9);
    wb_key(&b, 0x105);
    wb_paint(&b, screen, W);
    check(p->doc.controls[1].first > 0, "a text shown from further on");
    click_control(p, 2);
    check(!c->top && !p->doc.controls[1].first, "Reset: from their starts");
    check(c->caret == c->len && p->doc.controls[1].caret == p->doc.controls[1].len, "and the caret in what is there");

    /* the text of a short field, from where it is shown */
    p = open_page("https://forms.test/mb4", "<form><input name=a size=4 value='\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9'></form>");
    c = &p->doc.controls[0];
    click_control(p, 0);
    wb_key(&b, 0x105);
    wb_paint(&b, screen, W);
    check(c->first == 6, "two-byte characters: four cells hold three before the caret");
    check(fm_byte_at(c, 1) == 8 && fm_byte_at(c, 2) == 10 && fm_byte_at(c, 99) == 12, "a cell's byte is a character's start");
    /* the caret by the pointer */
    {
        long i0 = wl_control_item(&p->lay, 0);
        x = p->lay.items[i0].x + 4 + 8 + 1;
        y = p->lay.items[i0].y + 4 + WB_TOOLBAR;
        click(x, y);
        check(c->caret == 8, "a click: the caret at that character");
        wb_key(&b, 'x');
        check(c->len == 13 && !memcmp(c->value + 8, "x\xc3\xa9", 3), "where typing goes");
    }
    /* Backspace, with no paint between: what a field shows stays in it */
    p = open_page("https://forms.test/mb5", "<form><input name=a size=4></form>");
    c = &p->doc.controls[0];
    click_control(p, 0);
    for(i = 0; i < 30; i++) wb_key(&b, 'a');
    wb_paint(&b, screen, W);
    check(c->first > 20, "the end of a long text in view");
    wb_key(&b, 0x104);
    check(!c->first, "Home: from its start at once");
    wb_key(&b, 0x105);
    wb_paint(&b, screen, W);
    for(i = 0; i < 30; i++) wb_key(&b, 127);
    check(c->len == 0 && c->first == 0, "emptied by Backspace, with no paint between: it shows from its end");
    s = fm_shown(&p->doc, c, stars, &n);
    check(n == 0 && s == c->value, "nothing");
    for(i = 0; i < 10; i++) wb_key(&b, 'b');
    wb_paint(&b, screen, W);
    check(c->first > 0, "typed again");
    /* whatever first is, what is read stays in the text */
    c->first = c->len + 5;
    s = fm_shown(&p->doc, c, stars, &n);
    check(n == 0 && s == c->value + c->len, "shown from past its end: nothing");
    check(fm_byte_at(c, 3) == c->len, "a cell past its end: its end");
    c->first = 0;

    /* a field's limit, and what it takes */
    p = open_page("https://forms.test/mb6", "<form><input name=a maxlength=2 value='\xc3\xa9'><input name=b></form>");
    c = &p->doc.controls[0];
    click_control(p, 0);
    wb_key(&b, 0x105);
    type("xy");
    check(c->len == 3 && !strcmp(c->value, "\xc3\xa9x"), "maxlength counts characters");
    c = &p->doc.controls[1];
    wb_key(&b, 9);
    for(i = 0; i < (int)WD_VALUE_MAX + 50; i++) wb_key(&b, 'q');
    check(c->len == WD_VALUE_MAX && c->cap == WD_VALUE_MAX + 1 && !c->value[c->len], "a field holds what it may, and stops");
    wb_key(&b, 27);

    /* no memory to grow a field, or for what a submit needs: it stays as it was */
    p = open_page("https://forms.test/mb61", "<form action=/m><input name=a value=ab></form>");
    c = &p->doc.controls[0];
    click_control(p, 0);
    wb_key(&b, 0x105);
    ut_alloc_fail_after = 0;
    check(!wb_key(&b, 'x'), "a key with no memory for it");
    ut_alloc_fail_after = -1;
    check(c->len == 2 && !strcmp(c->value, "ab"), "the text as it was");
    check(wb_key(&b, 'x') && !strcmp(c->value, "abx"), "and the next goes in");
    ut_alloc_fail_after = 0;
    wb_key(&b, 13);
    ut_alloc_fail_after = -1;
    check(!b.want && strstr(b.status, "out of memory") && strstr(b.status, "nothing was sent"), "a submit with no memory: nothing goes, and it says so");
    wb_key(&b, 27);

    /* a password: more stars than are shown, and no old block keeps it */
    p = open_page("https://forms.test/mb7", "<form><input type=password name=p value=preset></form>");
    c = &p->doc.controls[0];
    click_control(p, 0);
    wb_key(&b, 0x105);
    for(i = 0; i < 200; i++) wb_key(&b, 's');
    s = fm_shown(&p->doc, c, stars, &n);
    check(n == 64 && s == stars && stars[63] == '*', "more than 64 characters: 64 stars");
    wb_paint(&b, screen, W);
    {
        int grew = 0, wiped = 1;
        for(i = 0; i < 2000; i++) {
            const char *old = c->value;
            watch(old, c->cap);
            wb_key(&b, 's');
            if(c->value != old) {
                grew++;
                if(!watched_gone || !watched_clean) wiped = 0;
            }
        }
        ut_alloc_on_free = NULL;
        check(grew == 3 && wiped, "as it grows, each old block is emptied before it goes");
    }
    {
        const char *value = c->value, *dflt = c->dflt;
        size_t cap = c->cap, dn = strlen(c->dflt);
        check(dn == 6, "what the page set in it stays until the page is left");
        watch(value, cap);
        open_page("https://forms.test/mb8", "<p>Next");
        check(all_zero(value, cap) && all_zero(dflt, dn), "left: emptied, in place");
        ut_alloc_on_free = NULL;
    }
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
    check(!strcmp(b.addr, "https://forms.test/s?q=a+xyz"), "the address field says where it goes");
    check(asks("https://forms.test/s?q=a+xyz"), "Enter: its form goes, by its button, its address in the field");
    check(b.focus == WB_FOCUS_PAGE && b.field == -1, "the keys are the page's");

    /* Escape, and the page's keys */
    click_control(p, 0);
    wb_key(&b, 27);
    check(b.focus == WB_FOCUS_PAGE && b.field == -1, "Escape: the keys are the page's");
    click_control(p, 0);
    wb_key(&b, 12);
    check(b.focus == WB_FOCUS_ADDR, "Ctrl+L still goes to the address");
    wb_key(&b, 27);

    /* Tab from a link to a field: the status bar no longer says the link's address */
    {
        struct wb_page *t = open_page("https://forms.test/a2", "<a href=/there>a link</a><form><input name=q></form>");
        wb_key(&b, 9);
        check(b.tab_link == 0 && strstr(b.status, "/there"), "a link's address");
        wb_key(&b, 9);
        check(b.field == 0 && !strstr(b.status, "/there"), "gone at the field");
        wb_key(&b, 27);
        (void)t;
        p = open_page("https://forms.test/a", "<form action=/s><input name=q value='a xyz'><input type=submit></form>");
    }
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
        check(q->doc.controls[1].len == 0 && all_zero(q->doc.controls[1].value, q->doc.controls[1].cap) && !q->doc.controls[1].caret && !q->doc.controls[1].first, "a page left keeps no password, in any byte");
        check(!q->doc.controls[1].dflt[0], "nor what the page set in it");
        check(strlen(q->doc.controls[0].value) == 2, "its other fields stay");
        wb_key(&b, 127);
        wb_cancel(&b);
        wb_paint(&b, screen, W);
        check(wb_current(&b) == q && q->doc.controls[1].len == 0, "back: an empty field");
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
    /* where the caret stands, on the screen */
    p = open_page("https://forms.test/q0", "<form><input name=a size=10 value=abcd><textarea name=t rows=2 cols=5>ab\ncd</textarea></form>");
    {
        const struct wl_item *it = &p->lay.items[wl_control_item(&p->lay, 0)];
        click_control(p, 0);
        wb_key(&b, 0x104);
        wb_key(&b, 0x103);
        wb_key(&b, 0x103);
        wb_paint(&b, screen, W);
        check(screen[(it->y + WB_TOOLBAR + it->h / 2) * W + it->x + 4 + 2 * 8] == 0x153d49 && screen[(it->y + WB_TOOLBAR + it->h / 2) * W + it->x + 4 + 2 * 8 - 1] == 0x153d49, "a text field's caret, before its third character");
        it = &p->lay.items[wl_control_item(&p->lay, 1)];
        wb_key(&b, 9);
        wb_paint(&b, screen, W);
        check(screen[(it->y + WB_TOOLBAR + 4 + 16 + 8) * W + it->x + 4 + 2 * 8] == 0x153d49, "an area's, at the end of its second line");
        wb_key(&b, 27);
    }
    /* what a password shows: as many stars as it has */
    p = open_page("https://forms.test/q1", "<form><input type=password name=p size=10></form>");
    click_control(p, 0);
    type("abc");
    wb_key(&b, 27);
    wb_paint(&b, screen, W);
    {
        const struct wl_item *it = &p->lay.items[wl_control_item(&p->lay, 0)];
        check(has_color(b.theme.field_fg, it->x + 4 + 2 * 8, it->y + WB_TOOLBAR + 2, 8, it->h - 4), "a third star");
        check(!has_color(b.theme.field_fg, it->x + 4 + 3 * 8, it->y + WB_TOOLBAR + 2, 8, it->h - 4), "and no fourth");
    }
    /* Shift+Enter is Enter but in find */
    p = open_page("https://forms.test/q2", "<form action=/v><input name=a value=1></form><textarea name=t></textarea><a href=/l>a link</a>");
    click_control(p, 0);
    wb_key(&b, WB_KEY_SHIFT_ENTER);
    check(asks("https://forms.test/v?a=1"), "in a field: it submits");
    click_control(p, 1);
    wb_key(&b, 'x');
    wb_key(&b, WB_KEY_SHIFT_ENTER);
    wb_key(&b, 'y');
    check(!strcmp(p->doc.controls[1].value, "x\ny"), "in a text area: a line");
    wb_key(&b, 27);
    wb_key(&b, 9);
    wb_key(&b, 9);
    wb_key(&b, 9);
    check(b.tab_link == 0, "on a link");
    wb_key(&b, WB_KEY_SHIFT_ENTER);
    check(asks("https://forms.test/l"), "it follows it");
    wb_key(&b, 12);
    wb_key(&b, 'q');
    wb_key(&b, WB_KEY_SHIFT_ENTER);
    check(b.want && !strncmp(b.req.url, "https://q", 9), "in the address field: it goes");
    wb_take(&b, (struct wb_request[1]){{0}});
    wb_stop(&b);
    wb_cancel(&b);
    /* the page shows what was typed, in the field's box */
    p = open_page("https://forms.test/q", "<form><input name=a placeholder=Words></form>");
    {
        const struct wl_item *it = &p->lay.items[wl_control_item(&p->lay, 0)];
        check(has_color(b.theme.placeholder, it->x + 3, it->y + WB_TOOLBAR + 2, it->w - 6, it->h - 4), "an empty field shows its placeholder");
    }
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
    /* Enter on a button is that button */
    p = open_page("https://forms.test/z", "<form action=/z><input type=submit name=one value=1><input type=submit name=two value=2></form>");
    wb_key(&b, 9);
    wb_key(&b, 9);
    check(b.field == 1, "the second button");
    wb_key(&b, 13);
    check(asks("https://forms.test/z?two=2"), "Enter: it, not the form's first");
    p = open_page("https://forms.test/z2", "<form action=/z><input name=a value=x><input type=reset><input type=submit></form>");
    click_control(p, 0);
    wb_key(&b, 0x105);
    type("yy");
    wb_key(&b, 9);
    check(b.field == 1, "a reset button");
    wb_key(&b, 13);
    check(!b.want && !strcmp(p->doc.controls[0].value, "x"), "Enter: it resets, and nothing goes");
    /* the list's edge */
    p = open_page("https://forms.test/z3", "<form><select name=s multiple><option>A<option>B</select></form>");
    click_control(p, 0);
    check(b.list == 0, "a list");
    {
        long i = wl_control_item(&p->lay, 0);
        int ly = p->lay.items[i].y + p->lay.items[i].h + WB_TOOLBAR - p->scroll;
        click(p->lay.items[i].x + 8, ly + 2 * (16 + 2) + 1);
        check(b.list == -1 && b.list_at == 0 && fm_chosen(&p->doc, 0) == -1, "a press on its frame, under its last option: no option, and it closes");
    }
    wb_key(&b, 27);
    /* the caret in a text area, by the pointer */
    p = open_page("https://forms.test/z4", "<form><textarea name=t rows=3 cols=10>abcdefghijklmn</textarea></form>");
    {
        long i = wl_control_item(&p->lay, 0);
        const struct wl_item *it = &p->lay.items[i];
        click(it->x + 4 + 2 * 8 + 1, it->y + WB_TOOLBAR + 4 + 1 * 16 + 2);
        check(p->doc.controls[0].caret == 12, "a click on its second line");
    }
    wb_key(&b, 27);
    /* a long password, left: nothing of where it was shown from */
    p = open_page("https://forms.test/z5", "<form><input type=password size=4 name=p></form>");
    click_control(p, 0);
    type("a long enough password to scroll");
    wb_paint(&b, screen, W);
    check(p->doc.controls[0].first > 0, "a password longer than its field");
    {
        struct wb_page *q = p;
        open_page("https://forms.test/z6", "<p>Next");
        check(!q->doc.controls[0].first && !q->doc.controls[0].len, "left: shown from its start again");
    }
    /* an address too long: the browser's limit on an address, under what fits the form's buffer */
    {
        static char big[6000];
        char *e = big;
        int i;
        e += snprintf(e, 40, "<form action=/s>");
        for(i = 0; i < 145; i++) e += snprintf(e, 64, "<input name=aaaaa value=bbbbbbb>");
        snprintf(e, 32, "<input type=submit>");
        p = open_page("https://forms.test/z7", big);
        check(!fm_submit(&p->doc, 0, -1, 0, 0, sent, WB_ADDR_MAX) && strlen(sent) > UT_URL_MAX, "(an address past the one, within the other)");
        wb_key(&b, 9);
        wb_key(&b, 13);
        check(!b.want && strstr(b.status, "do not fit") && strstr(b.status, "nothing was sent"), "fields past an address's limit: nothing goes");
    }
    /* emptied by a person: not what the page had */
    p = open_page("https://forms.test/r", "<form><input name=a value=abc></form>");
    click_control(p, 0);
    wb_key(&b, 0x105);
    wb_key(&b, 127);
    wb_key(&b, 127);
    wb_key(&b, 127);
    wb_key(&b, 27);
    wb_paint(&b, screen, W);
    {
        const struct wl_item *it = &p->lay.items[wl_control_item(&p->lay, 0)];
        check(!has_color(b.theme.field_fg, it->x + 2, it->y + WB_TOOLBAR + 2, it->w - 4, it->h - 4), "a field emptied by keys shows nothing");
    }
    /* what is typed shows at once: a paint leaves nothing for the next one to change */
    p = open_page("https://forms.test/s", "<form><input name=a size=4><textarea name=t rows=2 cols=4></textarea></form>");
    click_control(p, 0);
    type("abcdefghij");
    wb_paint(&b, screen, W);
    memcpy(screen2, screen, sizeof(screen));
    wb_paint(&b, screen, W);
    check(!memcmp(screen, screen2, sizeof(screen)), "a short field's text is as typed at the first paint");
    wb_key(&b, 9);
    type("abcdefghijklmnopqrstuvwxyz");
    wb_paint(&b, screen, W);
    memcpy(screen2, screen, sizeof(screen));
    wb_paint(&b, screen, W);
    check(!memcmp(screen, screen2, sizeof(screen)) && p->doc.controls[1].top > 0, "an area's lines too");
    /* an image button, where it was pressed */
    p = open_page("https://forms.test/t", "<form action=/i><input type=image name=pic alt=Go></form>");
    {
        const struct wl_item *it = &p->lay.items[wl_control_item(&p->lay, 0)];
        char want[96];
        snprintf(want, sizeof(want), "https://forms.test/i?pic.x=%d&pic.y=%d", it->w / 2, it->h / 2);
        click_control(p, 0);
        check(asks(want), "a click on an image button: that place, in the button");
    }
    /* a disabled option is not chosen, by key or by the pointer */
    p = open_page("https://forms.test/u", "<form><select name=s><option>One<option disabled>Two<option>Three</select></form>");
    wb_key(&b, 9);
    wb_key(&b, ' ');
    wb_key(&b, 0x101);
    wb_key(&b, 13);
    check(b.list == -1 && fm_chosen(&p->doc, 0) == 0, "Enter on a disabled option");
    click_control(p, 0);
    check(b.list == 0, "its list");
    {
        long i = wl_control_item(&p->lay, 0);
        click(p->lay.items[i].x + 8, p->lay.items[i].y + p->lay.items[i].h + WB_TOOLBAR - p->scroll + 1 + 1 * (16 + 2) + 4);
        check(b.list == -1 && fm_chosen(&p->doc, 0) == 0, "a click on one");
    }
    wb_key(&b, 27);
    p = open_page("https://forms.test/v", "<form><select name=s multiple><option disabled>One<option>Two</select></form>");
    wb_key(&b, 9);
    wb_key(&b, ' ');
    wb_key(&b, ' ');
    check(fm_chosen(&p->doc, 0) == -1, "Space on one in a multiple");
    wb_key(&b, 27);
    wb_key(&b, 27);
    /* radio buttons: a group is of one form */
    p = open_page("https://forms.test/w", "<form><input type=radio name=r value=1 checked></form><form><input type=radio name=r value=2 checked><input type=radio name=r value=3></form>");
    check(p->doc.controls[0].checked && p->doc.controls[1].checked, "one name in two forms: two groups");
    click_control(p, 2);
    check(p->doc.controls[0].checked && !p->doc.controls[1].checked && p->doc.controls[2].checked, "a click: only in its own");
    /* Reset gives back the one the page checked */
    p = open_page("https://forms.test/x", "<form><input type=radio name=r value=1 checked><input type=radio name=r value=2 checked><input type=radio name=r value=3><input type=reset></form>");
    click_control(p, 2);
    check(p->doc.controls[2].checked && !p->doc.controls[1].checked, "another chosen");
    click_control(p, 3);
    check(!p->doc.controls[0].checked && p->doc.controls[1].checked && !p->doc.controls[2].checked, "Reset: the last the page checked, and that alone");
    /* a page arrives while one is in use */
    {
        static const char same[] = "<form action=/s><select name=s><option>A<option>B</select><input name=a><input type=submit></form>";
        struct wb_request q;
        p = open_page("https://forms.test/y", same);
        wb_go(&b, "https://forms.test/y2");
        wb_take(&b, &q);
        click_control(p, 1);
        check(b.field == 1 && b.focus == WB_FOCUS_FIELD, "a person goes to a field meanwhile");
        wb_loaded(&b, page_at(q.url, same, 0), 0);
        wb_cancel(&b);
        wb_paint(&b, screen, W);
        p = wb_current(&b);
        check(b.list == -1 && b.field == -1 && b.focus == WB_FOCUS_PAGE, "the page arrives: the keys are the page's");
        wb_key(&b, 'x');
        check(!p->doc.controls[1].len && !b.want, "and what is typed then goes into no field");
        click_control(p, 0);
        check(b.list == 0, "a list open");
        wb_go(&b, "https://forms.test/y3");
        wb_take(&b, &q);
        wb_loaded(&b, page_at(q.url, same, 0), 0);
        wb_cancel(&b);
        wb_paint(&b, screen, W);
        p = wb_current(&b);
        check(b.list == -1, "it closes with its page");
        spot(p, 2, &x, &y);
        wb_pointer(&b, x, y, 0);
        wb_pointer(&b, x, y, 1);
        check(b.press_field == 2, "a button held");
        wb_go(&b, "https://forms.test/y4");
        wb_take(&b, &q);
        wb_loaded(&b, page_at(q.url, same, 0), 0);
        wb_cancel(&b);
        wb_paint(&b, screen, W);
        check(b.press_field == -1, "as a page arrives: let go of");
        wb_pointer(&b, x, y, 0);
        check(!b.want, "a button of the new page in the same place was not pressed");
    }

    editing();
    wb_free(&b);

    /* a page's end: a password's blocks are empty as free takes them */
    {
        struct wd_document d;
        check(!build("<form><input type=password name=p value=preset></form>", &d), "a password's page");
        watch(d.controls[0].value, d.controls[0].cap);
        wd_free(&d);
        check(watched_gone && watched_clean, "its value goes empty");
        build("<form><input type=password name=p value=preset></form>", &d);
        watch(d.controls[0].dflt, strlen(d.controls[0].dflt));
        wd_free(&d);
        check(watched_gone && watched_clean, "what the page set in it too");
        build("<form><input name=p value=preset></form>", &d);
        watch(d.controls[0].value, d.controls[0].cap);
        wd_free(&d);
        check(watched_gone && !watched_clean, "(the test sees a text field's, which is no password)");
        ut_alloc_on_free = NULL;
    }
}

/**** display: none (11/02/26, the integration of milestones 2 and 3) ****/

/* What display: none hides has no box, and is still its form's, as in a browser: it goes with it. Unlike what is
   disabled, and unlike what the hidden attribute holds, which this browser follows only so far (above). */
static void hidden_by_css(void) {
    struct wd_document d;
    size_t i;
    int boxes = 0;
    sends("<style>.h{display:none}</style><form action=/s><input class=h name=a value=1><input name=b value=2></form>",
          "https://forms.test/s?a=1&b=2", "an input display: none hides goes");
    sends("<form action=/s><input style='display:none' type=checkbox name=c checked>"
          "<input type=hidden name=t value=x style=display:none><input type=radio name=r value=v checked style=display:none></form>",
          "https://forms.test/s?c=on&t=x&r=v", "by its style attribute: a checkbox, a hidden field, a radio button");
    sends("<form action=/s><input hidden name=a value=1><input type=checkbox hidden name=c checked></form>",
          "https://forms.test/s?a=1&c=on", "an input with the hidden attribute, which is display: none");
    sends("<style>div{display:none}</style><form action=/s><div><input name=a value=1><textarea name=t>words</textarea>"
          "<select name=s><option>One<option selected>Two</select></div><input name=z value=9></form>",
          "https://forms.test/s?a=1&t=words&s=Two&z=9", "what is in an element display: none hides: fields, a text area, a select");
    sends("<form action=/s><select name=s style=display:none><option value=1>One<option>Two &amp; more</select>"
          "<select name=m multiple style=display:none><option selected>A<option>B<option selected>C</select>"
          "<select name=f style=display:none><option disabled>No<option>Yes</select></form>",
          "https://forms.test/s?s=1&m=A&m=C&f=Yes", "a select display: none hides: its options, chosen as the page has them");
    sends("<style>.h{display:none}</style><form action=/s><select class=h name=s><option>One</option>"
          "<option selected>Two words</option></select><input name=after value=3></form>",
          "https://forms.test/s?s=Two+words&after=3", "an option's text is its value there too, and the form goes on after it");
    sends("<form action=/s><fieldset disabled style=display:none><input name=a value=1></fieldset><input name=b value=2></form>",
          "https://forms.test/s?b=2", "a disabled fieldset display: none hides still disables what is in it");
    sends("<form action=/s style=display:none><input name=a value=1></form><input name=b value=2>",
          "https://forms.test/s?a=1", "a form display: none hides is a form, to its end");
    refuses("<form action=/s><div style=display:none><input type=password name=p></div></form>", "password",
            "a password field display: none hides still keeps its form from going");

    /* none of them has a box, and the page goes on after them */
    check(!build("<style>.h{display:none} .h2 select{display:none}</style><form><input class=h name=a><select class=h name=s>"
                 "<option>xyzzy</select><textarea class=h name=t>plugh</textarea><button class=h name=b>B</button>"
                 "<div class=h><input name=c><select name=d><option>quux</select></div>"
                 "<p class=h2>before <select name=e><option>frob</select> after</p><input hidden name=f></form><p>shown", &d),
          "controls display: none hides");
    check(d.control_count == 8, "are all controls");
    for(i = 0; i < d.control_count; i++) boxes += d.controls[i].run >= 0;
    check(boxes == 0, "with no box");
    for(i = 0; i < d.run_count; i++)
        if(d.runs[i].kind != WR_TEXT || (d.runs[i].len == 5 && !memcmp(d.text + d.runs[i].off, "xyzzy", 5))) boxes++;
    check(boxes == 0 && strstr(d.text, "before") && strstr(d.text, "after") && strstr(d.text, "shown") &&
          !strstr(d.text, "quux") && !strstr(d.text, "frob") && !strstr(d.text, "plugh"), "nor a word of theirs on the page, and the rest of it is there");
    check(d.control_count == 8 && d.controls[1].noptions == 1 && d.options[d.controls[1].first_option].on &&
          d.controls[3].role == WC_SUBMIT, "a select's choice, a button's role");
    wd_free(&d);

    /* the same forms with the page's sheet or without: what a person did goes over when a sheet comes (sheets.c) */
    {
        struct wd_document e;
        const char *page = "<form action=/s><div class=h><input name=a><select name=s><option>x<option>y</select>"
                           "<button name=go value=1>Go</button></div><textarea class=h name=t>w</textarea></form>";
        char styled[400];
        snprintf(styled, sizeof(styled), "<style>.h{display:none}</style>%s", page);
        check(!build(page, &d) && !build(styled, &e), "a form with a sheet that hides it and without");
        check(d.control_count == e.control_count && d.option_count == e.option_count && d.form_count == e.form_count,
              "has the same controls either way");
        wd_free(&d);
        wd_free(&e);
    }

    /* a hidden submit button is the form's first: Enter in a field submits by it, as in a browser */
    check(!build("<form action=/s><input name=q value=x><input name=r value=y><input type=submit name=go value=Go style=display:none></form>", &d),
          "a submit button display: none hides");
    check(fm_default_button(&d, 0) == 2, "is the form's default button, so Enter in one of two fields submits");
    check(!fm_submit(&d, 0, fm_default_button(&d, 0), 0, 0, sent, sizeof(sent)) && !strcmp(sent, "https://forms.test/s?q=x&r=y&go=Go"),
          "and it goes as what submitted");
    wd_free(&d);
}

int main(void) {
    model();
    encoding();
    hidden_by_css();
    printf("  display: none: %d builds of the pages sent here with sheets that hide them, %d sending otherwise\n", css_pages, css_differ);
    check(css_pages > 300 && !css_differ, "every page sent here sends the same whatever its CSS hides");
    by_hand();
    check(ut_alloc_live == 0, "nothing held at the end");
    if(failures) {
        printf("WUNDERTOW FORM TESTS FAILED: %d of %d checks\n", failures, checks);
        return 1;
    }
    printf("WUNDERTOW FORM TESTS PASSED: %d checks, 0 failures\n", checks);
    return 0;
}
