/* Host-side unit tests for driver/cp437.c, the console's character set (10/6/26): the code
   page 437 tables (round trips of all 256 entries, SurfOS's three places, Latin-1, the
   control characters), the UTF-8 decoder (ASCII, 2-, 3- and 4-byte sequences, characters
   outside the set, overlong forms, surrogates, values above U+10FFFF, stray continuation
   bytes, sequences cut short by ASCII or ESC, 0xFF), and the framebuffer font in the same
   order (every character that is not blank has a glyph, ▸ and ≋ have their own). Built
   like the kernel objects and run by `make unittest`. */
#include <stdio.h>
#include <string.h>
#include <surfos/types.h>
#include <sys/cp437.h>

void exit(int status);

static int failures, checks;

static void check(int ok, const char *what) {
    checks++;
    if(!ok) { failures++; printf("FAIL: %s\n", what); }
}

/* the cells a byte string comes out as, all of them in order; *last: what the last byte gave */
static int decode(struct utf8_dec *d, const char *s, int len, u8 *cells, int *last) {
    int n = 0, i, k = 0;
    u8 out[UTF8_OUT_MAX];
    for(i = 0; i < len; i++) {
        k = utf8_feed(d, (u8)s[i], out);
        memcpy(cells + n, out, k);
        n += k;
    }
    if(last) *last = k;
    return n;
}

/* the bytes s (with their length, so NUL and high bytes are fine) come out as exactly want */
static void expect(const char *s, int len, const char *want, int wlen, const char *what) {
    struct utf8_dec d;
    u8 cells[64];
    int n;
    memset(&d, 0, sizeof(d));
    n = decode(&d, s, len, cells, NULL);
    check(n == wlen && !memcmp(cells, want, wlen) && d.have == 0, what);
    if(n != wlen || memcmp(cells, want, wlen)) {
        int i;
        printf("      got %d cells:", n);
        for(i = 0; i < n; i++) printf(" %02x", cells[i]);
        printf("\n");
    }
}
#define EXPECT(s, want, what) expect(s, sizeof(s) - 1, want, sizeof(want) - 1, what)

