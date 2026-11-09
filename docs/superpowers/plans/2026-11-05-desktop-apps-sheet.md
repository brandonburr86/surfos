# The desktop applications, track sheet: Spreadsheet (`wsheet`)

The account of track sheet of the brief `2026-11-05-desktop-apps.md`: the model, its window, the wiring and the gates.

## The model

### What was built

| File | What |
|---|---|
| `user/wsheet/sheet.h` | the model's interface: everything main.c and the tests use |
| `user/wsheet/sheetpriv.h` | what the model's files share: cells, contents, formulas' programs, steps |
| `user/wsheet/cells.c` | memory counted against a budget; rows and cells; what typing makes of text; the reading half of `sheet.h` |
| `user/wsheet/number.c` | numbers read as typed and as CSV writes them; the shortest text of a double; General, fixed, percent; ROUND |
| `user/wsheet/formula.c` | the formula language's tokens, its parser (to a postfix program), and rewriting references when cells move |
| `user/wsheet/eval.c` | values, conversions, operators and the 24 functions |
| `user/wsheet/recalc.c` | recalculation in dependency order, cycles, the work limit |
| `user/wsheet/edit.c` | every change as a step of the history; Undo and Redo; copy and paste |
| `user/wsheet/files.c` | the sheet's own text format, CSV, and pasted tab-separated text |
| `user/wsheet/view.c`, `view.h` | the grid as the window shows it, without drawing it: geometry, the active cell and the selection, the keys, the pointer, the wheel, a column's width dragged, the status line |
| `tests/host/test_wsheet.c` | the host test, with `long` for the fuzzing |

Nothing in these files makes a system call or draws: `tests/host/test_wsheet.c` compiles them as they are.

### Decisions a reviewer would ask about

