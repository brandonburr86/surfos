# Account: track kit, libgui's toolkit

The brief is `docs/superpowers/plans/2026-11-05-desktop-apps.md`. This track built the window toolkit the desktop
applications stand on: `gk.h` in libsurf (user/libsurf/gk*.c), its fake desktop for host tests, and a demonstration,
`wkit`. Branch `apps/kit`, worktree `06-restoration-apps-kit`, volume `surfos-apps-kit`.

## Where the code comes from

Everything here is written for this tree, from its own code and from no other toolkit's source.

- **The look** is the desktop's own, read from this tree: `shell/guidraw.c` (the palette, buttons, scroll bars and
  their thumb arithmetic, `gd_thumb()` and `gd_scroll_to()`), `shell/guidialog.c` (the dialogs' layout, the text box,
  the buttons from the right), `shell/guicore.c` and `shell/guimon.c` (toolbars, the lit tab, status bars, list rows).
  Check boxes, switches and radio buttons, which the desktop has none of, are drawn here in its colours.
- **The layout** (boxes that stack their children and share what is left over by weight, a child that grows filling
  the box across) is the common idea of box layout, not any toolkit's code; the arithmetic is this file's.
- **Damage tracking** (rectangles merged when their box is not much more than they are, presented each) is the
  usual dirty-rectangle technique; the merging rule is this file's own.
- **UTF-8** follows the console's rules (`gd_char_len()` in shell/guidraw.c, RFC 3629's well-formed sequences), so
  that what a field edits and what it draws always agree.
- **The 64-bit division** (`gk_div64()`) is plain long division a bit at a time, because libsurf links no libgcc.
- **Behaviour matched:** the desktop's text box and Notepad for editing keys (typing replaces the selection, Ctrl+A,
  C, X, V, the first line of a paste, control characters as spaces), its lists and choosers for the keys and the
  double-click, its dialogs for the keys of a question (Left, Right, Tab, Enter, Esc). A chat line's history (Up
  and Down, the line being typed kept after the newest) behaves as shells' and chat clients' input lines commonly
  do; no program's code was looked at for it.

## What was built

### Stage 1, the core

