/*
SurfOS user program: wcode
--------------------
File: lex.c     Date: 11/05/26
--------------------
The Code Editor's tokenizers (syntax.h), one a language, each a line at a time from the state the
line starts in. They are written from the languages' own definitions: C's lexical grammar and its
preprocessor's line splicing (ISO/IEC 9899:2011, 5.1.1.2 and 6.4), NASM's manual (its comments,
strings, numbers and directives), GNU as's manual for the i386 (AT&T syntax: % registers, $
immediates, # comments, dot directives), the Python Language Reference (2. Lexical analysis), the
POSIX Shell Command Language (2.2 Quoting, 2.4 Reserved Words, 2.7.4 Here-Document), GNU make's
manual (rules, variables, recipes, define), CommonMark's specification (fences, headings, emphasis,
code spans, links) and RFC 8259 (JSON). Colouring is a picture, not a parse: where a rule would
take a full parser (C's typedef names, a NASM label written without its colon, CommonMark's
nested emphasis) the cheap guess is taken, and a guess is never wrong in a way that matters more
than a colour.

Every tokenizer reads only s[0..n) and writes classes only below ncls, whatever the bytes: a line
is as hostile as the file it came from.
*/
#include <string.h>
#include "syntax.h"

struct lx {
    const unsigned char *s;
    unsigned n;
    unsigned char *cls;
    unsigned ncls;
};

static void paint(struct lx *x, unsigned a, unsigned b, int c) {
    if(!x->cls) return;
    if(b > x->ncls) b = x->ncls;
    while(a < b) x->cls[a++] = (unsigned char)c;
}

static int blank(int c) { return c == ' ' || c == '\t' || c == '\f' || c == '\v'; }
static int digit(int c) { return c >= '0' && c <= '9'; }
static int alpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static int alnum(int c) { return alpha(c) || digit(c); }
static int ident0(int c) { return alpha(c) || c == '_' || c >= 0x80; }   /* a UTF-8 byte: a letter of some script */
static int ident1(int c) { return ident0(c) || digit(c); }
static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static unsigned skip_blanks(const struct lx *x, unsigned i) {
    while(i < x->n && blank(x->s[i])) i++;
    return i;
}

/* is s[0..len) in the sorted table (byte order)? fold: compare in lower case (NASM's words) */
static int in(const char *const *tab, unsigned count, const unsigned char *s, unsigned len, int fold) {
    char w[24];
    unsigned lo = 0, hi = count, mid, k;
    int r;
    if(!len || len >= sizeof(w)) return 0;
    for(k = 0; k < len; k++) w[k] = (char)(fold ? lower(s[k]) : s[k]);
    w[len] = 0;
    while(lo < hi) {
        mid = lo + (hi - lo) / 2;
        r = strcmp(w, tab[mid]);
        if(!r) return 1;
        if(r < 0) hi = mid;
        else lo = mid + 1;
    }
    return 0;
}
#define IN(tab, s, len, fold) in(tab, sizeof(tab) / sizeof(tab[0]), s, len, fold)

/* from just after an opening quote q: the index after the closing one, or n; *closed says which. A backslash takes
   the byte after it (esc) */
static unsigned quoted(const struct lx *x, unsigned i, int q, int esc, int *closed) {
    *closed = 0;
    while(i < x->n) {
        if(esc && x->s[i] == '\\') {
            i += 2;
            continue;
        }
        if(x->s[i] == q) {
            *closed = 1;
            return i + 1;
        }
        i++;
    }
    return x->n;
}

/* where "*" "/" ends a block comment from i on, the index after it, or n */
static unsigned comment_end(const struct lx *x, unsigned i, int *closed) {
    *closed = 0;
    for(; i + 1 < x->n; i++)
        if(x->s[i] == '*' && x->s[i + 1] == '/') {
            *closed = 1;
            return i + 2;
        }
    return x->n;
}

/* the line ends with a backslash: C's and make's line splicing */
static int spliced(const struct lx *x) {
    return x->n && x->s[x->n - 1] == '\\';
}

/* a number from i, as C's preprocessing number (6.4.8) takes it: digits, letters, dots, and a sign after an exponent */
static unsigned pp_number(const struct lx *x, unsigned i) {
    unsigned j = i + 1;
    while(j < x->n) {
        int c = x->s[j];
        if(alnum(c) || c == '.' || c == '_' || c == '\'') j++;
        else if((c == '+' || c == '-') && strchr("eEpP", x->s[j - 1])) j++;
        else break;
    }
    return j;
}

/**** C ****/

#define C_COMMENT     1u
#define C_STRING      2u
#define C_CHAR        4u
#define C_PP          8u                /* a directive goes on */
#define C_LINECOMMENT 16u               /* a // comment spliced onto the next line */

static const char *const c_keywords[] = {
    "_Alignas", "_Alignof", "_Atomic", "_Generic", "_Noreturn", "_Static_assert", "_Thread_local", "__asm__",
    "__attribute__", "__inline__", "__typeof__", "__volatile__", "alignas", "alignof", "asm", "auto", "break",
    "case", "const", "constexpr", "continue", "default", "do", "else", "enum", "extern", "for", "goto", "if",
    "inline", "register", "restrict", "return", "sizeof", "static", "static_assert", "struct", "switch",
    "thread_local", "typedef", "typeof", "union", "volatile", "while",
};
static const char *const c_types[] = {
    "FILE", "_Bool", "_Complex", "_Imaginary", "bool", "char", "double", "float", "int", "int16_t", "int32_t",
    "int64_t", "int8_t", "intptr_t", "long", "off_t", "pid_t", "ptrdiff_t", "s16", "s32", "s64", "s8", "short",
    "signed", "size_t", "ssize_t", "time_t", "u16", "u32", "u64", "u8", "u_char", "u_int", "u_long", "u_short",
    "uint16_t", "uint32_t", "uint64_t", "uint8_t", "uintptr_t", "unsigned", "va_list", "void", "wchar_t",
};
static const char *const c_constants[] = { "EOF", "NULL", "false", "nullptr", "true" };

