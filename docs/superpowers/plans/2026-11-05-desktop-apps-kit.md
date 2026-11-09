# Account: track kit, libgui's toolkit

The brief is `docs/superpowers/plans/2026-11-05-desktop-apps.md`. This track built the window toolkit the desktop
applications stand on: `gk.h` in libsurf (user/libsurf/gk*.c), its fake desktop for host tests, and a demonstration,
`wkit`. Branch `apps/kit`, worktree `06-restoration-apps-kit`, volume `surfos-apps-kit`.

## Where the code comes from

Everything here is written for this tree, from its own code and from no other toolkit's source.

- **The look** is the desktop's own, read from this tree: `shell/guidraw.c` (the palette, buttons, scroll bars and
  their thumb arithmetic, `gd_thumb()` and `gd_scroll_to()`), `shell/guidialog.c` (the dialogs' layout, the text box,
  the buttons from the right), `shell/guicore.c` and `shell/guimon.c` (toolbars, the lit tab, status bars, list rows).
  Check boxes, switches and radio buttons, which the desktop has none of, are drawn here in its colours; so are
  stage 2's slider and palettes. The progress bar is the desktop's meter (`shell/guimeter.c`), and the text of
  several lines edits as Notepad does (`shell/guinote.c`).
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

### Stage 2

- **A text of several lines** (`gk_text_area()`, `gktext.c`), for small texts (notes, a description, an address
  list): edited as Notepad edits, lines at \n that do not wrap, the view following the cursor down and across, a tab
  to the next multiple of 8 columns; the keys of a field (Ctrl with the arrows by words, Ctrl+Backspace and
  Ctrl+Delete a word) and Up, Down, Page Up, Page Down, Ctrl+Home and Ctrl+End, Up and Down keeping their column; the
  mouse (a click, a drag that scrolls past an edge, a double-click a word, a third click the line); the clipboard (all
  of a paste, a CR LF as one line end); `GK_READONLY` to show text that is selected and copied but never changed; Tab
  moves on unless `GK_TAKES_TAB`. It keeps one buffer of at most its max bytes and finds its lines by scanning it:
  its work grows with its text, which is why it is for small texts and an editor draws an area of its own.
- **A progress bar** (`gk_progress()`): the desktop's meter (System Monitor's bar), filled in teal, its text in the
  middle in the accent's text over the fill and in ink beyond it.
- **A slider** (`gk_slider()`): a whole number from min to max by a step, by the arrows, Page Up and Down (ten
  steps), Home and End, a click or a drag on the track (the value under the knob's middle), the wheel.
- **Palettes** (`gk_palette()`): cells of colours (`gk_palette_color()`) or small pictures
  (`gk_palette_icon()`, pixels whose top byte is set see-through), one chosen, by a click or the arrows: Paint's
  colours and tools.
- **What the app tracks report needing:** no app track had reported a need to this track by the time stage 2 was
  done (no message reached it), so stage 2 is the brief's list; an app that needs more draws it in an area, and the
  integrator can route a request here.
- **The docs:** `user/include/gui.h` (floating point is allowed: the kernel keeps each program's registers since
  robustness B2; libgui and the toolkit draw in whole pixels) and `docs/GUI.md`'s "Programs' windows" (the same, and
  the toolkit). CLAUDE.md's "integer-only" note in "Windows for programs" is the integrator's to change (the brief
  gives the docs to this track, the coordinator kept CLAUDE.md for the integrator): it should say that a windowed
  program may use floating point, and point to gk.h.
- **wkit** gains a fourth page, More: notes, the seconds of the minute, a volume, colours and tools.

### Stage 3, on the desktop's own calls

Desk's implementation with the review of its stage 2 (apps/desk 1447a4c2, which holds 2c5afd30) merged, and the
toolkit checked against the real calls where the fake and -ENOSYS had answered until then, on the screen
(tools/gui_apps_kit.py, through wkit) and on the host:

