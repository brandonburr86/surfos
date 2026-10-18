# The Terminal (track TERM of the desktop suite, 2026-10-17)

What track TERM of [the suite's brief](2026-10-17-gui-suite.md) built, for the integrator to
put into [docs/GUI.md](../../GUI.md): the user guide's part first, then the decisions behind it.

## For the user guide

**Terminal** runs surfsh, the SurfOS shell, in a window, on a pseudo-terminal of its own: every
command works as on F1-F4 (the prompt shows `≋@>`, as surfsh shows on any pseudo-terminal, a
telnet session's too). Open it with Ctrl+T, its tile, or Apps > Terminal. Each Terminal window
has one session, started when it opens; a desktop has one Terminal window.

* **Keys.** Every key is the shell's, but Ctrl+G and Ctrl+Space, which open the menus. So Ctrl+C
  stops the program that runs (`sleep 30000`, say), and Ctrl+Q, Ctrl+W, Ctrl+T, Tab, Esc and the
  arrows are the program's too (`edit` quits with Ctrl+Q). The arrows, Home, End, Page Up and Down,
  Delete and Insert reach the program as the escape sequences a terminal sends; Enter, Backspace,
  Tab and Esc as the bytes they are. To reach the desktop with the Terminal in front: Ctrl+G and
  the menus (Return to shell is in the ≋ SurfOS menu), or the mouse.
* **The size.** The window's size is the terminal's, and the status bar shows it. On a 1920x1080
  screen it opens at 77 by 21 characters and is 117 by 26 maximized; on a 1280x800 screen (a
  desktop of 640 by 400 pixels) it opens at 71 by 16 and is 77 by 18 maximized. A new size reaches the shell when the window is maximized or restored, or when its
  corner is let go; a program sees it when it asks (`pagetest size`; `edit` with Ctrl+L). Lines are
  not rewrapped: they are cut or padded.
* **Scrolling back.** About 1,000 lines are kept above the screen. Use the scroll bar (click above
  or below the thumb for a page, or drag it), or the wheel of a mouse that reports one (SurfOS's
  mouse drivers report none yet). While the view is scrolled back, new output does not move it:
  the status bar says how many lines came below. Any key goes back to the end (and to the program).
* **Copy and paste.** Drag to select (past the top or bottom edge the text scrolls), or
  double-click a word (also one that a line's end broke in two). Edit > Copy copies the selection,
  or the whole screen as it shows when nothing is selected. A line that wrapped is copied as one,
  also after the window got wider or when it is in the scrollback; one that a narrower window cut
  on the screen is copied as it shows. Of a line in the scrollback from when the window was wider,
  a copy of the screen takes what shows, and a selection all of it. A selection of nothing but
  blanks leaves the clipboard as it was. Edit > Select all selects everything kept, the scrollback
  too. Edit > Paste
  types the clipboard into the session, each line end as Enter; what the shell does not take at
  once follows a moment later. As on every SurfOS terminal, a pasted block of several lines runs
  its first line only (the rest of a paste is skipped until the input pauses); a paste into `edit`
  arrives whole. The clipboard is the desktop's, so text goes both ways with Notepad and the
  other windows. Ctrl+C is the program's, so copying is the Edit menu's (Ctrl+G, Right, Right).
* **The end of a session.** When the shell exits (`exit`, or Ctrl+D at an empty prompt), the
  session ends at once, and a line in the window and the status bar say so. A program the shell
  left running in the background (`pagetest hold 300000 &`, say) ends with it, as when the window
  closes. Enter, or the New session button in the status bar, starts a new session below; Esc or
  Ctrl+W then closes the window.
* **Closing.** Closing the window while a session runs, or leaving the desktop, asks first:
  Close (or Leave) ends the session and every program it runs; Cancel keeps it. The session goes
  on while the window is tucked into the taskbar and while another console has the screen.

Limits: no keyboard path to the scrollback (there is no Shift+Page Up to take for it: the keyboard
driver sends the special keys without their modifiers); Ctrl+G and Ctrl+Space (NUL) cannot be typed
into a program (edit's "go to line" needs another key there); no mouse reporting to programs, no bracketed paste, no
bold font (bright colours instead, as on the console); a font of code page 437, so a character
it lacks shows as `?`, as on the console.

A line for the Keys table: `Ctrl+T | The Terminal; in it every key but Ctrl+G and Ctrl+Space is the shell's`.

## Decisions

* **The emulator** (shell/guivt.c) is the terminal at the far end of a pseudo-terminal's stream
  console (kernel/console.c), so it follows that console's model of it exactly: LF is a new row
  at its first column (the pty passes LF as it is), and after a full row it is one row, not two
  (the screen console leaves an empty one, the stream console counts one); Backspace only moves
  (the stream console erases with BS SP BS, and at a pending wrap with CR ESC[999C ESC[K); Tab
  moves to the next multiple of 8 but never past the last column nor at a pending wrap. The
  parser has the console's states and limits (8 numbers, a number up to 1,000, strings to BEL or
  ESC or 256 bytes). Beyond the console's list it also does ESC[nG (the stream console counts it)
  and ESC[27m (the pager's question in a stream ends with it: without it everything after the
  question would stay inverse), plus 22, 39, 49 and 90-97/100-107, and passes over 38/48's own
  numbers (the console takes `38;5;31` as red; this does not). Erases use the colours in force,
  inverse included, as the console's ansi_blank() does. ESC[5n gets no answer, as on the console.
* **Colours**: the console's VGA attributes, drawn in the desktop's palette: black is NAVY (the
  window's background), light grey is SHELL (the text), light cyan FOAM, light red CORAL, light
  blue SKY, dark grey MUTED, and the others chosen to read on navy. Inverse is navy on light grey.
  A block cursor when the window has the keys, an outline otherwise; ESC[?25l hides it.
* **State**: the screen and its scrollback are 1,024 lines of 160 cells (2 bytes each: the
  character and the attribute) in a ring in gui_state, 320 KB; the widest window (1280 logical
  pixels, maximized) has 157 columns, the tallest 43 rows. With the clipboard's worth of input
  waiting (128.5 KB) the Terminal adds 460,448 bytes to gui_state (838,600 to 1,299,048 at -m32);
  10/18/26: with a frame's carry of read output (1 KB) and the count of cells written, 461,484
  (1,300,084); 10/18/26 (the second review): with the time a new size began to wait, 461,488
  (1,300,088).
* **Scrolled back**: output keeps the view where it is (it is anchored to a line's number, so the
  text under it stays still), and says so; a key brings it back. A view whose lines leave the
  ring shows from the oldest line kept.
* **A new size**: no reflow. A screen that loses rows loses those below the cursor first, then
  its top rows go into the scrollback; one that gains rows takes lines back from the scrollback
  before it adds blank ones, so the cursor's line stays on the screen and the text at the bottom.
  Columns beyond the width are blanked on the screen's rows (the scrollback keeps its text). The
  size goes to the session only when it changed, never while the corner is held (once at the
  release), and only after everything the session printed at the old size has been drawn, or
  (10/18/26, the second review) 250 ms after it came, whichever is first: output that never pauses
  (a program in the background, which the pager leaves alone, or any chatty one) held it up for
  as long as it lasted. Then the frame reads nothing more, draws what is left of what it read
  (all of it but for a flood of erases), and the size goes; what still waits in the pty is drawn
  at the new size.
* **Wraps** (10/18/26, the review): a line's wrap mark is the width it wrapped at. Copied text
  joins the line to the next there, whatever the width is now, so a long command copied after
  maximizing is still one line; a screen row that a narrower width cut loses its mark (its rest is
  gone), and so does the last line left when the rows below the cursor go. A middle line of a
  selection is copied whole, also the part of a line kept from a wider screen that does not show.
  A double-click's word follows a wrap into the next line and back. 10/18/26 (the second review):
  a character written at or past a row's mark (a program that moves beyond the old wrap of a row
  the window has since widened, and writes there) ends the mark: the row goes on in itself now,
  and is copied as it shows.
* **A frame's work** (10/18/26, the review): each frame draws at most READ_MAX bytes (8 KB, a
  pty's whole buffer) and WORK_MAX cells written (32,768, counted by the emulator: a character
  one, an erase or a new line each cell it blanks), and one byte's own work beyond (a screenful
  at most). What was read and not drawn waits for the next frame, and so does a new size, 250 ms
  at most (A new size, above). A flood of erases (ESC[2J at 160x64 writes 10,240 cells for 4
  bytes) costs a frame no more than a screenful of text.
* **Input**: one queue (pend) for typed keys and pastes, so their order holds; it holds the whole
  clipboard (GUI_CLIP_MAX) and a little more. A Ctrl+C drops what waits and goes first, as
  telnetd does; if the input queue cannot take even that (raw mode, full), it waits first in line.
  A paste sends CR for each line end (CR LF as one), as terminals do. A key goes into the queue
  whole or not at all (never half an escape sequence); a paste as far as there is room, not
  cutting a UTF-8 character: 10/18/26 (the second review), it goes into the queue in chunks that
  end between characters, and once a byte did not fit nothing after it goes, so the session gets
  the clipboard's start and never one with a hole in it.
* **Copy with nothing selected** copies the screen as shown. Blanks at a line's end are left out,
  and a line that wrapped is joined to the next again. 10/18/26 (the second review): as it shows
  also scrolled back over lines kept from a wider window, each cut at the window's width, and one
  of them that wrapped wider than that not joined (vt_shown()). Select all takes every line kept. A
  selection with only blanks copies nothing, and the clipboard keeps what it had; one whose lines
  have left the scrollback says so.
* **With no session** (it ended, or none could start) the desktop's Esc, Ctrl+W and Ctrl+Q work
  in the window again; Enter starts a session.
* **The kernel's side** (shell/guisys_term.c): a session is a pty master, an open file of its own
  (kernel/pty.c's new pty_open() and friends). The shell is `surfsh -l`, as init and telnetd
  start it, a child of the task running `gui`, in its directory. It is collected with
  task_wait_opts(WNOHANG) on the reads once it has ended. 10/18/26: once it has, the reads give
  what is left and then -EPIPE, though a program the shell left in the background still holds the
  pty: the session ends with its shell, and the Terminal's close then hangs that program up too
  (before, the window waited for every program on the pty, `sleep 100 &` for 100 seconds). At a close (the window, or
  gui_finish() at the desktop's end) the master's close hangs the pty up, the shell is collected
  if it is gone, and otherwise, being inside a call that ends at the hangup's kill, it is handed
  to init (task_detach()), which collects it: the desktop never waits, and leaves no zombie of its
  own and no orphan that nobody collects (what the shell leaves is init's when it dies). Collecting
  it on later frames instead cannot work everywhere: a closed window gets no tick, and gui_finish()
  has no later frame at all; init is what collects telnetd's shells too once telnetd ends. Each call
  checks the session is the calling task's, so two desktops on two consoles keep theirs apart.
  The kernel log gets "gui: Terminal: surfsh (pid N) on ptyM" and "... the session on ptyM ended";
  nothing typed or shown is logged.
* **A desktop that dies** without gui_finish() (10/18/26, the review: a fault in its task, or a
  kill that waited too long): the master is also a descriptor of the desktop's task (fd_install();
  the shell has only 0, 1 and 2, so it never holds one), so the task's end closes it when the task
  is collected (fd_close_all()): the hangup, the pty free again, its programs ended, the shell
  init's to collect. The next open takes back the table's places of desktops whose task is gone
  (or dead, not collected yet); nothing is closed twice. 10/18/26 (the second review): the master
  is a descriptor from the moment it is open, before the shell starts, and always: with no free
  descriptor (a task has 16) the session does not start, -EMFILE, as sys_openpty() answers a
  program ("No session: too many files are open."); so no master is held in the table alone. A close
  takes the master out of its place (interrupts off) before it closes the descriptor. A task
  killed at once in an open or a close from the moment the master is one of its descriptors thus
  leaves the master to its own end and never in a place to be closed again, and the next open
  takes back any place of a task that is gone, also one it was starting or closing; the table
  closes nothing itself. (Before that moment, inside pty_open, between a failed fd_install and
  pty_close, and inside fd_close itself, a task killed at once leaves a master nobody holds: the
  same few instructions as sys_openpty's and close's own, which this file cannot close.)
* **kernel/pty.c**: the system calls share the new code (the spawn, the size, the read's copy and
  the master's lookup) and keep their behaviour; the kernel's calls are declared in
  include/surfos/pty.h behind `#ifdef _SURFOS_TYPES_H`, so user programs, which include it
  without the kernel's types, still see only the numbers.

## Checks

* Host: tests/host/test_guiterm.c (the emulator alone: text, the wrap, scrolling, the ring,
  Backspace and Tab as the stream console sends them, moves, erases, colours, UTF-8, unknown
  sequences and strings, the pager's question, the ansi command's screen at three sizes; sizes;
  copied text and words; then the Terminal against the fake session: starting, keys to bytes, the
  size once per change and at the corner's release, the scrollback with the wheel, the scroll bar
  and output while scrolled back, selection and copy, paste with a full input queue and Ctrl+C,
  the end and a new session, a session that cannot start, busy and the close and leave
  questions, no system, the pixels drawn, and every window inside guard pixels at 640x400,
  960x540 and 1280x800, maximized and hanging off the edge). 10/18/26: the review's cases (wraps
  across a width change, the shrink's wrap mark, keys whole, blank copies, a word across a wrap,
  a frame's budget with the size waiting, a lost session, a selection gone from the ring, the
  sizes at 640x400 that the user guide gives); 10/18/26, the second review's (a new size during
  three floods that never pause: text, new lines and erases, at the frame 250 ms on; a write past
  an old wrap; a paste's character across its chunks, and no hole after a byte that did not fit;
  a copy of the screen scrolled back over lines from a wider window), the
  emulator and the Terminal against random input (150,000 steps, and 3 desktops of 4,000 events:
  their invariants), and the kernel's side, shell/guisys_term.c, against stand-ins for the tasks,
  descriptors and pseudo-terminals (spawn and its failure, PTY_MAX, the shell's end, a program it
  left, closing an ended session, the detach, another task's handle, a desktop that died with
  eight sessions; 10/18/26: places left by a task killed inside an open and inside a close, no
  free descriptor, each master a descriptor before its shell starts and out of its place before
  it is closed). kernel/pty.c's calls themselves are run by the
  smoke test's ptytest (the shared code) and by the guiapps checks (the kernel's calls).
* On the screen: tools/gui_apps_term.py, the guiapps scenario's first checks (see its header).
