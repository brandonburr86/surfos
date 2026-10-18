# The Terminal (track TERM of the desktop suite, 2026-10-17)

What track TERM of [the suite's brief](2026-10-17-gui-suite.md) built, for the integrator to
put into [docs/GUI.md](../../GUI.md): the user guide's part first, then the decisions behind it.

## For the user guide

**Terminal** runs surfsh, the SurfOS shell, in a window, on a pseudo-terminal of its own: every
command works as on F1-F4 (the prompt shows `≋@>`, as surfsh shows on any pseudo-terminal, a
telnet session's too). Open it with Ctrl+T, its tile, or Apps > Terminal. Each Terminal window
has one session, started when it opens; a desktop has one Terminal window.

* **Keys.** Every key is the shell's, but Ctrl+G, which opens the menus. So Ctrl+C stops the
  program that runs (`sleep 30000`, say), and Ctrl+Q, Ctrl+W, Ctrl+T, Tab, Esc and the arrows
  are the program's too (`edit` quits with Ctrl+Q). The arrows, Home, End, Page Up and Down,
  Delete and Insert reach the program as the escape sequences a terminal sends; Enter, Backspace,
  Tab and Esc as the bytes they are. To reach the desktop with the Terminal in front: Ctrl+G and
  the menus (Return to shell is in the ≋ SurfOS menu), or the mouse.
* **The size.** The window's size is the terminal's: 77 by 21 characters as it opens, 117 by 26
  maximized on a 1920x1080 screen. The status bar shows it. A new size reaches the shell when the
  window is maximized or restored, or when its corner is let go; a program sees it when it asks
  (`pagetest size`; `edit` with Ctrl+L). Lines are not rewrapped: they are cut or padded.
* **Scrolling back.** About 1,000 lines are kept above the screen. Use the scroll bar (click above
  or below the thumb for a page, or drag it) or the wheel. While the view is scrolled back, new
  output does not move it: the status bar says how many lines came below. Any key goes back to the
  end (and to the program).
* **Copy and paste.** Drag to select (past the top or bottom edge the text scrolls), or
  double-click a word. Edit > Copy copies the selection, or the whole screen as it shows when
  nothing is selected; Edit > Select all selects everything kept, the scrollback too. Edit > Paste
  types the clipboard into the session, each line end as Enter; what the shell does not take at
  once follows a moment later. As on every SurfOS terminal, a pasted block of several lines runs
  its first line only (the rest of a paste is skipped until the input pauses); a paste into `edit`
  arrives whole. The clipboard is the desktop's, so text goes both ways with Notepad and the
  other windows. Ctrl+C is the program's, so copying is the Edit menu's (Ctrl+G, Right, Right).
* **The end of a session.** When the shell exits (`exit`, or Ctrl+D at an empty prompt), a line in
  the window and the status bar say so. Enter, or the New session button in the status bar, starts
  a new session below; Esc or Ctrl+W then closes the window.
* **Closing.** Closing the window while a session runs, or leaving the desktop, asks first:
  Close (or Leave) ends the session and every program it runs; Cancel keeps it. The session goes
  on while the window is tucked into the taskbar and while another console has the screen.

Limits: no keyboard path to the scrollback (Shift+Page Up does not exist on this keyboard path:
the console's keyboard sends no modified keys); Ctrl+G cannot be typed into a program (edit's
"go to line" needs another key there); no mouse reporting to programs, no bracketed paste, no
bold font (bright colours instead, as on the console); a font of code page 437, so a character
it lacks shows as `?`, as on the console.

A line for the Keys table: `Ctrl+T | The Terminal; in it every key but Ctrl+G is the shell's`.

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
  window's background), light grey is SHELL (the text), the light ones FOAM, CORAL, SKY, MUTED,
  and others chosen to read on navy. Inverse is navy on light grey. A block cursor when the window
  has the keys, an outline otherwise; ESC[?25l hides it.
* **State**: the screen and its scrollback are 1,024 lines of 160 cells (2 bytes each: the
  character and the attribute) in a ring in gui_state, 320 KB; the widest window (1280 logical
  pixels, maximized) has 157 columns, the tallest 43 rows. With the clipboard's worth of input
  waiting (128.5 KB) the Terminal adds 460,448 bytes to gui_state (838,600 to 1,299,048 at -m32).
* **Scrolled back**: output keeps the view where it is (it is anchored to a line's number, so the
  text under it stays still), and says so; a key brings it back. A view whose lines leave the
  ring shows from the oldest line kept.
* **A new size**: no reflow. A screen that loses rows loses those below the cursor first, then
  its top rows go into the scrollback; one that gains rows takes lines back from the scrollback
  before it adds blank ones, so the cursor's line stays on the screen and the text at the bottom.
  Columns beyond the width are blanked on the screen's rows (the scrollback keeps its text). The
  size goes to the session only when it changed, never while the corner is held (once at the
  release), and only after everything the session printed at the old size has been read.
* **Input**: one queue (pend) for typed keys and pastes, so their order holds; it holds the whole
  clipboard (GUI_CLIP_MAX) and a little more. A Ctrl+C drops what waits and goes first, as
  telnetd does; if the input queue cannot take even that (raw mode, full), it waits first in line.
  A paste sends CR for each line end (CR LF as one), as terminals do.
* **Copy with nothing selected** copies the screen as shown. Blanks at a line's end are left out,
  and a line that wrapped is joined to the next again. Select all takes every line kept.
* **With no session** (it ended, or none could start) the desktop's Esc, Ctrl+W and Ctrl+Q work
  in the window again; Enter starts a session.
* **The kernel's side** (shell/guisys_term.c): a session is a pty master held as an open file of
  its own (kernel/pty.c's new pty_open() and friends; no descriptor in the desktop's task). The
  shell is `surfsh -l`, as init and telnetd start it, a child of the task running `gui`, in its
  directory. It is collected with task_wait_opts(WNOHANG) on the reads once it has ended (also
  while a program it left in the background still holds the pty). At a close (the window, or
  gui_finish() at the desktop's end) the master's close hangs the pty up, the shell is collected
  if it is gone, and otherwise, being inside a call that ends at the hangup's kill, it is handed
  to init (task_detach()), which collects it: the desktop never waits, and leaves no zombie of its
  own and no orphan that nobody collects (what the shell leaves is init's when it dies). Each call
  checks the session is the calling task's, so two desktops on two consoles keep theirs apart.
  The kernel log gets "gui: Terminal: surfsh (pid N) on ptyM" and "... the session on ptyM ended";
  nothing typed or shown is logged.
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
  960x540 and 1280x800, maximized and hanging off the edge).
* On the screen: tools/gui_apps_term.py, the guiapps scenario's first checks (see its header).