static void test_tables(void) {
    char utf[CP437_UTF8_MAX];
    u8 out[UTF8_OUT_MAX];
    struct utf8_dec d;
    int i, ok, n, k, latin;

    for(ok = 1, i = 0; i < 256; i++) if(cp437_from_unicode(cp437_unicode[i]) != i) ok = 0;
    check(ok, "Unicode -> index -> Unicode: all 256 entries come back");
    /* every cell's UTF-8, decoded again, is the same cell (the empty cell 0x00 is a space) */
    for(ok = 1, i = 0; i < 256; i++) {
        memset(&d, 0, sizeof(d));
        n = cp437_utf8((u8)i, utf);
        for(k = 0; k < n - 1; k++) if(utf8_feed(&d, (u8)utf[k], out) != 0) ok = 0;
        if(utf8_feed(&d, (u8)utf[n - 1], out) != 1 || out[0] != (i ? i : ' ')) ok = 0;
        if(n != (cp437_unicode[i] < 0x80 ? 1 : cp437_unicode[i] < 0x800 ? 2 : 3)) ok = 0;
    }
    check(ok, "index -> UTF-8 -> decoder -> index: all 256 entries come back");

    check(cp437_unicode[0x10] == 0x25B8 && cp437_unicode[0xF7] == 0x224B && cp437_unicode[0xF9] == 0x2026,
          "SurfOS's places: ▸ at 0x10, ≋ at 0xF7, … at 0xF9");
    check(cp437_from_unicode(0x25B8) == 0x10 && cp437_from_unicode(0x25BA) == 0x10 && cp437_from_unicode(0x25B6) == 0x10,
          "▸ and VGA's ► and ▶ are drawn at 0x10");
    check(cp437_from_unicode(0x224B) == 0xF7 && cp437_from_unicode(0x2248) == 0xF7, "≋ and VGA's ≈ at 0xF7");
    check(cp437_from_unicode(0x2026) == 0xF9 && cp437_from_unicode(0x2219) == 0xF9, "… and VGA's ∙ at 0xF9");
    n = cp437_utf8(0x10, utf);
    check(n == 3 && !memcmp(utf, "\xE2\x96\xB8", 3), "0x10 goes to the terminal as ▸ (E2 96 B8)");
    n = cp437_utf8(0xF7, utf);
    check(n == 3 && !memcmp(utf, "\xE2\x89\x8B", 3), "0xF7 as ≋ (E2 89 8B)");
    n = cp437_utf8(0xF9, utf);
    check(n == 3 && !memcmp(utf, "\xE2\x80\xA6", 3), "0xF9 as … (E2 80 A6)");
    n = cp437_utf8(0x82, utf);
    check(n == 2 && !memcmp(utf, "\xC3\xA9", 2), "0x82 as é (C3 A9)");
    check(cp437_utf8('A', utf) == 1 && utf[0] == 'A' && cp437_utf8(0, utf) == 1 && utf[0] == ' ',
          "ASCII as itself, the empty cell as a space");
    check(cp437_unicode[0x01] == 0x263A && cp437_unicode[0x1F] == 0x25BC && cp437_unicode[0x7F] == 0x2302 &&
          cp437_unicode[0xFF] == 0x00A0, "the graphic set: ☺ at 0x01, ▼ at 0x1F, ⌂ at 0x7F, no-break space at 0xFF");

    /* Latin-1: the letters and signs code page 437 has, where it has them */
    check(cp437_from_unicode(0xE9) == 0x82 && cp437_from_unicode(0xC4) == 0x8E && cp437_from_unicode(0xD6) == 0x99 &&
          cp437_from_unicode(0xDC) == 0x9A && cp437_from_unicode(0xF1) == 0xA4 && cp437_from_unicode(0xBD) == 0xAB &&
          cp437_from_unicode(0xB0) == 0xF8 && cp437_from_unicode(0xB1) == 0xF1 && cp437_from_unicode(0xDF) == 0xE1 &&
          cp437_from_unicode(0xA3) == 0x9C && cp437_from_unicode(0xBF) == 0xA8 && cp437_from_unicode(0xA0) == 0xFF &&
          cp437_from_unicode(0xB5) == 0xE6 && cp437_from_unicode(0xFF) == 0x98 && cp437_from_unicode(0xA7) == 0x15 &&
          cp437_from_unicode(0xB6) == 0x14, "Latin-1: é Ä Ö Ü ñ ½ ° ± ß £ ¿ nbsp µ ÿ § ¶");
    check(cp437_from_unicode(0xC0) < 0 && cp437_from_unicode(0xD7) < 0 && cp437_from_unicode(0xA9) < 0 &&
          cp437_from_unicode(0xF0) < 0 && cp437_from_unicode(0xFE) < 0, "Latin-1 code page 437 lacks: À × © ð þ");
    for(latin = 0, i = 0xA0; i < 0x100; i++) if(cp437_from_unicode(i) >= 0) latin++;
    check(latin == 55, "55 of Latin-1's 96 characters from U+00A0 are in the set");
    check(cp437_from_unicode(0x03B2) == 0xE1 && cp437_from_unicode(0x03BC) == 0xE6 && cp437_from_unicode(0x2126) == 0xEA,
          "β, μ and the ohm sign share ß, µ and Ω");
    check(cp437_from_unicode(0) == 0 && cp437_from_unicode(0x01) < 0 && cp437_from_unicode(0x1F) < 0 &&
          cp437_from_unicode(0x7F) < 0 && cp437_from_unicode(0x80) < 0 && cp437_from_unicode(0x9F) < 0,
          "control characters have no place (they are never drawn)");
    check(cp437_from_unicode(0x20AC) < 0 && cp437_from_unicode(0xFFFD) < 0 && cp437_from_unicode(0x10000) < 0 &&
          cp437_from_unicode(0x1F600) < 0 && cp437_from_unicode(0x110000) < 0, "€, U+FFFD and beyond U+FFFF: none");
}

