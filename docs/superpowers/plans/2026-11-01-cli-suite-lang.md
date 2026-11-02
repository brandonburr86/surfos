# The command-line suite, track lang: bc

The account of `bc`, for the review and the integrator. `awk` follows on this branch, once text's stage 1 is committed; it
has its own part here then. Brief: `2026-11-01-cli-suite.md`.

## What was built

`bc [-lq] [file...]`: POSIX `bc` in `user/bc/`, a program of several files. It is built by a block in the `Makefile`, as
the other programs of several files are.

| File | What |
|---|---|
| `num.c`, `num.h` | decimal numbers of any length: every operation, the conversions, printing |
| `parse.c` | the language: reads a character at a time and writes code for the machine in one pass, without a tree |
| `vm.c` | the stack machine, the names, arrays, calls |
| `lib.c` | the `-l` library, written in bc's own language |
| `main.c` | arguments, files, standard input, the status: the only file that makes system calls |
| `rootfs/man/bc.txt` | the manual page |
| `tests/host/test_bc.c`, `bc_alloc.h` | the host test (below) |
| `tools/qemu-run.py` | a smoke block, marker `echo cli-lang` |

## Decisions a reviewer would ask about

**Limbs in base 10000, in 16 bits.** A user program links no libgcc, so on 32-bit x86 C has no 64-bit division, and
`__udivdi3` would not link. In base 10000 every product and carry fits 32 bits, on a 486 and one day on SurfCPU, and
the digits cost 2 per byte, twice what a byte per digit does (GNU's). Memory is a limit here, so that counts. The decimal
point lies on a limb's edge and a scale that is no multiple of 4 leaves the lowest limb's last digits zero: limbs line up
in addition without shifting, and a cut at any decimal digit is one `%` of one limb. The sizes are worked out in 64 bits, and
divide only by powers of two, which gcc folds at every level. Any other constant links at -O2 and not at -O0, so build both.

**Numbers are immutable and counted.** `x = y` costs nothing, a stack slot and a variable share a number, and no operation
can leave a half-changed value behind when memory runs out.

**No C recursion that input drives without a limit.** The machine keeps its stacks and frames on the heap, so a bc function
recurses as deep as memory goes (`f(100000)` is in the test) and then says so. The parser recurses up to `BC_DEPTH_MAX`
(1000 brackets or blocks), then says `nested too deeply`. Arrays are two levels, so `a[16777215] = 1` costs a few pages.

**A mistake at a terminal costs its line; in a file or a pipe it ends bc with status 2.** POSIX leaves the second
undefined. GNU goes on, and what follows a mistake then means something else; Howard's stops. At a terminal the names and
the scale are put back and the next line starts clean. The status there stays 0: the session went well.

**quit is obeyed when read**, as POSIX has it: `if(0) quit` ends bc, and the line's statements before it run first.

**Constants keep their text** and are converted when run, as the base is the one at that moment; the last value is kept, so
a loop does not convert each time. A single digit is its own value in any base (POSIX asks it for `ibase` and `obase`,
and GNU and Howard do it everywhere).

**Multi-digit constants with a digit the base lacks are an error**, `digit too large for ibase`. POSIX leaves it undefined;
GNU clamps and Howard adds up: both give a wrong number without a word.

**`^` works out the exact power and then cuts**, as GNU and Howard do and as POSIX's truncation means. So
`1.0001^100000000` costs what its exact value costs. `power_fits()` refuses at once a power known to be too large, which
would otherwise take hours of squaring to reach the limit: it tries the memory the last square needs, and gives it back.

**Schoolbook multiplication only, no Karatsuba.** Columns of 32 bits are carried every 40 rows that add anything (40 *
9999^2 fits), which is about three times faster than a carry at each step. `2^100000` takes 0.12 s in QEMU, `scale=1000; 4*a(1)`
0.14 s, and 100,000 turns of a loop 0.24 s. The price: squaring towards the limit, `x=2; while(1) x=x*x`, grinds on for
longer than anyone waits, and Ctrl+C is the answer. It would be 60 lines more; left out until something needs it.

**The library is in bc's language**, parsed when `-l` asks, run by the same machine, so all its failures are the machine's
well-tested ones. Three things let it stay short and safe. Its constants are in base 10 whatever `ibase` says, so it does not
juggle `ibase`. Names starting `_` and the statement `_fail "text"` exist only for it, so `l(0)` is an error rather than
GNU's and Howard's `-99999999999999999999.0...`. And when a library function returns or fails the VM puts its caller's
scale back and cuts the result to it, so that a failure inside `e()` cannot leave `scale` changed. It is from the mathematics:
Machin's formula for pi and half angles for `a`; the arithmetic-geometric mean for `l`, with no series at all; the prime factors of three
fractions for `ln 2`; `x = k ln 2 + f` and `f`'s digits cut in runs that double for `e`; Miller's downward recurrence for `j`.

**The library's accuracy.** Each function carries about a dozen guard digits, so truncating gives the true value's digits,
and the test holds it to that by working each value out 25 digits further. The limit, every bc's: a value with a longer run
of 0s or 9s past the scale can be a unit out in the last place. `s(1.570796)` at scale 0 is 1, for .99999999999994...
The man page says so.

**`obase` stops at 100000.** Short division by a base stays in 32 bits only up to 429496. POSIX's minimum is 99.

**Extensions kept:** those every other bc takes and a script from elsewhere is likely to use, at almost no cost. Names longer
than a letter; `else`; relations anywhere; `&& || !`, which short-circuit; `#` comments; `continue`; an empty part in `for`;
`return e`. **`-q`** is accepted and does nothing, for the fingers of people who type `bc -lq`.

**Left out, as POSIX has none of them:** `print`, `read()`, `last` and `.`, `limits`, `warranty`, `halt`, `*a[]` references,
`void`, `BC_ENV_ARGS`, `BC_LINE_LENGTH`. Strings and expressions do `print`'s work. Each is small if it is wanted.

## What differs from the other bcs

Only Howard's bc 6.5.0 (the Mac's) was to hand; the Docker image has no bc, and GNU's behaviour below is from its manual and
from memory, not from a run.

