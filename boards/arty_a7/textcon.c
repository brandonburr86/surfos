/*
SurfOS board: SurfCPU on the Arty A7 -- the VGA text display
--------------------
File: textcon.c Date: 10/15/26 (SurfCPU)
--------------------
The console's screen on surf_video (MEMORY-MAP.md): 640x480 at about 60 Hz showing
80x30 cells of 8x16 glyphs (the repository's code page 437 font, so ▸ ≋ … and the box
characters the console draws are there), from 32-bit cells in text RAM, in 16 palette
colours, with a hardware cursor and hardware scroll. kernel/console.c keeps the cells
(arch/riscv32/fbcon.c passes them on); textscreen.c decides what to write.

At start the palette is set to the VGA colours the framebuffer console uses
(driver/fbcore.c), the screen is cleared, and the mode is switched from the reset test
pattern to text with a blinking cursor. Its cursor register is in display coordinates,
as the console's are. A bitstream built without video (sysinfo.FEATURES) has no display:
the console is the serial line alone.

10/16/26: kernel/console.c calls in here holding its console lock (audit C10), one caller at
a time, so interrupts stay on while a whole screen is compared and written: that takes
milliseconds on SurfCPU, and the UART must be heard meanwhile.

10/25/26 (SurfGPU G3): CTRL.MODE for the GPU driver (boards/arty_a7/gpu.c, gpuhw.c): text, the
graphics modes 2 and 3 (graphics with this text over it). This file keeps the copy of CTRL it
rewrites for the cursor, so a mode lives in that copy and nowhere else: the driver takes the
console lock (arty_text_lock()), then sets the mode with interrupts off (arty_text_set_mode()),
and the cursor's rewrites keep it. The cursor's own change of the copy is made with interrupts
off as well, so that no change of the copy comes between another's read and its write. A panic
puts the text back on the screen (tc_panic()), so its report is seen over any picture.
*/

#include <surfos/types.h>
#include <surfos/console.h>
#include <surfos/task.h>
#include <surfos/irq.h>
#include <asm/io.h>
#include <surfcpu/soc.h>
#include <riscv/board.h>
#include "arty.h"
#include "textscreen.h"
#include <blibc_common.h>

#define VREG(off) ((u_long)SURF_VIDEO_BASE + (off))
#define MODE_TEXT (1u << SURF_VIDEO_CTRL_MODE_LSB)

/* the sixteen text mode colours as RGB444 (R 11:8, G 7:4, B 3:0): driver/fbcore.c's vga_rgb */
static const u16 palette[16] = {
    0x000, 0x00A, 0x0A0, 0x0AA, 0xA00, 0xA0A, 0xA50, 0xAAA,
    0x555, 0x55F, 0x5F5, 0x5FF, 0xF55, 0xF5F, 0xFF5, 0xFFF
};

static void set_scroll(u_int row) {
    mmio_write32(VREG(SURF_VIDEO_SCROLL), row & SURF_VIDEO_SCROLL_ROW_MASK);
}

static struct textscreen screen = { (volatile u32 *)SURF_TEXTRAM_BASE, 0, set_scroll, 0, 0 };
static u32 ctrl;

static void tc_init(void) {
    u_int i;
    for(i = 0; i < 16; i++) mmio_write32(VREG(SURF_VIDEO_PALETTE + 4 * i), palette[i]);
    textscreen_clear(&screen, 0x07);
    mmio_write32(VREG(SURF_VIDEO_CURSOR), 0);
    ctrl = SURF_VIDEO_CTRL_ENABLE_MASK | MODE_TEXT | SURF_VIDEO_CTRL_CURSOR_EN_MASK | SURF_VIDEO_CTRL_CURSOR_BLINK_MASK;
    mmio_write32(VREG(SURF_VIDEO_CTRL), ctrl);
}

static void tc_draw_cell(u32 col, u32 row, u8 ch, u8 attr) {
    textscreen_put(&screen, col, row, ch, attr);
}

static void tc_present(const u8 *cells, u32 cols, u32 rows) {
    textscreen_present(&screen, cells, cols, rows);
}

static void tc_set_cursor(u32 col, u32 row, bool visible) {
    u_long flags;
    u32 want;
    if(visible) mmio_write32(VREG(SURF_VIDEO_CURSOR), ((row << SURF_VIDEO_CURSOR_ROW_LSB) & SURF_VIDEO_CURSOR_ROW_MASK) |
                                                      (col & SURF_VIDEO_CURSOR_COL_MASK));
    flags = irq_save();                             /* 10/25/26: the copy's read and write as one (above) */
    want = visible ? (ctrl | SURF_VIDEO_CTRL_CURSOR_EN_MASK) : (ctrl & ~SURF_VIDEO_CTRL_CURSOR_EN_MASK);
    if(want != ctrl) {
        ctrl = want;
        mmio_write32(VREG(SURF_VIDEO_CTRL), ctrl);
    }
    irq_restore(flags);
}

static const char *tc_describe(char *buf, size_t size) {
    static const char *const shows[4] = { "the test pattern", "text", "graphics (gfx mode text shows the text)",
                                          "graphics with the text over it" };
    u32 info = mmio_read32(VREG(SURF_VIDEO_INFO)), hz = mmio_read32(VREG(SURF_VIDEO_PIXCLK));
    snprintf(buf, size, "surf_video showing %s, %lux%lu cells of %lux%lu, 640x480 at a %lu Hz pixel clock; "
             "%lu cells written, %lu hardware scrolls", shows[arty_text_mode() & 3],
             (u_long)(info & SURF_VIDEO_INFO_COLS_MASK), (u_long)((info & SURF_VIDEO_INFO_ROWS_MASK) >> SURF_VIDEO_INFO_ROWS_LSB),
             (u_long)((info & SURF_VIDEO_INFO_GLYPH_W_MASK) >> SURF_VIDEO_INFO_GLYPH_W_LSB),
             (u_long)((info & SURF_VIDEO_INFO_GLYPH_H_MASK) >> SURF_VIDEO_INFO_GLYPH_H_LSB), (u_long)hz,
             screen.writes, screen.scrolls);
    return buf;
}

/**** the display mode, for the GPU driver (10/25/26, above) ****/

/* the console lock, for a mode change: what console_print_begin() took, for arty_text_unlock() */
int arty_text_lock(void) {
    return console_print_begin(curTask ? curTask->con : NULL);
}

void arty_text_unlock(int how) {
    console_print_end(how);
}

u32 arty_text_mode(void) {
    return (ctrl & SURF_VIDEO_CTRL_MODE_MASK) >> SURF_VIDEO_CTRL_MODE_LSB;
}

/* CTRL.MODE in the copy and the register; the console lock held, interrupts off or not */
void arty_text_set_mode(u32 mode) {
    u_long flags = irq_save();
    ctrl = (ctrl & ~SURF_VIDEO_CTRL_MODE_MASK) | ((mode << SURF_VIDEO_CTRL_MODE_LSB) & SURF_VIDEO_CTRL_MODE_MASK);
    mmio_write32(VREG(SURF_VIDEO_CTRL), ctrl);
    irq_restore(flags);
}

/* a panic draws over everything: in a graphics mode its report would not be seen */
static void tc_panic(void) {
    if(arty_text_mode() != 1) arty_text_set_mode(1);
}

static const struct board_display display = {
    "surf_video", TS_COLS, TS_ROWS, tc_init, tc_draw_cell, tc_present, tc_set_cursor, tc_describe, tc_panic
};

const struct board_display *board_display(void) {
    return arty_has(SURF_SYSINFO_FEATURES_VIDEO_MASK) ? &display : NULL;
}
