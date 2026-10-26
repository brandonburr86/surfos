/*
SurfOS AML Field Layout Reader
--------------------
File: amlfield.c Date: 10/12/26
--------------------
Original bounded reader for static declarations, not an AML interpreter.
Buffers, packages and method bodies are opaque. Conditional declarations are
never accepted. Integer encodings and FieldList follow ACPI chapter 20.

10/25/26 (robustness, L20): aml_sleep_type(), a sleep state's SLP_TYP values from
its Name (\_S5_ for poweroff), found the same way: at the root, unconditional, once.
*/
#include <sys/amlfield.h>
#include <string.h>

#define DEPTH 24
struct cursor { const u8 *p, *end; };
struct path { u32 part[DEPTH]; u32 count; };
struct name { const u8 *segments; u32 count, up; bool root; };
struct value { u64 number; char ref[4]; bool indirect; u32 found; };
struct reader {
    const char *region, *field;
    struct path region_scope, field_scope;
    struct value address, length;
    struct cursor fields;
    u32 regions, blocks;
    bool constants;
};

static bool lead(u8 c) { return (c >= 'A' && c <= 'Z') || c == '_'; }
static bool nameseg(const u8 *s) {
    u32 i;
    if(!lead(s[0])) return false;
    for(i = 1; i < 4; i++) if(!lead(s[i]) && (s[i] < '0' || s[i] > '9')) return false;
    return true;
}
static bool take(struct cursor *c, u32 bytes) {
    if((u32)(c->end - c->p) < bytes) return false;
    c->p += bytes;
    return true;
}
static bool read_name(struct cursor *c, struct name *n) {
    u32 i;
    memset(n, 0, sizeof(*n));
    if(c->p == c->end) return false;
    if(*c->p == '\\') { n->root = true; c->p++; }
    else while(c->p < c->end && *c->p == '^') { n->up++; c->p++; }
    if(c->p == c->end) return false;
    if(*c->p == 0) { c->p++; return true; }
    n->count = 1;
    if(*c->p == 0x2e) { n->count = 2; c->p++; }
    else if(*c->p == 0x2f) {
        if(!take(c, 2)) return false;
        n->count = c->p[-1];
        if(!n->count) return false;
    }
    n->segments = c->p;
    if(!take(c, n->count * 4)) return false;
    for(i = 0; i < n->count; i++) if(!nameseg(n->segments + i * 4)) return false;
    return true;
}
static bool simple(const struct name *n) { return n->count == 1 && !n->root && !n->up; }
static bool matches(const struct name *n, const char *target) {
    return n->count && !memcmp(n->segments + (n->count - 1) * 4, target, 4);
}
static bool same_path(const struct path *a, const struct path *b) {
    return a->count == b->count && !memcmp(a->part, b->part, a->count * 4);
}
static bool extend(struct path *out, const struct path *base, const struct name *n) {
    *out = *base;
    if(n->root) out->count = 0;
    if(n->up > out->count) return false;
    out->count -= n->up;
    if(n->count > DEPTH - out->count) return false;
    if(n->count) memcpy(out->part + out->count, n->segments, n->count * 4);
    out->count += n->count;
    return true;
}

/* The same encoding means bytes including PkgLength for package boundaries,
   but an independent bit count for FieldList entries. Keep those uses apart. */
