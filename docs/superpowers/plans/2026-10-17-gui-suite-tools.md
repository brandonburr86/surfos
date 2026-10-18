# Track TOOLS: Log, Clock and Character Map (2026-10-17)

What track TOOLS of the [brief](2026-10-17-gui-suite.md) built, for the integrator: the text that
docs/GUI.md needs, and the decisions behind it. Branch `gui/tools`, from the foundation 7b44434.

## For the user guide (docs/GUI.md)

All three open from the Apps menu and from their tiles on the desktop. Their keys are also in Help.

**Log** shows the kernel log, as `dmesg` does, and reads it again twice a second while its window
shows (a window tucked into the taskbar is read again when it comes back).

* **Follow.** When the window opens, Follow is on: the newest lines stay in view as they come.
  Scrolling up, or selecting lines, stops it; End, Follow in the toolbar, or scrolling back down to
  the end starts it again. Follow is lit in the toolbar while it is on, and the status bar says
  "Follow on" or "Follow off: End".
* **Reading.** Up, Down, Page Up, Page Down, Home, End, the wheel, and the scroll bar (click above
  or below its thumb to page, or drag the thumb). Long lines wrap at the window's edge, their later
  rows two cells in, so every line can be read whole; a wider window (maximize, or the corner)
  wraps less. Characters are shown as on the console: UTF-8 as code page 437, a tab to the next
  multiple of 8 columns; other control characters and colour codes are not shown.
* **Find.** Ctrl+F (or Edit > Find…) opens a find bar under the log, as in Notepad. Enter finds
  the next match, letters in either case, starting after the last match (or from the selected
  lines, or the top of the view), and starts again from the top at the end ("Found it again from
  the top"). The match's line is selected and the word marked in yellow. Edit > Find next finds
  the next one with the bar closed. Esc closes the bar; the next Esc closes the window.
* **Copy.** Click a line to select it, or drag over several (the log scrolls when you pass its
  edge). Ctrl+C, Copy in the toolbar or Edit > Copy puts the lines on the desktop's clipboard,
  each with its line end, ready for Notepad or the Terminal. Ctrl+A (Select all) selects the whole
  log. The status bar counts the lines and the selected ones.
* **Limits.** The kernel log keeps its last 16 KB. When more has been written, the oldest lines
  are gone (the status bar says "the newest 16 KB"; the first line shown may be the end of a line
  whose start has gone), and a selection or a match on lines that went is dropped. Without the
  kernel (the host tests' desktop), the window says there is no kernel log.

**Clock** has four pages, chosen in its toolbar or with the keys 1 to 4.

* **Clock.** The time, large, from the PC's clock (the CMOS clock, as the taskbar's clock),
  the weekday and the date ("Saturday", "October 17, 2026") and the day of the year. The weekday
  is worked out from the date. The clock is whatever the PC's clock keeps (under QEMU, UTC).
* **Calendar.** A month, weeks from Sunday, today marked. `<` and `>` (or Left and Right, Page Up
  and Page Down) go a month back or on, Up and Down a year, and Today (or Home) back to this month.
  A click on a day names it in the status bar ("Thursday, October 1, 2026: day 274").
* **Stopwatch.** Start and Stop (Space), Lap (L) and Reset (R). It shows tenths of a second, and
  hours from the first hour on. The newest five laps are listed, each with its time since the lap
  before.
* **Timer.** Set 0:00 to 99:59 with the buttons (one minute, ten seconds) or the keys: Up and
  Down a minute, Page Up and Page Down ten seconds, Left and Right one, Home 0:00. Start, Pause
  and Resume with Space or the button; Reset (R) sets it back to the time set. While it runs the
  title counts down ("Clock - 04:59"), in the taskbar too. At zero the title says "Time's up!", in
  the taskbar too, and a message comes up in front of everything, even when the Clock is tucked
  away or behind another window; if a question is open (Notepad's unsaved changes, say), the
  message waits until it is answered. OK (or Enter, or Esc) sets the timer back to its time.
* **In the background.** The stopwatch and the timer keep running while the window is minimized
  or behind another window, and the status bar of the other pages shows them. Closing the window
  ends both; while one is running, closing it or leaving the desktop asks first.
* **Copy.** Ctrl+C (Edit > Copy) copies what the page shows: the date and time, the month as `cal`
  prints it, the stopwatch's time and laps, or the timer's time.

**Character Map** shows the console's 256 characters (code page 437, with SurfOS's ▸ ≋ …) in a
grid of 16 by 16, the code's last hexadecimal digit along the top and its first down the side.

* **Choosing.** Click a character, or move with the arrows. Home and End go to the start and the
  end of the row, Page Up and Page Down to the top and the bottom of the column. A key typed
  chooses its own character (A, say). Edit > Go to code… (Ctrl+L) takes a code (247 or 0xF7), a
  Unicode code point (U+224B), or a character.
* **What it shows.** The chosen character large, with its code in decimal and hexadecimal, its
  Unicode code point and its UTF-8 bytes.
* **Copy.** Enter, Ctrl+C, Edit > Copy, a double-click or the Copy button puts the character on the
  desktop's clipboard as UTF-8; the status bar says so. Ctrl+V pastes it in Notepad (one
  character, saved as its UTF-8 bytes) or the Terminal. The laptop's keyboard types nothing beyond
  ASCII, so this is how ≋, é, ½ or █ get into a note.
