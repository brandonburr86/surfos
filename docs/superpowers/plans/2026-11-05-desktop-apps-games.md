# The desktop applications, track games: Mines, Solitaire and Swell

The account of branch `apps/games`, for the integrator and the review. The brief is `2026-11-05-desktop-apps.md`. The
branch merges kit's core (`apps/kit` 2ba18949, with desk's amended window ABI) and desk's implementation (`apps/desk`
2c5afd30), as the integrator named them.

## What was built

Three programs, each a folder of its own: the rules, the window's model, the window on the toolkit (`gk.h`) and the
loop.

| Program | Rules | The window's model | The window | Host tests |
|---|---|---|---|---|
| Mines, `wmines` | `user/wmines/mines.c` | `view.c` | `window.c`, `main.c` | `test_wmines.c`, `test_wmines_window.c` |
| Solitaire, `wsolitaire` | `user/wsolitaire/klondike.c` | `table.c` | `window.c`, `main.c` | `test_wsolitaire.c`, `test_wsolitaire_window.c` |
| Swell, `wswell` | `user/wswell/swell.c` | `view.c` | `window.c`, `main.c` | `test_wswell.c`, `test_wswell_window.c` |

- **The rules and the window's model** make no system calls and draw nothing: they take the milliseconds since boot and
  the keys' and the pointer's events as arguments, so `tests/host/test_<game>.c` runs them as they are, in plain C.
- **The window** (`window.c`, `window.h`) is the pattern of `user/wkit/`: open, then each event, then close, apart from
  the loop in `main.c`, so `tests/host/test_<game>_window.c` drives it on kit's fake desktop (`tests/host/gk_fake.c`)
  with libgui and the toolkit as they are, and looks at the pixels the desktop would show.
- **The wiring:** the Makefile's programs and rules after `USER_PROGS += ftp` and the host tests' block
  (`unittest-games`) before `unittest: $(BUILD)/host/test_blibc`; `rootfs/apps/wmines.app`, `wsolitaire.app`,
  `wswell.app` (Apps > Programs and a tile each, no types: a game opens no files); `rootfs/man/wmines.txt`,
  `wsolitaire.txt`, `wswell.txt`; the screen checks `tools/gui_apps_games.py` and their line in
  `tools/gui_apps_scenario.py`; a section in `docs/GUI.md`; `tests/games-sanitize.sh`, the six tests under AddressSanitizer
  and UBSan on the Mac.

**Mines.** Three fields of SurfOS's own: Small, 10 by 8 with 10 mines; Medium, 16 by 12 with 32; Large, 28 by 15 with
80. The mines are laid at the first reveal, never under it, and not around it either when the rest of the field has
room for every mine, so the first reveal always opens an area. An area opens through a queue of cells, each queued once
as it is opened: never recursion, and never more room than the field. Flags (the right button or F), chording (a click
on a number whose flags are placed, or the middle button), a timer from the first reveal to the end, and a count of the
mines less the flags. The keyboard plays it too. The toolbar has New, the field's size (radio buttons) and the counters;
the Game menu New field (Ctrl+N), the sizes (1 to 3) and Close (Ctrl+W); the session keeps each field's best time and
the games played and won.

**Solitaire.** Klondike, drawing one card or three, the waste taken back as often as wanted. Cards are dragged, or
clicked to go where they can (a single card to its foundation first, then the next pile it fits, looking right of its
own). A face-down top turns up by itself; every move can be undone (the newest 1,024); once no card on the piles is face
down and the cards can all go home from there, the game finishes by itself, a card each 60 ms. The toolbar has New,
Undo (greyed when there is nothing to undo), drawing one or three, the moves and the time; the Game menu New deal
(Ctrl+N), Undo (Ctrl+Z), Draw one (1), Draw three (3) and Close. The session keeps, for each way of drawing, the games
played and won, the best time and the fewest moves.

