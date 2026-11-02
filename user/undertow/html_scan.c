/*
SurfOS Undertow: the HTML scanner, shared by html.c and the browser's window
----------------------
File: html_scan.c  Date: 10/25/26
----------------------
html_scan.h's parts of reading HTML, moved here from html.c (10/11/26-10/15/26) as they were, with the
page's bytes and charset passed in instead of html.c's reader: the elements, the character references,
the charsets, a tag's attributes, the ends of raw text and comments.
*/

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "undertow.h"
#include "html_scan.h"

#define PRESCAN      1024               /* bytes searched for <meta charset> */

/**** elements ****/

static const struct element {
    const char *name;
    unsigned char id;
    unsigned short flags;
} elements[] = {
    { "a", E_A, 0 },
    { "address", E_ADDRESS, F_LINE|F_CLOSEP|F_PUSH|F_SPECIAL },
    { "applet", E_APPLET, F_HIDE|F_SCOPE|F_SPECIAL },
    { "area", E_AREA, F_VOID|F_SPECIAL },
    { "article", E_ARTICLE, F_LINE|F_CLOSEP|F_PUSH|F_SPECIAL },
    { "aside", E_ASIDE, F_LINE|F_CLOSEP|F_PUSH|F_SPECIAL },
    { "b", E_B, F_OUT },
    { "base", E_BASE, F_VOID|F_SPECIAL },
    { "basefont", E_BASEFONT, F_VOID|F_SPECIAL },
    { "bgsound", E_BGSOUND, F_VOID|F_SPECIAL },
    { "big", E_BIG, F_OUT },
    { "blockquote", E_BLOCKQUOTE, F_PARA|F_CLOSEP|F_PUSH|F_SPECIAL|F_OUT },
    { "body", E_BODY, F_SPECIAL|F_OUT },
    { "br", E_BR, F_VOID|F_SPECIAL|F_OUT },
    { "button", E_BUTTON, F_SPECIAL },
    { "caption", E_CAPTION, F_LINE|F_PUSH|F_SCOPE|F_SPECIAL },
    { "center", E_CENTER, F_LINE|F_CLOSEP|F_PUSH|F_SPECIAL|F_OUT },
    { "code", E_CODE, F_OUT },
    { "col", E_COL, F_VOID|F_SPECIAL },
    { "colgroup", E_COLGROUP, F_SPECIAL },
    { "datalist", E_DATALIST, F_HIDE },
    { "dd", E_DD, F_LINE|F_CLOSEP|F_PUSH|F_SPECIAL|F_OUT },
    { "details", E_DETAILS, F_LINE|F_CLOSEP|F_PUSH|F_SPECIAL },
    { "dialog", E_DIALOG, F_LINE|F_CLOSEP|F_PUSH|F_SPECIAL },
    { "dir", E_DIR, F_LIST|F_CLOSEP|F_PUSH|F_SPECIAL },
    { "div", E_DIV, F_LINE|F_CLOSEP|F_PUSH|F_SPECIAL|F_OUT },
    { "dl", E_DL, F_PARA|F_CLOSEP|F_PUSH|F_SPECIAL|F_OUT },
    { "dt", E_DT, F_LINE|F_CLOSEP|F_PUSH|F_SPECIAL|F_OUT },
    { "em", E_EM, F_OUT },
    { "embed", E_EMBED, F_VOID|F_SPECIAL|F_OUT },
    { "fieldset", E_FIELDSET, F_LINE|F_CLOSEP|F_PUSH|F_SPECIAL },
    { "figcaption", E_FIGCAPTION, F_LINE|F_CLOSEP|F_PUSH|F_SPECIAL },
    { "figure", E_FIGURE, F_PARA|F_CLOSEP|F_PUSH|F_SPECIAL },
    { "font", E_FONT, F_OUT },
    { "footer", E_FOOTER, F_LINE|F_CLOSEP|F_PUSH|F_SPECIAL },
    { "form", E_FORM, F_LINE|F_CLOSEP|F_PUSH|F_SPECIAL },
    { "frame", E_FRAME, F_VOID|F_SPECIAL },
    { "frameset", E_FRAMESET, F_SPECIAL },
    { "h1", E_H1, F_PARA|F_CLOSEP|F_PUSH|F_HEAD|F_SPECIAL|F_OUT },
    { "h2", E_H2, F_PARA|F_CLOSEP|F_PUSH|F_HEAD|F_SPECIAL|F_OUT },
    { "h3", E_H3, F_PARA|F_CLOSEP|F_PUSH|F_HEAD|F_SPECIAL|F_OUT },
    { "h4", E_H4, F_PARA|F_CLOSEP|F_PUSH|F_HEAD|F_SPECIAL|F_OUT },
    { "h5", E_H5, F_PARA|F_CLOSEP|F_PUSH|F_HEAD|F_SPECIAL|F_OUT },
    { "h6", E_H6, F_PARA|F_CLOSEP|F_PUSH|F_HEAD|F_SPECIAL|F_OUT },
    { "head", E_HEAD, F_SPECIAL|F_OUT },
    { "header", E_HEADER, F_LINE|F_CLOSEP|F_PUSH|F_SPECIAL },
    { "hgroup", E_HGROUP, F_LINE|F_CLOSEP|F_PUSH|F_SPECIAL },
    { "hr", E_HR, F_VOID|F_CLOSEP|F_SPECIAL|F_OUT },
    { "html", E_HTML, F_SPECIAL },
    { "i", E_I, F_OUT },
    { "iframe", E_IFRAME, F_RAW|F_SPECIAL },
    { "image", E_IMAGE, F_VOID|F_SPECIAL },
    { "img", E_IMG, F_VOID|F_SPECIAL|F_OUT },
    { "input", E_INPUT, F_VOID|F_SPECIAL },
    { "keygen", E_KEYGEN, F_VOID|F_SPECIAL },
    { "legend", E_LEGEND, F_LINE|F_PUSH },
    { "li", E_LI, F_LINE|F_CLOSEP|F_PUSH|F_SPECIAL|F_OUT },
    { "link", E_LINK, F_VOID|F_SPECIAL },
    { "listing", E_LISTING, F_PARA|F_CLOSEP|F_PUSH|F_SPECIAL|F_OUT },
    { "main", E_MAIN, F_LINE|F_CLOSEP|F_PUSH|F_SPECIAL },
    { "marquee", E_MARQUEE, F_SPECIAL },
    { "math", E_MATH, F_HIDE },
    { "menu", E_MENU, F_LIST|F_CLOSEP|F_PUSH|F_SPECIAL|F_OUT },
    { "meta", E_META, F_VOID|F_SPECIAL|F_OUT },
    { "nav", E_NAV, F_LINE|F_CLOSEP|F_PUSH|F_SPECIAL },
    { "nobr", E_NOBR, F_OUT },
    { "noembed", E_NOEMBED, F_RAW|F_SPECIAL },
    { "noframes", E_NOFRAMES, F_LINE|F_PUSH|F_SPECIAL },
    { "noscript", E_NOSCRIPT, F_SPECIAL },
    { "object", E_OBJECT, F_HIDE|F_SCOPE|F_SPECIAL },
    { "ol", E_OL, F_LIST|F_CLOSEP|F_PUSH|F_SPECIAL|F_OUT },
    { "optgroup", E_OPTGROUP, 0 },
    { "option", E_OPTION, 0 },
    { "p", E_P, F_PARA|F_CLOSEP|F_PUSH|F_SPECIAL|F_OUT },
    { "param", E_PARAM, F_VOID|F_SPECIAL },
    { "pre", E_PRE, F_PARA|F_CLOSEP|F_PUSH|F_SPECIAL|F_OUT },
    { "ruby", E_RUBY, F_OUT },
    { "s", E_S, F_OUT },
    { "script", E_SCRIPT, F_RAW|F_SPECIAL },
    { "search", E_SEARCH, F_LINE|F_CLOSEP|F_PUSH|F_SPECIAL },
    { "section", E_SECTION, F_LINE|F_CLOSEP|F_PUSH|F_SPECIAL },
    { "select", E_SELECT, F_SPECIAL },
    { "small", E_SMALL, F_OUT },
    { "source", E_SOURCE, F_VOID|F_SPECIAL },
    { "span", E_SPAN, F_OUT },
    { "strike", E_STRIKE, F_OUT },
    { "strong", E_STRONG, F_OUT },
    { "style", E_STYLE, F_RAW|F_SPECIAL },
    { "sub", E_SUB, F_OUT },
    { "summary", E_SUMMARY, F_LINE|F_CLOSEP|F_PUSH|F_SPECIAL },
    { "sup", E_SUP, F_OUT },
    { "svg", E_SVG, F_HIDE },
    { "table", E_TABLE, F_LINE|F_CLOSEP|F_PUSH|F_SCOPE|F_SPECIAL|F_OUT },
    { "tbody", E_TBODY, F_SPECIAL },
    { "td", E_TD, F_PUSH|F_SCOPE|F_SPECIAL },
    { "template", E_TEMPLATE, F_HIDE|F_SCOPE|F_SPECIAL },
    { "textarea", E_TEXTAREA, F_RCDATA|F_SPECIAL },
    { "tfoot", E_TFOOT, F_SPECIAL },
    { "th", E_TH, F_PUSH|F_SCOPE|F_SPECIAL },
    { "thead", E_THEAD, F_SPECIAL },
    { "title", E_TITLE, F_RCDATA|F_SPECIAL },
    { "tr", E_TR, F_LINE|F_PUSH|F_SPECIAL },
    { "track", E_TRACK, F_VOID|F_SPECIAL },
    { "tt", E_TT, F_OUT },
    { "u", E_U, F_OUT },
    { "ul", E_UL, F_LIST|F_CLOSEP|F_PUSH|F_SPECIAL|F_OUT },
    { "var", E_VAR, F_OUT },
    { "wbr", E_WBR, F_VOID|F_SPECIAL },
    { "xmp", E_XMP, F_PARA|F_CLOSEP|F_RAW|F_SPECIAL }
};