static bool pkg_length(struct cursor *c, u32 *out) {
    u32 value, extra, i;
    u8 first;
    if(!take(c, 1)) return false;
    first = c->p[-1]; extra = first >> 6;
    if(extra && (first & 0x30)) return false;
    value = first & (extra ? 0x0f : 0x3f);
    if(!take(c, extra)) return false;
    for(i = 0; i < extra; i++) value |= (u32)(c->p - extra)[i] << (4 + i * 8);
    *out = value;
    return true;
}
static bool package(struct cursor *c, struct cursor *body) {
    const u8 *start = c->p;
    u32 length;
    if(!pkg_length(c, &length) || length < (u32)(c->p - start) || length > (u32)(c->end - start))
        return false;
    body->p = c->p; body->end = start + length;
    c->p = body->end;
    return true;
}
static int literal(struct cursor *c, u64 *out) {
    u32 bytes, i;
    u64 number = 0;
    if(c->p == c->end) return AML_FIELD_INVALID;
    switch(*c->p) {
        case 0: c->p++; *out = 0; return 0;
        case 1: c->p++; *out = 1; return 0;
        case 0xff: c->p++; *out = ~(u64)0; return 0;
        case 0x0a: bytes = 1; break;
        case 0x0b: bytes = 2; break;
        case 0x0c: bytes = 4; break;
        case 0x0e: bytes = 8; break;
        default: return AML_FIELD_UNSUPPORTED;
    }
    c->p++;
    if(!take(c, bytes)) return AML_FIELD_INVALID;
    for(i = 0; i < bytes; i++) number |= (u64)(c->p - bytes)[i] << (i * 8);
    *out = number;
    return 0;
}
static int region_value(struct cursor *c, struct value *v) {
    struct name n;
    int result = literal(c, &v->number);
    if(result != AML_FIELD_UNSUPPORTED) return result;
    if(!read_name(c, &n) || !simple(&n)) return AML_FIELD_UNSUPPORTED;
    memcpy(v->ref, n.segments, 4); v->indirect = true;
    return 0;
}

/* Skip the syntax of an expression without evaluating it. Bare names are
   opaque references: at declaration level any following call arguments are
   visited independently. No raw payload is ever searched for declarations. */
static int expression(struct cursor *c, u32 depth) {
    struct cursor body;
    struct name n;
    u64 ignored;
    u32 op, args = 0, i;
    int result;
    if(depth == DEPTH || c->p == c->end) return AML_FIELD_INVALID;
    result = literal(c, &ignored);
    if(result != AML_FIELD_UNSUPPORTED) return result;
    op = *c->p++;
    if(lead(op) || op == '\\' || op == '^' || op == 0x2e || op == 0x2f) {
        c->p--; return read_name(c, &n) ? 0 : AML_FIELD_INVALID;
    }
    if(op >= 0x60 && op <= 0x6e) return 0; /* Local0..7, Arg0..6 */
    switch(op) {
        case 0x0d:
            while(c->p < c->end && *c->p) {
                if(*c->p++ > 0x7f) return AML_FIELD_INVALID;
            }
            return take(c, 1) ? 0 : AML_FIELD_INVALID;
        case 0x11: case 0x12: case 0x13:
            return package(c, &body) ? 0 : AML_FIELD_INVALID;
        case 0x71: case 0x75: case 0x76: case 0x83: case 0x87: case 0x8e: case 0x92: case 0xa4:
            args = 1; break;
        case 0x70: case 0x80: case 0x81: case 0x82: case 0x86: case 0x90: case 0x91:
        case 0x93: case 0x94: case 0x95: case 0x96: case 0x97: case 0x98: case 0x99: case 0x9d:
            args = 2; break;
        case 0x72: case 0x73: case 0x74: case 0x77: case 0x79: case 0x7a: case 0x7b:
        case 0x7c: case 0x7d: case 0x7e: case 0x7f: case 0x84: case 0x85: case 0x88:
        case 0x8a: case 0x8b: case 0x8c: case 0x8d: case 0x8f: case 0x9c:
            args = 3; break;
        case 0x78: case 0x9e: args = 4; break;
        case 0x89:
            for(i = 0; i < 4; i++) {
                result = expression(c, depth + 1);
                if(result) return result;
                if(i < 2 && !take(c, 1)) return AML_FIELD_INVALID;
            }
            return 0;
        case 0x9f: case 0xa3: case 0xa5: case 0xcc: return 0;
        case 0x5b:
            if(!take(c, 1)) return AML_FIELD_INVALID;
            switch(c->p[-1]) {
                case 0x12: case 0x20: case 0x25: case 0x28: case 0x29: args = 2; break;
                case 0x13: args = 4; break;
                case 0x1f: args = 6; break;
                case 0x21: case 0x22: case 0x24: case 0x26: case 0x27: case 0x2a: args = 1; break;
                case 0x23:
                    result = expression(c, depth + 1);
                    return result ? result : take(c, 2) ? 0 : AML_FIELD_INVALID;
                case 0x30: case 0x31: case 0x33: return 0;
                case 0x32:
                    if(!take(c, 5)) return AML_FIELD_INVALID;
                    args = 1; break;
                default: return AML_FIELD_UNSUPPORTED;
            }
            break;
        default: return AML_FIELD_UNSUPPORTED;
    }
    for(i = 0; i < args; i++) {
        result = expression(c, depth + 1);
        if(result) return result;
    }
    return 0;
}