static unsigned lex_c(struct lx *x, unsigned st) {
    const unsigned char *s = x->s;
    unsigned n = x->n, i = 0, j, pp = st & C_PP, include = 0, word_end = 0;
    int closed, q;
    if(st & C_LINECOMMENT) {                    /* the comment goes on while lines end with a backslash */
        paint(x, 0, n, WC_C_COMMENT);
        return spliced(x) ? st : 0;
    }
    if(st & C_COMMENT) {
        i = comment_end(x, 0, &closed);
        paint(x, 0, i, WC_C_COMMENT);
        if(!closed) return st;                  /* a directive goes on past a comment that does */
        st &= ~C_COMMENT;
    } else if(st & (C_STRING | C_CHAR)) {
        i = quoted(x, 0, st & C_STRING ? '"' : '\'', 1, &closed);
        paint(x, 0, i, WC_C_STRING);
        if(!closed) return spliced(x) ? st : 0;
    }
    st &= C_PP;
    if(!pp) {
        i = skip_blanks(x, i);
        if(i < n && s[i] == '#') {              /* a directive: the hash and its name */
            pp = 1;
            j = skip_blanks(x, i + 1);
            while(j < n && ident1(s[j])) j++;
            paint(x, i, j, WC_C_PREPROC);
            include = j - i >= 7 && !memcmp(s + j - 7, "include", 7);
            word_end = i = j;
        }
    }
    while(i < n) {
        int c = s[i];
        if(blank(c)) {
            i++;
            continue;
        }
        if(c == '/' && i + 1 < n && s[i + 1] == '/') {
            paint(x, i, n, WC_C_COMMENT);
            return spliced(x) ? (pp ? C_PP : 0) | C_LINECOMMENT : 0;
        }
        if(c == '/' && i + 1 < n && s[i + 1] == '*') {
            j = comment_end(x, i + 2, &closed);
            paint(x, i, j, WC_C_COMMENT);
            if(!closed) return (pp ? C_PP : 0) | C_COMMENT;
            i = j;
            continue;
        }
        if(c == '"' || c == '\'') {
            j = quoted(x, i + 1, c, 1, &closed);
            paint(x, i, j, WC_C_STRING);
            if(!closed) return spliced(x) ? (pp ? C_PP : 0) | (c == '"' ? C_STRING : C_CHAR) : 0;
            i = j;
            continue;
        }
        if(c == '<' && include && skip_blanks(x, word_end) == i) {     /* #include <name> */
            for(j = i + 1; j < n && s[j] != '>'; j++);
            if(j < n) j++;
            paint(x, i, j, WC_C_STRING);
            i = j;
            continue;
        }
        if(digit(c) || (c == '.' && i + 1 < n && digit(s[i + 1]))) {
            j = pp_number(x, i);
            paint(x, i, j, WC_C_NUMBER);
            i = j;
            continue;
        }
        if(ident0(c)) {
            for(j = i + 1; j < n && ident1(s[j]); j++);
            q = j < n ? s[j] : 0;
            if((q == '"' || q == '\'') && ((j - i == 1 && strchr("LuU", c)) || (j - i == 2 && c == 'u' && s[i + 1] == '8'))) {
                j = quoted(x, j + 1, q, 1, &closed);            /* L"..." u8"..." u'...' */
                paint(x, i, j, WC_C_STRING);
                if(!closed) return spliced(x) ? (pp ? C_PP : 0) | (q == '"' ? C_STRING : C_CHAR) : 0;
            } else if(IN(c_keywords, s + i, j - i, 0)) paint(x, i, j, WC_C_KEYWORD);
            else if(IN(c_types, s + i, j - i, 0)) paint(x, i, j, WC_C_TYPE);
            else if(IN(c_constants, s + i, j - i, 0)) paint(x, i, j, WC_C_NUMBER);
            i = j;
            continue;
        }
        i++;
    }
    return pp && spliced(x) ? C_PP : 0;
}

/**** x86 registers, for both assemblers (lower case) ****/

static int x86_register(const unsigned char *w, unsigned len) {
    static const char *const two[] = { "ah", "al", "ax", "bh", "bl", "bp", "bx", "ch", "cl", "cs", "cx", "dh",
                                       "di", "dl", "ds", "dx", "es", "fs", "gs", "ip", "si", "sp", "ss" };
    static const char *const wide[] = { "ax", "bp", "bx", "cx", "di", "dx", "ip", "si", "sp" };     /* eax, rax */
    char b[8];
    unsigned k, num, nd;
    if(len < 2 || len > 6) return 0;
    for(k = 0; k < len; k++) b[k] = (char)lower(w[k]);
    b[len] = 0;
    if(IN(two, (const unsigned char *)b, len, 0)) return 1;
    if(len == 3 && (!strcmp(b, "sil") || !strcmp(b, "dil") || !strcmp(b, "spl") || !strcmp(b, "bpl"))) return 1;
    if(len == 3 && (b[0] == 'e' || b[0] == 'r') && IN(wide, (const unsigned char *)b + 1, 2, 0)) return 1;
    /* a name and a number: r8-r15 (with d, w or b), cr, dr, st, mm, xmm, ymm, zmm, k */
    for(k = 0; k < len && alpha(b[k]); k++);
    if(k == len || k == 0) return 0;
    for(num = 0, nd = 0; k < len && digit(b[k]) && nd < 3; k++, nd++) num = num * 10 + (unsigned)(b[k] - '0');
    if(!nd) return 0;
    if(b[0] == 'r' && k - nd == 1) return num >= 8 && num <= 15 && (k == len || (k + 1 == len && strchr("dwb", b[k])));
    if(k != len) return 0;
    len -= nd;
    if(len == 2 && !memcmp(b, "cr", 2)) return num <= 8;
    if(len == 2 && !memcmp(b, "dr", 2)) return num <= 7;
    if(len == 2 && (!memcmp(b, "st", 2) || !memcmp(b, "mm", 2))) return num <= 7;
    if(len == 3 && (!memcmp(b, "xmm", 3) || !memcmp(b, "ymm", 3) || !memcmp(b, "zmm", 3))) return num <= 31;
    if(len == 1 && b[0] == 'k') return num <= 7;
    return 0;
}

/**** NASM ****/

static const char *const nasm_directives[] = {
    "absolute", "align", "alignb", "at", "bits", "common", "cpu", "db", "dd", "default", "do", "dq", "dt", "dw",
    "dy", "dz", "endstruc", "equ", "extern", "global", "iend", "incbin", "istruc", "org", "resb", "resd", "reso",
    "resq", "rest", "resw", "resy", "resz", "section", "segment", "static", "struc", "times", "use16", "use32",
    "use64",
};
static const char *const nasm_data[] = {       /* "name db 1" and "name equ 5": the name is a label */
    "db", "dd", "do", "dq", "dt", "dw", "dy", "dz", "equ", "resb", "resd", "reso", "resq", "rest", "resw", "resy",
    "resz", "times",
};
static const char *const x86_sizes[] = {
    "abs", "byte", "dword", "far", "near", "oword", "ptr", "qword", "rel", "seg", "short", "strict", "tword",
    "word", "wrt", "yword", "zword",
};
static const char *const x86_prefixes[] = {
    "a16", "a32", "a64", "lock", "o16", "o32", "o64", "rep", "repe", "repne", "repnz", "repz",
};

