/* Literal HID-over-I2C frames exercise the real pure framing helpers. */
#include <sys/i2chid.h>

int printf(const char *format, ...);

static unsigned checks, failures;
static void check(bool ok, const char *what) {
    checks++;
    if(!ok) { failures++; printf("FAIL: %s\n", what); }
}
static void fill(u8 *bytes, u_int len, u8 value) {
    u_int i;
    for(i = 0; i < len; i++) bytes[i] = value;
}
static bool equal(const u8 *a, const u8 *b, u_int len) {
    u_int i;
    for(i = 0; i < len; i++) if(a[i] != b[i]) return false;
    return true;
}
static bool filled(const u8 *bytes, u_int len, u8 value) {
    u_int i;
    for(i = 0; i < len; i++) if(bytes[i] != value) return false;
    return true;
}
static void copy(u8 *to, const u8 *from, u_int len) {
    u_int i;
    for(i = 0; i < len; i++) to[i] = from[i];
}

/* Nontrivial register high bytes catch reversed and truncated LE16 fields. */
static const u8 descriptor[30] = {
    0x1e, 0x00, 0x00, 0x01, 0x34, 0x02, 0x12, 0x34,
    0x56, 0x78, 0x00, 0x01, 0x9a, 0xbc, 0x80, 0x00,
    0xcd, 0xab, 0x34, 0x12, 0xf3, 0x04, 0x35, 0x31,
    0x20, 0x01, 0x00, 0x00, 0x00, 0x00
};
static const struct i2chid_desc commands = {
    .command_reg = 0xabcd, .data_reg = 0x1234
};

static void bad_descriptor(const u8 *bytes, u_int len, const char *what) {
    struct i2chid_desc out;
    fill((u8 *)&out, sizeof(out), 0xa5);
    check(!i2chid_parse_descriptor(bytes, len, &out), what);
    check(filled((const u8 *)&out, sizeof(out), 0xa5),
          "invalid descriptor leaves result unchanged");
}
static void test_descriptor(void) {
    struct i2chid_desc d;
    u8 bytes[33];
    unsigned i;
    static const unsigned needed_regs[] = {6, 8, 16, 18};
    fill((u8 *)&d, sizeof(d), 0);
    check(i2chid_parse_descriptor(descriptor, sizeof(descriptor), &d),
          "valid descriptor accepted");
    check(d.report_desc_len == 0x234 && d.report_desc_reg == 0x3412,
          "report descriptor fields decoded");
    check(d.input_reg == 0x7856 && d.max_input_len == 256,
          "input fields decoded");
    check(d.output_reg == 0xbc9a && d.max_output_len == 128,
          "output fields decoded");
    check(d.command_reg == 0xabcd && d.data_reg == 0x1234,
          "command and data fields decoded");
    check(d.vendor_id == 0x04f3 && d.product_id == 0x3135 && d.version_id == 0x0120,
          "device identity fields decoded");
    for(i = 0; i < 30; i++) bad_descriptor(descriptor, i, "truncated descriptor rejected");
    bad_descriptor(0, 30, "null descriptor rejected");
    check(!i2chid_parse_descriptor(descriptor, 30, 0), "null result rejected");
    copy(bytes + 1, descriptor, 30);
    bytes[31] = 0xff;
    check(i2chid_parse_descriptor(bytes + 1, 31, &d),
          "unaligned descriptor and trailing bytes accepted");
    copy(bytes, descriptor, 30);
    bytes[0] = 29;
    bad_descriptor(bytes, 30, "wrong descriptor length rejected");
    bytes[0] = 30; bytes[1] = 1;
    bad_descriptor(bytes, 30, "descriptor length high byte checked");
    copy(bytes, descriptor, 30);
    bytes[2] = 1;
    bad_descriptor(bytes, 30, "unsupported version rejected");
    bytes[2] = 0; bytes[3] = 0;
    bad_descriptor(bytes, 30, "zero version rejected");
    copy(bytes, descriptor, 30);
    bytes[4] = bytes[5] = 0;
    bad_descriptor(bytes, 30, "empty report descriptor rejected");
    bytes[4] = 0; bytes[5] = 16;
    check(i2chid_parse_descriptor(bytes, 30, &d), "4096-byte report descriptor accepted");
    bytes[4] = 1;
    bad_descriptor(bytes, 30, "oversized report descriptor rejected");
    copy(bytes, descriptor, 30);
    bytes[10] = 2; bytes[11] = 0;
    check(i2chid_parse_descriptor(bytes, 30, &d), "two-byte input accepted");
    bytes[10] = 1;
    bad_descriptor(bytes, 30, "one-byte input rejected");
    bytes[10] = 0;
    bad_descriptor(bytes, 30, "zero input rejected");
    bytes[10] = 1; bytes[11] = 1;
    bad_descriptor(bytes, 30, "257-byte input rejected");
    for(i = 0; i < sizeof(needed_regs) / sizeof(needed_regs[0]); i++) {
        copy(bytes, descriptor, 30);
        bytes[needed_regs[i]] = bytes[needed_regs[i] + 1] = 0;
        bad_descriptor(bytes, 30, "missing required register rejected");
    }
    copy(bytes, descriptor, 30);
    fill(bytes + 12, 4, 0);
    fill(bytes + 26, 4, 0xff);
    check(i2chid_parse_descriptor(bytes, 30, &d),
          "optional output and reserved bytes do not reject descriptor");
    check(d.output_reg == 0 && d.max_output_len == 0, "absent output fields preserved");
    bytes[18] = bytes[16]; bytes[19] = bytes[17];
    check(i2chid_parse_descriptor(bytes, 30, &d), "shared command/data register allowed");
}

