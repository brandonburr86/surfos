# The desktop applications: track desk's account

Branch `apps/desk`, worktree `06-restoration-apps-desk`, volume `surfos-apps-desk`. The brief is
`docs/superpowers/plans/2026-11-05-desktop-apps.md` (section "desk: the desktop's side"); this is what the track did,
why, what it left out, and how it was tested.

## Where the code comes from

Everything here was written for SurfOS from its own tree and from public descriptions of behaviour; no other
program's source was used or recalled.

- The window calls extend `include/surfos/win.h` and `kernel/win.c` as they were (milestone M12): the same copies in,
  checks first, short interrupts-off sections, counted buffers.
- The keys a program's window gets follow what SurfOS's own terminal sends in raw mode (`include/surfos/tty.h`), which
  is xterm's convention for modifiers on special keys (`CSI 1 ; m X`, `CSI n ; m ~`, m = 1 + Shift + 2 Alt + 4 Ctrl),
  as xterm's "ctlseqs" documentation describes it; the `WIN_MOD_*` bits are that m - 1.
- Menus with shortcut text, checked and greyed items, and Open, Save as and Choose folder choosers follow the
  *behaviour* of desktop menus and file choosers in general (classic Mac OS and Windows menu bars, GTK and Windows file
  dialogs) as a user sees them; nothing of their code or look was copied. SurfOS's own menus and choosers
  (`shell/guicore.c`, `shell/guidialog.c`) are what the new ones extend.
- The registry's text format is SurfOS's own, in the style of `surfos-config/settings.conf` (`net/netcfg.c`):
  "key = value" lines and # comments. Its icons are rows of palette letters, as `shell/guidraw.c`'s icons already
  are.

## Stage 1: the ABI

Committed first, for kit and the six applications to build on: `include/surfos/win.h` (its header comment is the
documentation), `rootfs/apps/README` (the registry's format), the argument checks in `kernel/win.c`, and their
scenarios in `tests/host/test_win.c`. A request that passes its checks is answered `-ENOSYS` until stage 2.

Decisions a reviewer would ask about:

- **WIN_VERSION stays 1; struct win_info stays 20 bytes.** The brief allows the version to stay. `struct win_info`
  could not grow in place: programs built before have 20 bytes for it (libgui's `gw_open` reads it into a stack
  struct), and `user/libsurf/syscalls.c` checks its size. So the theme is `WIN_THEME`, a struct of its own (68 bytes:
  the theme's number and 16 colours by role). A window whose program asked it once gets `WIN_EV_THEME` at each
  change: no program that never asked sees an event it does not know.
- **Theme colours by role, not by name.** `WIN_COLOR_WINDOW`, `_TEXT`, `_ACCENT` and so on, so that a dusk that one
  day darkens windows changes values, not names. Today ocean and dusk differ only in the desktop behind the windows
  (`WIN_COLOR_DESKTOP`), as the desktop's own windows do.
- **Keys.** The desktop reads its terminal in raw mode, so a key is what the tty sends. The modifiers come from the
  bytes where they say them (an escape sequence's m, the control bytes only Ctrl makes, an Alt key's ESC sent with
  it) and otherwise from the keyboard's state as the key is read (Shift on a control byte: Ctrl+Shift+S; Ctrl on
  Enter, Tab, Backspace, Esc). A Ctrl+letter is its control byte (1 to 26) with `WIN_MOD_CTRL`, as before but with the
  bit: old programs that compare the byte keep working. What the keyboard driver never passes (Ctrl with a digit or
  most punctuation) is said in the header rather than added: `kernel/keyboard.c` is desk's only for Print Screen.
  F5 to F12 arrive as `WIN_KEY_F5` to `F12` (`WIN_KEY_F(n)`, 0x111 to 0x11C); F1 to F4 switch consoles.
- **Mods on pointer and wheel events too** (Shift+click to extend a selection in the editor and the spreadsheet),
  read from the keyboard as the event is taken.
- **WIN_MENU replaces the whole set**, a fixed-size array of 56-byte entries copied in one piece: one bounds check,
  no pointers inside to chase. Titles and items are entries; a title starts a menu. The limits make every menu fit a
  640 x 400 desktop without scrolling (15 entries) and the bar fit beside "≋ SurfOS", Apps, Window and Help
  (`WIN_MENU_BAR`, 44 characters with 3 for each title). The program's menus take the place of the desktop's File and
  Edit while its window has the focus; Ctrl+G opens the bar as before.
- **WIN_PASTE copies during the program's call** from a copy of the clipboard the desktop gives the kernel while one
  of its windows has the focus; only a focused window may read it (as WIN_CLIP may only write while focused). A
  buffer too small gets nothing and `-ENOSPC`; size 0 asks the length.
- **WIN_DIALOG** asks for one chooser at a time per window, only while focused (a chooser takes the keys: a
  program in the background must not pop one up under a password being typed). The answer is an event; the path is
  copied in by `WIN_TAKE`, which never leaves a path half copied (`-ENOSPC` keeps it for a retry). The pair care
  (a `.tmp` beside a file) is the chooser's: it never answers a Save as with a file the desktop takes care of.
- **Strings in requests:** titles and labels are cut between characters as window titles are (lenient); paths, names
  and types must carry their NUL and no control character (strict), since they name files.

## Stage 2

(To come in this account as the work lands.)

## Tests

Stage 1: `test_win` 1,435 checks (225 new: the theme, the menus', pastes', choosers' and takes' arguments, nothing
left allocated after each), `make unittest`, `make test-all`.
