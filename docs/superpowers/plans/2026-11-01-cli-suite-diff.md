# The command-line suite, track diff: the account

Branch `cli/diff`. Built to `2026-11-01-cli-suite.md`. On the laptop none of this has run: emulation and the host only.

## What was built

| Tool | Where | Notes |
|---|---|---|
| `diff` | `user/bin/diff.c`, `user/diff/{dinput,dcore,dfile,dout}.c` | normal, `-u [n]`/`-U n`, `-c`/`-C n`, `-q -r -i -b -w -B` |
| `patch` | `user/bin/patch.c`, `user/diff/{dinput,phunk,papply}.c` | unified, context, normal; `-p -R -i -o -d -N -f --dry-run` and the long names |
| `cmp` `comm` `join` `paste` `fold` `nl` `rev` `basename` `dirname` `od` `base64` `strings` `file` `tree` `cal` | `user/bin/<name>.c` | shared helpers in `user/diff/cli.h`; `cal` links `user/wclock/clock.c` for its arithmetic |

A page for each in `rootfs/man/`. One smoke block (`echo cli-diff`). Tests: `tests/host/test_diff.c`, `tests/test_cli_diff.py`,
`tests/host/cli/` (host shim, `mac-sanitize.sh`, and `measure_dcore.c`, which measured the capped search).

## Decisions a reviewer would ask about

**diff does not hold the files.** diff.h describes it. Common start and end go by block-wise byte compare; each line between
costs a 32-bit hash and a flag byte; a final pass compares every pair called equal, byte by byte, and a pair the hash fooled
becomes a change. So a hash collision can cost only minimality. Without `-i -b -w -B` the output is a correct edit: `patch` gives
the second file back. With them it is only as exact as they ask, and `-B` leaves changes out, so there every hunk's numbers and
lines are checked on their own (below). Unix's first diff checked its "jackpots" the same way. `test_diff_weak` builds `dfile.c`
with hashes of 2 bits so that this path really runs.
Output rereads the files, forward only. Standard input and pipes are kept in memory: they cannot be read twice.