/**** character references ****/

static const struct entity {
    const char *name;
    unsigned int cp;
    int legacy;                         /* also without its semicolon, as browsers have it */
} entities[] = {
    { "AElig", 0xc6, 1 }, { "AMP", 0x26, 1 }, { "Aacute", 0xc1, 1 }, { "Acirc", 0xc2, 1 }, { "Agrave", 0xc0, 1 },
    { "Aring", 0xc5, 1 }, { "Atilde", 0xc3, 1 }, { "Auml", 0xc4, 1 }, { "COPY", 0xa9, 1 }, { "Ccedil", 0xc7, 1 },
    { "Dagger", 0x2021, 0 }, { "ETH", 0xd0, 1 }, { "Eacute", 0xc9, 1 }, { "Ecirc", 0xca, 1 }, { "Egrave", 0xc8, 1 },
    { "Euml", 0xcb, 1 }, { "GT", 0x3e, 1 }, { "Iacute", 0xcd, 1 }, { "Icirc", 0xce, 1 }, { "Igrave", 0xcc, 1 },
    { "Iuml", 0xcf, 1 }, { "LT", 0x3c, 1 }, { "Ntilde", 0xd1, 1 }, { "Oacute", 0xd3, 1 }, { "Ocirc", 0xd4, 1 },
    { "Ograve", 0xd2, 1 }, { "Oslash", 0xd8, 1 }, { "Otilde", 0xd5, 1 }, { "Ouml", 0xd6, 1 },
    { "Prime", 0x2033, 0 }, { "QUOT", 0x22, 1 }, { "REG", 0xae, 1 }, { "THORN", 0xde, 1 }, { "Uacute", 0xda, 1 },
    { "Ucirc", 0xdb, 1 }, { "Ugrave", 0xd9, 1 }, { "Uuml", 0xdc, 1 }, { "Yacute", 0xdd, 1 }, { "aacute", 0xe1, 1 },
    { "acirc", 0xe2, 1 }, { "acute", 0xb4, 1 }, { "aelig", 0xe6, 1 }, { "agrave", 0xe0, 1 }, { "amp", 0x26, 1 },
    { "apos", 0x27, 0 }, { "aring", 0xe5, 1 }, { "atilde", 0xe3, 1 }, { "auml", 0xe4, 1 }, { "bdquo", 0x201e, 0 },
    { "brvbar", 0xa6, 1 }, { "bull", 0x2022, 0 }, { "ccedil", 0xe7, 1 }, { "cedil", 0xb8, 1 }, { "cent", 0xa2, 1 },
    { "copy", 0xa9, 1 }, { "curren", 0xa4, 1 }, { "dagger", 0x2020, 0 }, { "darr", 0x2193, 0 }, { "deg", 0xb0, 1 },
    { "divide", 0xf7, 1 }, { "eacute", 0xe9, 1 }, { "ecirc", 0xea, 1 }, { "egrave", 0xe8, 1 },
    { "emsp", 0x2003, 0 }, { "ensp", 0x2002, 0 }, { "eth", 0xf0, 1 }, { "euml", 0xeb, 1 }, { "euro", 0x20ac, 0 },
    { "frac12", 0xbd, 1 }, { "frac14", 0xbc, 1 }, { "frac34", 0xbe, 1 }, { "ge", 0x2265, 0 }, { "gt", 0x3e, 1 },
    { "harr", 0x2194, 0 }, { "hellip", 0x2026, 0 }, { "iacute", 0xed, 1 }, { "icirc", 0xee, 1 },
    { "iexcl", 0xa1, 1 }, { "igrave", 0xec, 1 }, { "iquest", 0xbf, 1 }, { "iuml", 0xef, 1 }, { "laquo", 0xab, 1 },
    { "larr", 0x2190, 0 }, { "ldquo", 0x201c, 0 }, { "le", 0x2264, 0 }, { "lrm", 0x200e, 0 },
    { "lsaquo", 0x2039, 0 }, { "lsquo", 0x2018, 0 }, { "lt", 0x3c, 1 }, { "macr", 0xaf, 1 }, { "mdash", 0x2014, 0 },
    { "micro", 0xb5, 1 }, { "middot", 0xb7, 1 }, { "minus", 0x2212, 0 }, { "nbsp", 0xa0, 1 },
    { "ndash", 0x2013, 0 }, { "ne", 0x2260, 0 }, { "not", 0xac, 1 }, { "ntilde", 0xf1, 1 }, { "oacute", 0xf3, 1 },
    { "ocirc", 0xf4, 1 }, { "ograve", 0xf2, 1 }, { "ordf", 0xaa, 1 }, { "ordm", 0xba, 1 }, { "oslash", 0xf8, 1 },
    { "otilde", 0xf5, 1 }, { "ouml", 0xf6, 1 }, { "para", 0xb6, 1 }, { "permil", 0x2030, 0 }, { "plusmn", 0xb1, 1 },
    { "pound", 0xa3, 1 }, { "prime", 0x2032, 0 }, { "quot", 0x22, 1 }, { "raquo", 0xbb, 1 }, { "rarr", 0x2192, 0 },
    { "rdquo", 0x201d, 0 }, { "reg", 0xae, 1 }, { "rlm", 0x200f, 0 }, { "rsaquo", 0x203a, 0 },
    { "rsquo", 0x2019, 0 }, { "sbquo", 0x201a, 0 }, { "sect", 0xa7, 1 }, { "shy", 0xad, 1 }, { "sup1", 0xb9, 1 },
    { "sup2", 0xb2, 1 }, { "sup3", 0xb3, 1 }, { "szlig", 0xdf, 1 }, { "thinsp", 0x2009, 0 }, { "thorn", 0xfe, 1 },
    { "times", 0xd7, 1 }, { "trade", 0x2122, 0 }, { "uacute", 0xfa, 1 }, { "uarr", 0x2191, 0 },
    { "ucirc", 0xfb, 1 }, { "ugrave", 0xf9, 1 }, { "uml", 0xa8, 1 }, { "uuml", 0xfc, 1 }, { "yacute", 0xfd, 1 },
    { "yen", 0xa5, 1 }, { "yuml", 0xff, 1 }, { "zwj", 0x200d, 0 }, { "zwnj", 0x200c, 0 }
};

