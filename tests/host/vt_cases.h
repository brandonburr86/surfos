/* Escape sequences and the screen they leave (10/25/26, track A, A8): cases that more than one terminal emulator
   runs, so they draw alike. tests/host/test_pager.c runs them on kernel/console.c; tests/host/test_guiterm.c can
   run them on shell/guivt.c (track C's), which reads the same sequences. Each case starts on a blank screen of at
   least 40x8 cells, the cursor at the top left, the writer's colour light grey on black (VGA attribute 0x07), and
   writes input; then rows must read as text (the empty cell as a space, the blanks at a row's end dropped), the
   cells' VGA attributes must be as given (a terminal with other colours may leave these out), and the cursor must
   be at col, row (-1: not looked at). Plain C, for the host's headers and SurfOS's alike. */
#ifndef VT_CASES_H
#define VT_CASES_H

#define VT_ROWS_MAX 3
#define VT_ATTRS_MAX 5

struct vt_case {
    const char *name;
    const char *input;
    struct { int row; const char *text; } rows[VT_ROWS_MAX];    /* text NULL: no more */
    struct { int col, row, attr; } attrs[VT_ATTRS_MAX];       /* attr -1: no more */
    int col, row;                                               /* the cursor afterwards */
};

static const struct vt_case vt_cases[] = {
    { "CHA (ESC[nG): the column on the same row", "abc\033[10GX",
      { { 0, "abc      X" }, { 0, NULL } }, { { 0, 0, -1 } }, 10, 0 },
    { "VPA (ESC[nd): the row in the same column", "\033[5;3Habc\033[2dX",
      { { 1, "     X" }, { 4, "  abc" }, { 0, NULL } }, { { 0, 0, -1 } }, 6, 1 },
    { "ECH (ESC[nX): blanks from the cursor, which stays", "abcdef\033[3G\033[2X",
      { { 0, "ab  ef" }, { 0, NULL } }, { { 0, 0, -1 } }, 2, 0 },
    { "ESC[s and ESC[u: the cursor saved and put back", "ab\033[sXY\033[3;5H\033[uZ",
      { { 0, "abZY" }, { 2, "" }, { 0, NULL } }, { { 0, 0, -1 } }, 3, 0 },
    { "ESC 7 and ESC 8: the cursor and its colours saved and put back", "\033[1;31mR\0337\033[0mN\033[5;5H\0338S",
      { { 0, "RS" }, { 4, "" }, { 0, NULL } }, { { 0, 0, 0x0C }, { 1, 0, 0x0C }, { 0, 0, -1 } }, 2, 0 },
    { "SGR 22 and 27: bright off, inverse off", "\033[1;33mA\033[22mB\033[7mC\033[27mD",
      { { 0, "ABCD" }, { 0, NULL } }, { { 0, 0, 0x0E }, { 1, 0, 0x06 }, { 2, 0, 0x60 }, { 3, 0, 0x06 }, { 0, 0, -1 } }, 4, 0 },
    { "SGR 39 and 49: the writer's colours again", "\033[33;44mA\033[39mB\033[49mC",
      { { 0, "ABC" }, { 0, NULL } }, { { 0, 0, 0x16 }, { 1, 0, 0x17 }, { 2, 0, 0x07 }, { 0, 0, -1 } }, 3, 0 },
    { "SGR 90-97 and 100-107: bright colours (the background's brightness is not drawn)", "\033[92mG\033[104mB\033[0m.",
      { { 0, "GB." }, { 0, NULL } }, { { 0, 0, 0x0A }, { 1, 0, 0x1A }, { 2, 0, 0x07 }, { 0, 0, -1 } }, 3, 0 },
    { "SGR 38 and 48 (256 colours, RGB): dropped with their numbers", "\033[38;5;1mA\033[48;2;1;2;3mB\033[38;5;7;1mC",
      { { 0, "ABC" }, { 0, NULL } }, { { 0, 0, 0x07 }, { 1, 0, 0x07 }, { 2, 0, 0x0F }, { 0, 0, -1 } }, 3, 0 },
};

#define VT_CASES ((int)(sizeof(vt_cases) / sizeof(vt_cases[0])))

#endif