static int nasm_ident0(int c) { return alpha(c) || c == '_' || c == '.' || c == '?' || c == '@' || c == '$'; }
static int nasm_ident1(int c) { return nasm_ident0(c) || digit(c) || c == '#' || c == '~'; }

static unsigned lex_nasm(struct lx *x, unsigned st) {
    const unsigned char *s = x->s;
    unsigned n = x->n, i = 0, j, k;
    int start = 1, closed;
    (void)st;
    while(i < n) {
        int c = s[i];
        if(blank(c)) {
            i++;
            continue;
        }
        if(c == ';') {
            paint(x, i, n, WC_C_COMMENT);
            break;
        }
        if(c == '\'' || c == '"' || c == '`') {     /* backquoted strings take C's escapes */
            j = quoted(x, i + 1, c, c == '`', &closed);
            paint(x, i, j, WC_C_STRING);
            i = j;
            start = 0;
            continue;
        }
        if(digit(c) || (c == '$' && i + 1 < n && digit(s[i + 1]))) {
            for(j = i + 1; j < n && (alnum(s[j]) || s[j] == '_' || s[j] == '.'); j++);
            paint(x, i, j, WC_C_NUMBER);
            i = j;
            start = 0;
            continue;
        }
        if(c == '%') {                          /* the preprocessor: %define, %1, %%local, %{...} */
            j = i + 1;
            if(j < n && s[j] == '%') j++;
            if(j < n && s[j] == '{') {
                while(j < n && s[j] != '}') j++;
                if(j < n) j++;
            } else {
                if(j < n && (s[j] == '!' || s[j] == '+' || s[j] == '-')) j++;
                while(j < n && nasm_ident1(s[j])) j++;
            }
            paint(x, i, j, WC_C_PREPROC);
            i = j;
            start = 0;
            continue;
        }
        if(nasm_ident0(c)) {
            for(j = i + 1; j < n && nasm_ident1(s[j]); j++);
            k = skip_blanks(x, j);
            if(start && k < n && s[k] == ':') {
                paint(x, i, k + 1, WC_C_LABEL);
                i = k + 1;
                continue;                       /* an instruction may follow its label */
            }
            if(start) {
                unsigned e = k;
                while(e < n && nasm_ident1(s[e])) e++;
                if(IN(nasm_directives, s + i, j - i, 1)) paint(x, i, j, WC_C_PREPROC);
                else if(e > k && IN(nasm_data, s + k, e - k, 1)) paint(x, i, j, WC_C_LABEL);
                else if(x86_register(s + i, j - i)) paint(x, i, j, WC_C_TYPE);
                else paint(x, i, j, WC_C_KEYWORD);  /* the instruction */
                start = IN(x86_prefixes, s + i, j - i, 1) || (e > k && IN(nasm_data, s + k, e - k, 1));
            } else if(x86_register(s + i, j - i)) paint(x, i, j, WC_C_TYPE);
            else if(IN(x86_sizes, s + i, j - i, 1)) paint(x, i, j, WC_C_KEYWORD);
            else if(IN(nasm_directives, s + i, j - i, 1)) paint(x, i, j, WC_C_PREPROC);
            i = j;
            continue;
        }
        if(c == '[' && start) {                 /* [section .text], [bits 32] */
            i++;
            continue;
        }
        start = 0;
        i++;
    }
    return 0;
}

/**** GNU as (AT&T syntax, i386), with the C preprocessor of .S files ****/

#define G_COMMENT 1u
#define G_PP      2u

static const char *const cpp_directives[] = {
    "define", "elif", "else", "endif", "error", "if", "ifdef", "ifndef", "include", "line", "pragma", "undef",
    "warning",
};

static int gas_ident0(int c) { return alpha(c) || c == '_' || c == '.' || c == '$'; }
static int gas_ident1(int c) { return gas_ident0(c) || digit(c); }

static unsigned lex_gas(struct lx *x, unsigned st) {
    const unsigned char *s = x->s;
    unsigned n = x->n, i = 0, j, k, pp = st & G_PP;
    int start = 1, closed;
    if(st & G_COMMENT) {
        i = comment_end(x, 0, &closed);
        paint(x, 0, i, WC_C_COMMENT);
        if(!closed) return st;
    }
    if(!pp) {
        k = skip_blanks(x, i);
        if(k < n && s[k] == '#') {              /* a line of the preprocessor's, or else a comment */
            j = skip_blanks(x, k + 1);
            for(i = j; i < n && alpha(s[i]); i++);
            if(i > j && IN(cpp_directives, s + j, i - j, 0)) {
                paint(x, k, i, WC_C_PREPROC);
                pp = 1;
            } else {
                paint(x, k, n, WC_C_COMMENT);
                return 0;
            }
        }
    }
    while(i < n) {
        int c = s[i];
        if(blank(c)) {
            i++;
            continue;
        }
        if(c == '#' || (c == '/' && i + 1 < n && s[i + 1] == '/')) {
            paint(x, i, n, WC_C_COMMENT);
            return 0;
        }
        if(c == '/' && i + 1 < n && s[i + 1] == '*') {
            j = comment_end(x, i + 2, &closed);
            paint(x, i, j, WC_C_COMMENT);
            if(!closed) return (pp ? G_PP : 0) | G_COMMENT;
            i = j;
            continue;
        }
        if(c == ';') {                          /* a statement separator, not a comment */
            start = 1;
            i++;
            continue;
        }
        if(c == '"') {
            j = quoted(x, i + 1, c, 1, &closed);
            paint(x, i, j, WC_C_STRING);
            i = j;
            start = 0;
            continue;
        }
        if(c == '\'') {                         /* 'c, a character's value (or 'c' as some write it) */
            j = i + 1 < n && s[i + 1] == '\\' ? i + 3 : i + 2;
            if(j > n) j = n;
            if(j < n && s[j] == '\'') j++;
            paint(x, i, j, WC_C_STRING);
            i = j;
            start = 0;
            continue;
        }
        if(c == '%' && i + 1 < n && alpha(s[i + 1])) {
            for(j = i + 1; j < n && alnum(s[j]); j++);
            if(x86_register(s + i + 1, j - i - 1)) paint(x, i, j, WC_C_TYPE);
            i = j;
            start = 0;
            continue;
        }
        if(c == '$' && i + 1 < n && (digit(s[i + 1]) || s[i + 1] == '-')) {
            j = pp_number(x, i + 1);
            paint(x, i, j, WC_C_NUMBER);
            i = j;
            start = 0;
            continue;
        }
        if(digit(c)) {
            j = pp_number(x, i);
            k = skip_blanks(x, j);
            if(start && k < n && s[k] == ':') {     /* a local label, 1: */
                paint(x, i, k + 1, WC_C_LABEL);
                i = k + 1;
                continue;
            }
            paint(x, i, j, WC_C_NUMBER);
            i = j;
            start = 0;
            continue;
        }
        if(gas_ident0(c)) {
            for(j = i + 1; j < n && gas_ident1(s[j]); j++);
            k = skip_blanks(x, j);
            if(start && k < n && (s[k] == ':' || (s[k] == '=' && (k + 1 >= n || s[k + 1] != '=')))) {
                paint(x, i, s[k] == ':' ? k + 1 : j, WC_C_LABEL);
                i = s[k] == ':' ? k + 1 : j;
                start = s[k] == ':';
                continue;
            }
            if(start) {
                paint(x, i, j, c == '.' ? WC_C_PREPROC : WC_C_KEYWORD);
                start = c != '.' && IN(x86_prefixes, s + i, j - i, 1);
            }
            i = j;
            continue;
        }
        start = 0;
        i++;
    }
    return pp && spliced(x) ? G_PP : 0;
}