static int named_constant(struct reader *r, struct cursor value, const struct name *n,
                          const struct path *scope, bool conditional) {
    struct value *v;
    u32 i;
    int result;
    for(i = 0; i < 2; i++) {
        struct cursor initializer = value;
        v = i ? &r->length : &r->address;
        if(!v->indirect || !matches(n, v->ref)) continue;
        if(++v->found != 1) return AML_FIELD_AMBIGUOUS;
        if(conditional || !simple(n) || !same_path(scope, &r->region_scope)) return AML_FIELD_UNSUPPORTED;
        result = literal(&initializer, &v->number);
        if(result) return result;
    }
    return 0;
}

static int walk(struct reader *r, struct cursor c, const struct path *scope, u32 depth, bool conditional) {
    struct cursor body, saved;
    struct name n, ignored;
    struct path child;
    u32 op, extra;
    int result;
    if(depth == DEPTH) return AML_FIELD_INVALID;
    while(c.p < c.end) {
        saved = c; op = *c.p++;
        if(op == 0x5b) {
            if(!take(&c, 1)) return AML_FIELD_INVALID;
            op = 0x5b00 | c.p[-1];
        }
        switch(op) {
            case 0x08:
                if(!read_name(&c, &n)) return AML_FIELD_INVALID;
                if(r->constants && (result = named_constant(r, c, &n, scope, conditional))) return result;
                result = expression(&c, 0);
                if(result) return result;
                break;
            case 0x06:
                if(!read_name(&c, &n) || !read_name(&c, &ignored)) return AML_FIELD_INVALID;
                if(matches(&ignored, r->region) || matches(&ignored, r->field)) return AML_FIELD_UNSUPPORTED;
                if(r->constants && ((r->address.indirect && matches(&ignored, r->address.ref)) ||
                   (r->length.indirect && matches(&ignored, r->length.ref)))) return AML_FIELD_UNSUPPORTED;
                break;
            case 0x15:
                if(!read_name(&c, &n) || !take(&c, 2)) return AML_FIELD_INVALID;
                break;
            case 0x10: case 0x5b82: case 0x5b83: case 0x5b84: case 0x5b85:
                if(!package(&c, &body) || !read_name(&body, &n) || !extend(&child, scope, &n))
                    return AML_FIELD_INVALID;
                extra = op == 0x5b83 ? 6 : op == 0x5b84 ? 3 : 0;
                if(!take(&body, extra)) return AML_FIELD_INVALID;
                result = walk(r, body, &child, depth + 1, conditional);
                if(result) return result;
                break;
            case 0xa0: case 0xa1: case 0xa2:
                if(!package(&c, &body)) return AML_FIELD_INVALID;
                if(op != 0xa1 && (result = expression(&body, 0))) return result;
                result = walk(r, body, scope, depth + 1, true);
                if(result) return result;
                break;
            case 0x14:
                if(!package(&c, &body) || !read_name(&body, &n) || !take(&body, 1))
                    return AML_FIELD_INVALID;
                break;
            case 0x5b01: case 0x5b02:
                if(!read_name(&c, &n) || (op == 0x5b01 && !take(&c, 1))) return AML_FIELD_INVALID;
                break;
            case 0x5b80:
                if(!read_name(&c, &n) || !take(&c, 1)) return AML_FIELD_INVALID;
                if(!r->constants && matches(&n, r->region)) {
                    if(++r->regions != 1) return AML_FIELD_AMBIGUOUS;
                    if(conditional || !simple(&n) || c.p[-1]) return AML_FIELD_UNSUPPORTED;
                    r->region_scope = *scope;
                    result = region_value(&c, &r->address);
                    if(!result) result = region_value(&c, &r->length);
                } else {
                    result = expression(&c, 0);
                    if(!result) result = expression(&c, 0);
                }
                if(result) return result;
                break;
            case 0x5b81: case 0x5b86: case 0x5b87:
                if(!package(&c, &body) || !read_name(&body, &n)) return AML_FIELD_INVALID;
                if(!r->constants && matches(&n, r->region)) {
                    if(++r->blocks != 1) return AML_FIELD_AMBIGUOUS;
                    if(conditional || !simple(&n) || op != 0x5b81) return AML_FIELD_UNSUPPORTED;
                    if(!take(&body, 1)) return AML_FIELD_INVALID;
                    if((body.p[-1] & 0x80) || (body.p[-1] & 15) > 5 || (body.p[-1] & 0x60) == 0x60)
                        return AML_FIELD_INVALID;
                    r->field_scope = *scope;
                    r->fields = body;
                }
                break;
            case 0x5b88:
                if(!read_name(&c, &n)) return AML_FIELD_INVALID;
                if(matches(&n, r->region)) return AML_FIELD_UNSUPPORTED;
                for(extra = 0; extra < 3; extra++) {
                    result = expression(&c, 0);
                    if(result) return result;
                }
                break;
            default:
                c = saved;
                result = expression(&c, 0);
                if(result) return result;
                break;
        }
    }
    return 0;
}

