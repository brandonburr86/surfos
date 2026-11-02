# The command-line suite, track text

The account of track text of the brief `2026-11-01-cli-suite.md`: stage 1, the review of it, and stage 2 (`grep -E`, `sed`).

## Stage 1: the libsurf additions

### What was built

| File | What |
|---|---|
| `user/libsurf/regex.c`, `user/include/regex.h` | POSIX `regcomp regexec regerror regfree`, basic and extended, leftmost-longest |
| `user/libsurf/strtod.c` | `strtod`, `atof`, the rounding that `sscanf` and `math.c` share |
| `user/libsurf/math.c`, `user/include/math.h` | every function of the brief's list, in portable C |
| `user/libsurf/system.c` | `system`, `popen`, `pclose` |
| `user/libsurf/getopt.c` | `getopt`, `optarg`, `optind`, `opterr`, `optopt` |
| `user/bin/libtest.c`, `user/bin/libm_grid.h` | the same inside SurfOS, in the smoke test, in the block marked `echo cli-text` |
| `rootfs/man/regex.txt`, `rootfs/man/libtest.txt` | the manual: the syntax, which grep, sed and awk will point to, and the test program |
| `tests/host/test_regex.c`, `test_strtod.c`, `test_libm.c`, `test_getopt.c`, `text_names.h` | `make unittest` |

Outside the files the brief lists, by need:

- `user/libsurf/printf.c`: `sscanf`'s floats have to be there. It calls `__strtoflt()` in `strtod.c`, so
  `HOSTUSER_SRC` gained `strtod.c` by a line of its own in the Makefile, inserted, not changed.
- `user/libsurf/stdio.c`, `fflush()`: for a stream that read ahead it sought back and threw the buffer away. Only a file on a
  file system can be read again: from a pipe, a pty, a socket or the console the bytes are gone, and `fflush(NULL)`, which
  `system()` and `popen()` call, would have eaten the input of every open `popen()` stream. Whether the seek fails says
  nothing, as only pipes refuse `lseek`; `fstat` calls all of those regular files, so the test is the file system's name,
  fat or tarfs. For anything else the read-ahead stays, and `setvbuf()` refuses (-1) while it does, since that frees the
  buffer it is in. Files behave as before.
- `user/libsurf/libsurf.h`: the two internal functions. `tests/host/libsurf_names.h`: two lines for `strtod` and `atof`.

### regex

**The two jobs are separate.** Finding the match (`scan()`) is a set of states with, for each, the start of the
match it would make: the earliest wins when paths meet, and it is the leftmost-longest match in time pattern
times text. That is all `grep` and a plain `regexec` pay for.