/**** Python ****/

enum { PY_NONE, PY_SQ3, PY_DQ3, PY_SQ, PY_DQ };        /* inside ''' or """; inside ' or " spliced by a backslash */

static const char *const py_keywords[] = {
    "False", "None", "True", "and", "as", "assert", "async", "await", "break", "class", "continue", "def", "del",
    "elif", "else", "except", "finally", "for", "from", "global", "if", "import", "in", "is", "lambda",
    "nonlocal", "not", "or", "pass", "raise", "return", "try", "while", "with", "yield",
};
static const char *const py_builtins[] = {
    "bool", "bytearray", "bytes", "cls", "dict", "float", "frozenset", "int", "isinstance", "len", "list",
    "object", "open", "print", "range", "self", "set", "str", "super", "tuple", "type",
};

/* from i, inside a string of quote q (triple or not): the index after it, or n; *closed */
static unsigned py_string(const struct lx *x, unsigned i, int q, int triple, int *closed) {
    *closed = 0;
    while(i < x->n) {
        if(x->s[i] == '\\') {
            i += 2;
            continue;
        }
        if(x->s[i] == q && (!triple || (i + 2 < x->n && x->s[i + 1] == q && x->s[i + 2] == q))) {
            *closed = 1;
            return i + (triple ? 3 : 1);
        }
        i++;
    }
    return x->n;
}

/* a string's prefix: r, b, u, f and two of them, either case */
static int py_prefix(const unsigned char *s, unsigned len) {
    unsigned k;
    if(len > 2) return 0;
    for(k = 0; k < len; k++)
        if(!strchr("rRbBuUfF", s[k])) return 0;
    return 1;
}

static unsigned lex_py(struct lx *x, unsigned st) {
    const unsigned char *s = x->s;
    unsigned n = x->n, i = 0, j, k, from;
    int closed, q, triple, name = 0;
    if(st != PY_NONE) {
        q = st == PY_SQ3 || st == PY_SQ ? '\'' : '"';
        triple = st == PY_SQ3 || st == PY_DQ3;
        i = py_string(x, 0, q, triple, &closed);
        paint(x, 0, i, WC_C_STRING);
        if(!closed) return triple || spliced(x) ? st : PY_NONE;
    }
    k = skip_blanks(x, i);
    if(k < n && s[k] == '@' && !i) {            /* a decorator */
        for(j = k + 1; j < n && (ident1(s[j]) || s[j] == '.'); j++);
        paint(x, k, j, WC_C_PREPROC);
        i = j;
    }
    while(i < n) {
        int c = s[i];
        if(c == '#') {
            paint(x, i, n, WC_C_COMMENT);
            break;
        }
        from = i;
        if(ident0(c)) {
            for(j = i + 1; j < n && ident1(s[j]); j++);
            if(j < n && (s[j] == '\'' || s[j] == '"') && py_prefix(s + i, j - i)) i = j;    /* r'...', f"..." */
            else {
                if(IN(py_keywords, s + i, j - i, 0)) {
                    paint(x, i, j, WC_C_KEYWORD);
                    name = (j - i == 3 && !memcmp(s + i, "def", 3)) || (j - i == 5 && !memcmp(s + i, "class", 5));
                } else {
                    if(name) paint(x, i, j, WC_C_LABEL);
                    else if(IN(py_builtins, s + i, j - i, 0)) paint(x, i, j, WC_C_TYPE);
                    name = 0;
                }
                i = j;
                continue;
            }
            c = s[i];
        }
        if(c == '\'' || c == '"') {
            triple = i + 2 < n && s[i + 1] == c && s[i + 2] == c;
            j = py_string(x, i + (triple ? 3 : 1), c, triple, &closed);
            paint(x, from, j, WC_C_STRING);
            if(!closed) {
                if(triple) return c == '\'' ? PY_SQ3 : PY_DQ3;
                return spliced(x) ? (c == '\'' ? PY_SQ : PY_DQ) : PY_NONE;
            }
            i = j;
            name = 0;
            continue;
        }
        if(digit(c) || (c == '.' && i + 1 < n && digit(s[i + 1]))) {
            j = pp_number(x, i);
            paint(x, i, j, WC_C_NUMBER);
            i = j;
            continue;
        }
        i++;
    }
    return PY_NONE;
}

/**** shell ****/

#define SH_SQ      1u                   /* inside '...' */
#define SH_DQ      2u                   /* inside "..." */
#define SH_HERE    4u                   /* inside a here-document: its word's length and hash above */
#define SH_DASH    8u                   /* <<-: tabs before the end's word */