- **Menus.** wkit's File and View stand in the bar in place of File and Edit; Ctrl+G, Right, Enter chooses
  File > Open; View > Speed opens in View's place and its Normal is chosen (WIN_EV_MENU with View's id); Help >
  About the toolkit is the program's item first in the desktop's Help. The fake's menu checks are the kernel's
  functions of 1447a4c2 copied (`menu_cells()`, `reads_as()`, `desk_title()`, `desk_help_item()`, `menu_check()`),
  so the limits gk keeps as it builds (14 entries, 11 in Help, 48 bytes, the desktop's titles and Help items) are
  the kernel's.
- **Choosers.** The title bar names whom the chooser is for, then its own title: 2c5afd30 named the window,
  1447a4c2 the program ("wkit: Open a file"), so the screen checks know a chooser by its own title after a name
  (aac42b11 did this first for stage 2's check, which every app's full scenario runs); Esc answers 0. Another dialog that takes a
  chooser's place answers it 0 the same way (`client_chooser_dropped()`), which gk cannot tell from Esc and need not:
  no way was found to open one over a program's chooser from the screen (Print Screen's waits until no dialog is
  up), so that case is the answer 0 the screen checks see. **EAGAIN**, new in 2c5afd30's header (no room for the
  answer while WIN_EVENTS events wait unread): gk_choose() keeps the request, says 0, and gk_next() asks again once
  it has read the events (`gk_choose_again()`); refused by then (the focus gone), it is answered as cancelled, so
  the program hears of every request. On the screen, Ctrl+Shift+O keeps wkit busy for three seconds while 72
  presses of F12 fill its queue (wheel notches did not: QEMU's mouse merges them into few reports); the events
  lost in the spell (WIN_INFO) prove the queue was full when it asked, and the chooser comes all the same. **EACCES without input since the last request:** gk's own rule (only while the
  program answers a key, a press or a menu choice) is the stricter: a second request can follow only the first's
  answer, which gk reads after every event queued before that request, so an event that lets the program ask was
  sent after it. The kernel's refusal is a backstop gk does not reach but for the focus; the fake keeps it, as it
  keeps EAGAIN. Desk's fix round makes WIN_DIALOG say EBUSY while an answered path waits to be taken (2c5afd30
  let a second request replace it): gk takes the path (WIN_TAKE) as it dispatches WIN_EV_DIALOG, before the program
  hears of anything after it, so its next request is never refused for it; a request that meets EBUSY anyway comes
  back to the program as -1, and one the toolkit kept (EAGAIN) and asks again is answered as cancelled. The fake
  refuses the same way.