**Floating point, and the same bits everywhere.** A value is a double. libsurf and the programs are i486 code, so a
double would be computed in x87 registers of 64-bit mantissas, rounded twice and differently at -O0 and -O2. wsheet's
objects are built with `-march=pentium4 -msse2 -mfpmath=sse`, as the Makefile's own comment offers a program (the kernel
keeps XMM registers since L10-L13; QEMU's CPUs and the laptop's have SSE2). The only x87 instructions left in the model
are the `fstpl`/`fldl` of the i386 ABI handing a double to and from libsurf, which are exact. The host test is built with
`-ffp-contract=off` (clang on the Mac's arm64 would otherwise fuse `a*b+c`) and, at `-m32`, with SSE2 too, so the Mac, the
Docker image and SurfOS compute the same bits. Number to text goes through printf's `%.14e`, which libsurf, glibc and the
Mac all give exactly; text to number through `strtod()`, correctly rounded in all three. `pow()` is the only function whose
last bit could differ between libsurf's and a host's; the tests use cases where it cannot.

**What a person sees of a number is its 15 significant digits** (C's DBL_DIG, the decimal digits every double keeps). So
`=0.1+0.2` shows 0.3, `=0.1+0.2=0.3` is TRUE (numbers equal to 15 digits are equal in a comparison), and `=0.1+0.2-0.3`
shows 5.55111512312578E-17. What a format or ROUND() cuts after the 15 digits is cut a half away from zero, as people round:
2.675 with two decimals is 2.68, though the double is 2.67499999999999982236431605997495353221893310546875. This is what
spreadsheets show; it is done in decimal, on the 15-digit string, never by multiplying by powers of ten.

**What is kept is exact.** In a file and the formula bar a number is the shortest text that `strtod()` reads back as the
same double (15 to 17 digits, the zeros at the end dropped), laid out as ECMA-262's `Number::toString()` lays its digits
out (plain from 1E-7 to 1E+21), with a capital E. A percent shows as its digits two places on (`12.5%`) and is read back by
moving the decimal exponent, not by dividing, so it too comes back exact. The tests check 20,000 random doubles both ways.

**A cell holds what was typed.** `=` begins a formula, `'` text taken as it follows, then a number (`12`, `-3.5`, `.5`,
`1e3`, `50%`), TRUE or FALSE (either case), an error's name, or text. Typing a percent into a General cell makes it a
percent cell with the decimals typed, as spreadsheets do. The formula bar shows the same text, which typed again makes the
same cell: text that would be read as something else gets its `'` back. A formula is kept as typed with its references,
names and error names in capitals and a range's corners in order (of two equal rows or columns, the absolute one first, so
a range has one text); spaces stay.

**A formula that does not parse is kept, and shows #ERROR!.** Spreadsheets differ here: some refuse the entry. Keeping it
never loses what was typed (a long formula half written), the model has one path for typed and loaded formulas, and
`sh_problem()` says why, with the character, for the status bar: "a ( has no ) (at character 5)". A name that is no
function, or a call of one that is not there, is #NAME? (OpenFormula's), and `sh_problem()` names it.

**Errors.** OpenFormula's seven (#DIV/0!, #VALUE!, #REF!, #NAME?, #NUM!, #N/A, #NULL!) and three of this sheet's own,
each plain words: #CYCLE! for a formula in a cycle, #ERROR! for one that does not parse, #LIMIT! where a recalculation ran
out of its time or memory. An error is passed on by whatever meets it, the left operand's first.

**The language** is OpenFormula's (OASIS ODF 1.2 part 2) as spreadsheets show it: `,` separates arguments, and `;` does
too. Precedence from the tightest: the sign, `%`, `^`, `*` `/`, `+` `-`, `&`, the comparisons; every binary operator
groups from the left, so `-2^2` is 4 and `2^3^2` is 64. A range is a value only to a function; anywhere else it is
#VALUE! unless it is one cell (no implicit intersection). A number that runs into a name (`2A5`) is a mistake: before it
was, a move turned `2A5` into `2E6`, a valid number of another meaning; the fuzzing found that.

**The functions** follow OpenFormula and what spreadsheets agree on, each written out in eval.c's header: a reference or a
range gives its cells (text and truth values there left out of sums), a value in the call is taken as it is; SUM and
AVERAGE add with Neumaier's compensated sum, so ten 0.1 make exactly 1; MOD takes the divisor's sign; 0^0 is 1 as C's
`pow()` has it (OpenFormula leaves it to the implementation); UPPER and LOWER change ASCII's and Latin-1's letters (ÿ to
Ÿ too) and nothing else, as the console's font has no others to show; text compares in either case on the same letters;
LEN, LEFT, RIGHT and MID count UTF-8 characters, a byte that is no UTF-8 one each.

**Recalculation is whole, every time, in dependency order.** Tarjan's strongly connected components give the order (a
component finishes after all it reaches) and the cycles: every formula of a component of more than one, or one that
refers to itself, is #CYCLE!, also those reached only by a cross edge (A uses B and D, B uses C, C uses A, D uses B: all
four). A formula that only uses one gets #CYCLE! passed on like any error, or not: COUNTA counts it. A reference in an IF
branch not taken is still a reference, so it can make a cycle, as in every spreadsheet. The search keeps its own stack:
65,536 formulas each using the one above take no recursion (a test). Recalculating everything after each change means no
value can be stale; at SurfOS's sizes it costs milliseconds. Its work is bounded: every instruction run, reference
followed and row and cell looked at in a range is a step, and past 20 million it stops, and what it did not finish shows
#LIMIT! (a hostile file of 100,000 formulas each summing 300,000 cells would otherwise hang the window).

**The parser recurses on nothing.** It is Dijkstra's shunting-yard over its own stack, so 7,999 minus signs in a row
parse. Parentheses and calls inside one another are limited to 64 anyway, a function to 255 arguments, a cell to 8,192
bytes, a number in a formula to 255 characters.