static const char *const sh_reserved[] = {
    "!", "case", "do", "done", "elif", "else", "esac", "fi", "for", "function", "if", "in", "select", "then",
    "time", "until", "while", "{", "}",
};
static const char *const sh_command_after[] = {  /* reserved words that a command follows */
    "!", "do", "elif", "else", "if", "then", "time", "until", "while", "{", "}",
};
static const char *const sh_builtins[] = {
    ".", ":", "[", "[[", "alias", "bg", "break", "cd", "command", "continue", "echo", "eval", "exec", "exit", "export",
    "false", "fg", "getopts", "jobs", "kill", "local", "printf", "pwd", "read", "readonly", "return", "set",
    "shift", "source", "test", "trap", "true", "type", "ulimit", "umask", "unalias", "unset", "wait",
};

static unsigned word_hash(const unsigned char *s, unsigned n) {
    unsigned h = 2166136261u, k;            /* FNV-1a, folded to 16 bits */
    for(k = 0; k < n; k++) h = (h ^ s[k]) * 16777619u;
    return (h ^ (h >> 16)) & 0xFFFF;
}

static int sh_meta(int c) { return blank(c) || c == ';' || c == '&' || c == '|' || c == '(' || c == ')' || c == '<' || c == '>'; }

/* a variable reference at s[i] == '$': where it ends */
static unsigned sh_var(const struct lx *x, unsigned i) {
    const unsigned char *s = x->s;
    unsigned j = i + 1, depth;
    if(j >= x->n) return j;
    if(s[j] == '{') {
        for(depth = 0; j < x->n; j++) {
            if(s[j] == '{') depth++;
            else if(s[j] == '}' && !--depth) return j + 1;
        }
        return x->n;
    }
    if(s[j] == '(') return j + 1 < x->n && s[j + 1] == '(' ? j + 2 : j + 1;     /* $( and $((: their start */
    if(ident0(s[j]) && s[j] < 0x80) {
        while(j < x->n && (alnum(s[j]) || s[j] == '_')) j++;
        return j;
    }
    if(digit(s[j]) || strchr("@*#?$!-", s[j])) return j + 1;
    return i + 1;
}

/* inside "...": its $ references as variables; the index after the closing quote, or n */
static unsigned sh_dq(struct lx *x, unsigned i, int *closed) {
    unsigned j;
    *closed = 0;
    while(i < x->n) {
        if(x->s[i] == '\\') i += 2;
        else if(x->s[i] == '"') {
            *closed = 1;
            paint(x, i, i + 1, WC_C_STRING);
            return i + 1;
        } else if(x->s[i] == '$') {
            j = sh_var(x, i);
            paint(x, i, j, WC_C_VAR);
            i = j;
        } else {
            paint(x, i, i + 1, WC_C_STRING);
            i++;
        }
    }
    if(i > x->n) i = x->n;
    return i;
}

static unsigned lex_sh(struct lx *x, unsigned st) {
    const unsigned char *s = x->s;
    unsigned n = x->n, i = 0, j, k, here = 0, after_for = 0;
    int closed, cmd = 1;
    if(st & SH_HERE) {
        for(k = 0; (st & SH_DASH) && k < n && s[k] == '\t'; k++);
        if(n - k == ((st >> 4) & 0xFFF) && word_hash(s + k, n - k) == st >> 16) {
            paint(x, 0, n, WC_C_KEYWORD);       /* its end */
            return 0;
        }
        paint(x, 0, n, WC_C_STRING);
        return st;
    }
    if(st & SH_SQ) {
        i = quoted(x, 0, '\'', 0, &closed);
        paint(x, 0, i, WC_C_STRING);
        if(!closed) return st;
    } else if(st & SH_DQ) {
        i = sh_dq(x, 0, &closed);
        if(!closed) return st;
    }
    while(i < n) {
        int c = s[i];
        if(blank(c)) {
            i++;
            continue;
        }
        if(c == '#' && (i == 0 || sh_meta(s[i - 1]))) {
            paint(x, i, n, WC_C_COMMENT);
            break;
        }
        if(c == '\\') {
            i += 2;
            cmd = 0;
            continue;
        }
        if(c == '\'') {
            j = quoted(x, i + 1, '\'', 0, &closed);
            paint(x, i, j, WC_C_STRING);
            if(!closed) return SH_SQ | here;
            i = j;
            cmd = 0;
            continue;
        }
        if(c == '"') {
            paint(x, i, i + 1, WC_C_STRING);
            j = sh_dq(x, i + 1, &closed);
            if(!closed) return SH_DQ | here;
            i = j;
            cmd = 0;
            continue;
        }
        if(c == '`') {
            j = quoted(x, i + 1, '`', 1, &closed);
            paint(x, i, j, WC_C_STRING);
            i = j;
            cmd = 0;
            continue;
        }
        if(c == '$') {
            if(i + 1 < n && s[i + 1] == '\'') {     /* $'...' takes escapes */
                j = quoted(x, i + 2, '\'', 1, &closed);
                paint(x, i, j, WC_C_STRING);
                i = j;
            } else {
                j = sh_var(x, i);
                paint(x, i, j, WC_C_VAR);
                if(j > i + 1 && s[j - 1] == '(') cmd = 1;
                i = j;
                continue;
            }
            cmd = 0;
            continue;
        }
        if(c == '<' && i + 1 < n && s[i + 1] == '<' && (i + 2 >= n || s[i + 2] != '<')) {
            unsigned dash = 0, ws, we;
            j = i + 2;
            if(j < n && s[j] == '-') {
                dash = SH_DASH;
                j++;
            }
            j = skip_blanks(x, j);
            ws = j;
            if(j < n && (s[j] == '\'' || s[j] == '"')) {    /* a quoted word: what is inside it */
                int q = s[j];
                ws = ++j;
                while(j < n && s[j] != q) j++;
                we = j;
                if(j < n) j++;
            } else {
                while(j < n && !sh_meta(s[j])) j++;
                we = j;
            }
            if(we > ws && we - ws <= 0xFFF) here = SH_HERE | dash | (we - ws) << 4 | word_hash(s + ws, we - ws) << 16;
            paint(x, i, j, WC_C_STRING);
            i = j;
            cmd = 0;
            continue;
        }
        if(c == ';' || c == '&' || c == '|' || c == '(' || c == ')') {
            cmd = 1;
            i++;
            continue;
        }
        if(c == '<' || c == '>') {
            i++;
            continue;
        }
        for(j = i; j < n && !sh_meta(s[j]) && s[j] != '"' && s[j] != '\'' && s[j] != '`' && s[j] != '$'; j++)
            if(s[j] == '\\') j++;
        if(j > n) j = n;
        if(j == i) {
            i++;
            continue;
        }
        if(after_for) after_for--;
        if(cmd) {
            for(k = i; k < j && (alnum(s[k]) || s[k] == '_'); k++);
            if(k > i && k < j && s[k] == '=' && !digit(s[i])) {     /* name=value before a command */
                paint(x, i, k, WC_C_VAR);
                i = k + 1;
                continue;
            }
            if(IN(sh_reserved, s + i, j - i, 0)) {
                paint(x, i, j, WC_C_KEYWORD);
                after_for = (j - i == 3 && !memcmp(s + i, "for", 3)) || (j - i == 4 && !memcmp(s + i, "case", 4)) ||
                            (j - i == 6 && !memcmp(s + i, "select", 6)) ? 3 : 0;
                cmd = IN(sh_command_after, s + i, j - i, 0);
            } else {
                if(IN(sh_builtins, s + i, j - i, 0)) paint(x, i, j, WC_C_TYPE);
                cmd = 0;
            }
        } else if(after_for == 1 && j - i == 2 && !memcmp(s + i, "in", 2)) paint(x, i, j, WC_C_KEYWORD);
        i = j;
    }
    return here;
}

