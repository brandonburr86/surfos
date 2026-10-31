# Basic 2D graphics

SurfOS draws pixels, filled rectangles, lines and RGB images on the framebuffer
that GRUB passes to the kernel. This extends the existing text-console driver.
The target laptop is the **Lenovo IdeaPad 3 15IIL05** recorded in
[the hardware notes](hardware/ideapad-3-15iil05.md). Its documented i3 configuration
has Intel UHD graphics; the exact installed graphics PCI ID and current panel
mode still need a fresh reading from the laptop.

The [SurfOS desktop](GUI.md) uses this interface for a menu bar, movable windows,
Notepad, Files, Calculator and Sketch. Launch it with `gui`.

## Try it

Boot the UEFI USB image and use the laptop's active local console:

```text
gfxinfo
gfxtest
gfxtest 15
```

`gfxinfo` reports the actual boot framebuffer's resolution, depth, text scale and
memory caching. Since 10/25/26 it also says what the text console's screen has cost:
how many presents (passes over the whole grid) there were and the slowest one, how many
more were asked for while one was pending and folded into it, how many cells were drawn
whole and how many cursor moves were drawn as their two underline rows alone
(ARCHITECTURE.md section 7). `gfxtest` displays eight color bars, diagonal lines and a small
RGB image for five seconds by default. The optional duration is 1–60 seconds.
Ctrl+C ends the test early. Switching to another console with F1–F4 cancels it;
returning to the original console shows its shell. Background text output is
retained in the console buffers while the graphics are visible.

Only one task on the active local console may own graphics. A telnet session or
inactive console cannot take over the panel. The VGA text-mode boot reports that
graphics are unavailable. Redirecting `gfxtest` is refused by the shell's existing
interactive-command rule. `gfxinfo` works over telnet and with redirection.

## Driver interface

`include/sys/framebuffer.h` exposes a **kernel-only** interface. Rendering lives
in `driver/fbcore.c`, and `driver/framebuffer.c` owns the mapped framebuffer and
the transition between text and graphics. No new user-memory mapping or system
call is provided for the framebuffer itself. User applications can invoke the
test through the existing kernel-command interface.