| Case | Here | Howard's 6.5.0 | GNU |
|---|---|---|---|
| `1+.000` | `1.000`: POSIX's maximum of the scales | `1`: a zero loses its scale when added | `1.000` |
| `s(10^30)` | `-.09011690191213805803`, confirmed exactly in Python | `.27129...`: pi not carried far enough | |
| `c(18.17518)` at scale 3 | `.781` (the value is .78109...) | `.780` | |
| `x=1; x+=x+=1` | 3: the left side first, POSIX's `x = x + e` | 4 | 3 |
| `for(i=0;i<2;i++);i` | a syntax error: POSIX has no empty statement | runs `i` as the body | |
| `!0+1` | 0: `!` below relations, GNU's documented precedence | 2 | 0 |
| long numbers in a base other than ten | break at 68 characters, as POSIX and in base 10 | do not | break |
| `length(0.0012)` | 2: POSIX's significant digits | 2 | 4: it counts the zeros |
| `l(0)` | an error | a large negative number | a large negative number |

## The tests

- **`tests/host/test_bc`, in `make unittest`:** 68,000 checks, in about 6 s under the Docker image's emulation.
  - The numbers against the host's `long long` (4,000 cases of the four operations and the scales) and against identities
    (`a = q b + r`, `a (b + c) = a b + a c`, the root is the largest of its scale whose square is not above `a`) on patterns of
    0, 9 and 5000, at sizes on each side of the stack's columns and the carry rows. Knuth's step D6 by a worked case.
  - The language: over 400 programs from tables, each scale rule of POSIX's by name, the registers, printing, POSIX's own
    examples, dynamic names, arrays by value, quit, and every mistake and its message and line.
  - The library against known digits (up to 99 places) and against itself 25 digits further, at 5 scales.
  - A seeded corpus of 73 KB of output against the system's `bc` where there is one, and everywhere against the digest of
    what that bc printed. It compares values, not scales, and leaves out what the table above shows Howard's gets wrong.
    Without a `bc`, as in Docker, it says the comparison is skipped and checks the digest: gcc at `-m32` and clang at 64 bits
    agree with it.
  - Every allocation of 7 programs refused in turn, with no leak and a clear message each time; limits on memory for a number,
    an array, the calls and the library.
  - Input cut at every byte, random bytes, programs with tokens changed (a quarter of them also short of memory), 13 kinds of
    nesting 100,000 deep, and a megabyte each of number, name, string and comment.
- **Under the sanitizers:** never in `make unittest`, as the brief's correction says: a `-m32 -fsanitize` binary hangs under
  the image's emulation. `test_bc long`, ten times the damaged input, passes natively on the Mac with clang's
  `-fsanitize=address,undefined`, 226,000 checks. The test's header has the command.
- **Coverage, measured once with clang:** `parse.c` and `vm.c` every line, `num.c` all but the length check in `num_mul()`,
  which takes 512 MB to reach.
- **The smoke block**, 20 steps, in `make test-all`: both ends of the number sizes, scale, the bases, `-l`, a script,
  file then standard input, a terminal session with a mistake in it, the statuses, out of memory, a missing file, a reader that
  goes away, the manual's page.

**By hand in QEMU, 256 MB, O2:** unbounded recursion reaches the limit and says `f: out of memory` after 3.5 s: too slow for
the smoke test at -O0, so the host test holds it. A directory as a file is `Is a directory`.

**One existing line changed, against the brief's rule of pure insertion.** The smoke step `kernel ls /initrd/bin | head -2`
expected `cat` and `count` first, and `bc` now sorts ahead of them, as other tracks' names do. It fails, and the rest of its
block with it, so no insertion can help. It now expects two lines that each end in a name, which is what the step is for. The
first `test-all` found this: 5 of 540 checks at -O0, the first of them this step and the others following from it. The rest of
that run passed, and it was not load.

**Not run:** on the laptop, as nothing of this track has been, nor under any memory but QEMU's. `make test-usb` was not run: this
touches no boot path, console or USB. Only the new files, the two blocks and that one line changed, no kernel file, so SurfCPU's
check does not apply.