**The search is bounded three ways.** Memory: two arrays of `2 * cap + 3` ints, whatever the input size. Depth: the parts of
cut boxes wait on a stack of 64, the smallest part of each cut done next, so the box in hand at least halves for every two that
wait. Time: `SEARCH_CAP`, 4000 edits from each corner of a box; past it two rules of my own choose the cut (below, "The capped
search"), and the edit is then correct, not always the shortest. Lines whose hash the other file lacks drop out first, through a
table of bits: that is what makes two unrelated files quick. The algorithm is Myers' 1986 paper, section 4b.

**Sliding.** A change that only deletes or only inserts is moved as far down as it goes, so repeated lines give the same hunks
each time. A change that does both stays. Unlike GNU's this makes no second pass upward, so `diff` matches GNU's text in
584 of 924 generated cases; the rest are other edits of the same length (`--minimal` is compared on all of them).

**A bug the property test found:** sliding could run over the context lines kept from the trimmed end, leaving a hunk in the
middle of a file with no trailing context, which `patch` then pinned to the end of the file. Those lines now stay out of the
comparison (`struct dscript.tail`).

**lseek stops at 2 GB.** `dsrc_read` reads up to a place past it (`go_to`). The common end is skipped for a file past 2 GB, as
that is a seek a block. Larger files work, slower; this has not been run on a file that size, only reasoned about.

**patch holds 8 bytes a line of the target and one hunk.** A hunk is matched on hashes and confirmed byte by byte, and the unchanged
parts are copied in blocks. Hunks are applied in order and never go back over the last one, as GNU's do.

**Fuzz is stricter than GNU's in two cases, on purpose.** Fuzz lets up to two context lines at each end of a hunk differ, taken
from the outside in (`window_at()` in `papply.c`). First, as the review let stand: GNU will drop all of a hunk's context and put it
where it says with nothing compared (with `-U 1` and fuzz 1); here something must always be compared. Second, since the third round:
a hunk with less context at one end than at the other was made at that edge of the file, and here that end stays closed at every
fuzz. The hunk stands at the edge and that end is compared whole; only the other end loosens. GNU can fuzz such a hunk away from
the edge. `test_patch_against_real` compares placement, status, messages and the `.orig` with GNU patch 2.7.6's wherever the same
hunks went in, and elsewhere requires that ours refused more, never less: in 46 of its runs, with 3-line context, ours
refused a hunk GNU's placed. It does not compare with `-U 1`, for the first case.

**patch never asks.** GNU asks whether a reversed patch should be applied in reverse, and without a terminal skips it, writes the
`.rej` and exits 1. That is what happens here, always, so `-N` changes nothing and is accepted for scripts. `-f` skips the look.
The first hunk is tried forward and then reversed at each fuzz, so that a patch applied twice is seen as that, and does not land
with fuzz in a like block further down. A hunk `-R`ed goes into the `.rej` as it was tried. The `.rej` is always unified, also for
context and normal input. It adds up over a run, and one that was there before stays: the hunks go to `<file>.1.rej`, and so on,
and that is said.

**`.orig`.** Decided in the review, against my first version. When a hunk needed an offset or fuzz, or failed beside others that went in,
the old file is kept as `<file>.orig`, as GNU does. It is made with `O_EXCL`, so one that is there stays, and that is said; a copy
that cannot be made stops the file from being replaced. GNU also keeps one when no hunk went in at all, for a file it did not
change: here that makes none.

**`-o` takes its file's place only if all of the patch went in and there is something to write.** It opens at the first hunk that goes in,
and otherwise stays as it was; a new one is not made. So a patch that empties a file makes no `-o` file. `-o -` cannot take back what it
has written, and gets what there is.

**A malformed hunk stops the run** with status 2 and drops what was under way on that file, since a damaged patch may not be
trusted further. Files finished before it stay finished: GNU's behaviour too. A hunk line with no newline at the end of the input
is malformed: the patch was cut, and what is there is part of a line. The `\ No newline` marker is the one exception. Lines past a
hunk's counts that look like more of it are passed over and said; a mail's `-- ` and the next section's headers are not such lines.

**A hunk that may not be trusted with a no-newline.** A `+` line marked as ending the file, with file lines still after it, gets its
newline rather than joining the next line. A line added after a last line with no newline starts a new line.

**Names.** `safe_name()` runs after `-p`, on the name that will be used, so `-p2` on `../../x` gives `x`. A name on the command line
is the user's and is not checked. Without `-p` only the last part is used, as GNU does. `/dev/null` is recognized before
stripping. A name with no TAB ends at a real date (`2026-11-01`, or a day and month name), so names with spaces work. A CR at its end
is dropped. What is taken from a patch is cleaned before it is printed; `cli_clean()` does the same for `file`, `tree` and `diff`.

**`diff -r` pairs names without regard to case,** as FAT does. Two names differing only in case on a case-keeping file system pair up
in order. `-u 5` takes a number only while two operands still follow it.

**`nl -b p`** needs a regular expression, which is the text track's. `nl.c` uses `__has_include(<regex.h>)`: on the host it uses the host's and
is compared with GNU's; on SurfOS it says the build has none until the text track is merged, and then works with no edit. Confirmed:
on a scratch merge of `cli/text` at 7ad6259, built at O2 and run in the guest, `nl -b pa` and `nl -b 'p^[bf]'` numbered the right lines over
libsurf's regex, and `nl -b 'p['` said so with status 2. `nl.c` was not changed for it. That merge conflicts in `Makefile` and
`tools/qemu-run.py`, at the anchors both tracks insert at, and taking both sides in the smoke table needs care: the two blocks share their
last lines, so a plain union does not parse.

**`getopt`** is the text track's too, so the tools have option loops of their own, as the existing ones do.

**`file` reads the first 1 MB** for text, as the real one does. So does the man page.

**`strings` keeps valid UTF-8** (the rule that a character is a sequence), and so differs from GNU's on text with high bytes.
It is compared with GNU's on ASCII only.

**`fold -b`** does not split a sequence, so a line can come out shorter than the width, or wider when one sequence is wider than it.

**`cal` is proleptic Gregorian,** as `clock.c`'s month is: no gap in September 1752. The comparison with a real `cal` starts at 1753.

**Host builds.** `tests/host/cli/cli_host.h` is force-included to compile each tool as it is against the host's libc: it gives SurfOS's `struct
stat` fields their POSIX names, gives `open` a mode, and adds the failure injection (`CLI_FAIL=op:n[:errno]`) that pulls `write`, `fsync`
and `rename`. So the sources tested are the ones shipped, but over another libc: the smoke block is what runs them over libsurf.

## The capped search

The paper says how to find the shortest edit; it does not say where to cut a box when the search is stopped. The rules in `dcore.c`
were chosen by measuring, with `tests/host/cli/measure_dcore.c`, on files of seven shapes: the numbers 1 to n against their order
as text (`seq n | sort`, "sorted"); a file with its blocks of 1 to 100 lines shuffled ("blocks"); lines of 800 kinds, 40% of them
changed, deleted or added ("edited"); two files of the same 300 lines in other orders ("unrelated"); unique lines shuffled
("permuted"); and two kinds of drifting file, where stretches of about 1,500 lines are rewritten and lose half their lines, so
that the two files drift apart as they go, the new lines being lines of their own ("drifting") or copies of others ("copies").
Each figure is how much longer the edit is than the shortest. For the drifting files the edit they were made by stands in for
the shortest: for "drifting" it is the shortest, as nothing of a stretch is left to match; for "copies" it may be longer, so a
figure there can be below zero.

**The rules.** When the waves from the two corners share diagonals, the first rule cuts at both points of the one where they stopped
nearest each other, of those on which each wave's point has at least a part of its wave's best progress; otherwise each wave's
point nearest its course, the line between the corners, valued at four times its progress less how far it has strayed. The part:

| First rule | sorted, 20,000 lines | sorted, 100,000, at caps 4096, 5000, 6000 | drifting and copies, 60,000 lines: excess summed over 36 runs |
|---|---|---|---|
| none: the course alone | +98% | | |
| only where the waves stopped within an eighth of the cap | +98% | +36%, +22%, +27% | 56.7 |
| 3/4 | +19% | 0, 0, 0 | 108.5 |
| **4/5** | +19% | 0, 0, 0 | 96.3 |
| 5/6 | +19% | 0, 0, 0 | 86.7 |
| 7/8 | +19% | +2.3%, +6.5%, +11.1% | 72.8 |
| 9/10 | +19% | +36%, +22%, +27% | 67.1 |

The larger the part, the less often the first rule cuts, the better the drifting files do, and the more the sorted ones lose.
Four fifths is in the middle of the range that keeps the sorted files. A first version of the rule took a diagonal when either of
its two points had half the progress of the further wave: drifting files then came out at up to +34%, and copies at up to +58%. On
one of those files it cut at the far edges of both waves, where neither had matched anything.

**The cap.** With those rules, two files of each 20,000-line shape and six drifting pairs of 60,000 lines; times on the Mac, with
five such measurements running side by side, so that only their ratios count:

| Cap | 1000 | 2000 | 3000 | 4000 | 5000 | 6000 | 8000 |
|---|---|---|---|---|---|---|---|
| blocks | +5.7% | +3.6% | +3.1% | +1.3% | +1.8% | +1.7% | +1.1% |
| edited | +1.3% | +0.6% | +0.2% | +0.5% | +0.3% | 0.0% | 0.0% |
| unrelated, permuted | +0.5% | +0.3% | +0.2% | +0.2% | +0.1% | +0.2% | +0.1% |
| sorted | +19.2% | +19% | +19% | +19% | +19% | +19% | +19% |
| drifting and copies, 60,000 lines, mean of 12 | +14% | +4.7% | +3.7% | +2.7% | +3.0% | +3.3% | +1.8% |
| time for the five 20,000-line shapes | 0.30 s | 0.61 s | 0.92 s | 1.21 s | 1.46 s | 1.89 s | 2.23 s |

At 4000 every shape is within half a point of the best that any cap up to 6000 gives. A larger cap costs time in proportion, and
only 8000, at twice the time, did better, by 0.2 to 0.9 points. So `SEARCH_CAP` is 4000.

**Against the search it replaced** (it cut at the furthest point either wave reached, with a cap of 4096 that rose with the
square root of the lines past 16.7 million in the two files): times are the best of three on the Mac, with nothing else of mine running.

| | Before, cap 4096 | Now, cap 4000 |
|---|---|---|
| sorted, 20,000 / 100,000 / 200,000 lines | +10% / 0 / +91% | +19% / 0 / +19% |
| blocks, two files | +1.3%, +0.8% | +1.6%, +1.0% |
| edited, two files | 0, 0 | +0.7%, +0.2% |
| unrelated, permuted | +0.1% at most | +0.2% at most |
| drifting, 20,000 lines, ten files: worst, mean | +21.6%, +3.4% | +12.9%, +2.8% |
| copies, 20,000 lines, ten files: worst, mean | +1.9%, -0.2% | +11.6%, +2.5% |
| drifting, 60,000 lines, six files: worst, mean | +132%, +51% | +9.7%, +4.7% |
| copies, 60,000 lines, six files: worst, mean | +4.4%, +1.6% | +2.1%, +0.6% |
| drifting, 200,000 lines, three files | +143%, +74%, +102% | +4.0%, +6.5%, +3.3% |
| copies, 200,000 lines, three files | +4.3%, +0.6%, +1.0% | +6.6%, +2.1%, +2.4% |
| time: sorted / the other four 20,000-line shapes | 80 ms / 823 ms | 125 ms / 870 ms |

So the worst of everything measured went from +143% to +19%. What is worse: the sorted file of 20,000 lines, by 9 points; copies
at 20,000 and 200,000 lines; moved blocks and edits, by less than a point; and time, by 6%, and by 55% on the sorted file. In
`test_diff`'s drifting pair, cutting at the furthest point kept 415 of the 33,928 lines at a cap of 512 and 2,965 at 2048; these
rules keep 29,500, 32,382 and 33,197 at 512, 1024 and 2048.

Where no box reaches the cap the edit is the shortest, as before; which of several shortest edits comes out can differ, which is why
the number of cases with GNU's exact text changed. Memory is still two arrays of `2 * cap + 3` ints. Time is still proportional to the
cap times the lines: `dcore.c` gives the reason, that every cut takes out of the box a fixed part of what its search cost.

## Sanitizers

By the coordinator's instruction no sanitized binary runs in the Docker image: 32-bit ASan hangs there under qemu-i386, and one such hang of
mine is why. `tests/host/cli/mac-sanitize.sh` builds everything with the Mac's clang under ASan and UBSan and runs both C tests and the
Python test. It is outside `make unittest`, so `make unittest` itself runs unsanitized. It runs at the Mac's 64-bit ABI: 32-bit `long`
arithmetic is covered only by the unsanitized `test_diff` at `-m32` in the image.

## Left out

- `od -t f`: the digits other systems print are their own; refused with a reason.
- `nl`'s logical pages (`\:` lines) and `-h -f -d -n -p`: not in the brief.
- `diff -a -N -p -y -e`, `patch -b -F -E -l`, `cmp`'s skip operands: not in the brief.
- Two hunks of `diff -B` that GNU prints sharing context lines are one hunk here: see the review, item 2.
- `comm` and `join` do not check the sort order. Out-of-order input gives what a merge gives.
- `patch --dry-run` on a patch naming one file twice sees the file unchanged the second time, as nothing was written, so it can differ from
  the real run. The page says so.
- Empty directories that `patch` made for a file it then failed to write stay; a removal leaves empty parents.
- Added beyond the list: `join -e`, `od -v` and the `z` of `-t`, `-U`/`-C`: each a few lines, and each needed to make the listed ones usable.

## Tests

| Where | What | Result |
|---|---|---|
| `make unittest`, in the image | `test_diff`, at `-m32` (SurfOS's 32-bit `long`), 1,500 rounds | 53,285 checks, 0 failures |
| | `test_diff_weak`, hashes cut to 2 bits, same rounds | 52,770 checks, 0 failures |
| | `test_cli_diff.py`, the programs beside GNU's | 12,868 checks, 0 failures |
| `mac-sanitize.sh`, on the Mac, ASan + UBSan, clang | `test_diff`, 6,000 rounds | 139,055 checks, 0 failures |
| | `test_diff_weak`, 6,000 rounds | 137,106 checks, 0 failures |
| | `test_cli_diff.py`, the programs beside BSD's | 11,101 checks, 0 failures |
| smoke | the `echo cli-diff` block, O2 and through `make test-all` at O0 and O2 | 20 steps |

`test_diff`: the search against a table of longest common subsequences (edits as short as possible, for random pairs and for edits
of a file); a capped search still correct, and ending, on 200,000 lines with nothing in common, and keeping four fifths of what
the shortest keeps on a long drifting pair; `diff` then `patch` gives the second file back, forward and with `-R`, in all three
forms, at 0 to 4 lines of context, with and without last newlines, empty files, and lines that look like headers; sizes around the
32 KB block; `-i -b -w -B`; hunks moved, fuzzed, at the ends of the file and kept there at every fuzz, in order; patches written
by hand; lies in the numbers; thousands of damaged patches (bytes flipped, ends cut, lines gone); and, added by the
review, every hunk of thousands of outputs read by its own numbers against both files by a parser that shares no code with `dout.c` or
`phunk.c`, with what lies between hunks compared too. A round trip cannot see a wrong hunk under `-B`; this can.

`test_cli_diff.py`: every small tool against the real one on a corpus; our `diff` through the real `patch`, the real `diff` through our
`patch`, and `changed lines` against `diff --minimal`; `diff -r` text; `patch` against GNU patch 2.7.6 on files that moved on, comparing
the file, the status and the messages; every option and refusal of `patch` on real files; `patch` pulled at each write, fsync and
rename (`CLI_FAIL`), checking that the old file stays whole and nothing is left beside it, and that a rename that may be half done
deletes nothing; a real FAT12, FAT16 and FAT32 volume from `mkfs.fat` through `file`; junk through every tool.

The block's depth is small on purpose. What it adds is libsurf, i486 code and real FAT with its rename.

In a 256 MB guest under QEMU (ad hoc, not in the suite), measured with the search that was replaced: `diff` of a 200,000-line pair
0.9 s, `patch` of it 0.4 s, two 200,000-line files with nothing in common 5.5 s, 20,000 lines in another order 2.3 s. With this
search, at O2: `seq 20000` against its `sort`, 3.0 s by surfsh's `time`, twice, with the Mac under a load of about 8.

**Skipped, each saying so when it runs.** In the image: `file`, `tree` and `cal` are not installed. `tree` is on neither system, so its
expected output is by hand; `file` and `cal` are compared on the Mac only. On the Mac: `paste`, `fold`, `nl`, `od`, `strings` and exact `base64`
widths are compared only with GNU's, as BSD's differ on purpose; `diff`'s exact text likewise. The Mac's `patch` is left out for context
diffs: it fails or, worse, gives a wrong result with status 0 on the Mac's own `diff -c` (a last newline missing, an old side left out).

**Not tested at all.** A file past 2 GB (`go_to()`, the common end's refusal). Running out of memory for real in the guest. `nl -b p` over
libsurf (see above). The laptop. LeakSanitizer is off in the Mac run: macOS's clang does not have it. The sanitizers run at the Mac's
64-bit ABI only, so a 32-bit overflow would be seen by `test_diff` at `-m32` only if it changes a result.

## make test-all

This round's `make test-all`, O0 and O2, and `make unittest` ran on the tree of the last commit on the branch, which holds this
file; the result is in that commit's message, as this file cannot hold the result of a run on itself.

The runs of the earlier rounds were on trees this history no longer has (below, "What came from where"). Their failures, none in
this track's blocks:

The first `make test-all` failed 1 of 540 checks at O0, so O2 did not run: `httpd: six places`, its eight clients, `client 4, /wave.svg:
RemoteDisconnected`. Nothing of this track is in that check, and it was not changed. The same sub-check failed the same way in the gates
`hand-1`, `daily-3` (both 10/31) and `wt-m3-1` (11/01), none of which has these tools, while 22 others passed it; `CLAUDE.md`
describes QEMU's user network resetting connections made close together. Rerun alone at O0, `PART_PRE=dhcp`, on the Mac's QEMU, it passed
6 of 6. (Without `dhcp` first it fails every time, with no network: a mistake in the rerun, not a result.) That is why it reads as load, not
proof: the rerun is on the Mac's QEMU, not the image's. `test-all` was run again.

The review round's first `make test-all` failed 2 of 540 at O0, again not here: `@boot settingsdisk=shutdown`, and `@shutdown-disk` after it, which only
says its file is missing. In that boot the kernel found no disk (`ata(absent)`), so `/hda1` was not there, and the two boots after it, of the same image,
were fine. It is the first boot of a fresh `settingsdisk=` image. The Debian gate `ci-debian-2`, without these tools, shows the same `*0 disks` in a
`settingsdisk=hidden` boot, which there failed every time, so that may be another cause. This branch changes nothing in the kernel, and the probe runs
before anything of this track does. Alone at O0 on the Mac's QEMU, under load 14 to 17, it passed 6 of 6. Why the disk went missing is not explained: it is left open,
not called load.

The next run passed all 545 checks at O0 and failed 1 of 540 at O2: `selftest`, step 242 of 534, before any step of this track. It failed in `hand-1` too, without these
tools. The log keeps no transcript of it, so which line it missed is not known. Alone at O2 it passed 6 of 6. O2 was then run alone through the lock, on the same tree
(`a2af2956`): 545 checks, all passed, and `make unittest` after it. So O0 and O2 passed on one tree, but in two runs, not in one `make test-all`.

## The review of 114ac02

114ac02 was the branch's tip when it was reviewed; the history was written again since, so it is no longer on the branch.

Each fix has a test that failed on the old code first: on the old `user/diff` 747 failures in `test_diff`, on the old programs 38 in
`test_cli_diff.py`. The new strict checker gives 710 on the old `dout.c` and none on the new one.

| # | Found | Fixed |
|---|---|---|
| 1 | `-o` emptied or replaced the user's file on a failing hunk, an empty patch, a missing target, a reversed patch; `-o g g` erased the input | opens at the first hunk that goes in; takes its place only with status 0 and bytes to write |
| 2 | `diff -B` with `-u` or `-c` printed hunks that did not match their numbers: a group could start inside the last one's context, and the readers only go forward | two groups shown whose context would meet are one hunk. GNU prints them apart, sharing lines; this cannot go back, and one hunk is as true. So its text differs from GNU's in those cases |
| 3 | a second `join -o` overflowed the heap | appends |
| 4 | a patch cut in the middle of a line applied the fragment | such a line is malformed; `had_newline` is now read |
| 5 | reversed was tried only after every fuzz forward, so a patch applied twice landed in another block | forward, then reversed, at each fuzz |
| 6 | a second failing section lost the first `.rej`, and any `*.rej` could be replaced | adds up within a run; never replaces one that was there |
| 7 | `patch foo create.diff` prepended to a file with text in it | refused, as by name |
| 8 | `Not deleting` gave status 0 | status 1 |
| 9 | any `Xxx ` counted as a date, and cut a name there | real day and month names, and real dates |
| nits | `od` suffixes wrapped; `nl` number and width overflowed; a FAT label and names from a patch printed raw; lines past a hunk's counts dropped silently; `*** 0,4294967295 ****` wrapped to no lines, and the normal form's like it; a CR stayed in a name | each refused, cleaned or said |

**Settled: a patch naming its own `-i` file.** On real FAT `fat_rename()` refuses to replace a file with a descriptor open (`EBUSY`, "in use: not deleted
under its user"), and saveutil counts `EBUSY` as nothing changed. Run in the guest on FAT, by `-i` and by redirection: `Device or resource busy`, status 2,
the file as it was, no `.tmp`. It is a smoke step. On the host's file systems the rename goes through and the descriptor reads on: also harmless.

## What came from where

The review asked: did I write any function or table following a specific existing program's source from memory? **Yes,** I
answered: no source file of any other program was open, copied or pasted, as I had none to hand, but I know GNU diffutils, patch
and coreutils well enough to recall passages of them, I wrote several out from memory in my working notes while designing this
track, and I wrote code with that in mind. The closest were the first five rows below. In the third round each of them was written
again, from a specification or from what the behaviour requires, in a shape of my own; the history of `cli/diff` was then written
again so that the recalled forms are in none of its commits, each rewrite standing in the commit where the recalled form first came.

| Was, recalled | Followed, from memory | Now | From |
|---|---|---|---|
| `papply.c`: `fuzz_ends()`, and the anchors in `at_fuzz()` that pinned a hunk to an end of the file | GNU patch, `locate_hunk()`: its two formulas term by term | `window_at()`: fuzz takes context from the outside in at an end that is open; an end with less context than the other is closed, so the hunk stays at that edge at every fuzz and that end is compared whole. Stricter than GNU's (above) | what fuzz must mean: the unified and context forms, the page's description |
| `dcore.c`: `best_so_far()`, and a cap of 4096 rising with the square root | GNU diff, `diag()`'s too-expensive branch, and `too_expensive`; 4096 is GNU's number | two rules of my own, and a cap of 4000, fixed ("The capped search") | measurement: `tests/host/cli/measure_dcore.c` |
| `dcore.c`: `split_box()` | its shape, two loops and the test of `odd`, was how GNU's `diag()` reads | `find_cut()` over one `wave_edit()` that both waves use, each in coordinates of its own; the paper's overlap test is one comparison | Myers 1986, section 4b |
| `od.c`: `set_padding()`, `put_row()` | GNU coreutils `od`: the formula that spreads the spaces | `set_line_width()`: the j-th of v values in a line ends at column ⌈w × j / v⌉, w being the width the widest type gives a line | POSIX's description of `od`, and GNU's output observed |
| `fold.c`: `spaced()`, `break_at_blank()` | GNU coreutils `fold_file()`: hold the line, look back for the last blank, write through it, move the rest, count again | a segment held and measured as it grows, the place after its last blank marked on the way; at a break it is written through that place and the rest is measured again | POSIX's description of `fold -s`, and output observed |

Behaviour and interface stay as they were, and so do these rows of the review's table:

| Here | Followed | How |
|---|---|---|
| `dout.c`: the thresholds in `changes_together()`, `ignorable()` | GNU diff, `find_hunk()` and `analyze_hunk()` | The three distances and the blank rule are GNU's, on purpose, for the same hunks under `-B`. The code is mine |
| `dfile.c`: what a last line with no newline equals; `paste.c`: separators held back; `join.c`: reading groups | GNU diff's `find_and_hash_each_line()`, GNU paste's `delbuf`, GNU join | Behaviour and idea, not code |
| The messages of `patch` and `diff` (`Hunk #1 succeeded at`, `Only in`, `Common subdirectories`, ...) | GNU patch and diff | Copied exactly, on purpose: the tests compare them |

`slide()` is not GNU's `shift_boundaries()`, which I had recalled: I wrote a single walk, downward only, so as not to follow it. Nothing here follows the
source of BSD's tools, Plan 9's or busybox's, though their output was compared where they are installed.

From papers and standards: Myers 1986 (the O(ND) search, its linear-space form); Hunt and McIlroy 1976 (hash the lines, then check the matches); FNV-1a; RFC 3629;
RFC 4648; POSIX for `od` and `fold -s`, `od`'s character names, tar's checksum, `cmp`'s and `dirname`'s rules; Microsoft's rule for telling FAT12, FAT16 and
FAT32 by clusters; Sakamoto's weekday, as `clock.c` has it.

**The history.** Written again from `be8094d` with git's plumbing, with no interactive step. The rewrites went into: `diff: an edit between two
files` (`dcore.c`, `dcore.h`, and `measure_dcore.c` beside them); `patch: unified, context and normal diffs` (`window_at()` and the page's
sentence); `Tests of diff, patch and the small utilities` (the drifting pair, the hunks kept at an edge, `test_patch_against_real`'s
rule); `od: the bytes of files` (the columns); `fold: long lines broken` (`-s`). The commits after each carry them on. One message
said "the review of 114ac02", which this history does not hold; it says "the first review". The three commits that held only this
account are gone: the account is the last commit, whose tree is the one tested, byte for byte. Each commit before it was built,
the track's programs for SurfOS at O2 in the toolchain image and the tools and `test_diff` on the Mac, and `test_diff` ran at each
one that has it; `make unittest-diff` ran in the image at the tests' commit and at `patch: -o, a patch applied twice`, the first
with the final `papply.c`. `make test-all` ran on the last tree only. In no commit do `user/` or `tests/` have `fuzz_ends`,
`best_so_far`, `split_box`, `DEFAULT_LIMIT`, `set_padding` or `break_at_blank`.

## One existing line changed

The brief allows pure insertion in `tools/qemu-run.py`; this is the one exception, at the coordinator's instruction. The step `kernel ls
/initrd/bin | /initrd/bin/head -2` expected `cat` and `count` first, and `base64`, `basename`, `cal`, `cmp` and `comm` now sort ahead of them. It
takes the same line as track `cli-lang`, so that the merge stays trivial, and was checked with `part.py` from `@surfsh-corner` to `@surfsh-jobs`.