static void frame(const u8 *actual, int length, const u8 *expected,
                  u_int expected_len, const char *what) {
    check(length == (int)expected_len, what);
    check(equal(actual, expected, expected_len), "exact command bytes");
    check(actual[expected_len] == 0xa5, "command does not overwrite trailing guard");
}
static void test_fixed_commands(void) {
    u8 out[16];
    u_int i;
    int n;
    struct i2chid_desc bad = commands;
    static const u8 on[] = {0xcd, 0xab, 0x00, 0x08};
    static const u8 sleep[] = {0xcd, 0xab, 0x01, 0x08};
    static const u8 reset[] = {0xcd, 0xab, 0x00, 0x01};
    fill(out, sizeof(out), 0xa5);
    n = i2chid_set_power(&commands, I2CHID_POWER_ON, out, 4);
    frame(out, n, on, sizeof(on), "power-on length");
    fill(out, sizeof(out), 0xa5);
    n = i2chid_set_power(&commands, I2CHID_POWER_SLEEP, out, 4);
    frame(out, n, sleep, sizeof(sleep), "sleep length");
    fill(out, sizeof(out), 0xa5);
    n = i2chid_reset(&commands, out, 4);
    frame(out, n, reset, sizeof(reset), "reset length");
    for(i = 0; i < 4; i++) {
        fill(out, sizeof(out), 0xa5);
        check(i2chid_reset(&commands, out, i) == -1, "short reset capacity rejected");
        check(filled(out, sizeof(out), 0xa5), "reset failure is atomic");
        check(i2chid_set_power(&commands, 0, out, i) == -1, "short power capacity rejected");
        check(filled(out, sizeof(out), 0xa5), "power failure is atomic");
    }
    fill(out, sizeof(out), 0xa5);
    check(i2chid_set_power(&commands, 2, out, sizeof(out)) == -1, "unknown power state rejected");
    check(i2chid_set_power(&commands, ~0u, out, sizeof(out)) == -1, "wide power state rejected");
    check(i2chid_set_power(0, 0, out, sizeof(out)) == -1, "null power descriptor rejected");
    check(i2chid_set_power(&commands, 0, 0, sizeof(out)) == -1, "null power output rejected");
    check(i2chid_reset(0, out, sizeof(out)) == -1, "null reset descriptor rejected");
    check(i2chid_reset(&commands, 0, sizeof(out)) == -1, "null reset output rejected");
    bad.command_reg = 0;
    check(i2chid_reset(&bad, out, sizeof(out)) == -1, "reset requires command register");
    check(i2chid_set_power(&bad, 0, out, sizeof(out)) == -1, "power requires command register");
    check(filled(out, sizeof(out), 0xa5), "bad fixed commands leave output unchanged");
}