*10/25/26, reversed for programs' windows (milestone M12,
`docs/superpowers/plans/2026-10-25-app-windows.md`):* the owner chose a platform
for new applications, so an ordinary program now draws in a window of the desktop
through two system calls, `SYS_winopen` and `SYS_winctl` (`include/surfos/win.h`,
`kernel/win.c`; [GUI.md](GUI.md#programs-windows), ARCHITECTURE.md section 24). Its
pixels are copied into a buffer of the kernel's during its own call, never shared:
the framebuffer stays the desktop's, and still no frame of the kernel is mapped
into ring 3. libgui (`user/include/gui.h`) builds this renderer and the desktop's
drawing kit into libsurf, so a program draws into its own memory as the desktop
draws its scene.

All drawing colors use `0x00RRGGBB`. The driver converts them to the bootloader's
15-, 16-, 24- or 32-bit RGB/BGR format, honors scanline pitch and clips to visible
pixels. Coordinates are signed. Even lines spanning the full signed 32-bit range
take at most the screen width or height in iterations.

| Function | Purpose |
| --- | --- |
| `fb_pixel_rgb(surface, x, y, rgb)` | Draw one pixel |
| `fb_rect_rgb(surface, x, y, width, height, rgb)` | Fill a clipped rectangle; zero dimensions do nothing |
| `fb_line_rgb(surface, x0, y0, x1, y1, rgb)` | Draw an inclusive line in either direction |
| `fb_blit_rgb(surface, x, y, bitmap)` | Copy a clipped RGB image; return -1 for an invalid source |

Text: `fb_draw_glyph(surface, col, row, ch, attr, cursor)` draws a console cell in the
surface's font. Since 10/25/26 (track A, A7) that is `surface->font`, a `struct fb_font`
(`include/sys/framebuffer.h`: name, cell width and height, the 256 code page 437 glyphs'
rows in `rows8` or `rows16`): the session's surface carries the text console's font,
Spleen 12x24 on the laptop's 1920x1080 (160x45 cells) or 8x16. `fb_font_8x16`,
`fb_font_12x24` and `fb_font_16x32` are there to draw in another. The desktop draws its
own text with `font8x16` (`shell/guidraw.c`).

`struct fb_bitmap` holds a pointer to 32-bit RGB pixels, width, height, stride
**in pixels**, and the readable allocation size **in bytes**. Its source is
ordinary RAM and must not overlap the framebuffer. The driver checks dimensions,
stride and the last readable pixel before writing anything. The caller must
provide a valid kernel pointer for the declared allocation.

A graphics session follows this pattern:

```c
static void paint(const struct fb_surface *screen, void *context) {
    fb_rect_rgb(screen, 0, 0, screen->mode.width, screen->mode.height, 0x102030);
    fb_line_rgb(screen, 0, 0, 200, 100, 0xFFFFFF);
}

/* Called from a task on the active local console. */
if(framebuffer_graphics_begin() == 0) {
    framebuffer_graphics_draw(paint, NULL);
    /* Optional interruptible wait here, outside paint(). */
    framebuffer_graphics_end();
}
```

`paint()` runs synchronously with preemption disabled and interrupts enabled.
It must be short and must not block, allocate, print, reenter the session API,
or retain the surface pointer. Prepare substantial source images before drawing.
Check `framebuffer_graphics_draw()` for -1 if ownership might have been revoked;
`framebuffer_graphics_owned()` can also end an animation or interruptible wait.
Always call `framebuffer_graphics_end()` on exit; it is harmless after revocation.

The driver revokes ownership on console switching, task death and panic. Text
redrawing clears the whole screen, including margins, and invalidates the cached
character cells. This avoids leaving graphics under unchanged text. Serial
mirroring and the text buffers keep working throughout a graphics session.

## Scope and verification

The firmware-selected display mode stays in place. This driver uses CPU drawing;
it does not program Intel display registers, change resolution at runtime,
provide accelerated 2D/3D rendering, synchronize to vertical blank or manage HDMI.
Frame presentation is not atomic, so tearing is possible during large updates.
The [UEFI graphics specification](https://uefi.org/specs/UEFI/2.11/12_Protocols_Console_Support.html)
describes the framebuffer information supplied through GOP; GRUB passes the
selected mode to SurfOS through Multiboot.

The host framebuffer suite checks pixel formats, padded rows, clipped drawing,
all line octants, extreme coordinates and invalid image bounds. The `graphics`
scenario in `tools/qemu-usb.py` checks actual framebuffer pixels and text recovery
at 1920×1080. It is part of the complete UEFI suite:

```sh
./devenv/build.sh unittest
./devenv/build.sh test-all
O=0 ./devenv/build-usb.sh
./devenv/test-usb.sh
O=2 ./devenv/build-usb.sh
./devenv/test-usb.sh
```

Validation on 2026-10-10 used GCC 12.2.0, container QEMU 7.2 for kernel smoke tests,
and host QEMU 11.1.1 with its OVMF firmware for USB tests.
Both optimization levels were built with the existing file browser and Wi-Fi
firmware included in the USB image. Results:

| Check | Result |
| --- | --- |
| Complete host unit suite | Passed; framebuffer renderer: 143 checks |
| Kernel smoke suite | 281 checks passed at each of `-O0` and `-O2` |
| Complete USB/UEFI suite | 259 checks passed at each level, across 14 scenarios |
| Graphics scenario | 14 checks per level, including visible pager handling and Ctrl+C at the pager |
| Existing file browser regression suite | 51 checks passed at each level |

The kernels in the combined USB images were byte-identical to those used by the
kernel smoke suite. The USB tests retain screenshots and machine-readable results
in their output directory. Task-death and panic revocation were reviewed in code;
the graphics scenario exercises normal completion, interruption and console switching.

Physical-laptop verification is separate from emulator results: record `gfxinfo`,
the graphics controller from `lspci`, the displayed colors, Ctrl+C, F1/F2 recovery,
and the tested image's checksum. The laptop did not answer at its documented
network address during initial inspection on 2026-10-10, so this change must not
be treated as physically verified from the emulator checks alone.