/**** Makefiles ****/

#define MK_CONT    1u                   /* a line spliced onto the next */
#define MK_RECIPE  2u                   /* a recipe's line spliced onto the next */
#define MK_COMMENT 4u                   /* a comment spliced onto the next */
#define MK_DEFINE  8u                   /* inside define ... endef */

static const char *const mk_directives[] = {
    "-include", "define", "else", "endef", "endif", "export", "ifdef", "ifeq", "ifndef", "ifneq", "include",
    "override", "private", "sinclude", "undefine", "unexport", "vpath",
};
static const char *const mk_functions[] = {
    "abspath", "addprefix", "addsuffix", "and", "basename", "call", "dir", "error", "eval", "file", "filter",
    "filter-out", "findstring", "firstword", "flavor", "foreach", "guile", "if", "info", "join", "lastword",
    "notdir", "or", "origin", "patsubst", "realpath", "shell", "sort", "strip", "subst", "suffix", "value",
    "warning", "wildcard", "word", "wordlist", "words",
};

/* a reference at s[i] == '$': its end, painted, a function's name as a keyword */
static unsigned mk_var(struct lx *x, unsigned i) {
    const unsigned char *s = x->s;
    unsigned j = i + 1, depth, k;
    int open, close;
    if(j >= x->n) return j;
    if(s[j] == '$') return j + 1;               /* $$ is a dollar for the shell */
    if(s[j] != '(' && s[j] != '{') {
        paint(x, i, j + 1, WC_C_VAR);
        return j + 1;
    }
    open = s[j];
    close = open == '(' ? ')' : '}';
    for(k = j + 1; k < x->n && (alpha(s[k]) || s[k] == '-'); k++);
    for(depth = 0; j < x->n; j++) {
        if(s[j] == open) depth++;
        else if(s[j] == close && !--depth) {
            j++;
            break;
        }
    }
    paint(x, i, j, WC_C_VAR);
    if(k < x->n && blank(s[k]) && IN(mk_functions, s + i + 2, k - i - 2, 0)) paint(x, i + 2, k, WC_C_KEYWORD);
    return j;
}

/* variables and comments from i to the end: what a recipe or the rest of a line has */
static unsigned mk_rest(struct lx *x, unsigned i, int comments) {
    while(i < x->n) {
        if(x->s[i] == '\\') i += 2;
        else if(x->s[i] == '$') i = mk_var(x, i);
        else if(x->s[i] == '#' && comments) {
            paint(x, i, x->n, WC_C_COMMENT);
            return MK_COMMENT;
        } else i++;
    }
    return 0;
}

static unsigned lex_make(struct lx *x, unsigned st) {
    const unsigned char *s = x->s;
    unsigned n = x->n, i, j, k, c;
    int cont = spliced(x);
    if(st & MK_COMMENT) {
        paint(x, 0, n, WC_C_COMMENT);
        return cont ? st : st & MK_DEFINE;
    }
    i = skip_blanks(x, 0);
    if(st & MK_DEFINE) {
        if(n - i >= 5 && !memcmp(s + i, "endef", 5) && (n - i == 5 || blank(s[i + 5]) || s[i + 5] == '#')) {
            paint(x, i, i + 5, WC_C_KEYWORD);
            return 0;
        }
        mk_rest(x, 0, 0);
        return st;
    }
    if((st & MK_RECIPE) || (!(st & MK_CONT) && n && s[0] == '\t')) {
        c = mk_rest(x, 0, 1);
        return cont ? (c ? MK_COMMENT : MK_RECIPE) : 0;
    }
    if(st & MK_CONT) {
        c = mk_rest(x, 0, 1);
        return cont ? (c ? MK_COMMENT : MK_CONT) : 0;
    }
    if(i < n && s[i] == '#') {
        paint(x, i, n, WC_C_COMMENT);
        return cont ? MK_COMMENT : 0;
    }
    for(j = i; j < n && !blank(s[j]) && s[j] != ':' && s[j] != '=' && s[j] != '#'; j++);
    if(j > i && IN(mk_directives, s + i, j - i, 0)) {
        paint(x, i, j, WC_C_KEYWORD);
        c = mk_rest(x, j, 1);
        if(j - i == 6 && !memcmp(s + i, "define", 6)) return MK_DEFINE;
        if(j - i == 8 && (!memcmp(s + i, "override", 8) || !memcmp(s + i, "unexport", 8))) i = skip_blanks(x, j);
        else if(j - i == 6 && !memcmp(s + i, "export", 6)) i = skip_blanks(x, j);
        else return cont ? (c ? MK_COMMENT : MK_CONT) : 0;
    }
    /* an assignment (NAME = ..., :=, ::=, ?=, +=, !=) or a rule (targets: prerequisites), by what comes first */
    for(k = i; k < n; k++) {
        if(s[k] == '$') {
            unsigned e = mk_var(x, k);
            k = e - 1;
            continue;
        }
        if(s[k] == '=' || s[k] == '#') break;
        if(s[k] == ':' && !(k + 1 < n && s[k + 1] == '=') && !(k + 2 < n && s[k + 1] == ':' && s[k + 2] == '=')) break;
        if((s[k] == '?' || s[k] == '+' || s[k] == '!') && k + 1 < n && s[k + 1] == '=') break;
    }
    if(k < n && s[k] != '#' && s[k] != ':') {   /* the name before =, ?=, +=, != and := */
        for(j = k; j > i && (blank(s[j - 1]) || s[j - 1] == ':' || s[j - 1] == '?' || s[j - 1] == '+' || s[j - 1] == '!'); j--);
        paint(x, i, j, WC_C_VAR);
        c = mk_rest(x, k, 1);
    } else if(k < n && s[k] == ':') {           /* a rule: its targets */
        paint(x, i, k, WC_C_LABEL);
        for(j = k + 1; j < n && s[j] != ';' && s[j] != '#'; j++)
            if(s[j] == '$') j = mk_var(x, j) - 1;
        if(j > n) j = n;
        c = mk_rest(x, j < n && s[j] == ';' ? j + 1 : j, 1);
    } else c = mk_rest(x, i, 1);
    return cont ? (c ? MK_COMMENT : MK_CONT) : 0;
}