**Swell.** A falling-blocks game of SurfOS's own (how it differs from others is below): a sea 12 cells wide and 18 deep,
seven pieces of two, three and five cells, the swell lifting the sea a row of sand from level 3 on. The board has the
sea and a panel (the next piece, score, rows, level, time and the session's best); the status bar says what happened.
The Game menu has New game (Ctrl+N), Pause (P, checked while paused) and Close.

**The look**, drawn by each program's own code in the desktop's palette:
- Mines: covered cells sand, open ones paper, the numbers in the sea colours (teal, sky, coral, navy, lilac, kelp, ink,
  muted), a flag a teal pennant on an ink pole, a mine a navy sea mine with a foam glint, the one that went off on coral,
  wrong flags crossed out, the keyboard's cursor a coral frame. No bevelled grey tiles, no face button, no seven-segment
  counters.
- Solitaire: a deep teal table; faces of paper with the rank and the suit at the top left (the suits are the console's
  own code page 437 characters 3 to 6), the suit again at the bottom right, the suit large in the middle, a jack, queen
  or king its letter in a tinted panel; backs navy with a foam rim and the desktop's wave mark (≋) tiled in teal; empty
  places outlined, the stock's with the wave when the waste can be taken back, the foundations' with an A.
- Swell: deep water with a faint grid, each cell a block with a light crest and a dark foot in the piece's colour (sand,
  sky, coral, foam, kelp green, lilac, sea green), sand darker and speckled, the falling piece's resting place as its
  outline, rows washed away foam for a moment, a dark band across the sea saying Enter starts, Paused or Over the top.

The icons in the `.app` files are drawn here too: a sea mine on sand, a wave-mark back behind a face with a red
diamond, a coral shell falling into the sea over sand and a wave.

## Decisions a reviewer would ask about

**Three files a program, plus the loop.** The brief puts the model in files `tests/host/` compiles and the window in
`main.c`. A game's rules are big enough for a file of their own, and what the window does with a key or a press (the
layout, the cursor, a press becoming a drag, the question before closing, the session's scores) is host-testable too,
so it is a second file; the window on the toolkit is a third, as kit's `user/wkit/demo.c` is, so the fake desktop drives
it; `main.c` is the loop.

**Seeded randomness.** Marsaglia's xorshift32 (13, 17, 5) in each program, drawn below a bound by rejection, and
Durstenfeld's shuffle. The window's model holds a generator seeded once (`main.c`: the PC's clock and the milliseconds
since boot) and gives each new game its next number; a test seeds the model and every field, deal and piece repeats.
The generator is a dozen lines in each program rather than a shared file: the brief gives this track the three folders
and no other.

**Scores last for the session**, read as "while the program runs": nothing is written, so there are no files to make or
replace, and no file types in the `.app` entries.

**Closing asks only when something would be lost.** Esc, Ctrl+W, Game > Close and the close button close at once unless
a game is under way: Mines between the first reveal and the end, Solitaire between the first move and the win, Swell
while a piece falls or the game is paused. Then the toolkit's question comes up over the window with Cancel as its first
button, which Enter answers, as the Clock asks Cancel first about a running stopwatch. Swell pauses while it asks. A
second press of the close button after Cancel is answered either way the desktop has it: desk's fix round sends
`WIN_EV_CLOSE` again, and the game asks again; 2c5afd30 offers its own "End the program?" instead.

**Key repeats.** Desk's keys say when a key is held (`WIN_MOD_REPEAT`), and kit's notes ask programs to test the bit.
The arrows repeat in all three (and Down in Swell, a soft drop by holding it); no other key does, so a held F never takes
a flag back, a held Space never deals twice or drops piece after piece, a held Up never spins a piece. The brief's "play
by repeat" for Swell is Left, Right and Down held.

**The press comes from the toolkit.** The toolkit tells an area "the pointer left" with the buttons as they are, also a
press made elsewhere (a click on the toolbar); a model that worked the press out from its own memory of the buttons then
missed the next press in the board (the window test found it). So the models take the buttons pressed now from the
event (`ev->value`), and a release is a button held at the last event and not now.