static void test_get_report(void) {
    u8 out[16];
    u_int i;
    int n;
    struct i2chid_desc bad = commands;
    static const u8 unnumbered[] = {0xcd, 0xab, 0x10, 0x02, 0x34, 0x12};
    static const u8 id14[] = {0xcd, 0xab, 0x2e, 0x02, 0x34, 0x12};
    static const u8 id15[] = {0xcd, 0xab, 0x3f, 0x02, 0x0f, 0x34, 0x12};
    static const u8 id255[] = {0xcd, 0xab, 0x3f, 0x02, 0xff, 0x34, 0x12};
    fill(out, sizeof(out), 0xa5);
    n = i2chid_get_report(&commands, 1, 0, out, 6);
    frame(out, n, unnumbered, sizeof(unnumbered), "unnumbered GET_REPORT length");
    fill(out, sizeof(out), 0xa5);
    n = i2chid_get_report(&commands, 2, 14, out, 6);
    frame(out, n, id14, sizeof(id14), "compact report ID length");
    fill(out, sizeof(out), 0xa5);
    n = i2chid_get_report(&commands, 3, 15, out, 7);
    frame(out, n, id15, sizeof(id15), "ID 15 uses extended encoding");
    fill(out, sizeof(out), 0xa5);
    n = i2chid_get_report(&commands, 3, 255, out, 7);
    frame(out, n, id255, sizeof(id255), "ID 255 preserved");
    for(i = 0; i < 7; i++) {
        fill(out, sizeof(out), 0xa5);
        check(i2chid_get_report(&commands, 3, 15, out, i) == -1, "short GET_REPORT rejected");
        check(filled(out, sizeof(out), 0xa5), "short GET_REPORT leaves output unchanged");
    }
    fill(out, sizeof(out), 0xa5);
    check(i2chid_get_report(&commands, 0, 0, out, sizeof(out)) == -1, "zero report type rejected");
    check(i2chid_get_report(&commands, 4, 0, out, sizeof(out)) == -1, "unknown report type rejected");
    check(i2chid_get_report(&commands, ~0u, 0, out, sizeof(out)) == -1, "wide report type rejected");
    check(i2chid_get_report(&commands, 1, 256, out, sizeof(out)) == -1, "wide report ID rejected");
    check(i2chid_get_report(0, 1, 0, out, sizeof(out)) == -1, "null GET descriptor rejected");
    check(i2chid_get_report(&commands, 1, 0, 0, sizeof(out)) == -1, "null GET output rejected");
    bad.command_reg = 0;
    check(i2chid_get_report(&bad, 1, 0, out, sizeof(out)) == -1, "GET requires command register");
    bad = commands; bad.data_reg = 0;
    check(i2chid_get_report(&bad, 1, 0, out, sizeof(out)) == -1, "GET requires data register");
    check(filled(out, sizeof(out), 0xa5), "bad GET arguments leave output unchanged");
}

