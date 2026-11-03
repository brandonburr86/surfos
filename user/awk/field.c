/*
SurfOS user program: awk
--------------------
File: field.c   Date: 11/01/26
--------------------
The record and its fields. Most records are only printed or matched, so a record is split when a
field or NF is first asked for, and $0 is put back together when it is first asked for after a
field changed. So FS and OFS must not change under a record that waits: global_set() calls
before_fs_change() and before_ofs_change() first, and the old ones are used, as POSIX says: a new
FS is for the next record.

A field is an offset into $0 until it is asked for or changed. The record keeps its string for as
long as the offsets are in use, so no field costs an allocation unless it is used.
*/

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "awk.h"

struct field {
    unsigned start, len;                /* in the split string */
    struct value val;                   /* V_UNSET until it is asked for */
    int made;                           /* val is set */
};

static struct str *record;              /* $0: NULL while it has to be put back together */
static struct str *split_from;          /* what the offsets are into */
static struct field *fields;            /* fields[0] is $1 */
static unsigned nf, capf;
static int split_done = 1;              /* fields and nf are those of the record */

static void fields_room(unsigned n) {
    if(n > capf) {
        unsigned cap = capf ? capf : 32, i;
        while(cap < n) cap *= 2;
        fields = xrealloc(fields, (size_t)cap * sizeof(*fields));
        for(i = capf; i < cap; i++) fields[i].made = 0;
        capf = cap;
    }
}

static void fields_drop(void) {
    unsigned i;
    for(i = 0; i < nf; i++) {
        if(fields[i].made) {
            val_put(&fields[i].val);
            fields[i].made = 0;
        }
    }
    nf = 0;
}

void record_set(struct str *s) {
    str_put(record);
    record = s;
    fields_drop();
    split_done = 0;
}

static void add_field(void *ctx, unsigned start, unsigned len) {
    (void)ctx;
    fields_room(nf + 1);
    fields[nf].start = start;
    fields[nf].len = len;
    nf++;
}

/* Runs of blanks; a character; each character; or a regular expression, which an empty match
   does not end a field on. With RS="", a newline ends a field whatever FS is. */