/* 10/15/26 (Undertow review M4): every other name in WHATWG's table that begins with a legacy name, so that
   such a name with its semicolon is never read as the legacy name and the rest (&notin; is U+2209, not U+00AC
   and "in;"). 0: the standard gives two characters; it stays as written, as an unknown name does. */
static const struct entity longer[] = {
    { "centerdot", 0xb7, 0 }, { "copysr", 0x2117, 0 }, { "divideontimes", 0x22c7, 0 }, { "gtcc", 0x2aa7, 0 },
    { "gtcir", 0x2a7a, 0 }, { "gtdot", 0x22d7, 0 }, { "gtlPar", 0x2995, 0 }, { "gtquest", 0x2a7c, 0 },
    { "gtrapprox", 0x2a86, 0 }, { "gtrarr", 0x2978, 0 }, { "gtrdot", 0x22d7, 0 }, { "gtreqless", 0x22db, 0 },
    { "gtreqqless", 0x2a8c, 0 }, { "gtrless", 0x2277, 0 }, { "gtrsim", 0x2273, 0 }, { "ltcc", 0x2aa6, 0 },
    { "ltcir", 0x2a79, 0 }, { "ltdot", 0x22d6, 0 }, { "lthree", 0x22cb, 0 }, { "ltimes", 0x22c9, 0 },
    { "ltlarr", 0x2976, 0 }, { "ltquest", 0x2a7b, 0 }, { "ltrPar", 0x2996, 0 }, { "ltri", 0x25c3, 0 },
    { "ltrie", 0x22b4, 0 }, { "ltrif", 0x25c2, 0 }, { "notin", 0x2209, 0 }, { "notinE", 0, 0 },
    { "notindot", 0, 0 }, { "notinva", 0x2209, 0 }, { "notinvb", 0x22f7, 0 }, { "notinvc", 0x22f6, 0 },
    { "notni", 0x220c, 0 }, { "notniva", 0x220c, 0 }, { "notnivb", 0x22fe, 0 }, { "notnivc", 0x22fd, 0 },
    { "parallel", 0x2225, 0 }, { "timesb", 0x22a0, 0 }, { "timesbar", 0x2a31, 0 }, { "timesd", 0x2a30, 0 }
};

