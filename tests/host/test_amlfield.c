/*
SurfOS AML Field Reader Tests
--------------------
File: test_amlfield.c Date: 10/12/26
--------------------
Hand-built bytecode exercises the static layout subset and hostile lengths.
*/
#include <sys/amlfield.h>
#include <string.h>
#include <stdio.h>

static unsigned checks, failures;
static u8 table[4096];
static u32 used;
static void check(bool ok, const char *what) {
    checks++;
    if(!ok) { failures++; printf("FAIL: %s\n", what); }
}
static void byte(u8 n) { table[used++] = n; }
static void name(const char *s) { memcpy(table + used, s, 4); used += 4; }
static void integer(u64 n, u32 size) {
    u32 i;
    byte(size == 1 ? 0x0a : size == 2 ? 0x0b : size == 4 ? 0x0c : 0x0e);
    for(i = 0; i < size; i++) byte(n >> (i * 8));
}
static void begin(void) {
    memset(table, 0, sizeof(table));
    memcpy(table, "DSDT", 4); used = 36;
}
static void finish(void) { memcpy(table + 4, &used, 4); }
static void pkg(u32 value) {
    u32 extra = value < 64 ? 0 : value < 4096 ? 1 : value < 1048576 ? 2 : 3;
    u32 i;
    if(!extra) { byte(value); return; }
    byte((extra << 6) | (value & 15));
    for(i = 0; i < extra; i++) byte(value >> (4 + i * 8));
}
static void wrap(u8 op, const u8 *prefix, u32 bytes) {
    u8 saved[4096];
    u32 size = used - 36, length = size + bytes + 1;
    memcpy(saved, table + 36, size);
    if(length >= 64) length++;
    begin(); byte(op); pkg(length);
    memcpy(table + used, prefix, bytes); used += bytes;
    memcpy(table + used, saved, size); used += size;
}
static void region(void) {
    byte(0x5b); byte(0x80); name("GNVS"); byte(0);
    integer(0x12345000, 4); integer(0x400, 2);
}
static void field(void) {
    byte(0x5b); byte(0x81); byte(18); name("GNVS"); byte(0x10);
    byte(0); byte(3); name("BITS"); byte(5);
    name("TEST"); byte(16);
}
static int find(struct aml_field *out) {
    finish(); return aml_field_find(table, used, "GNVS", "TEST", out);
}
static void expect_fail(int wanted, const char *why) {
    struct aml_field out;
    struct aml_field zero;
    memset(&out, 0xa5, sizeof(out)); memset(&zero, 0, sizeof(zero));
    check(find(&out) == wanted, why);
    check(!memcmp(&out, &zero, sizeof(out)), "failure clears every output byte");
}