**A change is built before it is made.** A step lists the cells it changes and what each is to hold; making it swaps each
cell's content with the listed one, so afterwards the step holds the old contents and Undo and Redo are the same swap again,
copying nothing. Everything a change needs (contents, compiled formulas, the history's room) is asked for before any
cell changes. Only making a new cell can still need memory midway; then the swaps already made are swapped back, which never
asks for memory (an emptied row keeps its room until the change ends), and the change, Undo or Redo is refused with the
sheet as it was, its values too: a formula's cached value is dropped only once a change is whole (the fault tests found the
first version dropping it at the swap, so a refused change lost a text value until the next recalculation). A step of many
cells (a whole column's format) makes all its new rows in one merge, not a row at a time, which on 32,768 scattered rows
would have been quadratic.

**Inserting and deleting rows and columns** is a step with two parts around the move: before it, the cells that go (and
the widths of columns that go or are pushed off the end); after it, every formula whose text changes, rewritten. Every
reference moves with what it refers to, relative or absolute. A range grows when rows go in inside it, moves when they go
in at its first row, shrinks when some of it goes, and is #REF! when all of it goes; a range's end stays at the sheet's
end rather than leave it. A reference to a cell that went, or would be pushed off the sheet, is #REF!. Inserting is
refused when it would push cells off the end, as spreadsheets refuse it.

**Copy and paste.** Copy keeps the cells with their contents and formats, and makes their values as tab-separated text
for the desktop's clipboard: as they show (formats applied, no width), a field with a tab, a line end or a quote in
quotes. Paste moves relative references by as far as the cells moved, and what would leave the sheet is #REF!; the cells
the copy has nothing for are emptied, formats too. Text pasted from elsewhere is taken as typed, a field a cell, into the
cells' own formats (it has none). A copy covers 1,048,576 cells at most (16 whole columns), its text 128 KB (the desktop's
clipboard), past which only the Spreadsheet's own copy is made. Cut is Copy and then Delete, and since the review its
paste moves the cells (below); references from elsewhere to the cut cells are not moved, as they are by a
spreadsheet's Cut, which is more than the brief asks.

**The history** keeps 100 steps and 8 MB; a change bigger than that is made and cannot be undone (the history starts
again), as Notepad's does. A sheet is modified when its state is not the one its file holds: every step has a serial, so
undoing back to the saved state makes it unmodified again.

**The sheet's own format** ("SurfOS sheet", `.sheet`) is a line a record, so a person can read it and `diff` two
versions: `SurfOS sheet 1`, then `width <column> <pixels>` for columns not 80 wide, then `cell <name> <format> <content>`
in rows, the content being the formula bar's text (so reading a file is typing it), with `\\ \n \r \t \xHH` for the
bytes a line cannot hold. Its full description is files.c's header, and docs/GUI.md's section. A newer version is refused,
saying so; a cell given twice is refused; an empty file is an empty sheet.

**CSV is RFC 4180's.** Reading takes CRLF, LF or CR, a UTF-8 byte order mark, and is lenient where the RFC has no answer
(a quote inside an unquoted field, text after a closing quote: kept). A field is a number only if it is one as JSON writes
one (RFC 8259), so 007, 1,000, .5 and +1 stay text exactly as they were, and a bank's export of `"-42.50"` is a number
(quotes do not make text); TRUE and FALSE are truth values; a field that begins with `=` is text, never a formula. Saving
writes the values from A1 to the last cell that holds something, every record with the same number of fields, CRLF, and
numbers in their shortest exact text.

