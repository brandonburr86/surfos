# The desktop applications, track code: the Code Editor (`wcode`)

The account of track code of `docs/superpowers/plans/2026-11-05-desktop-apps.md`: the Code Editor, `wcode`, in
`user/wcode/`, branch `apps/code`, worktree `06-restoration-apps-code`, volume `surfos-apps-code`.

## Where it stands

- **Phase 1, the model: done** (11/05/26): everything that is not drawing, in new files, host-tested.
- **Phase 2, the window: done.** kit's toolkit core and desk's amended ABI merged (`git merge --no-ff 2ba18949`,
  9e0a471f); the window (`window.c`) and the program (`main.c`) on the toolkit, in new files, tested on its fake
  desktop. kit's stage 2 with its `gk_next()` fix merged later (eab009d3, see Merges).
- **Phase 3: done.** desk's implementation merged (2c5afd30, merge 6e952fad); the tracked files changed only by
  insertion (the Makefile, the `TRACKS` line, the section of `docs/GUI.md`), the new `rootfs/apps/wcode.app` and
  `rootfs/man/wcode.txt`, the one allowed fix in `tests/host/sim_fs.c`; the screen checks on the real desktop
  (`tools/gui_apps_code.py`); the gates (under Tests).
- Built to desk's fix round as the coordinator described it (11/05/26), not yet merged: no ≋ or window title
  assumed in a chooser's bar, `-EBUSY` from a chooser said as the desktop being busy, the Help item named after the
  program, the close button pressed again while the program asks (its question stands), no `ttyctl`.

- **The review's fix round: done** (11/09/26): every finding fixed and tested (under "The review"), kit's aac42b11
  merged (its chooser check), the gates passed.

Nothing of it has run on the laptop; it has run under QEMU only.

## What is built

Thirteen files. Only `fileio.c` and `main.c` make system calls; everything else is host-tested as it is.