/**** Markdown ****/

#define MD_FENCE 1u                     /* inside a fenced block: the tilde's bit, the fence's length above */
#define MD_TILDE 2u
#define MD_HTML  4u                     /* inside an HTML comment */

/* a run of c from i: its length */
static unsigned run_of(const struct lx *x, unsigned i, int c) {
    unsigned j = i;
    while(j < x->n && x->s[j] == c) j++;
    return j - i;
}

static int md_punct(int c) { return c >= 0x21 && c <= 0x7E && !alnum(c); }

/* the end of the HTML comment from i (after "<!--"), or n */
static unsigned md_comment_end(const struct lx *x, unsigned i, int *closed) {
    *closed = 0;
    for(; i + 2 < x->n; i++)
        if(x->s[i] == '-' && x->s[i + 1] == '-' && x->s[i + 2] == '>') {
            *closed = 1;
            return i + 3;
        }
    return x->n;
}

/* 11/09/26 (the review, C2): the next ']' (or ')') at or after from, escapes stepped over. A line asks from places
   that only go on, so a scan's answer holds for every place up to it, and the next scan starts after it: a line is
   read once for each of these, however many [ it has */
struct md_next { unsigned at; int known, c; };

static unsigned md_next(const struct lx *x, struct md_next *m, unsigned from) {
    unsigned j;
    if(m->known && from <= m->at) return m->at;
    for(j = from; j < x->n && x->s[j] != m->c; j++)
        if(x->s[j] == '\\') j++;
    m->known = 1;
    m->at = j < x->n ? j : x->n;
    return m->at;
}

static unsigned lex_md(struct lx *x, unsigned st) {
    const unsigned char *s = x->s;
    unsigned n = x->n, i = 0, j, k, ind, len;
    int closed, c;
    /* 11/09/26 (the review, C2): what a scan of this line found nowhere after where it began, which no later scan
       can find either: a closing run of backticks of a length (1 to 32; a longer run opens nothing), a closing *
       or _ (needing one, or two); and the next ] and ) */
    unsigned char no_tick[33], no_emph[2][3];
    struct md_next rb = { 0, 0, ']' }, rb2 = { 0, 0, ']' }, rp = { 0, 0, ')' };
    memset(no_tick, 0, sizeof(no_tick));
    memset(no_emph, 0, sizeof(no_emph));
    if(st & MD_FENCE) {
        int fc = st & MD_TILDE ? '~' : '`';
        k = skip_blanks(x, 0);
        len = run_of(x, k, fc);
        paint(x, 0, n, WC_C_CODE);
        if(k <= 3 && len >= st >> 8 && skip_blanks(x, k + len) == n) return 0;   /* the closing fence */
        return st;
    }
    if(st & MD_HTML) {
        i = md_comment_end(x, 0, &closed);
        paint(x, 0, i, WC_C_COMMENT);
        if(!closed) return st;
    }
    for(ind = 0; ind < n && s[ind] == ' '; ind++);
    if(!i && ind <= 3 && ind < n) {
        c = s[ind];
        len = run_of(x, ind, c);
        if((c == '`' || c == '~') && len >= 3) {    /* a fence opens */
            for(j = ind + len; c == '`' && j < n && s[j] != '`'; j++);
            if(c == '~' || j == n) {
                paint(x, 0, n, WC_C_CODE);
                return MD_FENCE | (c == '~' ? MD_TILDE : 0) | (len > 255 ? 255 : len) << 8;
            }
        }
        if(c == '#' && len <= 6 && (ind + len == n || blank(s[ind + len]))) {
            paint(x, 0, n, WC_C_HEADING);
            return 0;
        }
        if(c == '=' || c == '-' || c == '*' || c == '_') {     /* a setext underline, or a break */
            for(j = ind, k = 0; j < n && (s[j] == c || blank(s[j])); j++) k += s[j] == c;
            if(j == n && (c == '=' || k >= 3)) {
                paint(x, 0, n, c == '=' ? WC_C_HEADING : WC_C_PREPROC);
                return 0;
            }
        }
        while(i < n) {                          /* > quotes, then a list's marker */
            i = skip_blanks(x, i);
            if(i < n && s[i] == '>') {
                paint(x, i, i + 1, WC_C_PREPROC);
                i++;
            } else break;
        }
        if(i < n && (s[i] == '-' || s[i] == '*' || s[i] == '+') && (i + 1 == n || blank(s[i + 1]))) {
            paint(x, i, i + 1, WC_C_KEYWORD);
            i++;
        } else {
            for(j = i; j < n && j - i < 9 && digit(s[j]); j++);
            if(j > i && j < n && (s[j] == '.' || s[j] == ')') && (j + 1 == n || blank(s[j + 1]))) {
                paint(x, i, j + 1, WC_C_KEYWORD);
                i = j + 1;
            }
        }
    }
    while(i < n) {
        c = s[i];
        if(c == '\\' && i + 1 < n && md_punct(s[i + 1])) {
            i += 2;
            continue;
        }
        if(c == '`') {                          /* a code span: closed by a run of as many backticks */
            len = run_of(x, i, '`');
            j = n;
            if(len <= 32 && !no_tick[len])
                for(j = i + len; j < n; j += k) {
                    k = s[j] == '`' ? run_of(x, j, '`') : 1;
                    if(s[j] == '`' && k == len) break;
                }
            if(j < n) {
                paint(x, i, j + len, WC_C_CODE);
                i = j + len;
            } else {
                if(len <= 32) no_tick[len] = 1;
                i += len;
            }
            continue;
        }
        if(c == '<' && i + 3 < n && s[i + 1] == '!' && s[i + 2] == '-' && s[i + 3] == '-') {
            j = md_comment_end(x, i + 4, &closed);
            paint(x, i, j, WC_C_COMMENT);
            if(!closed) return MD_HTML;
            i = j;
            continue;
        }
        if(c == '<' && i + 1 < n && alpha(s[i + 1])) {     /* <https://...> */
            for(j = i + 1; j < n && s[j] != '>' && !blank(s[j]) && s[j] != '<'; j++);
            if(j < n && s[j] == '>' && memchr(s + i, ':', j - i)) {
                paint(x, i, j + 1, WC_C_LINK);
                i = j + 1;
                continue;
            }
        }
        if(c == '[' || (c == '!' && i + 1 < n && s[i + 1] == '[')) {     /* [text](where) and [text][name] */
            unsigned a = c == '!' ? i + 1 : i;  /* (its text to the first ], brackets in it not followed) */
            j = md_next(x, &rb, a + 1);
            if(j < n && j + 1 < n && (s[j + 1] == '(' || s[j + 1] == '[')) {
                k = md_next(x, s[j + 1] == '(' ? &rp : &rb2, j + 2);
                if(k < n) {
                    paint(x, i, j + 1, WC_C_LINK);
                    paint(x, j + 1, k + 1, WC_C_STRING);
                    i = k + 1;
                    continue;
                }
            }
            i++;
            continue;
        }
        if(c == '*' || c == '_') {              /* emphasis: a run that opens before a letter and closes after one */
            unsigned need;
            len = run_of(x, i, c);
            need = len > 2 ? 2 : len;
            if(i + len < n && !blank(s[i + len]) && !(c == '_' && i > 0 && alnum(s[i - 1])) &&
               !no_emph[c == '_'][need]) {
                for(j = i + len; j < n; j++) {
                    if(s[j] == '\\') {
                        j++;
                        continue;
                    }
                    if(s[j] == c && !blank(s[j - 1])) {
                        k = run_of(x, j, c);
                        if(k >= (len > 2 ? 2 : len) && !(c == '_' && j + k < n && alnum(s[j + k]))) break;
                        j += k - 1;
                    }
                }
                if(j < n) {
                    k = run_of(x, j, c);
                    paint(x, i, j + k, WC_C_EMPH);
                    i = j + k;
                    continue;
                }
                no_emph[c == '_'][need] = 1;    /* none needing one is none needing two either */
                if(need == 1) no_emph[c == '_'][2] = 1;
            }
            i += len;
            continue;
        }
        i++;
    }
    return 0;
}