**A press the board never saw let go is forgotten.** When the window loses the focus with a button held (another
window, a desktop dialog, the toolkit's question over it), the toolkit drops the button and the board hears no release:
the board's losing the keys (`GK_EV_FOCUS` 0) cancels the press, so Mines reveals nothing and Solitaire's dragged cards
go back to their pile, and an event that shows the button up without a release seen does the same. A drag starts only
from a press seen on the board itself, whatever the desktop sends (the integrator asked for that ahead of desk's fix
round, which stops sending a held button without its press after a click on a desktop dialog).

**Present only what changed.** Each window keeps what it shows: Mines and Swell each cell's look (Swell's ghost carries
the piece's kind, so a new piece's ghost is painted again in its colour), Solitaire a hash of each pile's cards, fan,
keyboard's card and dragged cards, and where the dragged cards were; after every event only what differs is marked
(`gk_damage()`), and the toolkit presents those rectangles. The window tests check it: a flag presents its cell and the
count, the clock its label, a moved piece a few cells, a dragged card less than half the window.

**No call to resize a window from the program.** The toolkit and libgui have none (WIN_SETSIZE is only the answer to the
user's corner), so the games do not change their window's size with the field. Each opens at the size its largest
board wants at the desktop's room (Mines: the large field with 24-pixel cells, 692 x 436; Solitaire: 72-pixel cards,
600 x 436; Swell: 22-pixel cells, 470 x 438), is resizable, and lays its board out in whatever room it has: a small
field gets larger cells (up to 40 pixels), a short window smaller cards (down to 40 pixels wide) and closer fans. On the
smallest desktop (640 x 400, content 638 x 317) the large field still fits with 16-pixel cells. A program-initiated
resize would be a toolkit call (kit's to add); the games would not need it.

**Status texts are short.** The status bar cuts its text where the right part begins, and the smallest desktop is 640
pixels wide, so every line is at most about fifty characters, and the session's record at the right ("Won 1 of 3, best
0:41") about twenty.

**Mines' first reveal clears its neighbours too, when there is room,** so it opens an area rather than a lone number,
which is how the genre is usually played; a field too crowded keeps only the cell clear. The timer starts at the first
reveal, not at a flag placed before it.

**Mines' press can be taken back** by moving off the field before letting go; meanwhile the cell under the pointer shows
pressed, and over an open number the covered cells around it show the chord about to be made. Left and right together
are not a chord (the right button flags at its press); the middle button and a click on a number chord.

**Solitaire's finish is played out on a copy first.** Once no tableau card is face down, a copy of the game is played
out by the finish's own rule (the lowest card that can go home goes; with none the stock deals; an empty stock takes the
waste back unless a whole pass went by without a card going home), and the game finishes by itself only when the copy
wins. Drawing one it always does (of the cards not home, the lowest is on top of its pile, or in the stock or the waste,
which a pass shows); drawing three a card can stay hidden on every pass, and the player plays on. Undo counts as a move
and is refused after a win and during the finish.

**Swell's rest, its room above the sea, and its pauses.** A landed piece sets 450 ms after it landed unless a move or a
turn lets it fall again; landing again on the row it rested on keeps the time it had there, so sliding over a gap
cannot keep a piece up. The falling piece may reach above the top row (a flat piece that just came in could not turn
otherwise); setting a cell there ends the game. Losing the focus pauses (the keys go elsewhere), and the game waits for
P, Enter or Space: a click on the sea starts a game that waits or a new one after the end, but never ends a pause, so a
click that brings the window back lets nothing fall. N in the middle of a game does nothing but say how (P, then N),
so a stray N loses no game; Game > New game is deliberate and starts one at once.