- **Paste.** Copy pastes back at once and gets the window's own copy, which the desktop has not taken yet (the
  kernel's rule); the notes' 71 bytes pasted over the 64-byte name (WIN_PASTE says ENOSPC to the toolkit's buffer)
  are read whole and the field takes the first line.
- **Theme.** SurfOS > Ocean / dusk colors reaches wkit as GK_EV_THEME, dusk and back. Dusk changes only the desktop's
  role (WIN_COLOR_DESKTOP); a window's colours stay as they are, so the toolkit's windows look the same in both.
- **Keys.** Alt+X (no Esc before it), Ctrl+0, Ctrl+Enter and Ctrl+Esc reach the program with their modifiers;
  Ctrl+Backspace deletes a word in a field; Ctrl+Tab moves the focus. WIN_MOD_REPEAT cannot be made on the screen:
  QEMU's sendkey gives a make and a break, never the keyboard's repeat, so repeats are host-tested only (a field
  types each; a shortcut held down repeats). Shortcuts are matched on the modifiers' bits, a repeat or an unknown
  bit beside them changing nothing (host-tested).
- **Menus, desk's review of its stage 2:** an item of the program's Help that reads as one of the desktop's own Help
  items (About SurfOS, Keyboard and mouse, which follow it in the same menu) is refused by gk_menu_item(), read as
  the kernel reads it (the cells drawn, blanks around them, any case), and by the fake, which copies the kernel's
  `desk_help_item()`. The fake also takes the kernel's new rule for a chooser's input: a press over the content
  (a button the event before did not have), not a button held from elsewhere.
- **A drag only from a press the window saw:** a widget hears buttons held only while it has the pointer from a press
  over it (gkcore.c's capture), so a button held since a press elsewhere (on a desktop dialog, outside a box) is a
  hover over a field or an area, never a selection or a stroke; an area's GK_EV_POINTER says no buttons then.
  1447a4c2 stops sending such a held button after a click on a desktop dialog, and a leave carries no buttons;
  the toolkit does not rely on either. The close button sends CLOSE again to a program that is alive (1447a4c2):
  gk passes each on, and wkit's question is then asked again (the one open answered first, as a box over a box is).
- **A shortcut acts once a press (the games review):** a menu item's shortcut matched the keyboard's repeats as well
  (shortcut_of() keeps only Shift, Alt and Ctrl), so a held key acted again and again: in Solitaire, Ctrl+Z with two
  repeats undid three deals, and a held Ctrl+N dealt on. A repeat of a shortcut's key (WIN_MOD_REPEAT) now does
  nothing, the program not hearing it as a key either, unless the item opts in with GK_ITEM_REPEAT, for what holding
  the key naturally does again (Undo, Redo, Zoom in and out). Fields still type every repeat. test_gk: Ctrl+Z pressed
  with two repeats is one Undo, Ctrl+= with GK_ITEM_REPEAT three Zoom ins, with or without a field's focus.
- **A box is modal for the menus too (the Paint review):** while gk_message(), gk_confirm() or gk_ask() was open,
  an item chosen in the desktop's bar still came to the program as GK_EV_MENU, so a question was no question (in
  Paint, Clear could act behind "Save the changes?"). Now, while a box is open, the menus go to the desktop greyed:
  every title WIN_MENU_DISABLED, which greys its menu and the program's part of Help with it, and they go again as
  they are when the box has gone (gk_menus_send() compares the box with how it last sent them). Greyed rather than
  only dropped: the bar then shows why nothing happens, instead of items that look ready and do nothing, and it costs
  two WIN_MENU calls a box, the menus being sent whole anyway. A choice made before the greyed menus reached the bar
  is dropped (gkcore.c), and no shortcut gets past a box, which takes every key. What else could get past a box was
  checked the same way: keys, the pointer and the wheel go to the box alone; the close button is the program's to
  answer (GK_EV_CLOSE comes), as are its own timers, descriptors, choosers' answers, the theme and a new size.
  test_gk: a box open, the titles greyed, gkf_menu_choose() refused, a choice sent as it is dropped, Ctrl+O and F6
  nobody's, the close button heard; the box gone, the menus as they were and an item chosen again.
- **A password's bytes, all of them (the Settings review):** gk_field_wipe() left the bytes of a character typed
  partway (gkfield.c's part[]), and part[] kept them after they went in too. Every zeroing of what a field, a text, a
  paste, a box's answer or a key event held goes through gk_wipe() now, a volatile loop no optimization drops
  (memset() before free() is a store a compiler may leave out); part[] is zeroed as its bytes go in and at a wipe;
  each key event's copies (the toolkit's, and its slot in gui.h's queue) are zeroed once dispatched. A field keeps no
  undo, last key or selection copy of its own, and a password field no history or draft. test_gk looks at the
  field's state (gkpriv.h's struct gk_line, there for it): after a wipe every byte of its buffer and part[] is 0, and
  no key is left in gui.h's queue; with part[]'s wipe taken out, the check fails.
- **Fixed first, a commit of its own on stage 2 (eab009d3), before the merge of desk:** `gk_next()` with a time that
  had run out (0 above all: a game's frame) answered 0 after one read of the window's events (gui.h's 16) that made
  nothing for the program, though more waited unread: a burst the toolkit takes itself left the rest, and what came
  after it, for a later call. It reads on now while the window has events, a few batches more (GK_BATCHES) once its
  time is up. With -1, as the documented loop waits, it never said 0 (test_gk's bursts: -1, 1000 and 0; only 0
  failed before). The EAGAIN test found it.
- **What the real desktop does that the header does not promise:** nothing found.

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
- **test_gk, stage 2:** the text of several lines (typing, lines joined and split, Up and Down keeping their column,
  a tab's columns, its max, the view following the cursor, the wheel, a click, read-only text copied but never
  changed, the amended keys: Ctrl+End, Ctrl+Backspace, Ctrl+Left, Alt+X, Ctrl+Enter and Ctrl+I the window's, a tab
  typed where it takes Tab and Ctrl+Tab moving on); the progress bar's fill and text; the slider by keys, a click, a
  drag and the wheel; palettes of colours and pictures by a click and the arrows.
- **test_gk, stage 3:** gk_next() after a burst the toolkit takes itself (-1, a time left, 0); choosers kept on
  EAGAIN and asked again, or answered as cancelled when refused by then; EBUSY for an untaken path; the window's own
  copy pasted; shortcuts on their modifiers' bits; Help items the desktop's refused; a drag only from a press seen;
  every byte of a wiped password; menus greyed under a box (gkf_menu_choose(), a late choice dropped, shortcuts
  nobody's, the close button heard); a shortcut once a press, and GK_ITEM_REPEAT's repeats.
- **test_wkit**: the demonstration's window driven as a person drives it, on the fake desktop (stage 2: its More
  page; stage 3: keys with their modifiers, the theme, its own paste, the busy Open).
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
- **Stage 3** (eab009d3, aac42b11, the merge of desk 1447a4c2 and five commits, tip 7a751f3b): `make unittest`
  passed, test_gk 470 checks and test_wkit 76 among it; `make test-all` passed at -O0 and -O2, 51 Files and 558 smoke
  checks each; the guiapps scenario passed at -O0 and -O2, 67 of 67 checks, 61 of them wkit's, on desk's real calls.
  (A test-all of eab009d3 alone that straddled the Mac's night asleep and the runaway's load failed 43 of 553, in
  kcmdtest, pipetest and ftpd; it was not looked into, as the coordinator asked: the tip's run, which holds that
  commit, passed.) Each of the five commits checked out alone, its host tests run natively with ASan and UBSan
  (test_gk 457, 461, 467, 470, 470; test_wkit 70, then 76 with wkit's commit).
- **Stage 2** (its five commits): `make unittest` passed, test_gk 430 checks and test_wkit 70 among it; `make test-all`
  passed at -O0 and -O2, 51 Files and 558 smoke checks each; the guiapps scenario passed at -O0 and -O2, 41 of 41
  checks, 35 of them wkit's (a first run at -O0 failed one of the More page's own checks: it read a palette cell's
  pixel under the pointer its click had left there; the check moves the pointer off first now). Natively with ASan
  and UBSan: clean; each commit checked out alone, compiled, its host tests run.

## The review

An independent review read d38ea834 (11/09/26). It found memory safety sound under many fuzz batches with ASan and
UBSan through the fake (failed allocations, extreme values, choosers and the pointer included, nothing drawn outside
the presented damage), the GK_PASSWORD field sound, and modality, shortcuts once a press, drags, choosers and
gk_next()'s drain holding; the code's origin clean. One fix round followed, each finding its own commit with its test
(F14 alone left, as documented):

- **F1, critical: a secret left in freed memory.** A chat line's draft was freed unwiped by Up, kept after Down,
  left by the program's gk_set_text(); a history shrinking or full freed lines unwiped (Chat masks NickServ IDENTIFY
  lines and relies on it). Every copy of a line goes through drop_copy() now (gk_wipe(), then free()), and the draft
  goes once it is back, when the program sets the text and at gk_field_wipe(). test_gksecret builds libgui and the
  toolkit with each free() looking for the secret in the block first (-Dfree=gks_free).
- **F2, high: timers starved the window.** A timer due again at once (a 10 ms timer, a handler of 20) was all a
  program heard, keys unread and nothing painted: each turn of gk_next() now paints and polls (at once when a timer is
  due) before it posts the timers due.
- **F3, high: Ctrl+X deleted what the clipboard refused** (over 128 KiB, or without the focus). A cut deletes only
  after gk_copy() returned 0, and a refused copy or cut is told as GK_EV_UNCOPIED with the errno.
- **F4, high: key repeats answered boxes and repeated buttons.** Boxes ignore the repeats of their own keys (Enter
  held on Delete no longer answers the question it opens); buttons, check boxes, radio groups and tabs act once a
  press; a line to type still takes its typing's repeats.
- **F5: a question's default and its ends.** gk_confirm_default() names the button Enter answers at first, and Left
  and Right stop at the ends: Left from a safe Cancel put first never comes round to the destructive choice.
- **F6: Ctrl+[ \ ] ^ and /** come as 27 to 31 with WIN_MOD_CTRL and now match the shortcuts written so.
- **F7: data changed without a word.** gk_set_text() returns 0 when the widget holds the text as given, 1 when it
  holds it changed to fit (cut at its max, a control character a space), -1 without memory.
- **F8: the fake desktop.** Its clock moves by poll()'s waits (the shim sends the toolkit's poll() to gkf_poll()), a
  wait for ever with nothing to come fails as EINTR instead of hanging; the window's queue merges and loses as
  kernel/win.c's does (so the EAGAIN tests fill it with keys, which do not merge); a chooser takes the keys while it
  is up; gkf_type() types capitals with Shift; test_gk's run() stops at 100000 events.
- **The lows:** a watch ends once told POLLHUP, POLLERR or POLLNVAL (F9); a field's view start in one pass, 64 KB in
  milliseconds instead of 6 s (F10); a label's lines without strlen() at each character (F11); a box's own line is
  the box's, the program hearing its answer only (F12); a lone first byte before a lone continuation byte no longer
  leaves the cursor inside the character they make (F13); sizes, gaps, padding, weights, damage and a slider's range
  held within GK_SIZE_MAX or done in 64 bits (F15); an event's text that cannot be kept (no memory) is a failure,
  never an empty success (F16).
- **A fuzz with invariants** (test_gk's fuzz): 20000 seeded events of every kind over every kind of widget, menus
  and boxes; after each, the window alive, the memory there, every line's text whole within its max with its cursor
  on a character, no password on the clipboard, no menu choice in a box, no event of a box's own widget.
- **The review's probes** (review-kit/probes, read only), built against the fixed toolkit in this track's scratchpad:
  secret 0 blocks freed unwiped; starve keys and the field typed among the timers; cut the 200,000 bytes and the
  field kept; gaps 64 KB fields and labels in milliseconds, a dead descriptor told once, no box event, Space held on
  a check box one change.
- **The slider's rounding** (F15's commit) used a 64-bit division, which i386 makes a call to libgcc, which libsurf
  does not link: the smoke test's build of wkit failed. It goes through gk_div64() now (ab8b676b), and the track's
  native runs compile the toolkit for i386 and look for any libgcc division.
- **Counts at the gates** (tip ab8b676b): `make unittest` passed, test_gk 514, test_wkit 76 and test_gksecret 10
  among it; `make test-all` passed at -O0 and -O2, 51 Files and 558 smoke checks each (a first run's -O2 smoke test
  failed the kernel's selftest while the owner's programs loaded the Mac; run again, as the coordinator asked for a
  boot-timing step, it passed; this track changes nothing in the kernel); the guiapps scenario passed at -O0 and -O2,
  67 of 67 checks, 61 of them wkit's. Natively, ASan and UBSan (undefined behaviour fatal) found nothing.

## A test run that ran away

On 11/05 an early stage-1 run of test_gk (scratchpad/apps-kit/native/test_gk), started without a time limit, did not
end: most likely the loop() test's watched pipe while it stayed readable (watches are level-triggered, so gk_next()
gave GK_EV_FD for ever) before the test read its byte. The Bash call's own timeout ended its shell and left the
binary orphaned; it ran 16 hours at full load, through the Mac's night asleep, until the integrator stopped it, and
QEMU boots of every track timed out meanwhile. Every native run of this track's since goes under perl's alarm
(`perl -e 'alarm N; exec @ARGV' ...`), mutation checks included.

## Left out, and why

- **No widget the six applications do not need:** no tree view, combo box, spin box, tooltip, drag and drop between
  widgets, or multiple selection in a list. A program that needs one draws it in an area.
- **A text of several lines neither wraps nor undoes nor finds:** it is for small texts; Notepad and the Code
  Editor are programs of their own.
- **On the screen, two things are host-tested only:** the keyboard's repeats (QEMU's sendkey gives a make and a
  break, never a typematic repeat), and a desktop dialog taking a program's chooser's place (no way was found to
  open one over a chooser from the keyboard or the pointer: Print Screen's waits until no dialog is up). Both reach
  the toolkit as what the host tests drive: a key with WIN_MOD_REPEAT, and WIN_EV_DIALOG 0.
- **One font, one size.** Text is the console's 8 x 16 cells (code page 437, '?' for a character it lacks), as the
  desktop's own windows are; `gw_text_scaled()` stays for large digits. No proportional text: that is the browser's
  own (user/wundertow/font.c).
- **No undo in a line of text.** A field is short; Esc is the program's (a chat line cleared, an edit cancelled).
- **No mnemonics (Alt and a letter):** the desktop's menus are reached with Ctrl+G, and a menu item's shortcut is a
  key of its own.
- **An area is not scaled.** Paint's zoom is Paint's: it draws its pixels at its scale, and scrolls by pixels.
- **The damage list is short** (16 rectangles, then the box around them): a window that changes everywhere at once
  presents itself whole, which is what it would cost anyway.