static void test_set_report(void) {
    u8 out[32];
    u_int i;
    int n;
    struct i2chid_desc bad = commands;
    static const u8 payload[] = {0x42, 0x81};
    static const u8 id0[] = {0xcd, 0xab, 0x20, 0x03, 0x34, 0x12, 0x04, 0x00, 0x42, 0x81};
    static const u8 id14[] = {0xcd, 0xab, 0x3e, 0x03, 0x34, 0x12, 0x05, 0x00, 0x0e, 0x42, 0x81};
    static const u8 id15[] = {0xcd, 0xab, 0x3f, 0x03, 0x0f, 0x34, 0x12, 0x05, 0x00, 0x0f, 0x42, 0x81};
    static const u8 id255[] = {0xcd, 0xab, 0x1f, 0x03, 0xff, 0x34, 0x12, 0x05, 0x00, 0xff, 0x42, 0x81};
    static const u8 empty[] = {0xcd, 0xab, 0x30, 0x03, 0x34, 0x12, 0x02, 0x00};
    static const u8 empty_id[] = {0xcd, 0xab, 0x31, 0x03, 0x34, 0x12, 0x03, 0x00, 0x01};
    fill(out, sizeof(out), 0xa5);
    n = i2chid_set_report(&commands, 2, 0, payload, 2, out, 10);
    frame(out, n, id0, sizeof(id0), "unnumbered SET_REPORT omits ID byte");
    fill(out, sizeof(out), 0xa5);
    n = i2chid_set_report(&commands, 3, 14, payload, 2, out, 11);
    frame(out, n, id14, sizeof(id14), "numbered SET_REPORT prefixes ID");
    fill(out, sizeof(out), 0xa5);
    n = i2chid_set_report(&commands, 3, 15, payload, 2, out, 12);
    frame(out, n, id15, sizeof(id15), "extended SET_REPORT contains both ID bytes");
    fill(out, sizeof(out), 0xa5);
    n = i2chid_set_report(&commands, 1, 255, payload, 2, out, 12);
    frame(out, n, id255, sizeof(id255), "SET_REPORT accepts all report types and full ID");
    fill(out, sizeof(out), 0xa5);
    n = i2chid_set_report(&commands, 3, 0, 0, 0, out, 8);
    frame(out, n, empty, sizeof(empty), "empty unnumbered report");
    fill(out, sizeof(out), 0xa5);
    n = i2chid_set_report(&commands, 3, 1, 0, 0, out, 9);
    frame(out, n, empty_id, sizeof(empty_id), "empty numbered report");
    for(i = 0; i < 12; i++) {
        fill(out, sizeof(out), 0xa5);
        check(i2chid_set_report(&commands, 3, 15, payload, 2, out, i) == -1,
              "short SET_REPORT rejected");
        check(filled(out, sizeof(out), 0xa5), "short SET_REPORT leaves output unchanged");
    }
    fill(out, sizeof(out), 0xa5);
    check(i2chid_set_report(&commands, 0, 0, payload, 2, out, 32) == -1, "SET zero type rejected");
    check(i2chid_set_report(&commands, 4, 0, payload, 2, out, 32) == -1, "SET unknown type rejected");
    check(i2chid_set_report(&commands, 3, 256, payload, 2, out, 32) == -1, "SET wide ID rejected");
    check(i2chid_set_report(&commands, 3, 0, 0, 2, out, 32) == -1, "SET null payload rejected");
    check(i2chid_set_report(0, 3, 0, payload, 2, out, 32) == -1, "SET null descriptor rejected");
    check(i2chid_set_report(&commands, 3, 0, payload, 2, 0, 32) == -1, "SET null output rejected");
    check(i2chid_set_report(&commands, 3, 0, payload, 65534, out, ~0u) == -1,
          "unnumbered report length overflow rejected before payload access");
    check(i2chid_set_report(&commands, 3, 1, payload, 65533, out, ~0u) == -1,
          "numbered report length overflow rejected before payload access");
    check(i2chid_set_report(&commands, 3, 255, payload, ~0u, out, ~0u) == -1,
          "integer wraparound report length rejected");
    bad.command_reg = 0;
    check(i2chid_set_report(&bad, 3, 1, payload, 2, out, 32) == -1, "SET requires command register");
    bad = commands; bad.data_reg = 0;
    check(i2chid_set_report(&bad, 3, 1, payload, 2, out, 32) == -1, "SET requires data register");
    check(filled(out, sizeof(out), 0xa5), "bad SET arguments leave output unchanged");
}

static void test_large_report(void) {
    static u8 payload[65533], out[65543];
    int n;
    fill(payload, sizeof(payload), 0x73);
    fill(out, sizeof(out), 0xa5);
    n = i2chid_set_report(&commands, 3, 0, payload, sizeof(payload), out, 65541);
    check(n == 65541, "maximum unnumbered report total length");
    check(out[6] == 0xff && out[7] == 0xff, "full LE16 report length encoded");
    check(filled(out + 8, sizeof(payload), 0x73), "large unnumbered payload preserved");
    check(out[65541] == 0xa5, "large unnumbered guard preserved");
    fill(out, sizeof(out), 0xa5);
    n = i2chid_set_report(&commands, 3, 255, payload, 65532, out, 65542);
    check(n == 65542, "maximum extended numbered report total length");
    check(out[7] == 0xff && out[8] == 0xff && out[9] == 0xff,
          "large numbered length and report ID encoded");
    check(filled(out + 10, 65532, 0x73), "large numbered payload preserved");
    check(out[65542] == 0xa5, "large numbered guard preserved");
}