static void test_decoder(void) {
    struct utf8_dec d;
    u8 cells[16];
    int n, last;

    EXPECT("SurfOS 1.0\n", "SurfOS 1.0\n", "ASCII comes out as itself, control characters too");
    EXPECT("\x1b[1m", "\x1b[1m", "ESC and a sequence's bytes (the console parses them before the decoder)");
    EXPECT("caf\xC3\xA9", "caf\x82", "2 bytes: é");
    EXPECT("\xC3\x84\xC3\x96\xC3\x9C \xC3\xB1 \xC2\xBD \xC2\xB0 \xC2\xB1", "\x8E\x99\x9A \xA4 \xAB \xF8 \xF1", "2 bytes: ÄÖÜ ñ ½ ° ±");
    EXPECT("\xE2\x96\xB8\xE2\x89\x8B\xE2\x80\xA6", "\x10\xF7\xF9", "3 bytes: ▸ ≋ …");
    EXPECT("\xE2\x96\xBA\xE2\x96\xB6\xE2\x89\x88\xE2\x88\x99", "\x10\x10\xF7\xF9", "3 bytes: ► ▶ ≈ ∙ in SurfOS's places");
    EXPECT("\xE2\x94\x8C\xE2\x94\x80\xE2\x94\x90", "\xDA\xC4\xBF", "3 bytes: ┌─┐");
    EXPECT("\xE2\x98\xBA\xE2\x8C\x82", "\x01\x7F", "3 bytes: ☺ and ⌂ are cells 0x01 and 0x7F");
    EXPECT("\xF0\x9F\x98\x80", "?", "4 bytes: U+1F600 is outside the set: ?");
    EXPECT("\xF4\x8F\xBF\xBF", "?", "4 bytes: U+10FFFF, the last character: ?");
    EXPECT("\xE2\x82\xAC\xC4\xB4\xE6\x97\xA5\xC2\x85\xEF\xBF\xBD", "?????", "outside the set: € Ĵ 日 a C1 control, U+FFFD");
    EXPECT("\xED\x9F\xBF", "?", "U+D7FF, just below the surrogates, is a character: ?");

    /* not UTF-8: the bytes are the code page 437 characters they are */
    EXPECT("\xC0\x80", "\xC0\x80", "overlong: C0 80 (a NUL in 2 bytes)");
    EXPECT("\xC1\xBF", "\xC1\xBF", "overlong: C1 BF");
    EXPECT("\xE0\x80\x80", "\xE0\x80\x80", "overlong: E0 80 80");
    EXPECT("\xE0\x9F\xBF", "\xE0\x9F\xBF", "overlong: E0 9F BF (U+07FF in 3 bytes)");
    EXPECT("\xF0\x8F\xBF\xBF", "\xF0\x8F\xBF\xBF", "overlong: F0 8F BF BF (U+FFFF in 4 bytes)");
    EXPECT("\xED\xA0\x80", "\xED\xA0\x80", "surrogate U+D800");
    EXPECT("\xED\xBF\xBF", "\xED\xBF\xBF", "surrogate U+DFFF");
    EXPECT("\xF4\x90\x80\x80", "\xF4\x90\x80\x80", "above U+10FFFF: F4 90 80 80");
    EXPECT("\xF5\x80\x80\x80", "\xF5\x80\x80\x80", "above U+10FFFF: F5 starts nothing");
    EXPECT("\x80\xBF\x84", "\x80\xBF\x84", "stray continuation bytes (0x84 is ä in code page 437)");
    EXPECT("\xFF\xFE", "\xFF\xFE", "0xFF and 0xFE");
    EXPECT("\xC3" "A", "\xC3" "A", "2 bytes cut short by ASCII: the lead byte, then the letter");
    EXPECT("\xE2\x96" "A", "\xE2\x96" "A", "3 bytes cut short by ASCII after two");
    EXPECT("\xF0\x9F\x98" "A", "\xF0\x9F\x98" "A", "4 bytes cut short by ASCII after three");
    EXPECT("\xC3\x1B[0m", "\xC3\x1B[0m", "a sequence cut short by ESC: ESC still comes out (and starts its sequence)");
    EXPECT("\xC3\n", "\xC3\n", "a sequence cut short by a newline");
    EXPECT("\xC3\xC3\xA9", "\xC3\x82", "cut short by another lead byte, which starts a sequence of its own");
    EXPECT("\xE2\x96\xE2\x96\xB8", "\xE2\x96\x10", "3 bytes cut short by the start of the next: ▸ still decodes");
    /* a code page 437 pair that happens to be UTF-8 is read as UTF-8: ═╗ is U+037B, outside the set */
    EXPECT("\xC9\xCD\xCD\xBB", "\xC9\xCD?", "code page 437 ╔══╗: the last pair is valid UTF-8 (U+037B)");

    /* how many cells each byte gives */
    memset(&d, 0, sizeof(d));
    n = decode(&d, "\xE2", 1, cells, &last);
    check(n == 0 && d.have == 1, "a lead byte alone gives nothing yet");
    n = decode(&d, "\x96", 1, cells, &last);
    check(n == 0 && d.have == 2, "nor the next one");
    n = decode(&d, "\xB8", 1, cells, &last);
    check(n == 1 && cells[0] == 0x10 && d.have == 0, "the last byte gives the cell");
    n = decode(&d, "\xF0\x9F\x98" "Z", 4, cells, &last);
    check(last == UTF8_OUT_MAX && n == 4, "a 4-byte sequence cut short at its end gives UTF8_OUT_MAX cells at once");
    n = decode(&d, "\xC3\xA9", 2, cells, &last);
    check(n == 1 && cells[0] == 0x82 && d.have == 0, "and the decoder is clean afterwards");
}

int main(void) {
    test_tables();
    test_decoder();
    printf("%s: %d checks, %d failures\n", failures ? "CP437 UNIT TESTS FAILED" : "CP437 UNIT TESTS PASSED", checks, failures);
    exit(failures ? 1 : 0);
    return 0;
}