**The grid's behaviour (view.c)** is the spreadsheets' a user knows. The active cell stays where a selection began and
Shift with an arrow, a drag or a Shift+click moves the other corner. Ctrl with an arrow goes to the edge of the data (along
filled cells to the last, across empty ones to the next filled one, or to the sheet's edge). Page Up and Down move the view
with the cell. Home goes to column A, Ctrl+Home to A1, End to the row's last filled cell, Ctrl+End to the last row and
column that hold anything. Enter moves down and Tab right, Shift back, and Enter after Tabs comes back to the column the
Tabs began in. Headers select whole rows and columns, the corner everything; a column header's edge drags the width, set
once when let go (one step), and all selected whole columns take it. A drag past the grid's edge scrolls a row or a column
each move. Text runs on over empty cells to its right; a value, "" too, stops it. The status line is the active cell's
trouble ("a ( has no ) (at character 5)"), or the selection's sum, average and count.

**Limits, for every file read and everything pasted:** 256 columns, 65,536 rows, 300,000 cells, 8,192 bytes a cell (the
' that keeps a text text not counted, since the review), 32 MB a file read or written, a line of the sheet's format 4 x
8,192 + 64 bytes (every byte escaped), and the model's memory 40 MB,
counted by its own allocator (`mem_alloc()`), so a file or paste too big for SurfOS is refused with a message instead of
taking every page the program may have. Within 64 MB with the window and libsurf.

### Left out

- Whole-column and whole-row references (`A:A`), names, other sheets in a file, functions beyond the brief's 24.
- Implicit intersection (a range where one value is meant): #VALUE!, which is never wrong, only less helpful.
- Cut that moves references (above); fill down and right; sorting; finding.
- Thousands separators and dates in formats: the brief asks for General, fixed decimals and percent.
- Recalculating only what changed: whole recalculation is simpler and never stale, and fast enough at SurfOS's sizes.

### The tests

`tests/host/test_wsheet.c`: 716 checks and four fuzzers, about a second natively, three seconds in the Docker image at
`-m32`. Names and ranges; numbers as typed and as CSV writes them; the shortest text of 20,000 random doubles and their
percents read back exactly; General at every width (5,000 random cases fit); fixed decimals and percents; ROUND and
15-digit equality; the language's precedence, text, errors and names, 34 kinds of formula that do not parse with their
messages, the depth, argument and length limits; conversions and comparisons; every function, edge cases included;
recalculation in reverse order, cycles of every shape, 65,536 chained formulas, the work limit; copy and paste with
relative, absolute and mixed references, #REF!, formats, the edge of the sheet, the clipboard's text, text pasted;
inserting and deleting rows and columns with every rule above and the widths; the history's every kind of step, the
modified state, its limits; the grid's geometry and hit tests, every key with Shift and Ctrl, Enter and Tab, the pointer
on cells, headers, the corner and an edge, a drag past the bottom, the wheel, text running on, the status line; both file formats (an exact expected file, a byte at a time, cut at every byte, 20 kinds of
refusal with their line, by hand with comments and CRLF); RFC 4180's examples; every allocation of 15 kinds of change,
Undo and Redo, Copy and both readers refused in turn, with the sheet, its values and the count of blocks unchanged; and
at the end nothing allocated.

`test_wsheet long` (not in `make unittest`): 200,000 generated formulas, a third of them damaged, each typed into a sheet
and checked to normalise to itself and to move there and back (by a paste's offset, and rows and columns inserted then
deleted) to its own text; 100,000 random CSV inputs fed in random pieces, each saved, read and saved again to the same
bytes; 30,000 random changes, each undone and redone to the same file, and the values checked against the sheet read
again from its own file; 50,000 damaged sheet files. It runs natively on the Mac under `-fsanitize=address,undefined`,
in about a minute, clean; the command is in the test's header. It found the two rewriting faults above.

## The window

### What was built

| File | What |
|---|---|
| `user/wsheet/app.c`, `app.h` | the window on kit's toolkit (gk.h): the formula bar, the grid (an area the program draws), the status bar, the menus, the choosers, the clipboard, the files' flow |
| `user/wsheet/io.c` | the files through the system calls: read a piece at a time; saved through `saveutil.h`, or made with `O_EXCL` |
| `user/wsheet/main.c` | the loop: `gk_next()`, and between events the next piece of a file being read |
| `tests/host/test_wsheet_win.c` | the window on kit's fake desktop, with files in memory |
| `tools/gui_apps_sheet.py` | the screen checks of the guiapps scenario |
| `rootfs/apps/wsheet.app`, `rootfs/man/wsheet.txt` | the registry's entry (`sheet csv`, Apps and a tile) and the manual's page |

Inserted, nothing changed: the Makefile (the program after `ftp`, its rules beside it, `unittest-sheet` before
`unittest:`), the scenario's TRACKS line, docs/GUI.md's section "Spreadsheet" before "Programs' windows".

### Decisions a reviewer would ask about

**Two ways of editing, as spreadsheets have them.** A key typed in the grid begins the cell anew and is drawn in the
cell and mirrored in the formula bar; Enter, Tab, Shift+Tab and every arrow put it in and move. Editing what a cell
holds (Ctrl+E, since F2 never reaches a program; a double-click; a click in the bar) happens in the formula bar,
kit's field, where the arrows move in the text; Enter puts it in, Esc cancels, and the grid taking the focus back (a
click, Tab) puts it in, to the cell the edit began in, whatever the click selects. Undo while typing is Esc.

**The grid is an area** with the toolkit's scroll bars in rows and default-width columns: one unit more down than the
sheet has rows, for the header's row, so the last row can reach the bottom. The view (view.c) keeps the scroll
position; the bars and the wheel move it (GK_EV_SCROLL), and keys that move far take the bars along (`gk_scroll_to()`,
called only when the view moved, never because they differ, which at the far right they may: the bar's last unit is
256 less a page, the keys reach column IV).

**What changed is all that is painted.** A move of the active cell damages the rectangles of the old and the new
selection and the two header strips; typing damages the edit's row from its cell on; a change of the sheet, a scroll or
a width paints the grid. The window's test measures a move: well under a quarter of the window presented.

**The clipboard.** Copy keeps the cells (`sh_copy`) and puts their values as tab-separated text on the desktop's
clipboard. A paste reads the clipboard: the same text as the copy's means the copy's cells (formulas moved); anything
else is text, pasted as typed. When the copy's text is too long for the desktop (128 KB), the clipboard as it was then
is remembered by a hash, and a paste of it unchanged is the copy's.

**Files.** Open and Save as are the desktop's choosers, for `sheet csv`; the chooser starts in the file's folder and
offers its name (or `Untitled.sheet`). A file is read 4 x 64 KB between two looks at the events, so the window answers
and Esc stops it; while it is read nothing else changes the sheet (a key says so), since the file replaces it. A file
that cannot be read leaves the sheet as it was and says why, with the line or the row. Saving as CSV asks first when
formulas, formats or column widths would be lost, and after such a save New, Open and Close ask again (the review).
New, Open and Close ask about unsaved changes (Save, Don't save, Cancel); Save on an untitled sheet is
Save as, and what waited for the save goes on when it is done. The desktop's chooser comes only for a person's key or
click: Open after a Save as on an untitled sheet cannot chain a second chooser, and says to press Ctrl+O again.

**io.c** is SurfOS's only (`saveutil.h` reads SurfOS's `struct stat`), reached through `struct sheet_io`, so the
window's test has files in memory in its place and the screen checks test the real one on a stick.

**Menus:** File, Edit, Sheet, Format (4 titles, 31 of the bar's 44 cells) and two items in the desktop's Help. Ctrl++
and Ctrl+- insert and delete rows, Ctrl+~, Ctrl+! and Ctrl+% set General, fixed and percent (characters with Ctrl, as
desk's keys deliver them). Undo and Redo are greyed when there is nothing to do; the Format menu's dot follows the
active cell.

**Found by running the window's test in the image.** Save over the sheet's own file copied its path onto itself
(`snprintf(a->path, ..., "%s", a->path)`), which C leaves undefined: the Mac's libc kept the path, glibc emptied it, and
the next Save became Save as. A path is no longer copied onto itself. Two more came from desk's review notes: a second
press of the close button (which desk's fix sends again to a live program) left a second question whose first's
answer cancelled what both were for; now the question asked stays, and a box answering because another took its place
cancels nothing.

**The status bar** says, in this order: the last action's message, a file being read, the edit going on, the active
cell's trouble, the selection's sum, average and count, or that some cells say #LIMIT!; and "Modified" at its right.

### The window's tests

- **`tests/host/test_wsheet_win.c`**, in `make unittest`: 122 checks, the window on kit's fake desktop with files in
  memory, driven as a person drives it: its menus in the desktop's bar (shortcuts' text, greyed Undo, the Format dot,
  Help's items); the grid drawn, the active cell's headers lit, numbers at the right, text at the left, #ERROR! in
  coral; typing, Enter and Tab, Enter back to Tab's column, an arrow putting it in, Esc, Backspace; the status bar's sum
  and a formula's trouble; the formula bar by Ctrl+E (again: what is typed kept), a double-click, Enter, Esc and a
  click on the grid; the cell's
  name going to a cell and a range, and refusing a name that is none; Copy and Paste with formulas moved, text from
  elsewhere, Cut; Undo and Redo, and Undo while typing; the formats by menu and by Ctrl+% and Ctrl+~; rows inserted by
  Ctrl++ and undone; a column's width dragged at its header's edge and typed in the box (a width refused); the wheel;
  Ctrl+L far away taking the bars along, Ctrl+Home back; a move presenting under a quarter of the window; Save through
  the chooser, then without it, a save that fails (a box), Save as CSV asking first; Open asking about unsaved changes,
  a CSV read, a file not there and a file not a sheet (boxes, the sheet kept); a file from the command line; a 400 KB
  CSV read four pieces a turn, a key refused meanwhile, Esc stopping it, then read whole; New after a sheet whose A1
  had a trouble (the status bar not keeping the old sheet's); the theme; a new size; Close with nothing unsaved, Close
  asking, the close button pressed again under its question, a question taking another's place, Cancel, Don't save.
- **Natively under the sanitizers** (x86_64 under Rosetta, as kit runs the toolkit's): the toolkit, libgui, the
  drawing kit and the fake desktop compiled with `-fsanitize=address,undefined`, then test_wsheet_win: clean.
- **The screen** (`tools/gui_apps_sheet.py`, the guiapps scenario): Files mounts the data stick and opens
  `sheet/budget.csv` in the Spreadsheet by the registry; the CSV in the grid; seven formulas typed and worked out on
  the screen; a range's sum in the status bar; Save as `total.sheet` through the desktop's chooser in the stick's
  folder, a change saved over it, Save as `values.csv` asking first; Ctrl+W; Eject. On the host, `total.sheet` holds
  the formulas, `values.csv` exactly the bytes the host's arithmetic gives (`0.30000000000000004`,
  `1.4142135623730951`, `2.68`), and `budget.csv` is as it was.

## The gates

The tree gated is c5317fe2: the track's six commits on kit's core (2ba18949) and desk's implementation (2c5afd30),
and kit's eab009d3 merged on top, the toolkit's stage 2 with gk_next()'s fix, which wsheet needs: its loop waits with
0 while a file is read, and there a burst the toolkit took itself held back the events after it. This account is the
only change since. Through the lock, 11/05/26 and 11/06/26:

- **`make unittest`** (the Docker image, gcc -m32 and glibc): passed; test_wsheet 716 checks, test_wsheet_win 122,
  test_guidesk 139 (wsheet.app among the registry's entries), test_gk 436, test_wkit 70, test_man (wsheet's page).
- **Natively under ASan and UBSan** (the Mac's clang): test_wsheet and `test_wsheet long` (64 s), and test_wsheet_win
  against the merged toolkit (x86_64 under Rosetta): clean.
- **`make test-all`:** passed; at -O0 and at -O2 the Files test's 51 checks and the smoke test's 558.
- **The guiapps scenario, every track's checks:** 350 of 352 at -O2 and at -O0. The Spreadsheet's 20 checks and
  desk's 40, which come after them, all passed. The two failures are kit's chooser check and what it leaves: "Ctrl+O,
  a menu's shortcut, asks for a chooser" looks for "Open a file" where desk's desktop now begins a chooser's title
  with the window's ("Toolkit: Open a file"), and the chooser left open stops the desktop's closing. They fail so on
  every tree with desk merged (chat's and games' runs too), and kit fixed them at apps/kit aac42b11, after this tree.
  With them failed the scenario runs neither the kernel heap's check nor the FAT checkers, so the Spreadsheet's
  checks also ran alone (GUIAPPS_ONLY=gui_apps_sheet): 26 of 26 at -O2 and at -O0, the kernel heap as it was before
  gui (668,144 bytes in use), macOS fsck_msdos and fsck.fat finding no error on the stick.
- **Before kit's merge** (341705c4): test-all's -O0 half failed one check of 553, `selftest`, which ran past its
  step's 120 s while other tracks' tests and builds shared the machine and passed alone (06-gates/tools/part.py); the
  -O2 smoke test passed (558, and the Files test's 51); the whole guiapps at -O0 was 350 of 352, the same two.
- Nothing of the Spreadsheet has run on the laptop.

## The review

An independent review read 882cd963 (11/09/26). What held up: files replaced only through saveutil.h (no O_TRUNC
anywhere, O_EXCL for a new one), a full disk and a pulled stick keeping the old file, the questions about unsaved
changes, the snprintf fix, bounds on files and no recursion, the 24 functions on about 100 edge cases, #CYCLE!,
#LIMIT! and the order of recalculation, numbers, rows and columns inserted and deleted, Undo, and the code's origin.
What it found, and what one fix round did:

- **Critical: an edit in the formula bar rewrote what it could not show.** Kit's field makes a byte below 32 a space
  and holds 8,192 bytes, and commit() wrote it back whatever happened, so a double-click and a click elsewhere made
  `line1\nline2` `line1 line2`. Now an edit that changed nothing changes nothing (commit() compares the bar with the
  cell's input), and a cell holding a byte below 32 (a line break or a tab, from a file, a CSV's quoted field or a
  paste) is not edited in the bar at all: a double-click, Ctrl+E, a click in the bar or a key typed there says so, and
  typing in the grid replaces the cell. Escapes in the bar, as the .sheet format writes them, were the other way, and
  were not taken: a backslash typed there would mean one thing in a cell that held a line break and another in every
  other cell, and such cells come only from files and pastes. The field holds 8,193 bytes now (below).
- **High: an 8,192-byte CSV field that needs its ' made a .sheet that wsheet refused.** The ' that keeps a text text is
  not counted: a cell's input is at most 8,192 bytes, or 8,193 beginning with ' (input_fits(): sh_set(), the .sheet
  reader, typing, the field). The rule of the round, anything wsheet saves wsheet opens, also gave the next two.
- **Medium: a CSV save said too little and the sheet counted as saved.** sh_csv_loss() says what CSV leaves out:
  formulas, formats, column widths, cells not worked out (#LIMIT!, written as that text); the question names them,
  and after such a save the window keeps what was lost, so New, Open and Close ask again, their Save as offering the
  file's name as a .sheet; not when a .sheet was saved since the sheet last changed (the window keeps that state), as
  the screen checks do: total.sheet, then values.csv, then Ctrl+W.
- **Medium: Cut then Paste shifted the moved formulas' references.** A Cut's clip (sh_clip_cut()) pastes as a move
  (fx_rewrite()'s FX_CUT): references into the cut cells move with them, $ or not, a range when all of it was cut,
  and the rest stay. Formulas elsewhere that referred to the cut cells are not changed: the page and docs/GUI.md say so.
- **Medium: a .sheet could be saved past the 32 MB a reader takes** (\xHH makes one byte four). sh_write() counts
  first and writes nothing past sh_file_max (a variable now, for the tests); the window says how big the file would be.
- **Medium: the work limit did not count text work.** Every 8 bytes compared, scanned or made is a step (eval.c's
  text_work()). Measured natively, a step of a range takes 4.5 ns and a byte compared 0.6 ns, so 8 bytes a step keeps
  20 million steps near a tenth of a second either way; 25,000 formulas comparing 8 KB texts stop with #LIMIT! after
  about 10,000 of them instead of running seconds on 100,000 counted steps.
- **L7, the recalculation's memory near the budget.** Each formula keeps its share of recalc()'s arrays back in the
  budget while it lives (formula_new(), recalc_per_formula), so they always fit; sh_read_finish() frees the reader's
  memory and the sheet being replaced before the new one is worked out, and the window opens files through it. A sheet
  holds somewhat fewer formulas: the reviewer's 150,000 small ones are refused at 122,637 ("not enough memory for the
  sheet") rather than opened with every one #LIMIT!.
- **L8**: the CSV question's answer leaves the close's question its place when that one took it. **L9**: kit's field
  has no call to put text at its cursor, so Edit > Paste is off while the bar holds an edit, and the field's own
  Ctrl+V pastes at its cursor; pasted into what is typed in the grid, control characters (a NUL, a tab) are spaces,
  as the field makes them. **L10**: the reading's percentage in 32 bits, the size divided by 100 first past 21 MB
  (the first fix divided a long long, which on i386 needs libgcc's __divdi3, and programs link no libgcc: the image's
  build caught it). **L11**: f_open() reads st only once fstat() filled it. **L12**: a formula that does not parse is
  kept as typed when cells move too, and the page says so: moving the references of a formula not finished would
  rewrite by tokens text that a later edit may read otherwise. **L13**: FNV-1a is in the sources' table.

The tests: test_wsheet has 32 checks more, 748 (the input rule, CSV fields of 8,190 to 8,192 bytes of three kinds
saved as .sheet, opened and saved again, the writer refusing past sh_file_max with nothing written, CSV's losses, Cut
pastes, text work, formulas filling the budget still worked out, sh_read_finish(), a broken formula kept), and
fuzz_changes now picks rows and columns at the sheet's far edges and middle too and pastes a third of its copies as a
Cut's; test_wsheet_win has 39 more, 161 (a cell with a line break by a double-click, Ctrl+E and a click in the bar, an
edit that changes nothing, the 8,193-byte input saved and opened, a file too big, CSV's questions and the close after
them, a sheet saved whole before its CSV, the CSV question replaced, Paste in the bar and into typing, a Cut, the
percentage of a 30 MB read). `test_wsheet long` under the sanitizers: clean, in 68 s. The
reviewer's probes against this tree: fuzz_cross 400 rounds, none bad; probe_win1 and probe_win2 as above; probe_size2's
write refused. io.c could not be built on the host (saveutil.h reads SurfOS's struct stat): its paths are the screen
checks' on SurfOS.

The gates of the fix round, on a29f36a5 (the round's commits, and kit's aac42b11 merged as the integrator named it:
kit's screen checks finding the desktop's chooser by its own title), 11/09/26:

- **`make unittest`** (the Docker image, gcc -m32 and glibc): passed; test_wsheet 748, test_wsheet_win 161, test_guidesk
  139, test_gk 436, test_wkit 70, test_man.
- **The guiapps scenario, every track's checks, through the lock:** 374 of 374 at -O2 and at -O0, the Spreadsheet's 20
  among them, the kernel heap as before gui (668,144 bytes in use), fsck_msdos and fsck.fat finding no error on the
  stick; the Spreadsheet's checks alone: 26 of 26 at both.
- **`make test-all`** was not run for the round: since 11/09/26 a track that changes only its own program's files leaves
  it to the integration's full gate (the lock's queue was nine hours long). The first gates above ran it.
- The image's build found the first L10 fix's division (above). Nothing of the Spreadsheet has run on the laptop.

## Where the code comes from

Written from standards, papers and this tree; no other program's source was recalled or looked at.

| What | From |
|---|---|
| The formula language: syntax, precedence, the error values, the functions' meanings | OASIS OpenDocument 1.2 part 2, "Recalculated Formula (OpenFormula) Format" |
| Behaviour where OpenFormula leaves a choice, and the look of numbers (15 digits, `#####`, `1.23E+15`, percents) | the behaviour of Microsoft Excel and LibreOffice Calc as a user sees it, matched the way the brief allows, never their code: `-2^2` is 4, TRUE+1 is 2, text compares in either case, MOD has the divisor's sign, ROUND rounds a half away from zero, General shows 15 digits, #REF! for references to deleted cells, a range moving or growing as rows go in; keeping a formula that does not parse and showing #ERROR! is Google Sheets' behaviour |
| The parser | E. W. Dijkstra's shunting-yard algorithm (Mathematisch Centrum report MR 35, 1961) |
| The order of recalculation and the cycles | R. Tarjan, "Depth-first search and linear graph algorithms", SIAM J. Computing 1(2), 1972 |
| SUM and AVERAGE | A. Neumaier's compensated summation, ZAMM 54, 1974 |
| The digits that matter | C's DBL_DIG and IEEE 754 binary64 |
| The layout of a number's shortest text | ECMA-262, `Number::toString` |
| CSV | RFC 4180; a number's form in it, RFC 8259 (JSON) |
| UTF-8 | RFC 3629's well-formed sequences |
| The tests' random numbers | G. Marsaglia's xorshift |
| The hash that knows the clipboard again (app.c) | FNV-1a, G. Fowler, L. C. Noll and K.-P. Vo's |
| `saveutil.h`, the drawing kit, the window calls | this tree |

The sheet's own file format is mine; its records are a line each, so it reads like the tree's other settings files.