/**** JSON ****/

#define JS_COMMENT 1u

static unsigned lex_json(struct lx *x, unsigned st) {
    const unsigned char *s = x->s;
    unsigned n = x->n, i = 0, j, k;
    int closed;
    if(st & JS_COMMENT) {
        i = comment_end(x, 0, &closed);
        paint(x, 0, i, WC_C_COMMENT);
        if(!closed) return st;
    }
    while(i < n) {
        int c = s[i];
        if(blank(c) || c == '\r') {
            i++;
            continue;
        }
        if(c == '/' && i + 1 < n && s[i + 1] == '/') {
            paint(x, i, n, WC_C_COMMENT);
            break;
        }
        if(c == '/' && i + 1 < n && s[i + 1] == '*') {
            j = comment_end(x, i + 2, &closed);
            paint(x, i, j, WC_C_COMMENT);
            if(!closed) return JS_COMMENT;
            i = j;
            continue;
        }
        if(c == '"') {                          /* a name when a colon follows it */
            j = quoted(x, i + 1, '"', 1, &closed);
            k = skip_blanks(x, j);
            paint(x, i, j, closed && k < n && s[k] == ':' ? WC_C_KEY : closed ? WC_C_STRING : WC_C_BAD);
            i = j;
            continue;
        }
        if(c == '-' || digit(c)) {
            for(j = i + 1; j < n && (digit(s[j]) || s[j] == '.' || s[j] == 'e' || s[j] == 'E' ||
                                     ((s[j] == '+' || s[j] == '-') && (s[j - 1] == 'e' || s[j - 1] == 'E'))); j++);
            paint(x, i, j, WC_C_NUMBER);
            i = j;
            continue;
        }
        if(alpha(c)) {
            for(j = i + 1; j < n && alnum(s[j]); j++);
            paint(x, i, j, (j - i == 4 && (!memcmp(s + i, "true", 4) || !memcmp(s + i, "null", 4))) ||
                           (j - i == 5 && !memcmp(s + i, "false", 5)) ? WC_C_KEYWORD : WC_C_BAD);
            i = j;
            continue;
        }
        if(!strchr("{}[]:,", c)) paint(x, i, i + 1, WC_C_BAD);
        i++;
    }
    return 0;
}

/**** the entry ****/

unsigned wc_lex(int lang, const char *s, unsigned n, unsigned state, unsigned char *cls, unsigned ncls) {
    struct lx x;
    x.s = (const unsigned char *)s;
    x.cls = cls;
    x.ncls = cls ? ncls : 0;
    if(cls) memset(cls, WC_C_TEXT, n < ncls ? n : ncls);
    x.n = n < WC_LEX_MAX ? n : WC_LEX_MAX;      /* 11/09/26 (the review, C2): a line's first bytes only, as if it ended */
    switch(lang) {
    case WC_LANG_C: return lex_c(&x, state);
    case WC_LANG_NASM: return lex_nasm(&x, state);
    case WC_LANG_GAS: return lex_gas(&x, state);
    case WC_LANG_PY: return lex_py(&x, state);
    case WC_LANG_SH: return lex_sh(&x, state);
    case WC_LANG_MAKE: return lex_make(&x, state);
    case WC_LANG_MD: return lex_md(&x, state);
    case WC_LANG_JSON: return lex_json(&x, state);
    }
    return 0;
}

/* the tables must be sorted for in(): the tests ask */
int wc_lex_tables_sorted(void) {
    static const struct { const char *const *t; unsigned n; } tabs[] = {
#define T(x) { x, sizeof(x) / sizeof(x[0]) }
        T(c_keywords), T(c_types), T(c_constants), T(nasm_directives), T(nasm_data), T(x86_sizes), T(x86_prefixes),
        T(cpp_directives), T(py_keywords), T(py_builtins), T(sh_reserved), T(sh_command_after), T(sh_builtins),
        T(mk_directives), T(mk_functions),
#undef T
    };
    unsigned i, k;
    for(i = 0; i < sizeof(tabs) / sizeof(tabs[0]); i++)
        for(k = 1; k < tabs[i].n; k++)
            if(strcmp(tabs[i].t[k - 1], tabs[i].t[k]) >= 0) return 0;
    return 1;
}