static int field_layout(const struct reader *r, struct aml_field *out) {
    struct cursor c = r->fields;
    const u8 *name;
    u32 offset = 0, bits, found = 0;
    while(c.p < c.end) {
        name = 0;
        if(*c.p == 1) { /* AccessField changes access semantics, not bit position. */
            if(!take(&c, 3)) return AML_FIELD_INVALID;
            if((c.p[-2] & 0x30) || (c.p[-2] & 15) > 5) return AML_FIELD_INVALID;
            continue;
        }
        if(*c.p == 2 || *c.p == 3) return AML_FIELD_UNSUPPORTED;
        if(*c.p == 0) c.p++;
        else {
            name = c.p;
            if(!take(&c, 4) || !nameseg(name)) return AML_FIELD_INVALID;
        }
        if(!pkg_length(&c, &bits) || (name && !bits) || bits > 0xffffffffU - offset)
            return AML_FIELD_INVALID;
        if(name && !memcmp(name, r->field, 4)) {
            if(++found > 1) return AML_FIELD_AMBIGUOUS;
            out->bit_offset = offset; out->bit_width = bits;
        }
        offset += bits;
        if((u64)offset > r->length.number * 8) return AML_FIELD_INVALID;
    }
    return found ? 0 : AML_FIELD_NOT_FOUND;
}

/* A literal initializer alone does not make an AML Name immutable. Refuse
   every other occurrence, including possible stores hidden in a method or
   an unevaluated branch. False positives in opaque data are deliberately
   refused; no execution or method argument resolution is needed. */
static bool constant_unused(const u8 *table, u32 bytes, const struct reader *r, const struct value *v) {
    u32 i, seen = 0, expected = 1;
    if(!v->indirect) return true;
    if(r->address.indirect && !memcmp(v->ref, r->address.ref, 4)) expected++;
    if(r->length.indirect && !memcmp(v->ref, r->length.ref, 4)) expected++;
    for(i = 36; i <= bytes - 4; i++) if(!memcmp(table + i, v->ref, 4) && ++seen > expected) return false;
    return seen == expected;
}

