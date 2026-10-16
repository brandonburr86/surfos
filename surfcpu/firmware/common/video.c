/*
SurfCPU firmware: VGA text console
--------------------
File: video.c Date: 10/15/26 (SurfCPU)
--------------------
80x30 text cells in text RAM (cell = glyph 7:0, foreground 11:8, background 15:12), shown by
the video block in text mode (docs/MEMORY-MAP.md, video at 0x1000_6000). Scrolling uses
the hardware SCROLL register: display row r shows text row (r + SCROLL) mod 30, so a new
line clears one text row and moves SCROLL on by one. The cursor register is in display
coordinates. Everything is skipped when sysinfo.FEATURES says there is no video.
*/
#include "fw.h"

#define COLS 80
#define ROWS 30
#define ATTR 0x0700u                /* light grey on black */

static int on;
static unsigned row, col, top;      /* cursor in display coordinates; text row shown first */

static volatile uint32_t *cell(unsigned display_row, unsigned c)
{
    unsigned text_row = (display_row + top) % ROWS;
    return (volatile uint32_t *)(SURF_TEXTRAM_BASE + 4u * (text_row * COLS + c));
}

static void clear_text_row(unsigned text_row)
{
    volatile uint32_t *p = (volatile uint32_t *)(SURF_TEXTRAM_BASE + 4u * text_row * COLS);
    for (unsigned c = 0; c < COLS; c++)
        p[c] = ATTR | ' ';
}

static void sync_cursor(void)
{
    REG(SURF_VIDEO_BASE, SURF_VIDEO_CURSOR) =
        (row << SURF_VIDEO_CURSOR_ROW_LSB) | (col << SURF_VIDEO_CURSOR_COL_LSB);
}

int video_present(void)
{
    return (sysinfo(SURF_SYSINFO_FEATURES) & SURF_SYSINFO_FEATURES_VIDEO_MASK) != 0;
}

void video_init(void)
{
    on = video_present();
    if (!on)
        return;
    for (unsigned r = 0; r < ROWS; r++)
        clear_text_row(r);
    row = col = top = 0;
    REG(SURF_VIDEO_BASE, SURF_VIDEO_SCROLL) = 0;
    sync_cursor();
    REG(SURF_VIDEO_BASE, SURF_VIDEO_CTRL) = SURF_VIDEO_CTRL_ENABLE_MASK
        | (1u << SURF_VIDEO_CTRL_MODE_LSB) | SURF_VIDEO_CTRL_CURSOR_EN_MASK
        | SURF_VIDEO_CTRL_CURSOR_BLINK_MASK;
}

static void newline(void)
{
    col = 0;
    if (row < ROWS - 1) {
        row++;
        return;
    }
    top = (top + 1) % ROWS;
    clear_text_row((top + ROWS - 1) % ROWS);
    REG(SURF_VIDEO_BASE, SURF_VIDEO_SCROLL) = top;
}

void video_putc(char c)
{
    if (!on)
        return;
    switch (c) {
    case '\r':
        col = 0;
        break;
    case '\n':
        newline();
        break;
    case '\b':
        if (col > 0)
            col--;
        break;
    case '\t':
        do {
            *cell(row, col) = ATTR | ' ';
            col++;
        } while (col % 8 && col < COLS);
        if (col >= COLS)
            newline();
        break;
    default:
        *cell(row, col) = ATTR | (uint8_t)c;
        if (++col >= COLS)
            newline();
        break;
    }
    sync_cursor();
}