/* windows-1252's 0x80-0x9F; its five holes stay C1 controls, which are never shown */
static const unsigned short w1252[32] = {
    0x20ac, 0x0081, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021, 0x02c6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008d,
    0x017d, 0x008f, 0x0090, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014, 0x02dc, 0x2122, 0x0161, 0x203a,
    0x0153, 0x009d, 0x017e, 0x0178
};

/* The WHATWG labels of the two charsets Undertow reads (and of UTF-16, which a <meta> may name) */
static const char *const utf8_labels[] = {
    "unicode-1-1-utf-8", "unicode11utf8", "unicode20utf8", "utf-8", "utf8", "x-unicode20utf8"
};
static const char *const w1252_labels[] = {
    "ansi_x3.4-1968", "ascii", "cp1252", "cp819", "csisolatin1", "ibm819", "iso-8859-1", "iso-ir-100", "iso8859-1",
    "iso88591", "iso_8859-1", "iso_8859-1:1987", "l1", "latin1", "us-ascii", "windows-1252", "x-cp1252"
};
static const char *const utf16_labels[] = {
    "csunicode", "iso-10646-ucs-2", "ucs-2", "unicode", "unicodefeff", "unicodefffe", "utf-16", "utf-16be", "utf-16le"
};

/**** small helpers ****/