int aml_field_find(const u8 *table, u32 bytes, const char region[4],
                   const char field[4], struct aml_field *out) {
    struct reader r;
    struct path root;
    struct cursor c;
    struct aml_field result;
    u32 size;
    int status;
    if(out) memset(out, 0, sizeof(*out));
    if(!out || !table || !region || !field || !nameseg((const u8 *)region) || !nameseg((const u8 *)field) ||
       bytes < 36 || bytes > AML_FIELD_MAX_TABLE) return AML_FIELD_INVALID;
    memcpy(&size, table + 4, 4);
    if(size != bytes || (memcmp(table, "DSDT", 4) && memcmp(table, "SSDT", 4))) return AML_FIELD_INVALID;
    memset(&r, 0, sizeof(r)); memset(&root, 0, sizeof(root)); memset(&result, 0, sizeof(result));
    r.region = region; r.field = field; c.p = table + 36; c.end = table + bytes;
    status = walk(&r, c, &root, 0, false);
    if(status) return status;
    if(!r.regions || !r.blocks) return AML_FIELD_NOT_FOUND;
    if(!same_path(&r.region_scope, &r.field_scope)) return AML_FIELD_UNSUPPORTED;
    if(r.address.indirect || r.length.indirect) {
        r.constants = true;
        status = walk(&r, c, &root, 0, false);
        if(status) return status;
        if((r.address.indirect && !r.address.found) || (r.length.indirect && !r.length.found))
            return AML_FIELD_NOT_FOUND;
        if(!constant_unused(table, bytes, &r, &r.address) || !constant_unused(table, bytes, &r, &r.length))
            return AML_FIELD_UNSUPPORTED;
    }
    if(!r.length.number || r.length.number > 0xffffffffU || r.address.number > ~(u64)0 - r.length.number)
        return AML_FIELD_INVALID;
    status = field_layout(&r, &result);
    if(status) return status;
    result.address = r.address.number; result.region_length = r.length.number;
    *out = result;
    return 0;
}

/**** 10/25/26 (robustness, L20): a sleep state's package ****/

struct hunt { const char *name; struct cursor value; u32 found; };

/* The Name of hunt->name at the root of the namespace: its value's cursor. The walk is walk()'s, with only the
   declarations that hold names: scopes and devices are entered, If/Else/While bodies too (a match there is refused),
   method bodies, buffers and packages stay opaque. */