static void advanced(void) {
    struct aml_field out;
    u32 before, i, j, length, seed = 1234;
    u8 valid[256];
    static const u8 scope[] = { '_', 'S', 'B', '_' };
    static const u8 method[] = { 'M', 'T', 'H', 'D', 0 };
    static const u8 root[] = { '\\', 0 };
    static const u8 one[] = { 1 };

    begin(); region(); field(); wrap(0x10, scope, 4);
    check(find(&out) == 0, "scope traversal finds nested declarations");
    wrap(0x10, root, 2);
    check(find(&out) == 0, "root scope resets the namespace path");
    begin(); region(); field(); wrap(0xa0, one, 1);
    expect_fail(AML_FIELD_UNSUPPORTED, "conditional declarations cannot become physical reads");
    begin(); region(); field(); wrap(0x14, method, 5);
    expect_fail(AML_FIELD_NOT_FOUND, "method-local declaration templates are opaque");
    begin(); region(); field(); wrap(0x11, one, 1);
    expect_fail(AML_FIELD_NOT_FOUND, "bytes inside Buffer cannot counterfeit declarations");
    begin(); region(); field(); wrap(0x12, one, 1);
    expect_fail(AML_FIELD_NOT_FOUND, "bytes inside Package cannot counterfeit declarations");
    begin(); field(); wrap(0x10, scope, 4); region();
    expect_fail(AML_FIELD_UNSUPPORTED, "same NameSeg in another namespace is not the region");

    begin(); byte(0x08); name("BASE"); integer(0xabcdef00, 4);
    byte(0x08); name("SIZE"); integer(4096, 2);
    byte(0x5b); byte(0x80); name("GNVS"); byte(0); name("BASE"); name("SIZE"); field();
    check(find(&out) == 0 && out.address == 0xabcdef00 && out.region_length == 4096,
          "same-scope literal Name constants resolve without evaluating AML");
    before = used;
    byte(0x08); name("BASE"); integer(0xabcdef00, 4);
    expect_fail(AML_FIELD_AMBIGUOUS, "duplicate constant definitions rejected");
    used = before; byte(0x06); name("SIZE"); name("BASE");
    expect_fail(AML_FIELD_UNSUPPORTED, "an alias cannot shadow an address constant");
    used = before; byte(0x70); integer(0xdead0000, 4); name("BASE");
    expect_fail(AML_FIELD_UNSUPPORTED, "a Store target invalidates an indirect address initializer");
    used = before; byte(0x75); name("SIZE");
    expect_fail(AML_FIELD_UNSUPPORTED, "an increment invalidates an indirect length initializer");
    used = before; byte(0x14); byte(16); name("MTHD"); byte(0);
    byte(0x70); integer(0xdead0000, 4); name("BASE");
    expect_fail(AML_FIELD_UNSUPPORTED, "a possible mutation inside an opaque method is refused");
    used = before; byte(0x0d); name("BASE"); byte(0);
    expect_fail(AML_FIELD_UNSUPPORTED, "opaque mentions of a constant are conservatively refused");
    used = before; table[41] = 0x60;
    expect_fail(AML_FIELD_UNSUPPORTED, "dynamic Name initializer is not a constant");

    begin(); byte(0x08); name("BOTH"); integer(4096, 2);
    byte(0x5b); byte(0x80); name("GNVS"); byte(0); name("BOTH"); name("BOTH"); field();
    check(find(&out) == 0 && out.address == 4096 && out.region_length == 4096,
          "one immutable Name can provide both region operands");

    begin(); byte(0x5b); byte(0x80); name("GNVS"); byte(0); name("MISS"); integer(4096, 2); field();
    expect_fail(AML_FIELD_NOT_FOUND, "unresolved region constants rejected");
    begin(); byte(0x5b); byte(0x80); name("GNVS"); byte(0); byte(0x72); byte(1); byte(1); byte(0); byte(1); field();
    expect_fail(AML_FIELD_UNSUPPORTED, "region address arithmetic is not evaluated");
    begin(); byte(0x5b); byte(0x80); name("GNVS"); byte(0); integer(0xffffffffffffffffULL, 8); byte(1); field();
    expect_fail(AML_FIELD_INVALID, "region physical end cannot wrap");
    begin(); byte(0x5b); byte(0x80); name("GNVS"); byte(0); byte(0); integer(0x100000000ULL, 8); field();
    expect_fail(AML_FIELD_INVALID, "region length must fit the interface");
    for(i = 1; i <= 8; i *= 2) {
        begin(); byte(0x5b); byte(0x80); name("GNVS"); byte(0); integer(32, i); integer(32, i); field();
        check(find(&out) == 0 && out.address == 32 && out.region_length == 32,
              "Byte/Word/DWord/QWord constants use identical value semantics");
    }

    begin(); region();
    byte(0x5b); byte(0x81); byte(16); name("GNVS"); byte(0);
    byte(1); byte(2); byte(0); byte(0); byte(3); name("TEST"); byte(5);
    check(find(&out) == 0 && out.bit_offset == 3 && out.bit_width == 5,
          "AccessField does not consume offset bits");
    table[used - 10] = 3;
    expect_fail(AML_FIELD_UNSUPPORTED, "ExtendedAccessField is outside the supported subset");
    table[used - 10] = 2;
    expect_fail(AML_FIELD_UNSUPPORTED, "ConnectField is outside the supported subset");
    begin(); region();
    byte(0x5b); byte(0x81); byte(18); name("GNVS"); byte(0);
    byte(0); pkg(0x400); name("TEST"); byte(8); name("REST"); byte(8);
    table[53] = used - 53;
    check(find(&out) == 0 && out.bit_offset == 0x400, "multi-byte reserved-field bit length decoded");
    memcpy(table + used - 5, "TEST", 4);
    expect_fail(AML_FIELD_AMBIGUOUS, "duplicate requested field names rejected");
    begin(); region(); field(); table[50] = 0; table[49] = 1;
    expect_fail(AML_FIELD_INVALID, "fields cannot exceed the region length");
    begin(); region(); field(); table[58] = 0x80;
    expect_fail(AML_FIELD_INVALID, "reserved FieldFlags bit rejected");
    begin(); region(); field(); table[59] = '2';
    expect_fail(AML_FIELD_INVALID, "field names require a valid leading character");
    begin(); region(); field(); table[53] = 0x70;
    expect_fail(AML_FIELD_INVALID, "reserved PkgLength bits rejected");

    begin(); region(); field(); finish();
    check(aml_field_find(table, used + 1, "GNVS", "TEST", &out) == AML_FIELD_INVALID,
          "header and input lengths must agree");
    check(aml_field_find(table, AML_FIELD_MAX_TABLE + 1, "GNVS", "TEST", &out) == AML_FIELD_INVALID,
          "oversized table rejected before accessing its bytes");
    check(aml_field_find(table, 35, "GNVS", "TEST", &out) == AML_FIELD_INVALID &&
          aml_field_find(0, used, "GNVS", "TEST", &out) == AML_FIELD_INVALID &&
          aml_field_find(table, used, "1NVS", "TEST", &out) == AML_FIELD_INVALID &&
          aml_field_find(table, used, "GNVS", "TEST", 0) == AML_FIELD_INVALID,
          "invalid public API inputs rejected");
    memcpy(table, "MSDM", 4); expect_fail(AML_FIELD_INVALID, "only definition tables accepted");
    memcpy(table, "SSDT", 4); check(find(&out) == 0, "SSDT uses the same declaration parser");

    begin(); region(); field();
    for(i = 0; i < 25; i++) wrap(0x10, root, 2);
    expect_fail(AML_FIELD_INVALID, "namespace nesting has a fixed bound");
    begin(); region(); field(); length = used; memcpy(valid, table, used);
    for(i = 0; i < 5000; i++) {
        memcpy(table, valid, length); used = length;
        seed = seed * 1664525U + 1013904223U; j = 36 + seed % (length - 36);
        seed = seed * 1664525U + 1013904223U; table[j] = seed >> 24;
        if(find(&out) == 0) check(out.bit_width && out.bit_offset <= out.region_length * 8 &&
                                  out.bit_width <= out.region_length * 8 - out.bit_offset,
                                  "accepted mutations remain inside the reported region");
        else check(!out.address && !out.region_length && !out.bit_offset && !out.bit_width,
                   "rejected mutations expose no partial metadata");
    }
}