| File | What |
|---|---|
| `text.c` | the bytes in a gap buffer, as they were read; a sparse index of line starts ("marks") that also keeps the colouring's states; UTF-8, characters, columns |
| `undo.c` | the history: steps of ops, runs merged, numbered texts for "unmodified again", bounded |
| `doc.c` | one file: selection, moving, editing, line ends, indentation, comments, brackets, copy and paste |
| `lex.c` | the tokenizers: C, NASM, GNU as, Python, shell, Makefile, Markdown, JSON |
| `syntax.c` | which language a file is; the states along the text; matching brackets |
| `find.c` | find and replace, plain or regular expression (libsurf's), either case, whole words |
| `grep.c` | find in files: a bounded, stepped, iterative walk through callbacks for the file system |
| `view.c` | the text area's cells: characters, classes, the selection, the brackets; clicks; the status line |
| `app.c` | the tabs, the commands keys and menus share, the unsaved-changes guard, the keys, the pointer, the wheel |
| `fileio.c` | reading a file in; saving through `saveutil.h`, new files `O_EXCL`, edit's care; the rescue copy |
| `window.c` | the window on kit's toolkit: tabs, the text area and its gutter, scroll bars, the find bar, find in files, the status bar, the menus, the choosers, the boxes |
| `main.c` | the arguments, the loop, find in files' calls to the file system, the rescue when the desktop goes |

`rootfs/apps/wcode.app` puts it in Apps > Programs with a tile (its icon a page of coloured lines, drawn for this
entry), and gives it the types Files opens in it (`c h asm nasm inc s py sh bash mk mak md markdown json =Makefile
=GNUmakefile`) and those Open with offers it for (`txt log conf cfg ini`, which stay Notepad's).

## Decisions a reviewer would ask about

**The buffer.** A gap buffer, not a piece table: a file is read straight into it (no second copy of 16 MB), typing
moves only what lies between the last edit and this one, and every search and tokenizer sees plain bytes (a line that
straddles the gap is made whole by moving the gap, at most that line's length). The line index keeps no per-line
array: marks at line starts, one in 128 lines or 16 KB whichever comes first, made by the scans that pass through text
without them and shifted by every change (`O(marks after the change)`). A lookup is a binary search of the marks and a
scan from one. So a 16 MB file of empty lines costs no more index than one of long lines, and nothing limits the number
of lines. `WC_MARK_EVERY` and `WC_MARK_BYTES` can be set small for a test build, which then crosses marks everywhere.

**Line ends kept as found.** The bytes are kept as read. A line ends at LF; a CR right before it belongs to the line
end, so no position ever falls between CR and LF, and the arrows, Backspace and Delete take CR LF as one character.
A lone CR is a character (drawn as the console's ♪, as Notepad does). New line ends (Enter, a paste, a replacement's
`\n`) are the file's kind: CR LF when most of its line ends are, LF otherwise; a file with both keeps both. The
clipboard holds LF line ends, so text goes to Notepad and the Terminal as they expect, and a paste gets the file's.

**Limits, and memory.** A file of at most 16 MB; all open files' buffers (gaps included) at most 32 MB; 8 tabs; a
history of at most 3 MB and 4,096 steps a file. The worst case is about 32 + 8 × 3 MB plus the index and the window,
under 64 MB. The size limit holds whatever room the gap has (the speed test found it did not at first, see below).

**Not text.** A file with a NUL byte is refused, as Notepad refuses it; Files gives it to Hex.

**The history.** A run of typing, of Backspace or of Delete is one step (edit's and Notepad's rule); Enter with its
indentation is a step; every other change (a paste, a cut, Replace, Replace all, an indentation of lines, comments) is
a step. Moving the caret ends a run. Each step keeps the selection before and after. Texts are numbered as edit numbers
them, so undoing back to the saved text shows it unmodified, and a save ends the open run so that typing on is a new
step. A change larger than the whole history (Replace all on every line of a large file, say) forgets the history
rather than keep part of it, as Notepad does, and the status bar says so.

**Find and replace.** A line at a time: a match never takes a line end, and a regular expression is never given more
than one line. libsurf's `regexec()` is bounded in time by the pattern, and `REG_STARTEND` lets a search start inside
a line while `^` still sees what is before it. A match is never empty: a pattern that matches an empty text is
refused (11/09/26, the review; one that matches nothing only at a place, as `\<`, steps over it). The
replacement is literal, or with a regular expression POSIX sed's: `&` (and `\0`) the match, `\1`-`\9` its groups,
plus `\n` (the file's line end) and `\t`. Replace all works out the file's size first and does all or none of it,
never half at the 16 MB limit; it is one step, it builds each changed line anew (one op pair a line, not a match), and
it drops the index's marks past the first change first, since they would move at every line.

**Find in files.** Iterative, through a stack of folder paths with their depths; a folder is read entry by entry and
each file read and searched in 64 KB chunks before the next entry, so at most one folder and one file are open (two of
the program's 16 descriptors). Bounded everywhere: 24 folders deep, 4,096 folders, 20,000 files, 256 MB read, 2,000
lines listed, a line searched in its first 64 KB and with a bound on its work, 256 KB of paths waiting; each limit
stops the search or leaves
something out and says which. Files of more than 16 MB, and files with a NUL in their first chunk, are skipped and
counted. `/dev` and `/.netman` are never entered. It reads files as they are on the disk, not unsaved changes. It works
256 KB at a time between the window's events (`gk_next()` with no wait while there is more), so the window answers
and Stop ends it. It starts with what the find bar found last, in the folder of the file shown.

**The colouring.** Each tokenizer reads one line from the state it starts in and gives every byte a class; states
live in the marks. After a change, the first look tokenizes from the mark before it to the first good mark after it and
compares: when the state there is the same (typing inside a line nearly always), nothing after it is stale; otherwise
the marks after it are, and they are worked out again when a screenful needs them, within a budget (512 KB a
screenful), drawing with the stale state meanwhile, or a little at a time while idle. A tokenizer reads a line's
first 8 KB only (11/09/26, the review), as if it ended there; the rest is drawn plain. A language is chosen by the file's name
(`.c .h`, `.asm .nasm .inc` NASM, `.s .S` GNU as, `.py`, `.sh .bash`, `Makefile makefile GNUmakefile .mk .mak`,
`.md .markdown`, `.json`, `surfshrc`, `.profile`) or its `#!` line, and View > Language changes it. The colours are
dark ones on the toolkit's paper (keywords plum, types blue, strings sea green, numbers rust, the preprocessor and
labels sand, comments the theme's muted), chosen for this editor.

**Brackets.** The bracket at the caret (or before it) and its partner, brackets in comments and strings left out (the
tokenizers say which bytes those are), forwards line by line, backwards in blocks of 256 lines tokenized forwards for
their states. Bounded: 2,000 lines for the highlight drawn at every move, 100,000 for Go to bracket.

**Indentation.** A file's own: tabs if most of its first 10,000 indented lines start with one, otherwise spaces in the
commonest step (2, 4 or 8, by the differences between lines); Makefiles always tabs; otherwise 4 spaces. Enter keeps the
indentation and adds a step after an opening bracket, Python's `:`, a shell's `then`, `do`, `else` (as words the
tokenizer calls keywords, so `echo then` does not), and a Makefile's rule (a tab); between brackets the closing one
goes on a line of its own. A line left with nothing but indentation loses it. A closing bracket typed alone on a line
takes the indentation of its opening one's line. Backspace in an indentation of spaces goes back a step. Tab with a
selection of several lines indents them, Shift+Tab takes them back; empty lines stay empty.

**Saving.** `fileio.c` keeps the rules of the command-line suite and of edit: a file the user has is replaced only
through `user/bin/saveutil.h` (never truncated); a file Save as makes is `O_EXCL` and goes again when the save fails;
after a rename that may have half happened, or with a name beside the file of its size and time (`su_twin_beside()`),
nothing is renamed or written over the file again for as long as it is open: each save puts a copy beside it (`O_EXCL`,
`.tmp` to `.9.tmp`, none when nothing changed since the last), the status bar says to have the disk checked, and closing
needs no question when the text is in the last copy. A rename that changed nothing but left its new file behind (it
would not go) is said, without that care. A short write is a full disk. A name given that is not there yet (`wcode
new.c`) is a new file, made by its first save.

**The guard.** Closing a tab (Ctrl+W, a tab's x), Quit (Ctrl+Q) and the close button ask about each file with unsaved
changes in turn (Save, Don't save, Cancel), in a box of the toolkit's over the window; a file never saved asks for its
name first, through the Save as chooser; a failed save ends the question and keeps everything open; nothing a Don't
save drops is closed before the last answer, so a Cancel loses nothing. While the guard asks, the close button pressed
again and the items of the desktop's bar wait (desk's fix round makes a second press send the close again, and a
second question over the first would have cancelled it). The one way out that cannot ask is the desktop's end, which
takes the window before anything can be shown: then `main.c` puts each such file's text beside it
(`wc_file_rescue()`, the copy-beside rule, `O_EXCL`) and says where on stderr; a file never saved has nowhere to go,
and stderr says that too. The status is 1 when anything was left unsaved.

**Keys.** As desk's ABI gives them (`include/surfos/win.h`): a character is its byte; a Ctrl+letter its control byte
with the Ctrl bit; Ctrl+[ 27, Ctrl+] 29, Ctrl+/ 31; Tab, Enter, Esc and Backspace with Ctrl are keys of their own
(`WIN_KEY_TAB` ...), so Ctrl+H (Replace) is no Ctrl+Backspace (a word back) and Ctrl+Tab no Ctrl+I. The program tests
the bits it uses (Shift, Alt, Ctrl), never the modifiers as a whole: a held key's repeats (`WIN_MOD_REPEAT`) type or
move as the key does, and a bit nobody uses yet changes nothing (`test_wcode` checks both). Alt with a key types
nothing. Find next is F5, and Shift+F5 the one before, since F1 to F4 never reach a window (they switch consoles). A
UTF-8 character typed a byte at a time (the serial line) is collected into one.

**The window.** 800 × 440 as it opens (the brief's limit is 958 × 457), at least 400 × 240, resizable; the desktop may
give it less, and the rows and columns follow. The toolkit lays out a column: the tab strip (`gk_tabs`, a strip with
no pages: the text area under it is the same for every tab), the text (`gk_area`, `GK_FOCUSABLE | GK_TAKES_TAB`, so
Tab indents), the find bar and the replacement's line, find in files (its fields, a list, the line that says what it
found), the status bar. The text area is drawn from cells the model renders (`view.c`): the gutter of line numbers
(right-aligned, the caret's in ink), the backgrounds a run at a time, the glyphs in their class's colour, the caret a
2-pixel bar. After each event the new cells are compared with those drawn, row by row, and only the rows that changed
are damaged (`gk_damage()`), so typing presents a row and the status bar, never the window (`test_wcode_win` checks
that no present taller than two rows covers the text); a scroll, another tab, a new size or a theme redraws it all. The
scroll bars are the toolkit's, in cells: the extent is the widest line on the screen and a cell for the caret after it
(not the widest in the file, which would need every line measured), and the rows of the whole file. The title and the
tab say the file's name with ` *` while it has unsaved changes.

**What the toolkit does, and the one thing it lacked.** Everything came from kit's toolkit, used as kit's `wkit`
uses it: the tabs with their x, the area with its scroll bars, fields, check boxes, buttons, the list, the status bar,
menus with shortcuts, the boxes (`gk_confirm()` for the guard, `gk_ask()` for Go to line, `gk_message()` for About),
`gk_choose()` (only in answer to a person's key, click or menu choice: Open, Save as, the folder of find in files),
`gk_copy()` and `gk_paste_all()`, the theme. A menu shortcut can name a letter but not Ctrl+[, Ctrl+] or Ctrl+/ (the
toolkit turns only the control bytes 1 to 26 into letters), so those three are keys of the text area and their menu
items (Edit > Indent lines, Outdent lines, Comment lines) carry no shortcut; nothing was written around the toolkit.
Its layout is deferred to the next paint, so after a bar is shown or hidden or the size changes, the window flushes
(`gk_flush()`) before it reads the text area's size; without that, the first screenful after Ctrl+F was cut to the old
height.

**Menus.** File (New, Open…, Save, Save as…, Close tab, Quit), Edit (Undo, Redo, Cut, Copy, Paste, Delete, Select
all, Indent lines, Outdent lines, Comment lines), Search (Find…, Find next, Find previous, Replace…, Find in files…,
Go to line…, Go to bracket), View (Language, a submenu of nine with the file's checked; Next tab, Previous tab), Help
(About Code Editor, which the desktop puts first in its own Help). Four titles of at most six letters, so the bar
holds them with Apps, Window and Help at 640 × 400. Items that cannot act are greyed (Undo and Redo with nothing to
undo or redo, Cut and Copy with nothing selected, Comment lines in a language without comments).

**Choosers.** The desktop's: Open and Save as start in the folder of the file shown, or of the last file chosen;
Save as offers the file's name, or `untitled.txt`. When none comes, the status bar says why: the window lacks the
focus (`EACCES`), the desktop is busy (`EBUSY`: one is open, or after desk's fix round an answer not yet taken;
`EAGAIN`: its queue of events is full), or it refused; a guard waiting for a name then stops, keeping everything open.
On a desktop without choosers the toolkit's line to type a path stands in.

**No console needed.** `wcode` runs no program and never calls `ttyctl()` (desk's fix round may start programs from
the desktop without a console); what it says outside the window goes to stderr, which may lead nowhere.

**Starting.** `wcode [file ...]`: each file a tab (an untouched new file gives its place to the first), a name not
there yet a new file; options are refused (status 2). Started from the Terminal in the foreground, its window takes
the focus (the desktop's rule); Files starts a new `wcode` for each file it opens; Apps and the tile bring a running
one to the front.

## Merges

- 9e0a471f: kit's 2ba18949 (the toolkit's core and desk's amended ABI), as the coordinator named it; no conflict.
- 6e952fad: desk's 2c5afd30 (the desktop's side). The Makefile and `tools/gui_apps_scenario.py` met kit's insertions
  at the same places; both sides are kept whole, desk's before kit's, as desk's own branch has them.
- c63b622a: kit's eab009d3 (its stage 2, and `gk_next()` reading on after a burst), as the coordinator named it for a
  program that polls with a short time; no conflict. wcode waits with -1 when it has nothing to do, but polls with 0
  while find in files or the colouring has work, and a burst the toolkit takes itself (the pointer over a label) could
  then leave a key or a click on Stop unread until a later call. Its loop never took 0 as the end.
- Not merged: desk's fix round (the integration brings it); the account says what was built to it.

## Left out, and why

- Word wrap, overwrite mode, several cursors, folding, auto-closing brackets, split views: not asked for; the brief's
  list is the scope.
- A search that spans lines: the find field is one line, and matching line by line is what bounds a regular
  expression's work.
- Find in files over unsaved text: it reads the disk; the manual says to save first.
- Encodings other than UTF-8, and converting line ends: not asked; bytes that are not UTF-8 are shown and kept.
- A copy larger than the desktop's clipboard (128 KB) is refused with a message (a cut too, so nothing is lost).
- Running a build from the editor: not asked, and it would need a console (see above).
- Printing: SurfOS has no printers.

## Tests

| Test | What | Checks |
|---|---|---|
| `test_wcode` | everything but `fileio.c`, `window.c` and `main.c`: random edits against a plain array, the history round trip, the colouring's states against a from-scratch tokenizing after random edits, hostile bytes in every tokenizer, the commands, the guard, the keys as desk's ABI sends them (the bits, repeats), the pointer; the review's findings (`review()`) | 527,441 (60 rounds, as `make unittest` runs it); 529,375 with marks every 3 lines |
| `test_wcode_save` | `fileio.c` on `tests/host/sim_fs.c`: every write, fsync and rename failure, half-done renames, twins, Save as, reading, the rescue copy; two windows of one file, a device by another name, lone CRs (every comparison by bytes and count) | 69 |
| `test_wcode_win` | the window on the toolkit's fake desktop (`tests/host/gk_fake.c`), files in memory: it opens (the tab, the numbers, the status bar); typing presents only the rows that change; Open through the chooser, colours; the menus in the bar (shortcuts, radio items, greyed ones); the find bar (Enter, Shift+Enter, Esc, F5); Go to line's box; the clipboard both ways; a click, a drag, a double click, the numbers; the wheel; a new size; find in files, its list, a line opened at its match; the guard on Close tab, on the close button (pressed again while it asks) and on Quit; a never-saved file named through the chooser; no choosers (a path typed); a busy desktop; a missing file; the theme; the gutter down to the area's foot; the colouring's catch-up after a jump, Esc stopping a search between its steps, the disk's question | 91 |
| `test_wcode speed` | a 16 MB file and a 16 MB single line, timed (below) | 23 |
| screen checks | `tools/gui_apps_code.py` in qemu-usb's guiapps scenario, on the real desktop (below) | 29 |

`make unittest` runs the first three at -m32 with libsurf's own objects (`unittest-code`). On the Mac,
`tests/host/wcode/mac-sanitize.sh` runs them natively with clang under AddressSanitizer and UndefinedBehaviorSanitizer
(`test_wcode` also with marks every 3 lines, so that every lookup and colouring crosses marks), and the speed test at
-O2; the window test is built for x86_64 and runs under Rosetta, as kit's are (blibc has x86 assembly).

**The screen checks** (`GUIAPPS_ONLY=gui_apps_code`): the Terminal mounts the data stick and runs `wcode
/usb1p1/code/hello.c` in the foreground; its window opens with the focus, titled after the file, the tab lit, the line
numbers, the C in its colours, its menus File, Edit, Search and View in the desktop's bar; Ctrl+End, Enter and a
comment typed (in the comment's colour, the title and the tab starred); Ctrl+S (the status bar says so, the star goes);
Ctrl+F, `return`, Enter (the match selected), Esc; Ctrl+Shift+F (what was found last, the file's folder), Enter (`1
line found in 2 files read`), Esc; Ctrl+N and something typed (Untitled 2); Ctrl+Q asks about it, Don't save ends the
program, the prompt comes back and `ps` finds no `wcode`; then Files, the stick, its code folder, Enter on `notes.md`
starts a new `wcode` with it (the registry's type), the Markdown heading in navy, its close button ends it at once.
After the guest stops, the host reads `hello.c` back as saved, with no `hello.c.tmp` beside it. The positions the
toolkit gives the bars were measured on the fake desktop, which runs the same layout code.

**The gates**, on the committed tree c63b622a (the account was committed after them, alone):

- `make unittest` (the Docker image, -m32): passed; test_wcode 527,096, test_wcode_save 58, test_wcode_win 76, and
  test_guidesk reads `wcode.app` (139), `tests/test_man.py` finds its page.
- `make test-all` through the lock: passed, the smoke test 558 of 558 and Files 51 of 51 at -O0 and at -O2. (A first
  run, on 7e37aa5d, failed at -O2 only, from the smoke test's command-line network step (telnet and ftp to the
  harness's scripted server) on, with 25 runs waiting on the lock; the -O2 smoke test alone then passed 558 of 558.
  The smoke test never runs wcode.)
- guiapps, the whole scenario, at O2 and at O0: 331 of 335 each. Two failures are kit's chooser check and the desktop's
  close after it (kit fixed its check at aac42b11, which this branch does not have); the chooser that check left open
  took this track's first Ctrl+T, so its checks stopped there, and the readback of the save they never made failed.
- guiapps again on the same images, every track's checks in their order but kit's: 348 of 348 at O2 and at
  O0, this track's 29 among them, and the two FAT checkers found the stick clean after them.
- Before them, this track's checks alone (`GUIAPPS_ONLY=gui_apps_code`) on 7e37aa5d's O2 image: 35 of 35.
- `tests/host/wcode/mac-sanitize.sh` (60 rounds): all passed.

**Speed** (the Mac, natively, -O2; QEMU's TCG is tens of times slower). A 16 MB C-like file of 474,265 lines: read in
0.035 s; Ctrl+End 0.003 s; a screenful there 0.002 s; every colouring state, idle, 0.037 s; 200 keys at the top and a
screenful 0.001 s; a screenful at the end after them 0.000 s (the states stand); a comment opened at the top, then its
states to the end 0.008 s; a plain find through it with no match 0.006 s, a regular expression 0.024 s; Replace all on
every line 0.031 s (history forgotten: too large), with a group on 15,112 lines 0.087 s (history kept), its undo
0.001 s; 2,000 Downs and 100 Page Downs 0.002 s. One 16 MB line: to its end and a screenful 0.074 s, 20 Lefts there
0.010 s.

**Found by the tests and fixed:** the 16 MB limit was checked only when the gap ran out (Replace all could grow a file
past it while the gap lasted); a plain search reported a match at a line's start when the line had no candidate
byte at all; a copy beside the file was marked as holding the text by the history's step number, which typing merges
into, so later typing looked saved (the step is now closed when a copy is made, as a save closes it); the horizontal
scroll bar came for a text that fit, from a cell counted twice for the caret; the window read the text area's size
before the toolkit's deferred layout had made it (now flushed first). The screen checks' pictures showed the gutter
stopping 4 pixels short of the status bar (the area is not a whole number of rows; now painted to its foot, and
checked), and gcc at -m32 two `snprintf()`s that could cut a path (now bounded).

**Outside my files: `tests/host/sim_fs.c`.** Its `write()` halves a short write until it reaches 0 bytes and then
copied 0 bytes to `x->bytes + pos` with `x->bytes` NULL (a new, empty file): pointer arithmetic on a null pointer is
undefined in C (C11 6.5.6), and UBSan's pointer-overflow check stops there, so `mac-sanitize.sh` had to build that file
without the check. The narrow fix the coordinator allowed: the copy only when there is something to copy (`if(len)`),
with a comment; nothing else in the file changed, and its other users' tests pass as before. `mac-sanitize.sh` now
builds it with every check.

## The review

An independent review read b13fb6c6 (11/09/26). What held up: saving (never truncated, `O_EXCL`, the care over a
half-done rename), byte-exact round trips, the guard on every way out, the model's bounds, no recursion, the keys,
the clipboard, the loop, the `sim_fs.c` fix, and where the code comes from. What it found, and what was done, each
with its test (the review's own probes, in its scratch folder, were run against the fixed tree too):

- **C1, Replace all could delete text no undo brings back.** It cut each line, then put its new text; a put that
  failed (no room) left the line cut, with the history perhaps already forgotten. Now the count works out the file's
  largest size along the way and its longest new line, has that room promised (`wc_text_reserve()`, which no deletion
  shrinks meanwhile) and the line's buffer made before the first cut, so nothing can fail after one: all of it or
  none, and a change too large to undo says so. Tests: no room (nothing changed, said), larger than the history (done,
  said); the probes' 4 MB line of JSON that went to 0 bytes, now refused whole or done whole.
- **M1, typing, Enter, a paste, Replace, Tab, indenting and comments over a selection could lose it.** The same cut
  before a put: each now asks for the room the change comes to first (`wc_doc_room()`), counting the selection's
  bytes out, so a paste that fits once the selection is cut is done (the old order refused it after the cut), and one
  that does not leaves everything as it was. Replace's refusal is no longer hidden by the search after it.
- **C2, the Markdown tokenizer was quadratic** (a line of `[` took 140 s at 1 MB). Its scans for `]`, `)`, a closing
  backtick run and a closing `*` or `_` now remember what a scan found nowhere after where it began, so a line is read
  once for each (link text runs to its first `]`: brackets in it are not followed). And every tokenizer reads a line's
  first 8 KB only (`WC_LEX_MAX`), as if it ended there, so no line costs more than that to colour or to carry a state
  over; the rest is plain, and a comment opened past 8 KB does not go on to the next line. Tests: 25 hostile units at
  64 KB and 1 MB in every language, the state the first 8 KB's, under a time bound; Markdown's scans at 8 KB, timed.
- **C3, regular-expression searches were unbounded and could not be stopped** (`".*"` with Find previous 8.2 s on a
  64 KB line, `(a*b)?` 35.6 s). A pattern that matches an empty text is refused (x+ for x*); Find previous walks a
  line's matches once; whole words go on from the next place a word can start; and every search is charged the text
  each `regexec()` may read, stops where it is when its budget (1 MB) is spent and goes on from there: Find, Replace
  and Replace all run between events (the status bar says so, a key or Esc stops a search; while Replace all changes
  the file the keys wait, and Esc takes it back unless it is too large to undo), and find in files bounds each line
  (`WC_GREP_LINE_WORK`), leaving out and counting one too costly. REG_ESTEPS (a back-reference giving up) is said, no
  longer "Not found". Left as it is: whole words with a pattern that runs to the line's end (`a.*`) on a line of many
  words costs the line's length at each word start; the budget keeps the window answering and Esc stops it.
- **H1, a save could go over a file changed on the disk, and one file could be open twice.** A document keeps the
  size, time and number of its file as read or last saved, and a save over it that finds them changed (another
  wcode, Notepad) asks first, in a box: save over it, Save as, or leave it (the guard's question waits for it). Paths
  are made absolute (`main.c` gives the program's folder) and taken in one form (`wc_path_canon()`), and compared as
  FAT names them, case aside, so `a.c`, `A.C` and `/m/./x/../a.c` are one tab; Save as onto another tab's file is
  refused. FAT keeps times to 2 seconds: two saves of the same size in those 2 seconds are not told apart.
- **H2, find in files could enter `/dev` through `.` or `/.`.** Its folder is taken from / in its one form (`.` from
  the program's folder, `/usb1p1/../dev` is `/dev`, refused), so the check holds on it and on every path joined to it;
  `wc_file_read()` takes the same form, and refuses whatever `fstat()` calls devfs, by any name.
- **Lows:** the colouring's catch-up is asked for after any jump, not only by find in files; the folders find in files
  leaves out are said, with why (unreadable, too deep, a path too long), with the files and lines left out; Enter
  or a paste right after a lone CR puts CR LF, so that CR stays lone and Backspace takes only what was put; Replace
  all's messages say what happened, its count only what it replaced; the history's count no longer underflows when
  its ops' block is refused after its bytes' grew; closing the last tab makes the new file first (it stays when there
  is no memory, said), no longer freeing what it then used; with several files named, a later one opened leaves an
  earlier refusal said; the manual says that empty matches are never found.

**The fix round's gates**, as the coordinator set them (a track that changes only its own files runs no test-all; the
integration's gate does): guiapps, the whole scenario, on cf82bafa (with kit's aac42b11): 383 of 383 at O2 and at
O0, this track's 29 among them. `make unittest` on cf82bafa failed one check of test_wcode, its own new time bound
(0.5 s for three searches on a 1 MB line that take 0.02 s on the Mac, in the image's emulated 32-bit build under
load); the bounds were widened tenfold (c55265cb: they catch the quadratic searches, which took hours there), and
`make unittest` on c55265cb passed: test_wcode 527,441, test_wcode_save 69, test_wcode_win 91. c55265cb changes only
that test, which is in no image, so the guiapps runs stand for it. `mac-sanitize.sh` (60 rounds): all passed.

The tests for these are in `tests/host/test_wcode.c` (`review()`), `test_wcode_save.c` (two windows of one file, a
device by another name, lone CRs, and every comparison by bytes and count) and `test_wcode_win.c` (the catch-up after
Ctrl+End in 4 MB, Esc stopping a search between its steps, the disk's question in its box). To reach two failures
that only a refused allocation brings, `undo.c` and `app.c` take a test's allocator when built with
`WC_UNDO_REALLOC` and `WC_APP_MALLOC` (the Makefile's test_wcode and `mac-sanitize.sh` do).

## Where the code comes from

Written for this tree from standards, specifications and the tree itself; no other program's source.

- The gap buffer: the classic structure for editors (Craig A. Finseth, *The Craft of Text Editing*, 1991), written
  from the idea. The marks, the states kept in them and the comparison at the next good mark are this editor's own; the
  idea of keeping a tokenizer's state at line starts for incremental colouring is common to editors.
- UTF-8: RFC 3629 (the well-formed sequences: no overlong forms, no surrogates, nothing past U+10FFFF).
- The tokenizers: ISO/IEC 9899:2011 (5.1.1.2 line splicing, 6.4 lexical elements, preprocessing numbers), the NASM
  manual (chapter 3: comments, strings, numbers, directives, local labels), the GNU as manual (i386 AT&T syntax:
  `%` registers, `$` immediates, `#` comments, dot directives, local labels), the Python Language Reference (2, Lexical
  analysis: string prefixes, triple-quoted strings, line joining), POSIX.1-2017 XCU 2 (2.2 Quoting, 2.4 Reserved Words,
  2.7.4 Here-Document), the GNU make manual (rules, variables and their flavours, recipes, `define`, functions), the
  CommonMark specification 0.30 (fences, ATX and setext headings, thematic breaks, code spans, emphasis, links,
  autolinks, HTML comments), RFC 8259 (JSON; comments as JSONC files have them). x86 register names from Intel's
  manuals.
- The here-document word's hash: FNV-1a (Fowler, Noll, Vo), folded to 16 bits.
- Regular expressions: libsurf's (POSIX); the replacement's `&` and `\1`-`\9`: POSIX sed's `s` command.
- The filter's `*` and `?`: the usual two-pointer matching, written from the idea.
- Saving: this tree's `user/bin/saveutil.h` and `user/bin/edit.c`.
- The window: kit's toolkit (`user/include/gk.h`) as kit's `user/wkit/` uses it, and desk's window calls
  (`include/surfos/win.h`); the row-by-row comparison of cells is this program's own. The icon and the colours were
  drawn and chosen for this program.
- Behaviour matched: SurfOS's Notepad and `edit` (runs as steps of the history, the NUL rule, the care over a half-done
  rename, CR LF kept); and what code editors generally do (tabs of files, a gutter of line numbers, auto-indent, a
  closing brace that dedents, Home to the indentation first, words for Ctrl+arrows, matching brackets, toggling
  comments, a find bar under the text, find in files listing matching lines), from no one program's code or look.