static int hunt_walk(struct hunt *h, struct cursor c, const struct path *scope, u32 depth, bool conditional) {
    struct cursor body, saved;
    struct name n, ignored;
    struct path child;
    u32 op, extra;
    int result;
    if(depth == DEPTH) return AML_FIELD_INVALID;
    while(c.p < c.end) {
        saved = c; op = *c.p++;
        if(op == 0x5b) {
            if(!take(&c, 1)) return AML_FIELD_INVALID;
            op = 0x5b00 | c.p[-1];
        }
        switch(op) {
            case 0x08:
                if(!read_name(&c, &n) || !extend(&child, scope, &n)) return AML_FIELD_INVALID;
                if(child.count == 1 && !memcmp(child.part, h->name, 4)) {
                    if(++h->found != 1) return AML_FIELD_AMBIGUOUS;
                    if(conditional) return AML_FIELD_UNSUPPORTED;
                    h->value = c;
                }
                result = expression(&c, 0);
                if(result) return result;
                break;
            case 0x06:
                if(!read_name(&c, &n) || !read_name(&c, &ignored)) return AML_FIELD_INVALID;
                if(matches(&ignored, h->name)) return AML_FIELD_UNSUPPORTED;    /* an alias may stand for it */
                break;
            case 0x15:
                if(!read_name(&c, &n) || !take(&c, 2)) return AML_FIELD_INVALID;
                break;
            case 0x10: case 0x5b82: case 0x5b83: case 0x5b84: case 0x5b85:
                if(!package(&c, &body) || !read_name(&body, &n) || !extend(&child, scope, &n))
                    return AML_FIELD_INVALID;
                extra = op == 0x5b83 ? 6 : op == 0x5b84 ? 3 : 0;
                if(!take(&body, extra)) return AML_FIELD_INVALID;
                result = hunt_walk(h, body, &child, depth + 1, conditional);
                if(result) return result;
                break;
            case 0xa0: case 0xa1: case 0xa2:
                if(!package(&c, &body)) return AML_FIELD_INVALID;
                if(op != 0xa1 && (result = expression(&body, 0))) return result;
                result = hunt_walk(h, body, scope, depth + 1, true);
                if(result) return result;
                break;
            case 0x14:
                if(!package(&c, &body) || !read_name(&body, &n) || !take(&body, 1)) return AML_FIELD_INVALID;
                break;
            case 0x5b01: case 0x5b02:
                if(!read_name(&c, &n) || (op == 0x5b01 && !take(&c, 1))) return AML_FIELD_INVALID;
                break;
            case 0x5b80:
                if(!read_name(&c, &n) || !take(&c, 1)) return AML_FIELD_INVALID;
                result = expression(&c, 0);
                if(!result) result = expression(&c, 0);
                if(result) return result;
                break;
            case 0x5b81: case 0x5b86: case 0x5b87:
                if(!package(&c, &body)) return AML_FIELD_INVALID;
                break;
            case 0x5b88:
                if(!read_name(&c, &n)) return AML_FIELD_INVALID;
                for(extra = 0; extra < 3; extra++) {
                    result = expression(&c, 0);
                    if(result) return result;
                }
                break;
            default:
                c = saved;
                result = expression(&c, 0);
                if(result) return result;
                break;
        }
    }
    return 0;
}

/* Package { SLP_TYPa, SLP_TYPb, ... } of integer constants, as ACPICA reads one: with a single element, its low byte
   is SLP_TYPa and the next one SLP_TYPb; with more, the first two. NumElements may claim more than the package holds:
   the elements there count. */
static int sleep_package(struct cursor c, u8 *typa, u8 *typb) {
    struct cursor body;
    u64 value[2];
    u32 count, n = 0;
    int result;
    if(c.p == c.end || *c.p != 0x12) return AML_FIELD_UNSUPPORTED;    /* not a Package: a Method, say */
    c.p++;
    if(!package(&c, &body) || !take(&body, 1)) return AML_FIELD_INVALID;
    count = body.p[-1];
    while(n < 2 && n < count && body.p < body.end) {
        result = literal(&body, &value[n]);
        if(result) return result;                   /* a name or an expression: nothing evaluates it here */
        n++;
    }
    if(!n) return AML_FIELD_INVALID;
    *typa = (u8)value[0];
    *typb = n == 1 ? (u8)(value[0] >> 8) : (u8)value[1];
    return 0;
}

int aml_sleep_type(const u8 *table, u32 bytes, const char name[4], u8 *typa, u8 *typb) {
    struct hunt h;
    struct path root;
    struct cursor c;
    u32 size;
    u8 a, b;
    int status;
    if(typa) *typa = 0;
    if(typb) *typb = 0;
    if(!typa || !typb || !table || !name || !nameseg((const u8 *)name) || bytes < 36 || bytes > AML_FIELD_MAX_TABLE)
        return AML_FIELD_INVALID;
    memcpy(&size, table + 4, 4);
    if(size != bytes || (memcmp(table, "DSDT", 4) && memcmp(table, "SSDT", 4))) return AML_FIELD_INVALID;
    memset(&h, 0, sizeof(h)); memset(&root, 0, sizeof(root));
    h.name = name; c.p = table + 36; c.end = table + bytes;
    status = hunt_walk(&h, c, &root, 0, false);
    if(status) return status;
    if(!h.found) return AML_FIELD_NOT_FOUND;
    status = sleep_package(h.value, &a, &b);
    if(status) return status;
    *typa = a; *typb = b;
    return 0;
}