**Subexpressions are found afterwards, top down.** POSIX's rule cannot be followed by a thread at a branch: which branch
of `(a|ab)` gives the longest depends on what comes after. Priorities on threads (Laurikari's tags, as in TRE) get close,
but a tag keeps only the last iteration of a star, and the rule needs the whole history: Kuklewicz showed where those engines
fail. Okui and Suzuki's algorithm is exact but keeps a matrix of comparisons between threads. The tree walk here is
simpler and follows the rule as it is written: given a node's span, a backward run over the node's states
(`make_table()`) says which states can still reach the end, and a forward run of one child among those states alone
(`fwd()`) finds its longest end. Every thread alive there finishes, so a child reads no further than its own longest
match and the children between them read the span once. A node costs its states times its span, and a node with no
group in it costs nothing. A repetition reports only its last iteration, so only that one's span goes further down.
The table is a bit per state and position of one node, freed before the node's children are walked, so the memory is
that of one node's table, not of a path of them.

**Time.** Finding the match costs at most the program's states, 65,000, per character. The subexpressions cost at most
the sum of the states of the nodes with a group in them, per character. The first account said that sum was bounded by
the nesting limit, which was wrong: each repetition operator is a level too, so it could reach 64 x 17 times the
program, and the review made a pattern of 1145 bytes take 41 s on 100 bytes. That sum now has a limit of its own,
`MAX_WALK`, 260,000 states: regcomp() refuses more with `REG_ESIZE` when groups can be asked for, and only then, so
the cost per character is bounded, whatever the pattern. `(a*)*b`, `(x+x+)+y` and `((((a*)*)*)*)*` on 20,000 bytes
take milliseconds (`test_regex`'s `long_text()`). A bracket's ranges are sorted and merged and searched by halves, so
a bracket costs its logarithm, not its length. `^foo` looks once, at the start: the first version meant to, but its
`anchored` was never true.

**A character** is a whole UTF-8 sequence, and every state moves on the same character, so nothing starts or ends
inside one. A byte in no sequence is a character with a code above Unicode's: `.` and `[^x]` take it, and a pattern's own
such byte matches it. Overlong forms, surrogates and anything above U+10FFFF are not sequences. This is stricter than
`cut -c`'s `char_len()`, which only looks at the bytes after the lead.

**Back-references** cannot be done by an automaton. `bt_try()` goes through the program depth first, on a stack of its
own on the heap, every way through, and keeps the longest. So it is exponential at worst, and counts steps: 20,000,000 and
16 per byte of text, then `REG_ESTEPS`. A step is about the cost of one state, and whatever costs more is charged what it
costs, before it is done: a comparison its bytes / 16, clearing groups one per group, a new start one per slot. The first
version charged a comparison only when it succeeded, and the review made 120 KB take 39 s before the limit came. Now every
run to the limit takes the same order of time. A back-reference matches a character's bytes, and must end where a
character ends: `\(.\)\1` does not match in `\xC3\xC3\xA9`. `scan()` runs first with each back-reference taken as any
text, which throws out most text that cannot match without a step and gives the first start worth trying. With a back-reference the
subexpressions are those of the first path found to the longest match, a star greedy and alternatives in order,
which is not always POSIX's choice: `(a*)*\1` on `aa` reports `(2,2)`, a valid parse but not the longest group. glibc makes no
promise here either. The manual says so.

**Decisions a reviewer would ask about:**

- *GNU's extensions are in*: `\+ \? \|` in basic syntax, `\1` in extended, `\w \W \s \S \< \> \b \B \` \'`. `sed` is compared
  with GNU's, and people write them. POSIX leaves them undefined.
- *`{` that starts no interval is itself in extended syntax*, at the start of an expression too. glibc's `regcomp` refuses
  `{`, `a{` and `a{1`; GNU grep takes them as text, and awk programs say `/{/`.
- *An unmatched `)` in extended syntax is an error.* glibc's `regcomp` takes it as itself; GNU grep and macOS refuse it. A
  typo should be said.
- *`*` with nothing to repeat in extended syntax is `REG_BADRPT`*, as in glibc. `a||b`, `(|a)` and `()` are allowed, as in
  glibc; macOS refuses them.
- *In basic syntax only the first `^` of a part is an anchor*: `^^a` is an anchor and a literal `^`, and `\<^a` a literal `^`,
  as glibc reads them. A `*` after any anchor is itself.
- *There is no locale.* Ranges are by code point. Classes, `\w` and `REG_ICASE` are ASCII: no character above 127 is in a
  class, and É is not é. Folding a few alphabets would be cheap, but half a case-folding table is harder to explain than none.
- *`RE_DUP_MAX` is 32767*, as in glibc. The limit that matters is the program's 65,000 states.
- *Added beyond the brief: `REG_STARTEND`* (glibc's and the BSDs' meaning of the offsets, glibc's of `^`). `sed`'s `s///g` and
  awk's `gsub` need to go on from the middle of a line with what is before it still seen by `^` and `\<`, a line with a NUL in it
  can only be searched this way, and without it each round pays a `strlen()`. *`REG_ESIZE` and `REG_ESTEPS`*, so that the
  messages say what happened.
- *`regexec()` writes into the compiled pattern*: its lists are allocated by `regcomp()`, so a plain match allocates nothing.
  Programs here have no threads. Two calls at once on one `regex_t` would not work.
- `regerror()`'s wording is SurfOS's own.

**Limits:** 65,000 states, 65,000 tree nodes, 64 nested parentheses, 16 repetition operators on one atom, 260,000 states in the
nodes with groups, and emitting at most 16 times the states in tree nodes visited, which stops `(()()()a){32767}`-like
patterns that make no code. `REG_ESIZE` for all of them. A table bigger than 1 GB is `REG_ESPACE`.

### strtod

Integers only. On x87 a double calculation would be rounded twice, and differently at -O0 and -O2. The digits go into a
big integer, and either its top 64 bits or a long division gives the result, with a sticky bit, and `__flt_pack()`
rounds to even. At most 800 digits are kept: a number halfway between two doubles has fewer, and a dropped digit that is not 0
turns into a last digit 1. Numbers beyond 1e310 or below 1e-340 never reach the arithmetic, which is what bounds the big
integers. A short number is a fraction of a microsecond. hex floats, `inf`, `infinity`, `nan`, `nan(chars)` as C has them.

**ERANGE** follows glibc: overflow, and an inexact subnormal. An exact subnormal is no error.

**`sscanf`:** `%f %e %g %a` with `l` and `L`, into a float directly (`__strtoflt(single)`, not a double converted, which would
round twice). `L` gets the nearest double, as `printf`'s `%Lf` prints one. Where the number stops is where `strtod` stops:
`"100ergs"` gives 100 and leaves `ergs`, as the BSDs do. C says it fails and glibc swallows the `e`.

### math

Integers again, and this is the decision to look at. `struct xf` is a software number of 96 bits with an `int`
exponent, and each function takes its argument apart, calculates in it, and rounds once at the end. The reasons:

- x87 keeps doubles in 64-bit registers until they are stored, so what a calculation in doubles gives depends on the
  optimisation level, and double-double arithmetic, which `pow` needs, does not work without `volatile` everywhere.
- SurfCPU has no FPU. Doubles there will be software in any case, so this is the fast way there rather than the slow one.
- The result does not depend on the machine. The fingerprint in `user/bin/libm_grid.h` is the same on arm64 under clang,
  under `-m32` x87 at -O1, and inside SurfOS at -O0 and -O2.
- Series with known coefficients need no fitted polynomials, and nothing is taken from another libm (`fdlibm` would be a new
  third-party source). The tables were calculated to 3000 bits in integers, with a throwaway script, and are in `math.c`.

The price is speed: on the Mac `sin` 0.8 us, `exp` 0.6, `log` 0.6, `pow` 1.2, `atan` 0.6, `sqrt` 0.07; a table-driven libm
takes some tens of nanoseconds. awk's `x^2` in a loop will feel it. `pow` with a small whole `y` could multiply instead; that
was left out until something needs it.

The accuracy far exceeds the brief's: against glibc the worst of 290,000 points is 1 ulp for every function (2 ulp for `tan`
against macOS's libm), and `sqrt`, `floor`, `ceil`, `round`, `trunc`, `fabs`, `fmod`, `modf`, `frexp`, `ldexp` agree to the bit.
`pow(i, k)`, `log10(10^k)`, `cbrt(i^3)` and `log2(2^k)` are exact where the answer is a double.

`errno` is set as glibc sets it: EDOM for a domain error, ERANGE for overflow, a pole and underflow. No `float` versions, which
nothing asks for.

### system, popen

`surfsh -c TEXT`, as the brief says, with one difference it could not foresee: **an argument is at most 255 bytes**
(`surfos/spawn.h`). A longer command goes the way a surfsh starts a surfsh (`surfsh --sub`), on descriptor 3 through a pipe. The whole
text fits in the pipe before the child starts, so nothing can wait or break. Over 8192 bytes it is `E2BIG`. awk's `system()` strings
may be long, so this matters.

`system()` and `pclose()` answer the **exit status**, 0 to 255, not a wait status: SurfOS has no `WEXITSTATUS`, and awk wants the
number. A killed command is 137 and one Ctrl+C ended 130, as `$?` says. -1 when it could not start.

Both flush stdio first, which C does not require, so that a tool's output comes before its command's. POSIX has `popen()` close
earlier streams' pipes in the child; here `spawnx()` hands a child only the descriptors it names, so there is nothing to close.
The pipes stay clear of 0, 1 and 2: in a program that closed one of those, the pipe's end would land there and be handed on
as that descriptor, and a command would read the far end of its own output for ever. `pclose(NULL)` is -1 with `EINVAL`.

Ctrl+C ends the whole job, the caller too: POSIX's `system()` ignores SIGINT while it waits, and SurfOS has no signals.

### getopt

POSIX's, with messages as the brief's `tool: what: why`, the tool being the last part of `argv[0]`: `sed: -x: unknown option`. A
leading `:` for silence, a leading `+` skipped. `optind = 0` starts again, as with glibc. `optind = 1` does too, unless the
last scan stopped inside a word of letters such as `-ab` at `argv[1]`: then nothing tells it from going on, and it goes on.

### Left out

- Case folding and classes above ASCII (above).
- `REG_ICASE` on back-references beyond A-Z.
- `strtof`, `strtold`, `fmodf` and the like: not asked for. `__strtoflt(single)` is what `strtof` would be.
- `popen` modes beyond `r`, `w`, and `e` (accepted and meaningless here).

### For lang's awk

- Regular expressions: `regcomp(&re, s, REG_EXTENDED)`. awk's `\` escapes inside brackets are awk's to turn into POSIX's before
  the call, since a `\` in a bracket is itself here, and `\n` and `\t` too: this library takes `\n` as `n`, as glibc does.
- `gsub` and `match` on a line with a NUL, or from the middle: `REG_STARTEND`. A match's offsets are from the string,
  and `length`, `substr` and `index` must count characters themselves.
- `strtod()` reads hexadecimal, `inf` and `nan`. POSIX awk says numeric strings are what `strtod` takes, but the one true awk and
  gawk's default refuse these in input, so awk may want to look before it calls.
- `system()` flushes, and returns the status directly. Long command strings work up to 8 KB.

### Tests

**Host, in `make unittest`:**

| Test | Checks | What |
|---|---|---|
| `test_regex` | 80,485 | 212 rows from Fowler's tables with Kuklewicz's corrections, each also through glibc; errors and limits; flags; UTF-8; back-references; 30,000 random patterns against an oracle in the test, which is POSIX's rule the slow way, in both syntaxes, and against glibc for the whole match; 30,000 fuzzed patterns, offsets checked to be on character boundaries; pathological patterns on long text; the limits in CPU time, each run measured against another that must cost the same order |
| `test_strtod` | 323,548 | bit for bit against glibc, with where it stops and ERANGE: every shape of number, the ends of the range, 40,000 random doubles in seven notations, exact halfway points with long double, near-numbers; `sscanf` |
| `test_libm` | 294,949 | a grid, random doubles and the awkward places against glibc; Annex F's cases against the test's own table; the fingerprint |
| `test_getopt` | 87 | step by step against glibc with `POSIXLY_CORRECT`; the messages |

Against glibc, 10 of the 212 table rows differ, all counted and none a failure: glibc keeps a group from an earlier iteration
(`((z)+|a)*`, `((..)|(.))*`, `((..)|(.)){2}`), does not give `(a|ab)(c|bcd)(d*)` and `(foo|foobar)(barfoo|foo)?` POSIX's split, nor
`(ab|a|c|bcd)*(d*)`, and refuses `{`, `a{`, `a{x}`, `a{1` by the decision above. On 100,000 random
patterns not one whole match differed from glibc's. By hand (`REGEX_FUZZ_HOST=1 test_regex`, in the C.UTF-8 locale) 100,000 fuzzed
patterns differed from glibc 270 times or so, every one by a decision above: glibc's `.` does not take a bad byte, it lets
a pattern's lone byte match inside a sequence, and its classes know é. On ASCII not one differed.

**Sanitizers:** by the coordinator's correction, none of this is in `make unittest` or Docker: a `gcc -m32 -fsanitize=...` binary hangs
there. All four tests were run on the Mac with clang under `-fsanitize=address,undefined`, clean. `printf.c` has x86 assembly, so
`sscanf`'s part was built with `clang -arch x86_64` and run under Rosetta, sanitizers on, also clean, buffers with no NUL
after a width included. libsurf's `string.c` is not 64-bit safe (its `memset` writes past the end there), so those runs use the host's
`memset`, `strlen` and the like. The host differences seen on the Mac are macOS's own: `nan(a b)`, ERANGE for an exact subnormal, `optind` after a
missing argument, `$` inside a group.

**The test the host cannot give:** `system()`, `popen()` and `pclose()` need surfsh and the kernel, so `system.c` has no host test.
`libtest` covers them inside SurfOS.

**Smoke:** one block of steps, `libtest` regex, numbers, math, getopt, system, popen, closed, and `libtest order | cat`, which shows a
program's output and its command's in order.

**`make test-all`:** on 39f79ee it passed through the lock: the smoke test's 545 checks at -O0 and at -O2, the Files test's 51 at
each. The result after the review and stage 2 is at the end of this account.

### The review of stage 1

The independent review of 39f79ee found no memory error and no wrong answer in regex, strtod, math or getopt. Its findings, all fixed
with a test that fails on the old code (`test_regex`, except where it says otherwise; the `libtest` ones were run in QEMU, linked with the old `stdio.c` and `system.c`):

| What | Fix |
|---|---|
| A failed back-reference comparison cost no steps: 120 KB took 39 s to reach the limit | charged before the comparison |
| Each repetition operator is a level of `walk()`, so levels were 64 x 17, not 64 | `MAX_WALK`, the time bound above |
| A bracket's ranges were searched in a line | sorted, merged, searched by halves |
| Clearing groups and each new start cost nothing | charged |
| `pclose(NULL)` closed NULL | -1, `EINVAL` (`libtest popen`) |
| A closed 0 or 1 handed the pipe's end to the command | the ends stay above 2 (`libtest closed`) |
| A back-reference could end inside a sequence | it must end where a character does; the fuzz test's boundary check is real now |
| `setvbuf()` freed the buffer holding kept input | refused while there is any (`libtest popen`) |
| Only pipes refuse `lseek`, so a pty's or a socket's read-ahead was lost | kept for all but files of fat and tarfs (`libtest popen`, on a pty) |
| `anchored` was always 0 | it works |
| `^^a` in basic syntax | as glibc |

## Stage 2: grep -E and sed

### grep

`user/bin/grep.c` keeps its options, its messages and its use of `lineutil.h`, and its own matcher goes: it was recursive, by the byte, and
could take exponential time. The library gives it `-E`, `\(\)`, `\{m,n\}`, classes and whole characters. A line goes in by length with
`REG_STARTEND`, so a NUL does not end it. Printing is `fwrite`, for the same reason.

What changes for someone who used the old one: a `\` before `( ) { } | + ?` and the digits used to mean the character, and now means what
it means in basic syntax. That is what moving onto the library is. What stays by decision: an `[` never closed is still itself in a basic
pattern, as the old grep's comment said, by rewriting it to `\[` when `regcomp()` says `REG_EBRACK`. With `-E`, which has no past, it is a
mistake. Other mistakes in a pattern used to be read somehow; they are said now, with status 2. Of `-E` and `-F` the last one counts.

`rootfs/man/grep.txt` is here because the page says what `-E` does. Track sys writes the pages of what exists today, so grep's may meet
theirs at the integration: this one should win.

### sed

`user/bin/sed.c`, one file. All of it, parser and machine, is in one place, as it is a filter with no protocol to take apart. What keeps
it testable is that it uses only descriptors and `saveutil.h`, with its own small buffers, and returns from `main()` rather than exiting, with
all its state in one struct. So the same source is built two more ways for the tests (below).

**What it follows.** POSIX, and GNU's choices where POSIX leaves one or where the brief's list is GNU's: `first~step`, `addr,+n`, `addr,~n`,
`0,/re/`, `I`, `T`, `Q`, one-line `a i c`, `\t` and the like, a status on `q`. Every odd rule was taken from GNU sed 4.9 by experiment, not from memory,
and they are not what its manual says: `~n` from a line that is a multiple goes on to the next multiple; the hold space is emptied at each file
under `-s`; `q` brings a missing newline and `Q` does not; `G` and `H` take the newness of the last line from the source; `#n` counts as the first two
characters whatever follows them; a label ends at `}` as well; a range whose first line was skipped by `n` starts where it is seen, and
`addr,+n` past its end still takes one line while `addr,n` does not.

**Different from GNU on purpose:**

- *Status.* The brief's: 2 for all trouble, where GNU has 1, 2 and 4. `q` and `Q` give their numbers.
- *`w` writes through saveutil.* A file that is there is not emptied at the start, and a new one appears when sed ends, as `file.tmp` until then.
  GNU truncates at the start, which the brief forbids. So a script that `r`eads a file it is `w`riting sees the old one. If the run fails, nothing
  has changed.
- *An empty match steps over a whole character.* GNU, in a UTF-8 locale, goes by the byte: `s/x*/-/g` on é gives `-\xC3-\xA9-`, which the brief's
  "never split a sequence" forbids.
- *UTF-8 always*, there being no locale. `y` works on characters.
- *A delimiter is one byte.* `s§a§b§` is refused: taken as bytes it would cut the character.

**`-i`.** Each file goes `su_begin()`, then at its last line `su_commit()`. With a suffix the old file is copied to the backup through a save of
its own *before* the rename, so that it is never missing from both names; GNU renames it away first. A failed backup leaves the file alone. A file
that could not be read to its end is not replaced, and neither is an empty one. `q` keeps what came out, as GNU does: `sed -i 10q` truncates. A failure
to write stops everything and takes back all saves in hand. Reading a directory, a missing file, or a twin beside the file is said, and the next file
is done.

**Left out, by the brief's list:** the commands `e F R W z v`, the flags `m M e`, `-z -u -l`, the long options, `r /dev/stdin`, and `\L \U \E \l \u` in
replacements: there `\U` is `U`, as POSIX has any other backslash. awk or a small script does case; it would be 40 lines if the owner wants it.

**Bounds.** A line is limited by memory, and 2 GB (the offsets of `regexec`), with a message. Nested `{` to 256. No recursion anywhere. A pattern cannot
hold a NUL byte (`\x00`), as `regcomp()` takes a string. Numbers that overflow are mistakes. All descriptors are looked after: each `w` file holds one,
and running out is said at the start.

### Tests of stage 2

| Test | Checks | What |
|---|---|---|
| `tests/test_sed.py` | 1,697 | `sed-host` and `grep-host` process against process with the real tools: stdout, status, and every file afterwards. 475 rows of corpus for sed and 54 for grep; 300 random scripts each alone and with `-s` on two files; bad scripts and input made at random; a megabyte's line; 200 nested `{`; a reader that goes |
| `test_sedsave` | 68 | `user/bin/sed.c` on the simulated file system: `-i`, backups and `w` with each write, fsync, close and rename failing, half-done renames, reads that fail, twins, read-only disks, 3000 runs in one process for leaks |

**Two builds of one source.** `tests/host/toolhost/` makes `sed.c` and `grep.c` programs of the host: the host's C library, the library's regex, and a
small `surf.h`. That compares with the real tools and runs under sanitizers anywhere. `tests/host/sed_bridge.c` builds it against SurfOS's headers on
`sim_fs.c`, as `edit` is, where system calls fail on request.

**Against GNU sed 4.9 in the Docker image.** Everything above. Random scripts found the range rules and the hold space's reset. By hand, 1500 rounds (6,248 checks) found
the last two differences, one rule with two faces; after that fix 400 rounds (2,076 checks) were clean, and `make unittest` runs 300. The real sed does not end on one of
the 400's scripts, which is skipped.

**Against BSD sed and grep on the Mac:** the 138 and 45 rows of corpus that POSIX covers and BSD agrees with; the rest is GNU's alone. The test says so when it runs there.

**Sanitizers,** on the Mac with clang under `-fsanitize=address,undefined`, none in Docker: `test_sed.py` with 1500 rounds of bad scripts and input, 2,903 checks,
clean, and `test_sedsave` with the host's `malloc`, clean. Those runs compare with BSD's sed, so the random scripts of the GNU comparison run only in Docker,
unsanitized. Scripts with `D`, `:` or `G` are left out of the bad scripts, as they may loop for ever as they should.

**Smoke:** the block has 19 steps now: grep's `-E` and back-references, sed's addresses, groups, `-E`, `-i` with its backup and nothing left beside the file, `w`,
`/dev/null`, `q`'s status, a mistake, a reader that goes, a whole character.

**`make test-all`:** passed through the lock on the tree of these commits (the account aside): the smoke test's 545 checks at -O0 and at -O2, the Files test's 51 at each. `make unittest` passed on the same tree.

**On the laptop:** none of this has run there. It runs only under QEMU until the owner tries it.