* **Codes 0-31 and 127.** On the console these are symbols (☺ ♥ ♪ ▸ ← … and ⌂), not control
  characters, and those symbols are what is copied (☺ is U+263A). 0 is the console's empty cell
  and copies nothing.

Rows for the Keys table:

| Keys | Action |
| --- | --- |
| Up, Down, Page Up, Page Down, Home, End | Scroll the Log (End: Follow on) |
| Ctrl+F, Ctrl+C, Ctrl+A | Find, copy the selected lines, select all (in the Log) |
| 1, 2, 3, 4 | The Clock's pages: clock, calendar, stopwatch, timer |
| Space, L, R | Start or stop, lap, reset (stopwatch); start or pause, reset (timer) |
| Arrows, Page Up, Page Down, Home | A month or a year, today (calendar); set the timer |
| Arrows, Home, End, Page Up, Page Down, a key | Choose a character (Character Map) |
| Enter, Ctrl+C, Ctrl+L | Copy the character, go to a code (Character Map) |

## Decisions

* **The Log wraps long lines** at the window's width rather than cutting them: a kernel line is
  read whole without scrolling sideways, and the later rows of a line start two cells in so that
  wrapped lines read as one. Selection is by whole lines (a log is read and copied by lines).
* **Places in the Log are counted from the first byte ever written** (as struct gui_sys's
  `written` is), not as offsets into the copy, so nothing has to be shifted when the log wraps
  around; a place that dropped off the front means the copy's start, and the selection and the
  match are dropped with their lines.
* **Follow** stops when the view moves up or lines are selected (so that they stay put while they
  are copied), and starts again when the view moves down to the end, besides End and the button:
  the view stays at the end exactly when the user has gone back to it.
* **The Log reads twice a second only while its window shows**, and at once when it opens or comes
  back from the taskbar, as the hint asked; a read that brings nothing new does not redraw.
* **klog_copy() was fixed** (its own commit): when a line written between two of its 1 KB chunks
  overwrote bytes it had yet to copy, it returned the oldest part of the copy while `written` said
  the copy ended at the log's end. The chunks now go from the end backwards, and a copy cut short
  is still the end.
* **The calendar's weeks start on Sunday**, as on US calendars and as the CMOS clock numbers its
  weekdays (1 is Sunday). The weekday shown anywhere is worked out from the date (Sakamoto's
  method), not taken from the CMOS clock, which firmware does not always keep.
* **The dates are written the US way** ("Saturday, October 17, 2026"), as the code's own dates
  (10/17/26) are; the time is 24-hour, as the taskbar's.
* **The stopwatch shows tenths**: hundredths would redraw the whole desktop every frame while it
  runs; tenths redraw it ten times a second, and only while its page shows.
* **Closing the Clock ends the stopwatch and the timer**, because gui_tick() runs only for open
  windows, so a closed window's timer could never ring; minimizing keeps them running. While one
  runs the window is busy: closing it, or leaving the desktop, asks first.
* **The timer's alarm** is a message ("Timer", "Time's up!", OK) and the window's title, which the
  taskbar shows; the window manager's taskbar has no other way to mark a button. The message never
  takes the place of a dialog that is open (it would lose that dialog's question, such as
  Notepad's unsaved changes): it waits for it to be answered. The title counts down while the
  timer runs, so the taskbar shows the time left too.
* **The Clock takes Ctrl+C for Copy** (what the page shows) rather than leaving the desktop, as
  the Log and the Character Map do; the keys 1 to 4 choose the pages (the arrows belong to the
  calendar and the timer).
* **The Character Map copies the symbol for codes 1-31 and 127** (cp437_utf8() gives the graphic
  character the console draws there, U+263A for 1), which Notepad shows as the same symbol and
  saves as UTF-8; a control character would save as a raw control byte. 0, the empty cell, copies
  nothing, although cp437_utf8() gives a space for it. ≋ is in the set, at 0xF7.
* **No shared file changed.** The host test gives the Log a kernel log of its own by copying
  fake_sys and replacing its `log`, rather than adding a fake to gui_fake.c; it includes
  kernel/klog.c itself, with stub headers, to test klog_copy(). The Makefile's GUI_TOOLS_OBJ stays
  empty: the three applications are in their stubs' files.

## Files

| File | What |
| --- | --- |
| shell/guilog.c | Log |
| shell/guiclock.c | Clock, and the calendar's arithmetic |
| shell/guichar.c | Character Map |
| include/sys/gui_tools.h | their state in gui_state |
| shell/gui_tools.h (new) | the track's private header: the calendar's arithmetic, the Log's view, the Character Map's UTF-8 |
| kernel/klog.c, include/surfos/klog.h | klog_copy() always hands back the log's end (the header: its comment) |
| tests/host/test_guitools.c | the host tests |
| tools/gui_apps_tools.py | the guiapps scenario's checks |

gui_state grows by 580 bytes (838,600 to 839,180 at -m32): the Log's text was already in the
foundation's state.

## Limits and open points

* The Log keeps only what the kernel log keeps (16 KB).
* The Clock shows the CMOS clock as it is: there is no time zone (QEMU's clock is UTC; the laptop's
  is whatever its firmware keeps, usually local time under Windows).
* tests/host/test_guitools.c includes kernel/klog.c; the shared rule's dependency file only covers
  gui_fake.c, so after a change to klog.c alone, rebuild the test (`make -B`, or a clean build).
* Copying into the Terminal relies on the Terminal's paste handing the clipboard's bytes to the
  session (track TERM); the console decodes the UTF-8.
