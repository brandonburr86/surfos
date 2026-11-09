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
  as xterm's "ctlseqs" documentation describes it; the `WIN_MOD_*` bits are that m - 1. The desktop's own raw mode
  (`TTY_RAW_KEYS`, after the review) sends a character with modifiers as `CSI c ; m u`, the form of the "fix keyboard
  input on terminals" proposal published by Paul Evans (and taken up by several terminals since), from its
  description; the repeat's 16 in m is SurfOS's own, since only SurfOS's keyboard driver and desktop speak it.
- A menu's text is checked with the console's own UTF-8 decoder (`driver/cp437.c`'s `utf8_feed()`, which follows
  Unicode's table 3-7), so that what passes is what the desktop draws.
- Menus with shortcut text, checked and greyed items, submenus, and Open, Save as and Choose folder choosers follow
  the *behaviour* of desktop menus and file choosers in general (classic Mac OS and Windows menu bars, GTK and Windows
  file dialogs) as a user sees them; nothing of their code or look was copied. SurfOS's own menus and choosers
  (`shell/guicore.c`, `shell/guidialog.c`) are what the new ones extend: a submenu drills down in its menu's place
  as Apps > Programs does.
- The registry's text format is SurfOS's own, in the style of `surfos-config/settings.conf` (`net/netcfg.c`):
  "key = value" lines and # comments. Its icons are rows of palette letters, as `shell/guidraw.c`'s icons already
  are.

## Stage 1: the ABI, and its amendment

Committed first, for kit and the six applications to build on (1acdce47): `include/surfos/win.h` (its header comment
is the documentation), `rootfs/apps/README` (the registry's format), the argument checks in `kernel/win.c`, and their
scenarios in `tests/host/test_win.c`. A request that passed its checks was answered `-ENOSYS` until stage 2.

An independent review of that commit asked for changes before anyone built on it; they are b513bfe2, touching only
the header, the checks, test_win, the README, the size checks and one Makefile prerequisite (test_win links
`driver/cp437.o`):

- **Menu text as the desktop draws it.** A label or shortcut is UTF-8 by the console's rules (whole sequences, no
  overlong form or surrogate) with no control character (C0, DEL, C1): `-EINVAL` otherwise, since the desktop would
  draw a stray byte as a cell of its own. The bar's width is counted in the cells the desktop draws.
- **No menu passes for the desktop's.** A title that draws the desktop's sign (U+224B ≋, or U+2248, which the console
  draws alike), or reads SurfOS, Apps, Window or Help in any case with blanks around it, is refused, compared on the
  cells drawn (a no-break space is a blank). A chooser's title bar names the window it is for (stage 2).
- **Sizes that fit.** 14 entries a menu (15 did not fit a 640 x 400 desktop); labels of 48 bytes (an IRC channel's
  name); `WIN_MENU_SUBMENU` and `WIN_MENU_CHILD` (one level, the children right after their item) and
  `WIN_MENU_HELP` (one title, at most 11 items, merged into the desktop's Help, not in the bar).
- **Room to grow.** `struct win_dialog` gained `flags` (0, `-EINVAL` otherwise); its types are 16 within 144 bytes,
  an extension of 1 to 15 letters and digits or a whole name after '=' (`=Makefile`), read by one parser in the
  header, `win_types()`, which the registry shares. `struct win_theme` gained a count and 32 slots, the rest 0.
  Their sizes (72, 604, 136 bytes; `win_menu` and `win_buffer` 8) are checked at compile time in `kernel/syscall.c`
  and libsurf's `syscalls.c`.
- **Going on without.** `-ENOSYS`, like an old kernel's `-EINVAL`, means a program goes on without the call;
  `WIN_THEME` is the test for the new ops.
- **The keys, exactly.** Ctrl+Tab, Ctrl+Enter, Ctrl+Esc and Ctrl+Backspace are `WIN_KEY_TAB` .. `WIN_KEY_BACKSPACE`
  (no Ctrl+I, Ctrl+M or Ctrl+[); Ctrl with a digit or other punctuation comes as the character with `WIN_MOD_CTRL`
  (Zoom 100%, Ctrl+0); `WIN_MOD_REPEAT` marks a key the keyboard repeats; programs test the bits they use. Stage 2
  makes the keyboard driver deliver this.
- **The chooser, documented for stage 2**: a request needs a key, a press or a menu's item sent to the window since
  its last one (`-EACCES`), so that a program that asks again after each answer cannot hold the desktop's keys; its
  answer's place in the queue is kept and it always comes (0 when the desktop lets the window go); `WIN_TAKE` works
  after the hang-up and keeps the path on `-ENOSPC` and `-EFAULT`.

Decisions of stage 1 that stand:

- **WIN_VERSION stays 1; struct win_info stays 20 bytes.** Programs built before have 20 bytes for it (libgui's
  `gw_open` reads it into a stack struct), and `user/libsurf/syscalls.c` checks its size. So the theme is
  `WIN_THEME`, a struct of its own. A window whose program asked it once gets `WIN_EV_THEME` at each change: no
  program that never asked sees an event it does not know.
- **Theme colours by role, not by name** (`WIN_COLOR_WINDOW`, `_TEXT`, `_ACCENT` ...), so that a dusk that one day
  darkens windows changes values, not names. Ocean and dusk differ only behind the windows today, as the desktop's
  own windows do.
- **WIN_MENU replaces the whole set**, a fixed-size array of entries copied in one piece: one bounds check, no
  pointers inside to chase.
- **WIN_PASTE copies during the program's call**, only while the window has the focus (as WIN_CLIP writes only
  then); a buffer too small gets nothing and `-ENOSPC`; size 0 asks the length.
- **WIN_DIALOG** asks for one chooser at a time per window, only while focused (a chooser takes the keys: a program
  in the background must not pop one up under a password being typed). The path comes with `WIN_TAKE`, never half
  copied. The pair care (a `.tmp` beside a file) is the chooser's: it never answers a Save as with a file the
  desktop takes care of.
- **Strings in requests:** titles and labels are cut between characters as window titles are; paths, names and
  types must carry their NUL and no control character, since they name files.

## Stage 2: what was built

**The calls in the kernel** (`kernel/win.c`).
- *Menus* are the kernel's checked copy of the last WIN_MENU (`struct menus`, held by a count), lent to the desktop
  with its pixels: `win_update()` gives it the copy in force (`struct win_view`'s `menu`, `menus`), which stays its
  until its next update or close. Nothing is copied with interrupts off (the review); a program that sets new menus
  meanwhile only drops its own count on the old copy.
- *The clipboard*: WIN_PASTE copies from a copy the desktop gives the kernel (`win_paste_give()`), held by a count
  while a paste copies, so that a new copy given meanwhile frees the old one only afterwards. The window's own
  WIN_CLIP that the desktop has not taken yet is newer than that copy, so a paste gets it; when the desktop takes it
  (`win_clip_take()`, from the focused window) it becomes the copy in the same section, so a paste never sees text
  older than the last copy. Another window's pending text is not the clipboard's (its window lost the focus; the
  desktop drops it) and is never pasted. (`struct desk`'s pending text is a `struct paste` now, for the count.)
- *Choosers*: a request waits in the window until the desktop takes it (`win_dialog_take()`, which numbers it); the
  desktop answers by window and number (`win_dialog_answer()`, `-EINVAL` for a number not waiting), which sets the
  path and queues WIN_EV_DIALOG in one section. A request needs input since the last one (`struct win`'s `input`,
  set by a KEY, a MENU or a press that was queued: since the review of stage 2, a pointer event over the content with
  a button the one before did not have) and room for its answer: while it
  waits, the queue's last place is the answer's (other events are lost there, and counted), and a request made with
  a full queue is `-EAGAIN` (the header says so). The hang-up answers a waiting request with 0 in that place; a path
  chosen stays for WIN_TAKE, which works after the hang-up, until the program lets go.
- *The theme*: WIN_THEME reads the window's own copy of its desktop's theme (`win_theme_set()` updates every window's
  and queues WIN_EV_THEME to those that asked), so a window let go says the last it had.
- What a window holds goes when either side lets go (`hang_up()`, `win_release()`; the desktop's lent menus at its
  close); every scenario of `test_win` ends with nothing allocated. The order of refusals: the argument first
  (-EFAULT, -EINVAL, -ENOMEM for its copy), then -EPIPE (but WIN_TAKE), then -EACCES (focus), -EBUSY, -EACCES
  (input), -EAGAIN. WIN_TAKE's: its buffer (-EFAULT, as many bytes as size or a path's longest), then -ENOENT, then
  -ENOSPC.

**The keys** (`kernel/keyboard.c`, `kernel/tty.c`, `shell/gui.c`, the model). The review asked for Ctrl+Tab apart
from Ctrl+I, Ctrl+0 at all, and a repeat bit if the driver can tell; stage 1 had read Shift and Ctrl from the
keyboard's state as the desktop took a byte, which missed a modifier let go before then. So the desktop's console now
reads in `TTY_RAW_KEYS` (`tty.h`, `TTY_RAW`'s 2): `keyboard_byte()` hands that tty each key before Ctrl makes a
control byte of it, and `tty_key()` sends a plain character as its byte and any other key as `ESC [ c ; m u` (or
xterm's form for the special keys, always with m). The keyboard says a repeat by its make code again with no break
between (typematic; `key_seen()`, Shift's fake E0 prefixes ignored), and m carries 16 for it. TTY_RAW 1 is
unchanged (the repeat is masked there), and the mode ends with the task that set it, as raw mode does.
The desktop turns bytes and sequences into "full keys" (`gui_byte_key()`, `gui_sequence_key()`: a character as typed
with GUI_KEY_* bits), and the model gives them out: a program's window as the header says (`gui_program_key()`), the
desktop's own windows as before (`gui_own_key()`: the control bytes; nothing for a key Ctrl made nothing of, as the
driver dropped it). Ctrl+G and Ctrl+Space open the bar, Ctrl+@ is a program's. A serial terminal's bytes keep their
rules (an ESC with a byte in the same pass is an Alt key). **What changed for programs built before:** an Alt key is
no longer an Esc and then the key (that Esc closed wcalc, wclock and wcharmap); Ctrl with a digit or punctuation now
reaches them as the character with WIN_MOD_CTRL (a program that ignores mods sees the character: Ctrl+1 types 1 in
the Calculator); Ctrl+Tab, Ctrl+Enter, Ctrl+Esc and Ctrl+Backspace are their WIN_KEY_*. The desktop's Terminal still
sends an Alt key's Esc to its shell (surfsh's Alt+.), as a terminal does; the desktop's other windows get the key
without the Esc.

**The desktop** (`shell/gui*.c`).
- *Menus* (`shell/guicore.c`): the bar is a list (`bar()`): the six, or "≋ SurfOS", the program's menus but its Help,
  Apps, Window, Help. A program's Help items come first in the desktop's Help, a separator after them. The menus an
  item opens (Apps > Programs, File > Open with, a program's submenu, `MENU_CLIENT_SUB`) drill down in their parent's
  place, rather than opening beside it: no second box to keep on a 640-pixel screen, Right or Enter goes in, Left
  goes back, and a submenu whose item is gone closes at the next tick. An item that opens a submenu, and the
  children of a greyed one, are never chosen. A menu taller than its room scrolls (`menu_box_of()`); Apps grows by
  Programs only when the registry has a program for it, which is why Programs comes after Run program... (its
  keyboard place, 13 down, is what `tools/gui_apps_win.py` counts). A full bar at 640 x 400 is 79 of the 80 cells;
  "Ride the wave." gives way, and every menu stays on the screen.
- *The clipboard* is counted (`gui_clipped()`, every writer of `g->clip` in `shell/gui*.c` goes through it) and given
  to the kernel at the start of a tick, before that tick's FOCUS events, whenever a program's window has the focus and
  the text changed since it was given.
- *Choosers for programs* (`client_tick()`'s `chooser()`, `dialog_chooser_for()`): shown when taken, after the tick's
  updates, if the window still has the focus and no dialog is up; otherwise answered as cancelled at once. Their
  title bar reads "the window's title: the chooser's" (the window's cut to 20 cells), so no program's chooser passes
  for another's or the desktop's. There is one dialog at a time: one that takes a chooser's place ("End the
  program?" included) answers it as cancelled (`client_chooser_dropped()`, from `begin()`); one whose window goes is
  closed. Answers name the request's number. Save as checks the answer as Notepad's does (`note_guarded()`,
  Replace?). Types filter the list through `win_types()` (extensions and whole names); Choose folder lists folders.
- *The registry* (`shell/guireg.c`, `include/sys/gui_reg.h`): read in the first tick into the desktop's own state (a
  4 KB buffer there, nothing allocated), bounded as the README says. Its types and open-with are read by
  `win_types()`. A file whose name is not letters, digits, '.', '-' and '_' is left out with a line that does not
  name it, and no line of the log carries a value's bytes (a key it does not know is "a key"). A second entry for one
  program is left out, and the log says which came first. Apps > Programs, tiles after the suite's, Files' Open
  (first, by its types) and Open with, icons of the windows it started. A program started from Apps or a tile is
  remembered, so choosing it again brings its window back; one started with a file is new each time, with the path as
  its one argument (`gui_sys.run_file`).
- *Screenshots*: `kernel/keyboard.c` remembers a Print Screen (E0 37) for the console in front and wakes the desktop;
  `keyboard_print_taken()` takes it for the desktop of that console only. The shell copies the frame just drawn into
  a 24-bit BMP and hands it to the model, which asks where (`Screenshot YYYY-MM-DD HHMMSS.bmp` in the first writable
  volume) once no dialog is up and saves it through `gui_fs.write` (the .tmp-and-rename save). The shell frees it when
  the model is done, and at the desktop's end.
- *Preferences*: `desktop.theme` and `pointer.speed` in `net/netcfg.c`'s table; the desktop polls them each second
  through `gui_sys.prefs`, edge-triggered (a change made by Settings applies at once); Pointer & desktop writes a
  change with `netman_set()` (`gui_sys.pref_set`) once it has stood (the review of stage 2) and says in its last line
  whether it was saved.
- *The theme* to the kernel: once at the start and at each change (`client_theme()`).

**Tests and tools.** `wintest desk [file]` (`user/bin/wintest.c`) uses the new calls bare: menus with a submenu and a
Help, a chooser asked with its title set first (what the chooser's bar names); `rootfs/apps/wintest.app` (apps = no,
desktop = no, the type `wintest`) is the registry's one entry on this branch, so that Files' Open and Open with have
something to open on the screen. `tools/gui_apps_desk.py` drives it.

## Outside my files

The brief gives desk `kernel/keyboard.c`'s Print Screen; the review's keys needed more, and these are the closest
sound places for it:
- `kernel/keyboard.c`: the keys of a `TTY_RAW_KEYS` tty before Ctrl, and the repeat (`key_seen()`); `kernel/tty.c`:
  `tty_key()`'s encoding for that mode, `tty_ctl()` taking 2; `include/surfos/tty.h` (its documentation, the
  constant) and `include/surfos/keyboard.h` (`KEY_REPEAT`, `keyboard_print_taken()`). `make test-usb` covers them
  (the boot path and the keyboard), and `shell/selftest.c`'s `test_tty` checks the encoding (its check that TTY_RAW 2
  is refused is now one for 3).
- `kernel/netman.c`: two lines in `copy_key()`, for `NETCFG_THEME` and `NETCFG_POINTER`, without which a change made
  through netman never reaches the desktop until the next start (pure insertion).
- `user/bin/wintest.c`: its desk mode. `tests/host/guisys_host.c`, `tests/host/gui_fake.c` and `.h`: stand-ins for the
  new `gui_sys` calls (and the fake's file system holds 128 files, for a registry of 40 programs).
- `rootfs/man/gui.txt` and `rootfs/man/settings.txt`: a paragraph each.

## What was left out, and why

- **F1 to F4** never reach a program: they switch consoles.
- **A dusk look for windows**: ocean and dusk differ behind the windows only, as the desktop's own windows do.
- **Open with for folders**, and an Open with that remembers a choice: not asked for.
- **Screenshots at the screen's own resolution**: the desktop's pixels are its logical ones; at 2x each is a 2 x 2
  block on the screen, so the BMP holds the same picture at a quarter of the bytes.
- **A submenu beside its item**: it drills down in its menu's place, as the desktop's own did already.

## The review of stage 2

An independent review read bb0a14ff (the code of 2c5afd30): no critical or high finding; the kernel's calls, the counted
objects and the refusals held up under ASan and UBSan. What it found, and what this fix round did; its probes are
tests now (K1-K5 in `test_win`, P1-P4 in `test_guiclient` and `test_guidesk`, the repeat harness in `test_keyscan`):

- **M1, the desktop's own pointer event defeated the chooser's input rule.** A click on a desktop dialog over a
  window, the pointer moving before the release, gave the window a "left" event with the button held, which the
  kernel counted as input (and Paint could take for a drag). Now `leave()` never carries a button (only a window
  holding the pointer, a press begun in it, hears of buttons), and a desktop dialog that takes the pointer ends the
  window's hover and its grab with no button held (`client_pointer_lost()`). The kernel counts as a press only a
  pointer event over the content with a button the one before did not have.
- **M2, a program's chooser could pass for the desktop's.** Its title bar named the window by its title ("≋ SurfOS:
  Save as" for a window titled SurfOS). Now it is named by what the program cannot set: its registry title, else its
  program's file name (the task's), else its pid; and a program's chooser has no ≋, which marks the desktop's own
  dialogs alone. The kernel refuses a Help menu's item that reads as the desktop's own Help items (Keyboard and mouse,
  About SurfOS), on the cells drawn, in any case.
- **M3, Pointer & desktop wrote the stick on each change, a held key 30 times a second.** A change applies at once
  and is written once it has stood 1.5 s (`GUI_PREF_MS`), or at the desktop's end; the toggles ignore the keyboard's
  repeat (checked before `gui_own_key()` drops it).
- **M4, the close button never sent WIN_EV_CLOSE again** (since 10/25: `closing` was never cleared). The rule now:
  each press sends CLOSE while the program reads its events; the kernel tells the desktop whether a CLOSE it was sent
  waits unread (`win_state.close_waiting`), and then the press offers "End the program?" (the program is stuck or
  busy). A program that reads CLOSE and ignores it can still be ended from its close button: a third press within
  `GUI_CLOSE_MS` (3 s), its two CLOSEs before read and ignored, offers "End the program?" too. Why these: whether a
  program reads its events is the one sign of life the kernel has without guessing at time, and three presses in a row
  is a person who means it; a program that asks its own question meanwhile is asked again at the next press.
- **L1, WIN_TAKE was not bound to its request** (an Open answered, then a Save asked before the events were read:
  WIN_TAKE for the Open gave the Save's path). WIN_DIALOG is now -EBUSY while an answered path waits to be taken; a
  cancelled answer leaves none (win.h says so).
- **L2.** WIN_EV_THEME is merged: one waits at most, the newest (100 changes queued 64 and lost 36 before).
- **L3.** A tile the screen has no room for is no longer clicked (the suite's and the registry's alike).
- **L4.** The controller's error forgets the key held (its break may be lost: the next press read as a repeat). The
  scan codes' prefix and repeat moved into `include/surfos/keyscan.h`, which `tests/host/test_keyscan.c` runs on the
  host, Print Screen's sequences among them; Print Screen held asks once. A keyboard that resumes repeating a held key
  after another was tapped gives its first resumed make as a new press (keyscan.h says so).
- **L5.** An unreadable chooser folder now falls back to the last chooser's, then /, as win.h says; win.h and this
  account give WIN_TAKE's refusals in the code's order (-EFAULT, -ENOENT, -ENOSPC).
- **L6.** A path the kernel refuses in an answer is answered as cancelled, so a request never waits on.
- **L7.** The registry's log lines keep their newline when cut.
- **L8.** Registry titles get the menu titles' checks: UTF-8 by the console's rules, no control character, not the
  desktop's sign or its menus' names (`gd_text_cells()`, `gd_reads_as()` in `shell/guidraw.c`, the desktop's copy of
  kernel/win.c's walk: the kernel's is tested by test_win without the desktop's objects).
- **L9, a program the desktop started could take its keys.** It shares the desktop's console as its terminal, so its
  `ttyctl(TTY_RAW, ...)` took the console out of TTY_RAW_KEYS, and its end dropped it to line mode. Chosen: while a
  terminal is in TTY_RAW_KEYS, TTY_RAW from any task but the one that set it is -EBUSY (`kernel/tty.c`). Why: it keeps
  the programs' terminal as it is (their output, Ctrl+C, the job rules) and changes only the one thing that broke; a
  program that wants raw keys has none there anyway (its input is /dev/null). `selftest` checks it, and the screen
  check runs `wintest raw` from the desktop before typing on.
- **Test gaps:** K5 (the answer's kept place at the close and at the desktop's death with a nearly full queue) is a
  scenario; `gui_fake` lends each update's menus in a copy of their own and poisons the copy before, so a stale
  pointer would be seen; `test_paste_given` checks that the clipboard is given before FOCUS; the screen check also
  cancels a chooser with the mouse, and closes wintest through its own question twice.

## The second review

The same reviewer read the fix round (2c5afd30..1447a4c2): it holds; the gates passed on 1447a4c2 (the coordinator
ran the clean test-all again: 558 smoke checks and 51 of Files at O0 and at O2). One medium and four lows, a low
from the image track's review, and a cleanup:

- **N1, a CLOSE lost to a full queue read as "the program read it".** A stuck program typed at fills its 64 events;
  the close button's CLOSE was refused, `close_waiting` said 0, and only three presses in 3 s offered "End the
  program?". Chosen: the window remembers a CLOSE its full queue refused until the program reads again
  (`close_lost` in kernel/win.c), and `win_update()` says a CLOSE waits meanwhile. Why: it changes nothing else of
  the queue (every event's room stays as win.h gives it, and the place kept for a chooser's answer), and a program
  that reads again gets the next press's CLOSE; a place kept for CLOSE would have taken one from every other event.
  The reviewer's K6 is `test_close_lost` in test_win, and the probe file gives "close_waiting 1" now.
- **N2, slow presses never reached "End the program?".** Now the third press asks, however slow: the first two send
  CLOSE while the program reads them, and the third, two read and the window still there, asks (the 3 s window is
  gone). Cancel there counts the presses from none again, so a program's own question ("Save the changes?") comes
  back at the next press instead of the desktop's for good. A CLOSE waiting unread asks at once, as before.
  (CLAUDE.md's "a second press ... asks" is the coordinator's to update at the integration.)
- **N3, the name "the program cannot set" could be set** by copying a program under another file name. A program's
  chooser now names its pid after the name, "Paint (pid 41): Save as", as "End the program?" does: the pid tells a
  copy from the registry's Paint, and costs nothing to keep true.
- **N4, the Help items' check was exact.** `reads_as()` (kernel/win.c) and the desktop's copy `gd_reads_as()` read a
  run of blanks (the no-break space too) as one and leave ASCII punctuation at the end out, so "About  SurfOS." and
  "Keyboard and mouse..." are refused, and a title "Window:" too. Known limits, in win.h: punctuation before the
  name, and code page 437's letters that look like ASCII ones; a line stands above the desktop's own items.
- **The image track's low: a program that fails before its window said nothing.** Programs the desktop starts (run,
  run_file) are `TF_TOLD`: kernel/task.c keeps the end of such a task, with whether it ever opened a window
  (`TF_WINDOW`, set by kernel/win.c), for the task that started it (`task_ends_take()`, a few kept, the oldest making
  room), and struct gui_sys's `ended` gives them to the desktop. One that ended killed, or with a status other than 0,
  before any window is said in a message, one at a time while no other dialog is up: "wpaint (pid 41) ended: status
  2", "It opened no window; its text went to the console." One that ends with 0 says nothing. The ends kept before a
  desktop began are let go at its first pass. Chosen over keeping the programs as the desktop's children: detached,
  init collects them as before, and nothing else of their life changes. The selftest checks the kept end.
- **Cleanup.** `gui_client.closing` (set, never read) is gone, with the 3 s rule's `close_at` and `GUI_CLOSE_MS`.
- Outside this round's files: `docs/ARCHITECTURE.md` still says the second press asks "End the program?".

## Tests

- Stage 1 (1acdce47): `test_win` 1,435 checks, `make unittest`, `make test-all` (558 smoke checks at O0 and O2).
- The amendment (b513bfe2): `test_win` 1,615 (the menus' text, titles that pass for the desktop's, submenus and a
  Help, a chooser's flags and types); `make unittest`; `make test-all` (558 at O0 and O2).
- Stage 2 (2c5afd30):
  - host: `test_win` 1,931 checks (menus set, lent to the desktop, the copy before freed at its next update, taken
    away; pastes given, whole, refused small, held by a count, the window's own copy first and made the copy as the
    desktop takes it, another window's never; choosers asked only after input, numbered, answered by number,
    cancelled, without memory, answered 0 at the hang-up in the place kept, a full queue's -EAGAIN and the last place
    kept for the answer, the path taken after the hang-up and gone with the program; the theme; each scenario
    ending with nothing allocated); `test_guiclient` 534 (full keys to either side, Ctrl+Tab, Ctrl+0, the repeat,
    Alt with no Esc but in the Terminal; submenus and the merged Help; the full bar at 640x400; choosers naming their
    window, answers by number, "End the program?" in a chooser's place); `test_guidesk` 136 (the registry's format
    with whole-name types, every refusal without a value's bytes, one entry a program, a file name the log cannot
    say, Programs at three sizes, tiles, Open and Open with, screenshots, preferences, and every .app file of
    rootfs/apps/). Unchanged: `test_gui` 576, `test_guiterm` 547, `test_guisys` 367, `test_guitools` 179,
    `test_guimedia` 1,086, `test_guisuite` 181, `test_netcfg` 146. The whole `make unittest` passes.
  - the kernel's selftest (`test_tty`, in the smoke test): TTY_RAW_KEYS' sequences, and raw mode 1 unchanged.
  - screen: `tools/gui_apps_desk.py` in the guiapps scenario, 40 checks (of the scenario's 319) at O0 and at O2: the
    menus by keys and mouse, a submenu's item and the program's Help item, seven modified keys (Ctrl+Tab, Ctrl+0 and
    Ctrl+Enter among them), two choosers naming the window and cancelled, the theme both ways, a paste, a file opened
    from Files with a space in its path and Open with, a screenshot saved on the stick and read back as a 960 x 540
    BMP.
  - `make test-all`: 558 smoke checks and 51 of Files at O0 and at O2 (the selftest's new tty checks among them).
  - the full `make test-usb`: 881 of 881 checks at O0 and at O2.
  - `make -C surfcpu test-os-qemu`: 137 of 137 (kernel/tty.c is one of the RISC-V port's shared files).
  - each commit of stage 2 builds on its own (the kernel at O0 and the host tests, from a clean tree of that commit).
- The review of stage 2 (1447a4c2): `test_win` 2,243 (K1-K5, the press rule, Help items, `close_waiting`),
  `test_guiclient` 556 (P1-P2, choosers named by program, the close button's two cases, the paste before FOCUS),
  `test_guidesk` 151 (P3-P4, titles' checks), `test_keyscan` 14 (new), the rest unchanged; `make unittest`; the
  selftest's TTY_RAW_KEYS owner check; screen: `gui_apps_desk.py` 47 checks, wdemo hang's under the new rule; the
  full `make test-usb` 890 of 890 at O0 and at O2; `make test-all` 558 and 51 at O0 and O2 (the coordinator's clean
  rerun: mine at 19:38 lost the owner's-stick boot's 90 s wait to a guest clock behind under load, and its rerun was
  cut by the Mac's sleep); `make -C surfcpu test-os-qemu` 137 of 137; each commit builds on its own.
- The second review, at the tip of this branch: `test_win` 2,294 (K6, `test_close_lost`; look-alike Help items and
  titles), `test_guiclient` 565 (the third press however slow, Cancel counting again, a refused CLOSE; a program's
  chooser with its pid; programs that end before a window, one message at a time, the earlier ones let go),
  `test_guidesk` 152 (a title with punctuation after), the rest unchanged, and the whole `make unittest`; the
  selftest's kept end; screen: `gui_apps_desk.py` 49 checks (a chooser's title read whole for its pid, nothing said
  after `wintest raw`, the message after `wintest nosuch`), wdemo hang's third press after 3.5 s; the full `make
  test-usb` 892 of 892 at O0 and at O2 and `make -C surfcpu test-os-qemu` 137 of 137, at 3c3b6535; `make test-all`
  558 and 51 at O0 and at O2, at 2799ff19, whose selftest line fits a screen line and is looked for where the
  selftest prints it (at 3c3b6535 the smoke test missed it twice: in the wrong order, then cut by the pager; the
  checks themselves passed). 2799ff19 changes only that line and its expectation. Each commit builds on its own.

Nothing of this has run on the laptop, and nothing will until the owner tries it. The keyboard's repeat in
particular is seen only there (QEMU's sendkey presses and lets go, with no typematic): `wintest desk` in the
desktop's Terminal shows "desk: key 0x61 mods 8" for an a held down, after its first "mods 0".