- **libgui's view** (gui.h, gui.c, guikit.c/h): every drawing goes through an origin and a clip
  (`gw_get_view()`, `gw_set_view()`, `gw_clip_to()`, `gw_origin()`), so that the toolkit draws a widget, or lets a
  program draw an area, in coordinates from its top left and never outside it. A program that never sets a view
  draws as before. Also `gw_char()` (a character's code and bytes, as `gw_text()` draws it) and `gw_text_cut()`
  (text cut to cells with the desktop's dot).
- **The toolkit** (`user/include/gk.h`; `gkcore.c` the window, tree, layout, damage, focus and events;
  `gkwidget.c` labels, buttons, check boxes, switches, radio groups, separators, spacers, status bars, scroll bars,
  tabs; `gkfield.c` the line of text; `gklist.c` lists; `gkarea.c` areas a program draws; `gkbox.c` modal boxes).
- **Track desk's calls** (merged from apps/desk 1acdce47, and b513bfe2, the ABI as the review of desk's stage 1
  amended it): libgui's thin wrappers `gw_menu()`, `gw_paste()`, `gw_dialog()`, `gw_take()`, `gw_theme()`, and the
  toolkit's side, `gkdesk.c`: menus in the desktop's bar with submenus, the program's part of the desktop's Help and
  their shortcuts, the desktop's choosers, the clipboard read, the theme followed.
- **The fake desktop** (`tests/host/gk_fake.c`, `gk_fake.h`, `tests/host/gk_shim/surf.h`): libgui and the toolkit
  built for the host as they are, with the window calls answered as the kernel answers them, desk's included (its
  menu checks are kernel/win.c's `menu_cells()`, `desk_title()` and `menu_check()`, copied, reading labels with
  driver/cp437.c's own `utf8_feed()`, so a label the toolkit cleans is held to the kernel's rule, not the toolkit's).
- **The demonstration** `wkit` (`user/wkit/`: `demo.c` the window, `main.c` the loop), its host test
  `tests/host/test_wkit.c` and its screen checks `tools/gui_apps_kit.py`; its page `rootfs/man/wkit.txt`.

### Decisions a reviewer would ask about

- **Events, not callbacks.** `gk_next()` returns what the program must hear (`GK_EV_CLICK`, `GK_EV_CHANGE`, ...),
  from a queue the widgets' handlers fill, so a widget never calls the program and a program's loop looks like
  wclock's and wcharmap's. The one callback is an area's draw function, which only draws: the toolkit decides when
  to paint (damage), so it must ask. A widget changed by the program (`gk_set_value()`, `gk_set_text()`) says
  nothing: only a person's change is an event, so a program never hears its own changes back.
- **Painting by damage.** Each change adds the rectangle it touches; a layout paints again what moved, where it was
  and where it is; the focus paints only what it changes (`lit_part`: a tab strip's strip, nothing of an area,
  which hears `GK_EV_FOCUS` and paints its own cursor). Two rectangles merge only when the box around them is at
  most their sum and a quarter of the smaller: touching or nested ones merge, distant ones are presented apart.
  test_wkit checks that a click on a cell presents the two cells and the status bar, nothing else.
- **Areas scroll in the program's units.** A spreadsheet scrolls by rows and columns, an editor by lines, a canvas by
  pixels: `gk_area_extent(area, total_x, unit_x, total_y, unit_y)`. The toolkit shows a bar each way that does not
  fit, keeps the positions within it, and moves them by the bars and the wheel (three rows a notch, as the desktop's
  windows; 48 pixels of an area that scrolls by pixels), telling `GK_EV_SCROLL`; the program moves them itself
  (`gk_scroll_to()`) to keep its cursor in view. The positions are 32-bit, the bars' arithmetic 64-bit: an editor
  of millions of lines is fine.
- **The focus.** Tab and Shift+Tab (`WIN_KEY_BACKTAB`, or Tab with Shift) go through the widgets that take it in the
  tree's order, past hidden and disabled ones and pages put away; a click gives it. A focused widget that becomes
  unreachable loses it at the next paint. An area takes the focus only with `GK_FOCUSABLE`, and Tab only with
  `GK_TAKES_TAB` (an editor's Tab is a tab). A key the focused widget does not use is the window's
  (`GK_EV_KEY` with `w` NULL): shortcuts.
- **Lit means the window has the desktop's focus too.** A field's teal border and cursor, a list's pick colour and
  a button's teal show only while the window has the focus (`WIN_EV_FOCUS`), as the desktop's own windows do.
- **Modal boxes are answered once.** A box opened over another answers the other -1 first, so that a program
  waiting for an answer always gets one; `gk_dismiss()` answers -1. A box takes every key and click of the window;
  the program still hears its timers, descriptors and the close button. The text typed into `gk_ask()` comes with
  the answer and is zeroed after the next `gk_next()`.
- **The line of text** stores at most its `max` bytes and never part of a character: bytes typed one at a time
  (a byte beyond ASCII from the desktop is one key) wait until they make a whole character; a paste or a typed
  character that does not fit is cut at a character's start. Deleted bytes are zeroed. A password field
  (`GK_PASSWORD`) shows a dot (code page 437's 7) for each character, copies and cuts nothing, keeps no history,
  and is zeroed when it goes or by `gk_field_wipe()`.
- **Watched descriptors are level-triggered**, as poll() is: `GK_EV_FD` comes again while the descriptor is ready,
  so the program reads what made it so (a chat's socket).
- **Timers** run on uptime()'s milliseconds; a repeating one keeps its beat and skips the beats it missed, so a
  game that falls behind never gets a burst.
- **The fake desktop is at the system calls,** not above the toolkit: `winopen()` and `winctl()` are answered as the
  kernel answers them (the same checks of a present's rectangle and stride, `WIN_CLIP` only with the focus), and the
  window's descriptor is a pipe's read end, so libgui's own read() and poll() run unchanged. A test therefore runs
  the program's whole window code, gui.c included, and sees what the desktop would show (`gkf_pixel()`,
  `gkf_text()`) and how much was presented (`gkf_presented()`, `gkf_presented_rect()`).
- **Menus' shortcuts are the toolkit's.** The desktop only shows a shortcut's text (surfos/win.h), so the toolkit
  turns a key that is an enabled item's shortcut into `GK_EV_MENU`, as a click on the item in the bar is: a program
  handles each command once, by its id. A field's and a text's own keys come first (Ctrl+C copies in a field), and
  then the shortcuts; an area's shortcuts come before the area, which hears every other key (an editor's Ctrl+S is
  File > Save). A greyed item's shortcut is the window's key. A shortcut is written as a key with `GK_CTRL`,
  `GK_SHIFT`, `GK_ALT` (`GK_CTRL | 'S'`, `WIN_KEY_F5`, `GK_CTRL | WIN_KEY_TAB`), and its text in the bar is made from
  it ("Ctrl+Shift+Z"). A control byte without `WIN_MOD_CTRL`, as a desktop before the modifiers sends Ctrl+S, still
  matches; a character Shift makes ('+', '!') matches with or without Shift, since the key comes as the character
  with Shift held (Ctrl+Shift+= is '+'). An area takes a key that types a character before the shortcuts (an editor's
  'p' is a p, even when a game's Pause is P), and the shortcuts come first for any other. The menus are sent whole at
  the next paint after anything in them changed (WIN_MENU replaces them whole); a menu or submenu with no items is
  left out (the kernel refuses one); a desktop that does not show them (ENOSYS until desk's stage 2, EINVAL before
  the calls) keeps every command by its keys, and `gk_menus_shown()` says which.
- **Menus as the amended ABI has them.** `gk_submenu()` makes an item that opens a submenu, one level deep, its
  children sent right after it as `WIN_MENU_CHILD` entries; it has an id, so `gk_menu_enable()` greys it, and with it
  every shortcut inside. A menu titled Help becomes the program's part of the desktop's Help (`WIN_MENU_HELP`, eleven
  items, not counted in the bar), since the kernel refuses a plain title Help; titles that would pass for the
  desktop's (SurfOS, Apps, Window, ≋) are refused at `gk_menu()`, judged by the characters they draw as the kernel
  judges them. A label is cleaned before it goes: a byte that is no whole character by the console's rules, or a
  control (C0, DEL, C1), is sent as one '?', because the kernel refuses the whole WIN_MENU for one such byte, and a
  menu that names a chat channel or a file from elsewhere must not lose the program its bar. The limits (eight
  menus, the Help menu among them, fourteen items each, fourteen in a submenu, 128 entries, 44 cells of titles) are
  checked as the menus are built, so a menu that does not fit is refused there and the rest still go.
- **Choosers degrade to a line to type.** Which kernel the window has is WIN_THEME's answer as it opens (the amended
  ABI makes it the feature test: EINVAL is a kernel before the calls). On a kernel with them, the kernel checks a
  request and its EINVAL comes back to the program (a type list that is none, say), with EBUSY (one waits for its
  answer) and EACCES (no focus, or nothing the person did since the last request); where the desktop has no
  choosers yet (ENOSYS), and on a kernel before them, the toolkit opens its own line over the window ("The path (this
  desktop has no chooser):"), starting with the folder and the name offered, and answers it as `GK_EV_CHOSEN` too, a
  name alone made a path in that folder. So a program never needs a second way to open a file. A first version
  checked the types itself, to tell a kernel without the call from a bad request; WIN_THEME made that copy of
  win_types() (which surfos/win.h keeps from programs) unnecessary. The path is taken (WIN_TAKE, which works after
  the desktop let the window go too) as `WIN_EV_DIALOG` comes, and handed over as the event's text.
- **Paste reads the clipboard whole when it is longer than the buffer** (WIN_PASTE says how long it is, and copies
  nothing into a buffer too small), then keeps what fits, cut at a character's start, and zeroes the rest. A field
  takes the first line.
- **The theme by its roles.** The colours are read at the opening (WIN_THEME, which also subscribes the window) and
  again at each `WIN_EV_THEME`, which paints the whole window again and says `GK_EV_THEME`; an area's draw function
  reads `gk_theme()` as it draws. Text on the accent is `on_teal` (WIN_COLOR_ON_ACCENT), a box's frame `frame`, its
  shadow `shadow`, the theme's darkest (WIN_COLOR_DARK, navy): the desktop's own dialogs use a shadow a shade darker
  (guidraw.h's SHADOW), which is no role of the theme, so a program's box follows the theme instead. A kernel before
  WIN_THEME leaves the ocean's colours. Only the roles the desktop fills (`count`) are read. A greyed widget's text
  is `grey` (WIN_COLOR_DISABLED), the desktop's greyed items' colour, lighter than `muted`.
- **Keys as desk defines them** (surfos/win.h as amended): Ctrl and a letter is its control byte with
  `WIN_MOD_CTRL`, Ctrl+I and Ctrl+M among them (9 and 13 with it are letters, not Tab and Enter); Tab, Enter, Esc
  and Backspace are 9, 13, 27 and 127, and with Ctrl `WIN_KEY_TAB` .. `WIN_KEY_BACKSPACE`. A control byte alone,
  from a desktop before the modifiers, is still read as Ctrl and its letter, but 8, 9, 10 and 13, which were
  Backspace, Tab and Enter there. Every test is of the bits (a key held down comes again with `WIN_MOD_REPEAT`, and a
  field types each repeat). A field or a list types or jumps only for a key without Ctrl or Alt (Ctrl+0 and Alt+X
  are the window's); buttons, check boxes, radio groups and tabs take no key with Ctrl or Alt. Ctrl with the arrows
  moves by words in a field, Ctrl+Backspace and Ctrl+Delete delete a word, Shift and a click selects to there; Enter
  in a field says `GK_EV_ACTIVATE` with its modifiers (Shift+Enter told apart), and Ctrl+Enter is the window's.
  Ctrl+Tab and Ctrl+Shift+Tab move the focus from any widget, one that takes Tab too, unless a menu item has Ctrl+Tab
  for its shortcut (a code editor's next tab), which then wins.
- **Memory.** Every widget is allocated; a failed allocation returns NULL, a NULL parent makes nothing, and
  `gk_ok()` says 0 from then on, so a program builds its tree and checks once. The toolkit allocates nothing while
  painting, and an area's draw function may not change the tree (calls that would are refused while painting).

## Tests

- **test_gk** (`make unittest`, unittest-kit): libgui's view and a button's UTF-8 label; every widget by the pointer
  and the keys; layout (boxes, growing, alignment, padding, gaps, hidden widgets, pages, a new size); damage (only
  what changed presented); the focus (Tab, Shift+Tab, past what cannot have it); areas (drawn clipped in their own
  coordinates, the pointer, the keys, the focus, scroll bars in their units, the wheel down and across, Ctrl+wheel
  the program's, a part damaged, a range revealed); modal boxes (each answered once, a box over a box, a password
  asked); timers and watched descriptors; the window let go; menus (sent whole when they change, chosen in the bar
  or by their shortcuts, greyed ones, a field's keys first and an area's after, the bar's limits, a desktop without
  them); submenus, the Help menu, labels cleaned and titles refused as the kernel refuses them, shortcuts as the
  keys come (Ctrl+Shift+= for Ctrl++, Ctrl+Tab); paste (first line, as much as fits, a character whole, without the
  focus, a desktop without it); choosers (asked, answered, cancelled, refused by the desktop's checks, never but in
  answer to a person, answered 0 when the window is let go, a path chosen just before it still taken, the line typed
  instead on a desktop without them, from a kernel before them); the theme (read at the opening, followed);
  modifiers on keys and the pointer as desk defines them (Ctrl+I, Ctrl+M, Ctrl+Enter, Alt, repeats, Ctrl+Tab).
- **test_wkit**: the demonstration's window driven as a person drives it, on the fake desktop.
- **The guiapps scenario** (`GUIAPPS_ONLY=gui_apps_kit`): wkit on the desktop of a USB-booted SurfOS.
- On the Mac, natively (x86_64 under Rosetta, as the Docker image is 32-bit x86): test_gk and test_wkit with
  AddressSanitizer and UndefinedBehaviorSanitizer, kept out of `make unittest` as the brief says.

Counts at the gates:

- **Stage 1** (the core's commits, on desk's ABI as amended): `make unittest` passed, test_gk 396 checks and
  test_wkit 63 among it; `make test-all` passed at -O0 and -O2, 51 Files checks and 558 smoke checks each (a first
  try stopped in the smoke test's entropy check, whose second QEMU could not forward a port the harness's own HTTP
  server had just been given: a port race in tools/qemu-run.py, `free_port_range()` against `HTTPServer(port 0)`,
  which this track does not touch; the step run again passed); the guiapps scenario (`GUIAPPS_ONLY=gui_apps_kit`)
  passed at -O0 and -O2, 35 of 35 checks, 29 of them wkit's. Natively with ASan and UBSan: test_gk and test_wkit
  clean. Each of the six commits was also checked out alone and its libgui and toolkit compiled natively, its host
  tests run where it has them.

## Left out, and why

- **No widget the six applications do not need:** no tree view, combo box, spin box, tooltip, drag and drop between
  widgets, or multiple selection in a list. A program that needs one draws it in an area.
- **One font, one size.** Text is the console's 8 x 16 cells (code page 437, '?' for a character it lacks), as the
  desktop's own windows are; `gw_text_scaled()` stays for large digits. No proportional text: that is the browser's
  own (user/wundertow/font.c).
- **No undo in a line of text.** A field is short; Esc is the program's (a chat line cleared, an edit cancelled).
- **No mnemonics (Alt and a letter):** the desktop's menus are reached with Ctrl+G, and a menu item's shortcut is a
  key of its own.
- **An area is not scaled.** Paint's zoom is Paint's: it draws its pixels at its scale, and scrolls by pixels.
- **The damage list is short** (16 rectangles, then the box around them): a window that changes everywhere at once
  presents itself whole, which is what it would cost anyway.