/* 10/25/26 (robustness, L20): \_S5_, soft off's SLP_TYP values, as poweroff reads them */
static int sleep(u8 *a, u8 *b) {
    finish(); return aml_sleep_type(table, used, "_S5_", a, b);
}
static void s5(const u8 *bytes, u32 count) {          /* Name (_S5_, ...) with bytes for its value */
    byte(0x08); name("_S5_");
    memcpy(table + used, bytes, count); used += count;
}
static void sleep_types(void) {
    static const u8 ovmf[] = { 0x12, 0x06, 0x04, 0x00, 0x00, 0x00, 0x00 };
    static const u8 intel[] = { 0x12, 0x08, 0x04, 0x0a, 0x07, 0x0a, 0x07, 0x00, 0x00 };
    static const u8 one[] = { 0x12, 0x05, 0x01, 0x0b, 0x05, 0x07 };
    static const u8 short_pkg[] = { 0x12, 0x04, 0x04, 0x0a, 0x05 };
    static const u8 named[] = { 0x12, 0x07, 0x02, 'S', 'L', 'P', 'A', 0x00 };
    static const u8 empty[] = { 0x12, 0x02, 0x00 };
    static const u8 integer_only[] = { 0x0a, 0x05 };
    static const u8 scope_root[] = { '\\', 0 };
    static const u8 scope_sb[] = { '_', 'S', 'B', '_' };
    static const u8 cond[] = { 1 };
    static const u8 method[] = { '_', 'S', '5', '_', 0 };
    u8 a = 9, b = 9, valid[256];
    u32 i, j, length, seed = 4321;

    begin(); s5(ovmf, sizeof(ovmf));
    check(sleep(&a, &b) == 0 && a == 0 && b == 0, "OVMF's _S5_: Package (4) { Zero, Zero, Zero, Zero }");
    begin(); s5(intel, sizeof(intel));
    check(sleep(&a, &b) == 0 && a == 7 && b == 7, "Intel's _S5_: Package (4) { 7, 7, Zero, Zero }");
    begin(); byte(0x08); byte('\\'); name("_S5_"); memcpy(table + used, intel, sizeof(intel)); used += sizeof(intel);
    check(sleep(&a, &b) == 0 && a == 7 && b == 7, "a root-prefixed name (\\_S5_)");
    begin(); s5(intel, sizeof(intel)); wrap(0x10, scope_root, 2);
    check(sleep(&a, &b) == 0 && a == 7, "inside Scope (\\)");
    begin(); s5(one, sizeof(one));
    check(sleep(&a, &b) == 0 && a == 5 && b == 7, "one element: its low byte is SLP_TYPa, the next SLP_TYPb");
    begin(); s5(short_pkg, sizeof(short_pkg));
    check(sleep(&a, &b) == 0 && a == 5 && b == 0, "NumElements claims 4, one is there: that one counts");
    begin(); region(); field(); byte(0x14); byte(6); name("MTHD"); byte(0); s5(intel, sizeof(intel));
    check(sleep(&a, &b) == 0 && a == 7, "other declarations around it are walked past");

    begin(); s5(intel, sizeof(intel)); wrap(0x10, scope_sb, 4);
    a = b = 9; check(sleep(&a, &b) == AML_FIELD_NOT_FOUND && !a && !b, "_S5_ in \\_SB_ is another name: not found, both 0");
    begin(); s5(intel, sizeof(intel)); wrap(0xa0, cond, 1);
    check(sleep(&a, &b) == AML_FIELD_UNSUPPORTED, "inside If: refused, never guessed");
    begin(); s5(intel, sizeof(intel)); s5(ovmf, sizeof(ovmf));
    check(sleep(&a, &b) == AML_FIELD_AMBIGUOUS, "declared twice: refused");
    begin(); s5(named, sizeof(named));
    check(sleep(&a, &b) == AML_FIELD_UNSUPPORTED, "an element that is a name: refused (nothing evaluates it)");
    begin(); s5(empty, sizeof(empty));
    check(sleep(&a, &b) == AML_FIELD_INVALID, "an empty package: invalid");
    begin(); s5(integer_only, sizeof(integer_only));
    check(sleep(&a, &b) == AML_FIELD_UNSUPPORTED, "a bare integer is not the package");
    begin(); byte(0x14); byte(9); memcpy(table + used, method, 5); used += 5; byte(0xa4); byte(0x0a); byte(5);
    check(sleep(&a, &b) == AML_FIELD_NOT_FOUND, "a Method called _S5_ is not the Name");
    begin(); byte(0x06); name("SLP5"); name("_S5_"); s5(intel, sizeof(intel));
    check(sleep(&a, &b) == AML_FIELD_UNSUPPORTED, "an Alias that makes another _S5_: refused");
    begin(); s5(intel, sizeof(intel)); finish();
    check(aml_sleep_type(table, used, "_S5_", &a, 0) == AML_FIELD_INVALID &&
          aml_sleep_type(table, used, "5S5_", &a, &b) == AML_FIELD_INVALID &&
          aml_sleep_type(table, used - 1, "_S5_", &a, &b) == AML_FIELD_INVALID, "bad arguments refused");
    memcpy(table, "SSDT", 4);
    check(sleep(&a, &b) == 0 && a == 7, "an SSDT is read the same way");

    begin(); s5(intel, sizeof(intel)); length = used;
    for(i = 36; i < length; i++) {
        used = i;
        check(sleep(&a, &b) != 0 && !a && !b, "every truncated _S5_ fails and says 0");
    }
    begin(); region(); field(); s5(intel, sizeof(intel)); length = used; memcpy(valid, table, used);
    for(i = 0; i < 5000; i++) {
        memcpy(table, valid, length); used = length;
        seed = seed * 1664525U + 1013904223U; j = 36 + seed % (length - 36);
        seed = seed * 1664525U + 1013904223U; table[j] = seed >> 24;
        if(sleep(&a, &b) != 0) check(!a && !b, "a refused mutation says 0 for both");
    }
}