static int ws(unsigned char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r'; }
static int alpha(unsigned char c) { return (c | 0x20) >= 'a' && (c | 0x20) <= 'z'; }
static int digit(unsigned char c) { return c >= '0' && c <= '9'; }
static int alnum(unsigned char c) { return alpha(c) || digit(c); }
static unsigned char lower(unsigned char c) { return c >= 'A' && c <= 'Z' ? (unsigned char)(c + 32) : c; }

static int hexval(unsigned char c) {
    if(digit(c)) return c - '0';
    c |= 0x20;
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

/* s (n bytes) is the lowercase word w in any case */
static int ci_eq(const unsigned char *s, size_t n, const char *w) {
    size_t i;
    for(i = 0; i < n; i++) if(!w[i] || lower(s[i]) != (unsigned char)w[i]) return 0;
    return !w[n];
}

/* s (avail bytes) starts with the lowercase word w in any case */
static int ci_prefix(const unsigned char *s, size_t avail, const char *w) {
    size_t i;
    for(i = 0; w[i]; i++) if(i >= avail || lower(s[i]) != (unsigned char)w[i]) return 0;
    return 1;
}

unsigned int ut_html_flags(int el) {
    return el > 0 && el < E_COUNT ? elements[el - 1].flags : 0;    /* the enum follows the table's order */
}

/* Never shown: ut_unsafe_char()'s (NUL and every control, the bidirectional marks and controls) and the
   characters that are invisible on a console: the soft hyphen (&shy;), the combining grapheme joiner, the
   Mongolian vowel separator, the zero-width space, the word joiner and the invisible operators, and the
   variation selectors. The zero-width non-joiner and joiner stay: scripts and emoji need them. */
int ut_html_hidden(unsigned int cp) {
    return ut_unsafe_char(cp) || cp == 0xad || cp == 0x34f || cp == 0x180e || cp == 0x200b ||
           (cp >= 0x2060 && cp <= 0x2064) || (cp >= 0xfe00 && cp <= 0xfe0f);
}

/* HTML's whitespace, and the line and paragraph separators, which ut_safe_text() makes spaces too */
int ut_html_space(unsigned int cp) {
    return cp == ' ' || cp == '\t' || cp == '\n' || cp == '\f' || cp == '\r' || cp == 0x85 || cp == 0x2028 || cp == 0x2029;
}

/**** the page's characters ****/

/* The character at in[*pos] (before end) in the page's charset; *pos moves past it. CR LF and CR are LF,
   as the HTML standard has it before anything else looks at the text. */
unsigned int ut_html_char(const unsigned char *in, int cs, size_t *pos, size_t end) {
    const unsigned char *s = in + *pos;
    unsigned int cp;
    if(*s < 0x80) {
        *pos += 1;
        if(*s != '\r') return *s;
        if(*pos < end && in[*pos] == '\n') *pos += 1;
        return '\n';
    }
    if(cs == UT_CS_UTF8) {
        *pos += ut_utf8_decode(s, end - *pos, &cp);
        return cp;
    }
    *pos += 1;
    if(cs == UT_CS_1252) return *s < 0xa0 ? w1252[*s - 0x80] : *s;
    return '?';
}

static const struct entity *find_in(const struct entity *table, size_t count, const unsigned char *s, size_t n) {
    size_t lo = 0, hi = count;
    while(lo < hi) {
        size_t mid = (lo + hi) / 2;
        int c = strncmp(table[mid].name, (const char *)s, n);
        if(!c && table[mid].name[n]) c = 1;             /* the table's name goes on: it is the greater */
        if(!c) return &table[mid];
        if(c < 0) lo = mid + 1;
        else hi = mid;
    }
    return NULL;
}

static const struct entity *find_entity(const unsigned char *s, size_t n) {
    return find_in(entities, sizeof(entities) / sizeof(entities[0]), s, n);
}

/* A numeric reference's character, as browsers make it */
static unsigned int numeric(unsigned int v) {
    if(!v || v > 0x10ffff || (v >= 0xd800 && v <= 0xdfff)) return UT_REPLACEMENT;
    if(v >= 0x80 && v <= 0x9f) return w1252[v - 0x80];
    return v;
}

/* A character reference at in[*pos], which is '&' (before end): its character, with *pos past it; or '&'
   alone, with *pos past the '&' only, when it is none: unknown names stay as written. A name without its
   semicolon counts only for the legacy names (Latin-1, amp, lt, gt, quot), and in an attribute only when
   neither '=' nor a letter or digit follows it, as in browsers. 10/15/26 (review M4): that is so also when
   the letters and digits go on to a semicolon but make no name (&notit; is U+00AC and "it;" in text, as
   written in an attribute), as in WHATWG's named character reference state, which takes the longest name
   that matches. Bounded: a name has 32 letters at most. */
unsigned int ut_html_ref(const unsigned char *in, size_t *pos, size_t end, int attr) {
    const unsigned char *s = in;
    const struct entity *e;
    size_t p = *pos + 1, start, k;
    unsigned int v = 0;
    int hex = 0, d;
    if(p < end && s[p] == '#') {
        p++;
        if(p < end && (s[p] == 'x' || s[p] == 'X')) {
            hex = 1;
            p++;
        }
        start = p;
        while(p < end && (d = hex ? hexval(s[p]) : digit(s[p]) ? s[p] - '0' : -1) >= 0) {
            if(v <= 0x10ffff) v = v * (hex ? 16 : 10) + (unsigned int)d;        /* past that it is U+FFFD anyway */
            p++;
        }
        if(p == start) {
            *pos += 1;
            return '&';
        }
        if(p < end && s[p] == ';') p++;
        *pos = p;
        return numeric(v);
    }
    start = p;
    while(p < end && p - start < 32 && alnum(s[p])) p++;
    if(p > start && p < end && s[p] == ';') {
        e = find_entity(s + start, p - start);
        if(!e) e = find_in(longer, sizeof(longer) / sizeof(longer[0]), s + start, p - start);
        if(e && e->cp) {
            *pos = p + 1;
            return e->cp;
        }
        if(e) {                                 /* two characters in the standard: as written */
            *pos += 1;
            return '&';
        }
    }
    if(p > start) {                             /* no name with a semicolon: a legacy name at the start */
        for(k = p - start < 6 ? p - start : 6; k >= 2; k--) {
            e = find_entity(s + start, k);
            if(!e || !e->legacy) continue;
            if(attr && start + k < end && (s[start + k] == '=' || alnum(s[start + k]))) break;
            *pos = start + k;
            return e->cp;
        }
    }
    *pos += 1;
    return '&';
}

/**** which charset ****/

/* The charset a label names (any case, whitespace around it): UT_CS_UTF8, UT_CS_1252, UT_CS_UTF16, or -1 */
static int charset_of(const char *label) {
    const unsigned char *s = (const unsigned char *)label;
    size_t n = strlen(label), i;
    while(n && ws(s[n - 1])) n--;
    while(n && ws(*s)) {
        s++;
        n--;
    }
    for(i = 0; i < sizeof(utf8_labels) / sizeof(utf8_labels[0]); i++) if(ci_eq(s, n, utf8_labels[i])) return UT_CS_UTF8;
    for(i = 0; i < sizeof(w1252_labels) / sizeof(w1252_labels[0]); i++) if(ci_eq(s, n, w1252_labels[i])) return UT_CS_1252;
    for(i = 0; i < sizeof(utf16_labels) / sizeof(utf16_labels[0]); i++) if(ci_eq(s, n, utf16_labels[i])) return UT_CS_UTF16;
    return -1;
}

/* A charset Undertow cannot read: only ASCII is shown, and the notice names it (its printable ASCII) */
static int unknown_charset(char *name, size_t cap, const char *label) {
    size_t i, k = 0;
    for(i = 0; label[i] && k + 1 < cap; i++)
        if((unsigned char)label[i] > ' ' && (unsigned char)label[i] < 0x7f) name[k++] = label[i];
    name[k] = 0;
    if(!k) snprintf(name, cap, "%s", "a charset it does not name");
    return UT_CS_ASCII;
}

/* One attribute of a tag, as the standard's prescan reads it: 1 with its name (lowercase) and value (cut to
   their buffers), or 0 at the end of the tag. *i moves past it. */
static int pre_attr(const unsigned char *s, size_t n, size_t *i, char *name, size_t ncap, char *val, size_t vcap) {
    size_t p = *i, k = 0, v = 0;
    unsigned char q;
    while(p < n && (ws(s[p]) || s[p] == '/')) p++;
    if(p >= n || s[p] == '>') {
        *i = p;
        return 0;
    }
    do {
        if(k + 1 < ncap) name[k++] = (char)lower(s[p]);
        p++;
    } while(p < n && !ws(s[p]) && s[p] != '/' && s[p] != '>' && s[p] != '=');
    name[k] = 0;
    while(p < n && ws(s[p])) p++;
    if(p < n && s[p] == '=') {
        p++;
        while(p < n && ws(s[p])) p++;
        if(p < n && (s[p] == '"' || s[p] == '\'')) {
            q = s[p++];
            while(p < n && s[p] != q) {
                if(v + 1 < vcap) val[v++] = (char)s[p];
                p++;
            }
            if(p < n) p++;
        } else {
            while(p < n && !ws(s[p]) && s[p] != '>') {
                if(v + 1 < vcap) val[v++] = (char)s[p];
                p++;
            }
        }
    }
    val[v] = 0;
    *i = p;
    return 1;
}

static void copy_cut(char *dst, size_t cap, const char *src) {    /* src, cut to cap - 1 bytes */
    size_t n = strlen(src);
    if(n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

/* The charset in a Content-Type value ("text/html; charset=windows-1252"), as the standard extracts it */
static int charset_in_content(const char *content, char *label, size_t cap) {
    const unsigned char *s = (const unsigned char *)content;
    size_t n = strlen(content), i = 0, k = 0;
    unsigned char q;
    while(i + 7 <= n) {
        if(!ci_prefix(s + i, n - i, "charset")) {
            i++;
            continue;
        }
        i += 7;
        while(i < n && ws(s[i])) i++;
        if(i >= n || s[i] != '=') continue;
        i++;
        while(i < n && ws(s[i])) i++;
        if(i < n && (s[i] == '"' || s[i] == '\'')) {
            q = s[i++];
            if(!memchr(s + i, q, n - i)) return 0;
            while(s[i] != q) {
                if(k + 1 < cap) label[k++] = (char)s[i];
                i++;
            }
        } else
            while(i < n && !ws(s[i]) && s[i] != ';') {
                if(k + 1 < cap) label[k++] = (char)s[i];
                i++;
            }
        label[k] = 0;
        return k > 0;
    }
    return 0;
}

/* <meta>'s attributes (*i just past "<meta"): 1 and the charset it gives */
static int meta_charset(const unsigned char *s, size_t n, size_t *i, char *label, size_t cap) {
    char name[16], val[256], content[256];
    int pragma = 0;
    label[0] = content[0] = 0;
    while(pre_attr(s, n, i, name, sizeof(name), val, sizeof(val))) {
        if(!strcmp(name, "http-equiv")) pragma = ci_eq((const unsigned char *)val, strlen(val), "content-type");
        else if(!strcmp(name, "content") && !content[0]) copy_cut(content, sizeof(content), val);
        else if(!strcmp(name, "charset") && !label[0]) copy_cut(label, cap, val);
    }
    if(label[0]) return 1;
    return pragma && charset_in_content(content, label, cap);
}

/* The first <meta charset> or <meta http-equiv=Content-Type> in the first PRESCAN bytes, outside comments */
static int prescan(const unsigned char *s, size_t n, char *label, size_t cap) {
    const unsigned char *j;
    char name[16], val[256];
    size_t i = 0, k;
    if(n > PRESCAN) n = PRESCAN;
    while(i < n && (j = memchr(s + i, '<', n - i)) != NULL) {
        i = (size_t)(j - s);
        if(n - i >= 4 && !memcmp(s + i, "<!--", 4)) {
            for(k = i + 2; k + 2 < n && !(s[k] == '-' && s[k + 1] == '-' && s[k + 2] == '>'); k++) ;
            i = k + 3;
        } else if(n - i >= 6 && ci_prefix(s + i + 1, n - i - 1, "meta") && (ws(s[i + 5]) || s[i + 5] == '/')) {
            i += 5;
            if(meta_charset(s, n, &i, label, cap)) return 1;
        } else if(i + 2 < n && (alpha(s[i + 1]) || (s[i + 1] == '/' && alpha(s[i + 2])))) {
            i += 2;
            while(i < n && !ws(s[i]) && s[i] != '>') i++;
            while(pre_attr(s, n, &i, name, sizeof(name), val, sizeof(val))) ;
        } else if(i + 1 < n && (s[i + 1] == '!' || s[i + 1] == '/' || s[i + 1] == '?')) {
            j = memchr(s + i, '>', n - i);
            i = j ? (size_t)(j - s) + 1 : n;
        } else i++;
    }
    return 0;
}

/* Well-formed UTF-8; a character cut short by the end of the body is forgiven when a whole one came
   before it (a single Latin-1 letter at the end is not taken for the start of one) */
static int utf8_valid(const unsigned char *s, size_t n) {
    size_t i = 0, k, need;
    unsigned int cp;
    int whole = 0;
    while(i < n) {
        if(s[i] < 0x80) {
            i++;
            continue;
        }
        k = ut_utf8_decode(s + i, n - i, &cp);
        if(k > 1) {
            i += k;
            whole = 1;
            continue;
        }
        need = s[i] >= 0xc2 && s[i] <= 0xdf ? 2 : s[i] >= 0xe0 && s[i] <= 0xef ? 3 : s[i] >= 0xf0 && s[i] <= 0xf4 ? 4 : 0;
        if(!need || n - i >= need || !whole) return 0;
        for(k = 1; i + k < n; k++) if((s[i + k] & 0xc0) != 0x80) return 0;
        return 1;
    }
    return 1;
}

int ut_html_utf16(const unsigned char *in, size_t len, const char *label, unsigned char **result, size_t *outlen) {
    const unsigned char *s = in;
    unsigned char *out;
    size_t n = len, i, o = 0;
    unsigned int u, v, cp;
    int be;
    if(n >= 2 && ((s[0] == 0xff && s[1] == 0xfe) || (s[0] == 0xfe && s[1] == 0xff))) {
        be = s[0] == 0xfe;
        s += 2;
        n -= 2;
    } else if(!(n >= 3 && s[0] == 0xef && s[1] == 0xbb && s[2] == 0xbf) && charset_of(label) == UT_CS_UTF16)
        be = ci_eq((const unsigned char *)label, strlen(label), "utf-16be") ||
             ci_eq((const unsigned char *)label, strlen(label), "unicodefffe");
    else return 0;
    out = malloc(n / 2 * 3 + 4);
    if(!out) return -1;
    for(i = 0; i + 1 < n; i += 2) {
        u = be ? (unsigned int)s[i] << 8 | s[i + 1] : (unsigned int)s[i + 1] << 8 | s[i];
        cp = u;
        if(u >= 0xd800 && u <= 0xdbff && i + 3 < n) {
            v = be ? (unsigned int)s[i + 2] << 8 | s[i + 3] : (unsigned int)s[i + 3] << 8 | s[i + 2];
            if(v >= 0xdc00 && v <= 0xdfff) {
                cp = 0x10000 + ((u - 0xd800) << 10) + (v - 0xdc00);
                i += 2;
            }
        }
        o += ut_utf8_encode(cp, out + o);           /* a lone surrogate is written as U+FFFD */
    }
    if(i < n) o += ut_utf8_encode(UT_REPLACEMENT, out + o);
    out[o] = 0;
    *result = out;
    *outlen = o;
    return 1;
}

int ut_html_charset(const unsigned char *in, size_t len, const char *label, int html, size_t *start,
                    char *unknown, size_t cap) {
    char meta[UT_HTML_LABEL_MAX];
    int cs;
    if(len >= 3 && in[0] == 0xef && in[1] == 0xbb && in[2] == 0xbf) {
        *start = 3;
        return UT_CS_UTF8;
    }
    if(label[0]) {
        cs = charset_of(label);
        if(cs == UT_CS_UTF8 || cs == UT_CS_1252) return cs;
        return unknown_charset(unknown, cap, label);
    }
    if(html && prescan(in, len, meta, sizeof(meta))) {
        cs = charset_of(meta);
        if(cs == UT_CS_UTF8 || cs == UT_CS_UTF16) return UT_CS_UTF8;    /* a page that says UTF-16 in ASCII is not */
        if(cs == UT_CS_1252) return UT_CS_1252;
        return unknown_charset(unknown, cap, meta);
    }
    return utf8_valid(in, len) ? UT_CS_UTF8 : UT_CS_1252;
}

int ut_html_declared(const unsigned char *in, size_t len) {
    char meta[UT_HTML_LABEL_MAX];
    return prescan(in, len, meta, sizeof(meta));
}

int ut_html_sniff(const unsigned char *s, size_t n) {
    static const char *const starts[] = {
        "!doctype html", "html", "head", "script", "iframe", "h1", "div", "font", "table", "a", "style", "title",
        "b", "body", "br", "p", "!--"
    };
    size_t i = 0, k, m;
    if(n >= 3 && s[0] == 0xef && s[1] == 0xbb && s[2] == 0xbf) i = 3;
    while(i < n && ws(s[i])) i++;
    if(i >= n || s[i] != '<') return 0;
    i++;
    for(k = 0; k < sizeof(starts) / sizeof(starts[0]); k++) {
        m = strlen(starts[k]);
        if(ci_prefix(s + i, n - i, starts[k]) && i + m < n && (ws(s[i + m]) || s[i + m] == '>')) return 1;
    }
    return 0;
}

/**** tags ****/

static int element_of(const char *name) {
    size_t lo = 0, hi = sizeof(elements) / sizeof(elements[0]);
    while(lo < hi) {
        size_t mid = (lo + hi) / 2;
        int c = strcmp(elements[mid].name, name);
        if(!c) return elements[mid].id;
        if(c < 0) lo = mid + 1;
        else hi = mid;
    }
    return E_OTHER;
}

static int attr_of(const struct ut_html_tag *t, const char *name) {
    int i;
    for(i = 0; i < t->nattrs; i++) if(!strcmp(name, t->attrs[i].name)) return i;
    return -1;
}

/* An attribute's value from in[q] (quote: '"', '\'' or 0 for an unquoted one): decoded into the tag's
   buffer for it when which >= 0, cut at its size (over is set); returns where it stops, at the quote */
static size_t attr_value(const unsigned char *in, size_t n, int cs, struct ut_html_tag *t, int which, size_t q,
                         unsigned char quote) {
    const unsigned char *s = in, *j;
    size_t k;
    unsigned char b[4];
    unsigned int cp;
    while(q < n && (quote ? s[q] != quote : !ws(s[q]) && s[q] != '>')) {
        if(which < 0 || t->over[which]) {           /* nothing more is kept: find the end */
            if(!quote) {
                q++;
                continue;
            }
            j = memchr(s + q, quote, n - q);
            q = j ? (size_t)(j - s) : n;
            break;
        }
        cp = s[q] == '&' ? ut_html_ref(in, &q, n, 1) : ut_html_char(in, cs, &q, n);
        if(!cp) continue;                           /* NUL */
        k = ut_utf8_encode(cp, b);
        if(t->len[which] + k > t->attrs[which].cap) {
            t->over[which] = 1;
            continue;
        }
        memcpy(t->val[which] + t->len[which], b, k);
        t->len[which] += k;
    }
    if(which >= 0) t->val[which][t->len[which]] = 0;
    return q;
}

int ut_html_tag(const unsigned char *in, size_t len, int cs, size_t q, int end, struct ut_html_tag *t, size_t *next) {
    const unsigned char *s = in;
    size_t n = len, k = 0, an;
    char aname[16];
    unsigned char quote;
    int i, which, long_name = 0;
    for(i = 0; i < t->nattrs; i++) {
        t->seen[i] = t->over[i] = 0;
        t->len[i] = 0;
        t->val[i][0] = 0;
    }
    while(q < n && !ws(s[q]) && s[q] != '/' && s[q] != '>') {
        if(k + 1 < sizeof(t->name)) t->name[k++] = (char)lower(s[q]);
        else long_name = 1;
        q++;
    }
    t->name[k] = 0;
    t->el = long_name ? E_OTHER : element_of(t->name);
    t->end = end;
    t->selfclose = 0;
    for(;;) {
        while(q < n && (ws(s[q]) || s[q] == '/')) {
            if(s[q] == '/' && q + 1 < n && s[q + 1] == '>') t->selfclose = 1;
            q++;
        }
        if(q >= n) {
            *next = n;
            return 0;
        }
        if(s[q] == '>') break;
        an = 0;
        long_name = 0;
        do {                                        /* an attribute's name; its first character may be '=' */
            if(an + 1 < sizeof(aname)) aname[an++] = (char)lower(s[q]);
            else long_name = 1;
            q++;
        } while(q < n && !ws(s[q]) && s[q] != '/' && s[q] != '>' && s[q] != '=');
        aname[an] = 0;
        which = long_name || end ? -1 : attr_of(t, aname);
        if(which >= 0 && t->seen[which]) which = -1;               /* the first of two counts */
        while(q < n && ws(s[q])) q++;
        if(q < n && s[q] == '=') {
            q++;
            while(q < n && ws(s[q])) q++;
            quote = q < n && (s[q] == '"' || s[q] == '\'') ? s[q] : 0;
            if(quote) q++;
            q = attr_value(in, n, cs, t, which, q, quote);
            if(quote) {
                if(q >= n) {                        /* a quote that never closes: the rest is this tag */
                    *next = n;
                    return 0;
                }
                q++;
            }
        }
        if(which >= 0) t->seen[which] = 1;
    }
    *next = q + 1;
    return 1;
}

/**** raw text and comments ****/

size_t ut_html_raw_end(const unsigned char *in, size_t len, size_t pos, const char *name) {
    const unsigned char *s = in, *j;
    size_t n = len, q = pos, k = strlen(name);
    while(q < n && (j = memchr(s + q, '<', n - q)) != NULL) {
        q = (size_t)(j - s);
        if(q + 2 + k < n && s[q + 1] == '/' && ci_prefix(s + q + 2, k, name) &&
           (ws(s[q + 2 + k]) || s[q + 2 + k] == '/' || s[q + 2 + k] == '>')) return q;
        q++;
    }
    return n;
}

/* "</script" or "<script" at in[q], followed by whitespace, '/' or '>' */
static int script_tag_at(const unsigned char *s, size_t n, size_t q, int end) {
    size_t k = q + 1 + (end ? 1 : 0);
    if(end && (q + 1 >= n || s[q + 1] != '/')) return 0;
    return k + 6 < n && ci_prefix(s + k, 6, "script") && (ws(s[k + 6]) || s[k + 6] == '/' || s[k + 6] == '>');
}

/* Where a script's text ends, with the HTML standard's escaped states: after "<!--" a script may hold
   "<script>...</script>" (as document.write() strings do) and goes on to its real end tag. Linear. */
size_t ut_html_script_end(const unsigned char *in, size_t len, size_t pos) {
    const unsigned char *s = in, *j;
    size_t n = len, q = pos;
    int state = 0, dashes = 0;                      /* 0: script data, 1: escaped, 2: double escaped */
    while(q < n) {
        if(!state) {
            j = memchr(s + q, '<', n - q);
            if(!j) return n;
            q = (size_t)(j - s);
            if(script_tag_at(s, n, q, 1)) return q;
            if(n - q >= 4 && s[q + 1] == '!' && s[q + 2] == '-' && s[q + 3] == '-') {
                state = 1;
                dashes = 2;
                q += 4;
            } else q++;
            continue;
        }
        if(s[q] == '-') {
            dashes++;
            q++;
            continue;
        }
        if(s[q] == '>' && dashes >= 2) state = 0;   /* "-->" ends the escape */
        else if(s[q] == '<' && state == 1) {
            if(script_tag_at(s, n, q, 1)) return q;
            if(script_tag_at(s, n, q, 0)) {
                state = 2;                          /* "<script": the next "</script" is its own */
                q += 7;
                dashes = 0;
                continue;
            }
        } else if(s[q] == '<' && state == 2 && script_tag_at(s, n, q, 1)) {
            state = 1;
            q += 8;
            dashes = 0;
            continue;
        }
        dashes = 0;
        q++;
    }
    return n;
}

size_t ut_html_comment_end(const unsigned char *in, size_t len, size_t q) {
    const unsigned char *s = in, *j;
    size_t n = len;
    if(q < n && s[q] == '>') return q + 1;
    if(q + 1 < n && s[q] == '-' && s[q + 1] == '>') return q + 2;
    while(q < n && (j = memchr(s + q, '-', n - q)) != NULL) {
        q = (size_t)(j - s);
        if(q + 2 < n && s[q + 1] == '-' && s[q + 2] == '>') return q + 3;
        if(q + 3 < n && s[q + 1] == '-' && s[q + 2] == '!' && s[q + 3] == '>') return q + 4;
        q++;
    }
    return n;                                       /* an unterminated comment hides the rest */
}

size_t ut_html_skip_past(const unsigned char *in, size_t len, size_t q, const char *end) {
    const unsigned char *s = in, *j;
    size_t n = len, k = strlen(end);
    while(q < n && (j = memchr(s + q, end[0], n - q)) != NULL) {
        q = (size_t)(j - s);
        if(n - q >= k && !memcmp(s + q, end, k)) return q + k;
        q++;
    }
    return n;
}