**Status 2 for trouble.** `wmines` and the others exit 0 when their window closed and 2 for a word given or no window
(the brief's 0, 1, 2; wkit says 1 for no window).

## How Swell differs from other falling-blocks games

- **The pieces.** No piece has four cells. Seven of SurfOS's own, named for the sea: the pebble (two cells side by side),
  the drift (three in a line), the shell (a corner of three), the crest (a square with a cell under it), the reef (a
  cup), the gull (a wide corner of five) and the kelp (a bar on a stem). Their average is four cells, so a row of 12
  takes about three.
- **The sea.** 12 columns by 18 rows.
- **The swell.** From level 3 on, every 16 pieces the whole sea rises a row and a row of sand with two gaps comes in
  under it: the game's name, and pressure that grows with the level.
- **The draw.** A shuffled set holding each piece twice.
- **The turns.** A turn keeps the piece's box centred, then tries one column left, one right, a row up, two left, two
  right, in that order.
- **The score.** Rows washed at once score the level times 10, 25 or 50 (no piece is taller than three), an empty sea
  200 times the level more; Down is a point a row and Space two. The level rises every 8 rows; a row falls every 800 ms
  at level 1, down to 100 ms at level 16.
- **The look**, above: SurfOS's sea colours and blocks, a band across the sea, no other game's colours or frame.

A careful player (the host test's, which weighs washed rows, holes, height and a ragged top) lasts about 330 pieces and
110 rows over 200 games, every one of them into the swell: the game is playable, and the swell matters.

## What was left out, and why

- **Saved scores.** The brief: "Scores last for the session". Nothing is written.
- **Resizing the window to the field.** No call for it in the toolkit (above).
- **Mines' question marks and custom fields; Solitaire's points tables and a limit on passes; Swell's hold and more
  than one piece previewed.** Not in the brief.
- **Solitaire's dragging by the keyboard.** The keyboard chooses a card and Enter sends it where it can go; a card to a
  pile of the player's choice is the mouse's.

## The tests and their counts

Natively on the Mac (Apple clang 16; the models arm64, the windows x86_64 under Rosetta, as the toolkit's host build
takes in blibc's x86 code), under AddressSanitizer and UBSan (`tests/games-sanitize.sh`), with `-Wall -Wextra`:

| Test | Checks | What |
|---|---|---|
| `test_wmines` | 393 | the generator; fields refused; the first reveal on 6,000 seeded fields and crowded ones; hand-laid fields: areas, a serpentine area through the largest field, flags, chords, a wrong flag, the win, the loss, the timer past 2^32 ms; 3,000 random games; the model: sizes at 958 x 457 and 638 x 317, the board's layout, presses taken back and made elsewhere, pressed cells, buttons, the keyboard and its repeats, closing, the session |
| `test_wsolitaire` | 179 | the deal and its evenness over 52,000 deals; the stock one and three at a time and taken back; every move and refusal; Undo of every kind, back through 400 random moves, the ring's limit; where a click sends a card; finishing by itself, and not when drawing three hides the ace; 400 greedy and random games (59 won, each finished by itself); the layout at four sizes, a 19-card pile kept in the board; clicks, drags let go over a pile, by the card's middle, or back, a press made elsewhere; the keyboard; the finish a card a tick; closing; the session |
| `test_wswell` | 150 | the pieces and their turns; the set; falling on time; moves, turns, kicks, a tunnel, the room above the sea; resting, and the rest a gap cannot stretch; Down and Space; rows washed, two apart, three at once, the empty sea's bonus; levels; the swell and its end; the end two ways; the pause; 200 games, half random, half careful (every careful one into the swell); the model: sizes, cells, the ghost, the keys in every state and their repeats, focus, the washed rows' flash, closing, a restart, the session |
| `test_wmines_window` | 57 | no desktop; the window's size; toolbar, board and status bar drawn; a click revealing an area, the clock and its presents; a flag and what it presents; the cursor with and without the keys; Esc and the close button asking, Cancel first; the size by the radio buttons, the menu and the keys; a game won and lost by clicks; Help; a new size and theme; the smallest desktop |
| `test_wsolitaire_window` | 58 | no desktop; the window; backs, faces and places drawn; the stock dealing; Undo by the toolbar, Ctrl+Z, greyed; a drag onto a pile and what it presents; a drag back; drawing three, fanned; the keyboard's frame with and without the keys; repeats; Esc asking; a game finishing by itself to the win, and the record; Help; the close button; the smallest desktop |
| `test_wswell_window` | 44 | no desktop; the window; the band and the panel; Enter, the piece and its ghost; arrows pressed and held, what they present; the timer; Space and a held Space; a washed row's foam; P and the menu's Pause; focus lost, and a click that does not resume; Esc asking; the end and the best; the close button; New game and Help; the smallest desktop |

The same six tests run in the Docker image under `make unittest` (gcc, -m32, -O1), and the models compile for i386
against SurfOS's own headers without a warning.

**The gates, on the tree committed** (the branch at this account's commit, desk's 2c5afd30 and kit's 2ba18949 merged in;
kit's eab009d3 is not needed here: every game's loop waits with -1, and the falls are the toolkit's timers):

- `make unittest` in the Docker image: passed, the six tests of this track among it (393, 179, 150, 57, 58 and 44
  checks), with desk's `test_guidesk` reading the three `.app` files (145 checks) and `test_man` finding their pages;
  gcc warns about nothing of this track's (a `-Wmaybe-uninitialized` at -O2 in Mines' pointer code, a false one, was
  answered by initializing the two variables it named).
- `make test-all` through the suite lock: passed at -O0 and -O2, 558 smoke checks and 51 Files checks each.
- The guiapps scenario through the suite lock, every track's checks, at -O0 and -O2: 360 of 362 checks each. All 30 of
  this track's pass at both levels. The two that fail are not this track's: kit's "Ctrl+O, a menu's shortcut, asks for a
  chooser" (with desk's implementation merged, the desktop's chooser names the window it is for, "Toolkit: Open a
  file", and kit's check looks for "Open a file" where the title starts), and then the scenario's own "the desktop
  closes and the text console comes back", which that chooser, left open, stops (so the kernel heap's check after it
  did not run). `tools/gui_apps_kit.py` is kit's to change, and this account's report says so to the integrator. A
  run of every track but kit, to show the end checks with the games in, was queued and withdrawn when the queue grew
  to more than an hour: the games-only run below covers them.
- Before those, the games' checks alone (`GUIAPPS_ONLY=gui_apps_games`) at -O0, not under the lock: 36 of 36, the desktop
  closing and the text console back, the kernel heap back to the byte (668512 bytes in use before gui and after),
  `fsck_msdos` and `fsck.fat` clean on the stick. That tree differed from the one committed only by that
  initialization.

None of this has run on the laptop: the games are emulator-tested only until the owner tries them.

## Where the code comes from

Written for SurfOS from these, and from this tree's own programs (`user/wclock/`, `user/wcharmap/`, `user/wkit/`,
`shell/gui*.c`) for the shape and the look:

- **Random numbers:** George Marsaglia, "Xorshift RNGs", Journal of Statistical Software 8(14), 2003 (the 32-bit
  generator with shifts 13, 17 and 5). Drawing below a bound by rejecting the values past the last whole multiple is
  the standard method.
- **Shuffling:** Richard Durstenfeld, Algorithm 235 "Random permutation", Communications of the ACM 7(7), 1964, as Knuth
  gives it (The Art of Computer Programming, vol. 2, section 3.4.2, algorithm P).
- **Opening an area without recursion:** a breadth-first visit through a queue, the textbook method.
- **Drawing:** filled circles by spans from an integer square root, triangles by rows, a hash of what a pile shows by
  FNV-1a (Fowler, Noll and Vo's published function); the rest is rectangles, lines and the console's glyphs through
  libgui.
- **Behaviour matched, from playing the games, not from any program's source:**
  - Mines follows the minesweeping genre as Microsoft's Minesweeper and GNOME Mines play it: the numbers, flags,
    chording on a number whose flags are all placed (with the middle button, or a click on the number, as such games
    commonly allow), the first reveal always safe. Its field sizes, its look and its name are SurfOS's own.
  - Solitaire is Klondike as it is commonly played, Microsoft's Solitaire among others: the deal, drawing one or
    three, passes through the stock without limit, a click sending a card where it can go, finishing by itself once
    the cards are up. Its cards, backs, table and icon are drawn by SurfOS's code.
  - Swell is of the falling-blocks genre that Alexey Pajitnov's Tetris (1984) began: pieces falling a row at a time,
    full rows cleared, levels. A rest before a landed piece sets, an outline of where it will land and pieces drawn
    from a shuffled set are common in the genre's later games. Its pieces, sea, swell, turns, scoring, timing, colours
    and name are SurfOS's own (above).

No other program's source was used or recalled.