/* Optional captured, ignored fixture checks. These addresses describe the
   October 12 capture only, never assumptions in the production reader. */
int open(const char *, int, ...);
long read(int, void *, unsigned long);
int close(int);
static void native(const char *dsdt_path, const char *ssdt_path) {
    static u8 input[AML_FIELD_MAX_TABLE + 1];
    static const struct { const char *region, *field; u64 base; u32 size, offset, bits; } want[] = {
        { "GNVS", "OSYS", 0x4bba0298, 0x7d4, 0, 16 },
        { "GNVS", "FMH1", 0x4bba0298, 0x7d4, 0x243, 16 },
        { "GNVS", "FML1", 0x4bba0298, 0x7d4, 0x245, 16 },
        { "GNVS", "FMD1", 0x4bba0298, 0x7d4, 0x247, 16 },
        { "PNVA", "PCHS", 0x4bba0018, 0x234, 0, 16 },
        { "PNVA", "SBRG", 0x4bba0018, 0x234, 0x8b, 32 },
        { "PNVA", "IM01", 0x4bba0018, 0x234, 0x11a, 8 },
        { "PNVA", "IC01", 0x4bba0018, 0x234, 0x127, 64 },
        { "LFCN", "TPTY", 0x4b311e98, 0xfd, 0x84, 8 },
        { "SANV", "M32B", 0x4bba0d18, 0x1f4, 0xf2, 32 },
        { "SANV", "M32L", 0x4bba0d18, 0x1f4, 0xf6, 32 }
    };
    struct aml_field out;
    u32 i, first, last, file;
    long bytes;
    int fd;
    for(file = 0; file < 2; file++) {
        fd = open(file ? ssdt_path : dsdt_path, 0);
        check(fd >= 0, "native fixture opens");
        if(fd < 0) return;
        bytes = read(fd, input, sizeof(input)); close(fd);
        check(bytes >= 36 && bytes <= (long)AML_FIELD_MAX_TABLE, "native fixture length is bounded");
        if(bytes < 36 || bytes > (long)AML_FIELD_MAX_TABLE) return;
        first = file ? 9 : 0; last = file ? 11 : 9;
        for(i = first; i < last; i++) check(aml_field_find(input, bytes, want[i].region, want[i].field, &out) == 0 &&
            out.address == want[i].base && out.region_length == want[i].size &&
            out.bit_offset == want[i].offset * 8 && out.bit_width == want[i].bits,
            "native metadata exactly matches the independently decoded AML layout");
    }
}