static void input_result(const u8 *bytes, u_int received, u_int maximum,
                         enum i2chid_input_result expected, const char *what) {
    const u8 *payload = (const u8 *)1;
    u_int length = 0xfeed;
    check(i2chid_get_input(bytes, received, maximum, &payload, &length) == expected, what);
    if(expected != I2CHID_INPUT_DATA)
        check(payload == 0 && length == 0, "nondata input clears borrowed slice");
}
static void test_input(void) {
    u8 bytes[257];
    const u8 *payload;
    u_int length;
    fill(bytes, sizeof(bytes), 0x42);
    bytes[0] = 0; bytes[1] = 0;
    input_result(bytes, 2, 256, I2CHID_INPUT_RESET, "zero length indicates reset");
    input_result(bytes, 256, 256, I2CHID_INPUT_RESET, "reset ignores trailing padding");
    bytes[0] = 2;
    input_result(bytes, 2, 2, I2CHID_INPUT_EMPTY, "length two is empty");
    input_result(bytes, 256, 256, I2CHID_INPUT_EMPTY, "empty input ignores padding");
    bytes[0] = 5; bytes[2] = 15; bytes[3] = 0x12; bytes[4] = 0x34;
    payload = 0; length = 0;
    check(i2chid_get_input(bytes, 5, 256, &payload, &length) == I2CHID_INPUT_DATA,
          "complete input accepted");
    check(payload == bytes + 2 && length == 3 && payload[0] == 15,
          "input slice retains report ID");
    payload = 0; length = 0;
    check(i2chid_get_input(bytes, 256, 256, &payload, &length) == I2CHID_INPUT_DATA &&
          payload == bytes + 2 && length == 3, "input slice excludes padding");
    input_result(bytes, 4, 256, I2CHID_INPUT_INVALID, "truncated payload rejected");
    input_result(bytes, 5, 4, I2CHID_INPUT_INVALID, "length exceeds descriptor maximum");
    input_result(bytes, 0, 256, I2CHID_INPUT_INVALID, "missing input prefix rejected");
    input_result(bytes, 1, 256, I2CHID_INPUT_INVALID, "short input prefix rejected");
    input_result(0, 256, 256, I2CHID_INPUT_INVALID, "null input rejected");
    input_result(bytes, 5, 1, I2CHID_INPUT_INVALID, "invalid input maximum rejected");
    input_result(bytes, 5, 257, I2CHID_INPUT_INVALID, "oversized input maximum rejected");
    bytes[0] = 1;
    input_result(bytes, 256, 256, I2CHID_INPUT_INVALID, "one-byte report length rejected");
    bytes[0] = 0; bytes[1] = 1;
    payload = 0; length = 0;
    check(i2chid_get_input(bytes, 256, 256, &payload, &length) == I2CHID_INPUT_DATA &&
          length == 254 && payload == bytes + 2, "maximum input length accepted");
    bytes[0] = 1;
    input_result(bytes, 257, 256, I2CHID_INPUT_INVALID, "257-byte input rejected");
    bytes[0] = bytes[1] = 0xff;
    input_result(bytes, 256, 256, I2CHID_INPUT_INVALID, "0xffff input is not an empty report");
    bytes[1] = 3; bytes[2] = 0; bytes[3] = 0x42;
    payload = 0; length = 0;
    check(i2chid_get_input(bytes + 1, 3, 256, &payload, &length) == I2CHID_INPUT_DATA &&
          length == 1 && payload[0] == 0x42, "unaligned prefix decoded");
    length = 0xfeed;
    check(i2chid_get_input(bytes, 256, 256, 0, &length) == I2CHID_INPUT_INVALID && length == 0,
          "null payload result rejected and length cleared");
    payload = (const u8 *)1;
    check(i2chid_get_input(bytes, 256, 256, &payload, 0) == I2CHID_INPUT_INVALID && payload == 0,
          "null length result rejected and payload cleared");
}

int main(void) {
    test_descriptor();
    test_fixed_commands();
    test_get_report();
    test_set_report();
    test_large_report();
    test_input();
    printf("i2chid: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