void split_text(struct str *text, struct str *sep, const struct regex *re, int paragraph,
                void (*add)(void *, unsigned, unsigned), void *ctx) {
    const char *s = text->s;
    unsigned n = text->len, i = 0, start;
    if(!n) return;
    if(!re && sep->len == 1 && sep->s[0] == ' ') {
        for(;;) {
            while(i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n')) i++;
            if(i == n) return;
            start = i;
            while(i < n && s[i] != ' ' && s[i] != '\t' && s[i] != '\n') i++;
            add(ctx, start, i - start);
        }
    }
    if(!re && !sep->len) {
        while(i < n) {
            unsigned k = char_len(s + i, n - i);
            add(ctx, i, k);
            i += k;
        }
        return;
    }
    if(!re && sep->len == 1) {
        char c = sep->s[0];
        for(start = 0; ; i++) {
            if(i == n) {
                add(ctx, start, n - start);
                return;
            }
            if(s[i] == c || (paragraph && s[i] == '\n')) {
                add(ctx, start, i - start);
                start = i + 1;
            }
        }
    }
    start = 0;
    while(i < n) {
        regoff_t so, eo;
        unsigned next;
        if(!regex_find(re, s, i, n, &so, &eo)) break;
        if(eo == so) {                  /* an empty match separates nothing: from the next character */
            if((unsigned)so >= n) break;
            i = (unsigned)so + char_len(s + so, n - (unsigned)so);
            continue;
        }
        if(paragraph) {                 /* an earlier newline counts first */
            const char *nl = memchr(s + start, '\n', (unsigned)so - start);
            if(nl) {
                add(ctx, start, (unsigned)(nl - s) - start);
                start = (unsigned)(nl - s) + 1;
                i = start;
                continue;
            }
        }
        add(ctx, start, (unsigned)so - start);
        next = (unsigned)eo;
        start = next;
        i = next;
    }
    if(paragraph) {
        const char *nl;
        while((nl = memchr(s + start, '\n', n - start))) {
            add(ctx, start, (unsigned)(nl - s) - start);
            start = (unsigned)(nl - s) + 1;
        }
    }
    add(ctx, start, n - start);
}

static void split_record(void) {
    struct str *fs, *rs;
    int paragraph;
    if(split_done) return;
    fs = to_str(&A.global[S_FS]);
    rs = to_str(&A.global[S_RS]);
    paragraph = !rs->len;
    str_put(rs);
    str_put(split_from);
    split_from = str_ref(record);
    nf = 0;
    split_done = 1;
    if(fs->len > 1) {
        struct value v;
        v.kind = V_STR;
        v.u.s = fs;
        split_text(record, fs, regex_of(&v), paragraph, add_field, NULL);
    } else {
        split_text(record, fs, NULL, paragraph, add_field, NULL);
    }
    str_put(fs);
    val_put(&A.global[S_NF]);
    A.global[S_NF].kind = V_NUM;
    A.global[S_NF].u.num = nf;
}

void before_fs_change(void) {
    split_record();
}

static void make_field(unsigned i) {
    struct field *f = &fields[i];
    if(f->made) return;
    f->val.kind = V_INPUT;
    f->val.u.s = str_new(split_from->s + f->start, f->len);
    f->made = 1;
}

static void put_together(void) {
    struct buf b = { NULL, 0, 0 };
    struct str *ofs = to_str(&A.global[S_OFS]);
    unsigned i;
    for(i = 0; i < nf; i++) {
        struct str *s;
        make_field(i);
        s = to_str(&fields[i].val);
        if(i) buf_add(&b, ofs->s, ofs->len);
        buf_add(&b, s->s, s->len);
        str_put(s);
    }
    str_put(ofs);
    record = str_new(b.p ? b.p : "", b.len);
    free(b.p);
}

void before_ofs_change(void) {
    if(!record) put_together();
}

struct str *record_get(void) {
    if(!record) put_together();
    return record;
}

static unsigned field_index(double d) {
    if(d != d || d < 0) fatal("a negative field number, $%g", d);
    if(d >= 100000000.0) fatal("field number %g is too large", d);
    return (unsigned)d;
}

void field_load(double index, struct value *to) {
    unsigned i = field_index(index);
    if(!i) {
        to->kind = V_INPUT;
        to->u.s = str_ref(record_get());
        return;
    }
    split_record();
    if(i > nf) {
        to->kind = V_UNSET;
        return;
    }
    make_field(i - 1);
    val_copy(to, &fields[i - 1].val);
}

/* the fields up to n exist, the new ones empty */
static void fields_extend(unsigned n) {
    fields_room(n);
    while(nf < n) {
        fields[nf].start = fields[nf].len = 0;
        fields[nf].val.kind = V_STR;
        fields[nf].val.u.s = str_new("", 0);
        fields[nf].made = 1;
        nf++;
    }
    val_put(&A.global[S_NF]);
    A.global[S_NF].kind = V_NUM;
    A.global[S_NF].u.num = nf;
}

void field_store(double index, const struct value *v) {
    unsigned i = field_index(index);
    if(!i) {
        record_set(to_str(v));
        return;
    }
    split_record();
    if(i > nf) fields_extend(i);
    make_field(i - 1);
    val_put(&fields[i - 1].val);
    /* the field keeps its value: a number there stays a number until $0 needs its text */
    val_copy(&fields[i - 1].val, v);
    str_put(record);
    record = NULL;
}

double nf_get(void) {
    split_record();
    return nf;
}

void nf_set(double n) {
    unsigned want = field_index(n), i;
    split_record();
    if(want > nf) {
        fields_extend(want);
    } else {
        for(i = want; i < nf; i++) {
            if(fields[i].made) {
                val_put(&fields[i].val);
                fields[i].made = 0;
            }
        }
        nf = want;
        val_put(&A.global[S_NF]);
        A.global[S_NF].kind = V_NUM;
        A.global[S_NF].u.num = nf;
    }
    str_put(record);
    record = NULL;
}