int main(int argc, char **argv) {
    struct aml_field out;
    u32 cut, length;
    begin(); region(); field();
    check(find(&out) == AML_FIELD_OK && out.address == 0x12345000 &&
          out.region_length == 0x400 && out.bit_offset == 8 && out.bit_width == 16,
          "literal SystemMemory region and bit fields");
    begin(); region(); field(); region(); expect_fail(AML_FIELD_AMBIGUOUS, "duplicate regions refused");
    begin(); region(); field(); field(); expect_fail(AML_FIELD_AMBIGUOUS, "duplicate Field blocks refused");
    begin(); region(); field(); table[42] = 1; expect_fail(AML_FIELD_UNSUPPORTED, "SystemIO is not readable memory");
    begin(); region(); field(); table[used - 1] = 0; expect_fail(AML_FIELD_INVALID, "zero-width named fields refused");
    begin(); region(); field(); table[used - 1] = 0x7f; expect_fail(AML_FIELD_INVALID, "truncated field length refused");
    begin(); region(); field(); table[53] = 63; expect_fail(AML_FIELD_INVALID, "package cannot escape input");
    begin(); region(); field(); table[53] = 2; expect_fail(AML_FIELD_INVALID, "package cannot end inside its name");
    begin(); region(); field(); table[50] = 0; expect_fail(AML_FIELD_INVALID, "zero-length region refused");
    begin(); region(); field(); length = used;
    for(cut = 36; cut < length; cut++) {
        used = cut;
        check(find(&out) != AML_FIELD_OK, "every truncated input fails");
    }
    begin(); field(); expect_fail(AML_FIELD_NOT_FOUND, "field alone supplies no region");
    begin(); region(); expect_fail(AML_FIELD_NOT_FOUND, "region alone supplies no field");
    advanced();
    sleep_types();
    if(argc == 3) native(argv[1], argv[2]);
    printf("AML FIELD UNIT TESTS %s: %u checks, %u failures\n", failures ? "FAILED" : "PASSED", checks, failures);
    return failures ? 1 : 0;
}
