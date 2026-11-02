# The command-line suite, track lang: bc and awk

The account of `bc` and `awk`, for the review and the integrator. Brief: `2026-11-01-cli-suite.md`. `bc` passed `make test-all`
on its own before `awk` began: 545 smoke checks and 51 of the Files test, at -O0 and at -O2.

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
undefined. GNU goes on, and what follows a mistake then means something else; Howard's stops. At a terminal the next line starts
clean: the rest of the line with the mistake is read and dropped, and none of its statements run. When a run-time mistake stops a
function, its parameters and autos give the names back their values as the calls unwind. `scale` goes back only for the library's
functions: a person's function that set `scale` and then failed leaves it set. The status there stays 0: the session went well.

**quit is obeyed when read**, as POSIX has it: `if(0) quit` ends bc, and the line's statements before it run first.

**Constants keep their text** and are converted when run, as the base is the one at that moment; the last value is kept, so
a loop does not convert each time. A single digit is its own value in any base (POSIX asks it for `ibase` and `obase`,
and GNU and Howard do it everywhere).

**A digit the base lacks counts at its own value**, so with `ibase=2`, `12` is 4, and in base ten `1E5` is 245 and `.F` is 1.5. POSIX
leaves it undefined. This was an error until the review: a script should not stop at status 2 where other bcs go on. Every case in
the test was checked on Howard's, which is this rule. GNU's manual has another, from memory and not run here: such a digit becomes
`ibase-1`, which would make those 3 and 195, so a script that counts on either has a problem anyway. The page says the rule.

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
fractions for `ln 2`; `x = k ln 2 + f` and `f`'s digits cut in runs that double for `e`; Miller's downward recurrence for `j`. "Where the
code comes from", near the end, says why.

**The library's accuracy.** Each function carries about a dozen guard digits, so truncating gives the true value's digits,
and the test holds it to that by working each value out 25 digits further. The limit, every bc's: a value with a longer run
of 0s or 9s past the scale can be a unit out in the last place. `s(1.570796)` at scale 0 is 1, for .99999999999994...
The man page says so. `e`, `l` and `j` at 13 scales from 0 to 100 against the Mac's bc at 40 more digits, cut, and against Python's
`decimal`, an exact reference of another origin: 3,874 values, of which 4 differ from both, all four that limit, such as
`l(1.000000000000000000000000000001)` at scale 33, whose digits past the scale are 27 9s, or `e(.6931471805599453)` at scale 0, which is
1.99999999999999998. `j` no longer loses digits to cancelling, so it needs no extra digits for a large `x`.

**`obase` stops at 100000.** Short division by a base stays in 32 bits only up to 429496. POSIX's minimum is 99.

**Extensions kept:** those a script from elsewhere is likely to use, at almost no cost. Names longer than a letter; `else`;
relations anywhere; `&& || !`, which short-circuit; `#` comments; `continue`; an empty part in `for`; `return e`. **`-q`** is
accepted and does nothing, for the fingers of people who type `bc -lq`.

**`else` after `;` and on a later line,** as on Howard's: `if(0)2;else 3`, and an `else` after blank lines and comments. `;` may
stand directly after the statement, then newlines, and not the other way round, which is where Howard's draws the line too.
`else_follows()` looks at the characters and leaves them to be read, so an `if` still ends its item as before, and a mistake on the
next line is that line's and comes after the `if` has run. Looking ahead in a file costs the next line's first bytes, and a message
from running the `if` still names its own line (`item_line`).
At a terminal, outside braces, it does not look past the line typed. Howard's does: there `if(1)4` shows nothing until another line
is typed, as I checked on a pty. Here the answer comes at once, and the price is that an `else` on a later line is a mistake at a
terminal, unless it is all in braces. A person needs the answer more than the form, and a file or a pipe has both. The page says so.
GNU's grammar, as I remember it and not from a run, takes `else` only straight after the statement, so a script that uses these
forms is written for Howard's, or for this one.

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
| `ibase=2; 12` | 4 | 4 | 3, by its manual: such a digit becomes `ibase-1` |
| a number of exactly 69 characters | one line, 70 with the newline: POSIX's | the same; every break of 825 cases of text and digits is the same byte for byte | a backslash and one more line, from memory |
| `if(1)4` at a terminal | the answer at once; an `else` on the next line is a mistake | no answer until the next line, for its `else` | |
| `2e2` | a syntax error: POSIX has no exponents | 200 | |
| `if(1)` at the end of the input | `unexpected end of input`, status 2 | takes it, status 0 | |
| `1; )` in a file | nothing of that line runs | prints 1 first | |

## The tests

- **`tests/host/test_bc`, in `make unittest`:** 68,500 checks, in about 6 s under the Docker image's emulation. `test_bc long`
  adds `l` of numbers of up to two million digits.
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
  `-fsanitize=address,undefined`, 226,470 checks. The test's header has the command.
- **Coverage, measured once with clang, before the fix round:** `parse.c` and `vm.c` every line, `num.c` all but the length check
  in `num_mul()`, which takes 512 MB to reach.
- **The smoke block**, 22 steps, in `make test-all`: both ends of the number sizes, scale, the bases, `-l`, a script,
  file then standard input, a terminal session with a mistake in it, the statuses, a digit above the base and `else`, out of memory,
  a missing file, a reader that goes away, the manual's page.

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

## awk

`awk [-F fs] [-v var=value]... [program | -f file...] [argument...]`: POSIX `awk` in `user/awk/`, on text's library, merged
into this branch at `39f79ee`.

| File | What |
|---|---|
| `parse.c` | the program to code for the machine, in one pass, without a tree |
| `vm.c` | the stack machine, calls, the places that assignment, `++`, `getline` and `sub` share |
| `value.c` | strings, numbers, the rules between them, arrays |
| `field.c` | the record and its fields, split and put together when asked for |
| `io.c` | one reader with its own buffer; the files and commands that programs name |
| `fmt.c` | `printf`, and through it `CONVFMT` and `OFMT` |
| `func.c` | regular expressions, `sub`, `gsub` and the built-in functions |
| `main.c` | the command line |
| `tests/host/test_awk.c`, `awk_host.h`, `awk/` | the host test (below) |

### Decisions a reviewer would ask about

**A mistake ends awk, by `exit(2)`.** libsurf has no `setjmp`, and awk has no session to recover, unlike `bc` at a terminal. So
`fatal()` is the whole of error handling, also for memory, and nothing below it unwinds. Everything allocated goes through
`xalloc()`, which checks.

**No recursion that input drives without a limit, again.** A function's calls are heap stacks. The parser recurses up to
`AWK_DEPTH_MAX`; every way down the grammar passes through `primary()`, so depth is counted there. Precedence climbing keeps a
bracket to a few frames, so 1000 fit in 4 MB. The test nests 15 kinds of thing 100,000 deep. It found one: `$$$$...` overran
the stack until the count moved to `primary()`.

**`print`'s `>`.** POSIX's grammar bars comparisons from an unbracketed `print` list. This awk takes any but `>`, so
`print a == b` works, as in gawk, where the one true awk says syntax error. `print a, b > c ? d : e` is a syntax error, as in
every awk.

**One reader, on the descriptor.** A record may end at a byte, a string, an empty line or a regular expression, and input may be a
pipe or a terminal, so `read()` into a buffer of its own rather than stdio. Only one record is held, and what was read past it. An RS
of more than a character is a regular expression, which POSIX leaves open and every awk in use does.

**A pattern as RS waits for 64 KB past its match, or the end.** The review found that a match stopping short of what had been read
could still change: `ab(cd)?` over `xabc`, then `dy`. "Could more input extend or change this" is a question about the states still
alive at the end of the text, which `regexec()` does not answer, and `user/libsurf/regex.c` is text's. So there are three cases:

- An RS with no operator in it is a string, found with a plain search that goes on where it stopped: exact, at once, and in step
  with the input. `\r\n` and `---` are here, and they are most of what people write. An RS with a byte in no sequence goes the
  regular expression's way, which works by characters, so that it never lands inside one.
- Any other pattern, off a terminal, takes a match only when `RS_MARGIN`, 64 KB, is read past it, or at the end of the input. The same
  bytes then give the same records however they arrive, **with one limit: a match that more input would change by way of something
  more than 64 KB further on.** `a(b*c)?` over 70 KB of `b` is the shape. An exact answer needs partial matching in the library. The
  price is that from a slow pipe such records come in bursts, or at the end. The page says so.
- At a terminal it stays a byte past the match: a person will not type 64 KB, and the newline they typed is that byte.

**A long record is searched about three times over, not once a read.** Each search goes from the start of the record, as a match may
begin anywhere, so each one reads as much again as the one before. `test_awk` counts the bytes given to `regexec()` and holds a
record of 2 MB to 5 times that; it was 124 times. 32 MB with no match takes 0.04 s natively, for the 20 s the review measured.
The buffer moves its contents down only when the room behind them has gone, and doubles if it is then over half full.

**`>` empties a file, against the brief's rule, on purpose.** The brief says tools replace files through `saveutil.h` and "Never
truncate a file the user already had". `print > "f"` truncates on first open, as POSIX requires of awk and as the shell's `>` does,
and it is the program's author who wrote `>`. It cannot go through saveutil and stay what it is: the file must exist and hold what
was printed while the program runs, because a program may `close()` it and read it, or `fflush()` and run a command on it, and it
may print for ever. A half-written replacement beside the file would break all three. `sed -i` and the like choose to replace a file
as a tool; here the language says what to do with it. The page says what `>` does and that `>>` keeps what was there.

**A write that fails ends awk,** in `print` as it always did, and now at the close as well, when the last of a buffer goes out and
a full stick is often only found. `stream_end()` flushes first, as libsurf's `pclose()` does its own flush and drops the result, then
checks `fclose()`, and says `awk: name: why`. At the program's end each such file is named and the status is 2, even after `exit 3`;
`close()` stops there, as carrying on would lie about what it wrote. A file only read loses nothing, so reads say nothing.
The kernel's pipes end a program that writes to one nobody reads with status 141, like SIGPIPE on Unix, so a command that went away
is no write failure here.

**`fatal()` ends things as the end of a program does**, the review's suspicion, confirmed: it left commands unwaited for, so what they
wrote came after the shell's next prompt. It now flushes, closes and waits, after its message. The test: `print "x" | "sleep 0.3;
cat > f"; print 1 / 0`, and `f` holds `x` when awk has returned. No guard against coming back is needed: nothing in `streams_end()`
calls `fatal()`.

**A name that cannot take a value is refused** in `var=value`, at `-v` and among the arguments: `ENVIRON` and `ARGV` by number, as
`-v` comes before they are made, any name that holds an array, a function's, and a word of the language. mawk refuses all of these,
and the Mac's awk the arrays and the functions, but takes `length=1` and drops it. `-v f=1` with a function `f` gets the parser's
`both a function and a variable`. A name
that only later turns out to be an array in the program gets `a scalar used as an array`. That was the type confusion on `ARGV`:
now nothing can make it anything but an array, so `open_next_file()` needs no check of its own.

**Fields are offsets into `$0` until they are asked for,** so `{ print $1 }` on wide records allocates once. A record is split
when a field or `NF` is first wanted, so `FS` and `OFS` must not change under a record that waits: `global_set()` splits or
puts together first, with the old ones. That is POSIX's rule that a new `FS` is for the next record, and gawk's for `OFS`.

**What is numeric in input.** Decimal only, by a check before `strtod()`, which also takes `0x10`, `inf` and `nan`: POSIX's
NUMBER token, so that a word in a column stays a word. Both awks at hand read `0x10` as 16.

**An array as an argument.** Which a bare name is can only be known when it runs, so such an argument compiles to `OP_ARGV`. An
unset name goes as a `V_REF` to where it lives, so that a function that makes an array of it gives it to the caller; read as a
scalar it stays unset whatever happens to the name meanwhile, which is by value.

**`x op= e` runs `e` first**, then reads `x`: `a = 1; a += a += 2` is 6 in the one true awk and in mawk. (`bc` is the other
way, by POSIX's text for it.)

**`printf`'s format never reaches the C library.** Each conversion is taken apart and rebuilt from the flags, numbers and one
letter found, so a program's format cannot read a missing argument or use `%n`. `CONVFMT` and `OFMT` go the same way, as a
program sets them. Too few arguments is an error. `%c` of a number above 127 is UTF-8. `+` and a space are dropped for `%u %x %o`:
C says nothing of them there, and libsurf's `fmt_num` prints them, which only the smoke test can see.

**`CONVFMT` and `OFMT` cannot call themselves.** A number is no format: `CONVFMT = 0.5` leaves the default in use, as on the Mac's
awk and in mawk, which both print 3.14. A string is, so `-v CONVFMT=0.5` gives `0.5`, as there too. `%s` in the format asks for the same
conversion again, so a static `converting` in `num_str()` gives that one the default. The depth is 2 at most. The Mac's prints
3.14 for `CONVFMT = "%s"`, and mawk 1.3.4 ends with a segmentation fault.

**`substr` gives the Mac's answers, by POSIX's text:** `m` and `n` are cut to whole numbers and a start below 1 is 1, with the count
running from there, so `substr("hello", -1, 3)` is `hel`. POSIX says little here. Checked in the Docker image: mawk 1.3.4 20200120
gives `hell` for that, and `he` for `substr("hello", 0, 2)`, as here. The page shows both. On long strings that are not all ASCII the string keeps where every 64th
character starts, built on the first look past the 64th: a loop over 200,000 `é` took 21.6 s by the review's clock and takes 0.03 s,
in any order.

**`index` finds whole characters**: `index("é", "\251")` is 0, as it would otherwise be 2 in a string of length 1. In a string where
every byte is a character it stays `memchr`. `index("", "")` is 0, as on the Mac's; mawk gives 1.

**`\x` is an escape, of at most two digits**, in strings and patterns, as mawk reads it; the Mac's goes on to any number of digits,
so `"\x41B"` is `AB` here and one byte there. With no digit the backslash stays, as for any other that is no escape.

**A file and a command of one name are two streams**, each way, and `close(name)` closes them all. `>` and `>>` of a name are one.

**`sub`'s replacement is POSIX's text and nothing else:** `&` is the match, a backslash before `&` or before a backslash makes that
character plain, and any other backslash is itself. So two backslashes are one wherever they stand, as in mawk, on 1,500 random
replacements with no difference. The Mac's awk keeps both when no `&` follows.

**`"cmd" | getline` leaves NR alone**, by POSIX's text for that form, and as the one true awk and mawk do.

**A field past NF is the uninitialized value**, as POSIX says outright, so `$9 == 0` is true when there is no `$9`. Both awks at
hand take it as an empty string. It is the one place here where I kept POSIX against both.

**Left out:** gawk's extensions: `gensub`, `strftime`, `BEGINFILE`, `IGNORECASE`, `PROCINFO`, `@include`, `\y`, two-way pipes,
`printf %a`, hex constants. Case folding above A-Z, as the library has none: there is no locale.

### What differs from the one true awk on the Mac

Mac's `awk` version 20200816, and mawk in the Docker image where the table says so. gawk is not installed, so nothing here
was run against it. Of the 219 programs compared while writing this, 211 print the same, and these are the rest:

| Case | Here | The Mac's |
|---|---|---|
| `length("héllo")` | 5, as the brief asks | 6: bytes |
| `0x10` in input | a string | 16, and in mawk |
| `$1 < $2` where there is no `$2` | by number: POSIX's uninitialized value | by string, and in mawk |
| `/[/]/` | a / in brackets | an error |
| `print 1 < 2 ? "y" : "n"`, `print x != ""` without brackets | taken | a syntax error |
| `printf "%z"`, `printf "%5%"` | printed as they stand | an error |
| `ARGV[0]` | `awk` | the path it was run by |
| `\\q` as `sub`'s replacement | `\q`: POSIX's text, and mawk's | `\\q` |

That table is what the 219 show. Beyond them,
`printf "%c", 233` is UTF-8 here and one byte there, and a whole number past 2^31 prints in full, where the Mac's gives up to 30
digits and mawk `2.14748e+09`.

### The tests

- **`tests/host/test_awk`, in `make unittest`:** each run is a fork, so a status, a message and a crash are seen as they are.
  - 215 programs in a table, each compared with the Mac's awk and every difference read, then frozen with what this awk prints.
  - About 400 more by hand: characters and bad bytes, NUL bytes, records and fields, every `getline` and redirection, `close`, the command
    line, the grammar's corners, awk's escapes in regular expressions, functions and their names, every mistake and its message.
  - A corpus of 92 programs over 31 KB, byte for byte against the system's `awk`: the one true awk on the Mac, mawk in the Docker
    image. For-in's output goes through `sort`.
  - Limits on memory for strings, arrays, calls and one huge record, each with its message; and loops of a few hundred thousand turns
    under a small limit, which a leak would reach.
  - Programs cut, tokens changed, any bytes as input; 15 kinds of nesting; a megabyte each of name, string and comment; 3000
    functions and 3000 rules; a million fields.
  - Writes that fail, through an `fclose()` told to fail, and `/dev/full` where the system has one, as the Docker image does.
  - Records whichever way the input arrives, from a command that pauses and at the end of a read, and the bytes given to `regexec()`
    counted, in memory the run's fork shares.
  Without `awk` on a machine the comparison says it is skipped. 1,240 checks in all.
- **Under the sanitizers:** never in `make unittest`. `test_awk long` runs natively on the Mac with clang's
  `-fsanitize=address,undefined,float-cast-overflow`, by the command in the test's header, on 20,000 damaged programs and 5,000
  damaged inputs.
- **Coverage, measured once with clang, before the fix round:** `io.c` and `main.c` every line, the others from 94 to 99%. What is
  left is mostly messages for what the parser already stops, such as an array reaching `to_num()`.
- **The smoke block**, 29 steps, marker `echo cli-lang`, a block of its own after `bc`'s. Seven of them show what a host cannot:
  libsurf's `printf`, a write refused at the close, in `close()`, a command waited for after a mistake, and RS at a terminal, twice.

**Not run:** on the laptop, as nothing of this track has been. No kernel file changed, so SurfCPU's check does not apply. `awk`'s
floating point runs on x87 like the rest of SurfOS's programs: nothing here uses SSE.

### The fix round

From the independent review of `6f2cab4`. Each item had a test first, seen to fail on the unfixed code: 58 failures in `test_awk`, 43 in
`test_bc`, and on SurfOS `+ff| 10|+3| f` and a silent `status 0`. The reviewer's three crash programs now end cleanly (two exit 0, one
with `a scalar used as an array`), and `fc.txt`, 18 float-cast probes, gives no runtime error.

| Item | Where | The test that failed first |
|---|---|---|
| 1 HIGH, `ARGV=1`: type confusion | `assign_operand()` | `command_line()`: 9 refused names, operands and `-v` |
| 2 HIGH, `CONVFMT` recursion | `num_str()` | `language()`: number and `%s`, `CONVFMT` and `OFMT` |
| 3 MEDIUM, `fclose()`'s result | `stream_end()`, `stream_close()` | `writes_that_fail()`, through a failing `t_fclose`; `/dev/full` where there is one; the smoke step on `/dev/config/hostname`, which opens with `>>` and refuses each write |
| 4 `getline_cmd` | `getline_follows()` | `language()` |
| 5 one stream for a file and a command | `stream_find()` | `files_and_commands()`: both ways round |
| 6 regex RS at a read's end | `read_to_regex()` | `records()`: a command that pauses, a file with the read's end after the `c` |
| 7 `index` in a character | `index_of()` | `characters()` |
| 8 `substr`, regex RS quadratic | `char_at()`, `read_to_regex()` | `characters()` at 262,144 characters within 20 s; the count of bytes searched |
| 9 bc: `$ 5` after a line | `skip_line()` | `at_terminal`: 8 cases |
| 10 `%+x` on SurfOS | `put_number()` | a smoke step; the host's `printf` cannot show it |
| Suspected, `fatal()` | `stop()` | `writes_that_fail()`, confirmed: `f` was empty |
| nits | | `\x`, `index("", "")`, the pages, bc's digits and `else` each have theirs |

The smoke blocks now have 22 and 27 steps. One trap: a step typed at a program that has printed nothing can go to the shell's editor,
so the terminal's RS step waits for `ready`.

## Where the code comes from

The review asked whether anything followed another program's source from memory, and some of it did, though I had no source open and
pasted nothing. GNU bc is GPL, this repository is public and the brief takes no third-party source, so the coordinator's decision was
that what I rated close goes, as it went for `sed` and `inflate`. It has gone, and `cli/lang`'s history was rewritten so that it was
never there: the merges of text's commits keep their parents, and the last tree is the one that passed.

| What | Now | From |
|---|---|---|
| `l(x)` | `pi / (2 AGM(1, 4/s))` for a large `s`, and no series at all | Gauss's AGM and the elliptic integral; Salamin and Brent's use of it |
| `ln 2`, new | `7 ln(16/15) + 5 ln(25/24) + 3 ln(81/80)`, whose factors of 3 and 5 cancel, kept as pi is, each by `1/m + 1/(2m^2) + ...` | prime factors, and Mercator's series |
| `e(x)` | `x = k ln 2 + f`, `f`'s digits cut in runs that double, a short series for each, their product, then `2^k` | Brent's method; no halving and no squaring |
| `j(n, x)` | Miller's recurrence going down, whole numbers, normalised by `1 = J0 + 2 J2 + ...` | J. C. P. Miller's method, 1952; no power series |
| `put_number_char()` | each character waits for the next, as what may stand in the 69th place depends on it | POSIX's 70 characters, and the Mac's breaks, mapped from output |
| `print_fraction()` | one unit of the last decimal place is multiplied along with the number, and the digit at which it passes the point is the last | the Mac's digit counts: 744 cases, 24 bases |
| `put_replacement()` | one rule in one pass | POSIX's text for `sub` |
| `substring()` | a window of positions, from and to | POSIX's text, and the answers the tests pin |

What each change costs or gains, natively on the Mac. `e` is level with the old form at ordinary sizes and twice as fast at `e(100000)`, 10 s
against 22 s. `l` is the price: 40 µs a call at the default scale against 15, and 1.4 s against 0.84 s at scale 5000, as a mean works at half
as many digits again. `j` is 3 times slower for a small `x`, 60 µs a call, and 25 times faster for a large one: `j(3, 20000)` takes 0.4 s against
9.6 s. Numbers of 69 characters now stay on one line, and `sub` turns `\\` into one backslash everywhere. My first form of `e`, a
`sinh` series, and of `ln 2`, by a mean, were 17 times slower at scale 5000, which is why these are not those.

**Where the behaviour leaves one form.** `times_base()` and taking the digit that passes the point are what a base conversion is. The
signs of `j`, `J(-n) = (-1)^n J(n)`, are the mathematics. Any series is a loop that adds terms until they vanish, so `e`'s `for` is
that, with a short number in it. `put_replacement()` is short because the rule is. In `substring()`, the start cut to 1 and the
numbers cut to whole ones are behaviour the tests pin, so only the arithmetic round them is free. All the other choices are mine.

The series in `_lm()` has the shape of `_at()`, which stays, and of every series with a whole divisor.

**Left as it was, and why.** `a`, `s`, `c` and pi, by the coordinator's word: their plans were never GNU's. `power()` in awk and `num_pow()` in bc
are repeated squaring, which is common knowledge; `num_pow()`'s scale rule is POSIX's. Knuth's algorithm D, TAOCP 4.3.1, for `num_div()`, and
the LCG constants of `rand()`, which are Knuth's MMIX. A Newton integer root. FNV-1a in `value.c`, Bernstein's hash of names, Marsaglia's
xorshift in the tests. awk's grammar and `getline` from POSIX, bc's scale rules from POSIX, and `test_bc.c` quotes POSIX's example `e(x)`
as a test. I never recalled the source of mawk, gawk, plan9, OpenBSD's bc or dc, or Howard's bc: those I only ran.

### The last round

From the review of `1eb1f78`, which found no memory-safety issue and every fix of the first review holding.

- **`l` of a number of half a million digits or more was wrong**, a regression of the rewrite. `k`, the power of 2 that brings `x` near
  1, came from its count of digits times 3322/1000, which is log2 10 and 7.2e-5 more: at a million digits 75 bits too many, so `x / 2^k`
  was nowhere near 1 and the mean's identity failed. `l(10^1000000)` gave 2302588.518954... for 2302585.092994.... It is now
  3321928094887 / 10^12, 3.6e-13 below log2 10, and a number holds at most 2^31 digits (2^29 limbs of four), so `k` is out by under
  a thousandth of a bit at any length. The review's four cases, `10^500000`, `10^1000000`, `10^-1000000` and `3^2000000`, are in
  `test_bc long` with the true values from Python's `decimal`; each takes seconds natively and minutes under the image's emulation,
  so they are not in `make unittest`. On `1eb1f78`'s library `l(10^500000)` came out ...200901239 for ...200899572.
- **`j`'s work had no bound.** Its start loop worked at the full scale at every step, and had no limit on `x`: `j(0, 5000000000)` ran
  without end. The bound on `J(m)` is now kept as twelve digits and a power of ten, so a step costs the same at any scale. And `j`
  first sets the scale the library before this one worked at, `.45 x` digits more than asked: where that one refused, at once, with
  `j: scale must be from 0 to 2147483647`, this one does too.
- **Memory too small is said at once.** Under a program's 256 MB, `scale=100000000; j(0,1)` and `e(1)` ground on where the library
  before ran out in 0.3 s and 26 s. Now `e`, `l` and `j` take what they will keep at once and give it back before they start, as a
  power does (`_room()`, a third of a number in parts of up to a billion digits): about 7 numbers of the working scale for `e`, 10 for
  `l`, whose mean works at half as many digits again, and 6 of the scale the old `j` used. "out of memory" comes at once.
- **A message names the function the program called**, `e: out of memory` and not `_room` or `_lm`: the machine looks out through the
  library's own calls to the one the program made.
- **awk:** `getline` after `|`, a backslash and a newline is a pipe from a command, as on the Mac's awk, where it was a syntax error.
  At a terminal a regex RS match that ends the line typed is taken then: with `[0-9\n]` the line's last record no longer waits for
  the next line. A smoke step types it; on `1eb1f78`'s build it waited for `<b>` until the step gave up.

### What the last run covered

This round's tree, on `1eb1f78`. `make unittest` passed on it: `test_bc` 68,516 checks and `test_awk` 1,240. `make test-all` passed
at the first try: 547 smoke checks and 51 of the Files test, at -O0 and at -O2. This track's two smoke blocks, 22 and 29 steps, also passed
alone at -O2 before it.

Under the sanitizers, natively on the Mac, on this tree: `test_awk long`, 26,659 checks, and `test_bc long`, 229,710, with the four
huge `l` cases in it, with `-fsanitize=address,undefined,float-cast-overflow`. `regex.c` is still built with
`-fno-sanitize=pointer-overflow`: its line 341, in text's `set_sort()`, adds 0 to a NULL, and `cli/text`'s newer commit guards that line
(`s->n ? r->range + s->first : NULL`); by the coordinator's word it is not merged here, and the test header's command keeps the flag until
it is.

`e`, `l` and `j` against the Mac's bc at 40 more digits and Python's `decimal`, again on this tree: 3,874 values at 13 scales, twice with
different random values, 4 differ each time, all the long-run limit the page states.

The round before this one is in the history below: `test-all` took four runs then. Two failed at -O2 on the memhog check, as
this tree's memory leaves `ls` refused at its stack; `d119cbb`'s three lines settled that (`30a584b`). One failed at -O0 on httpd's
"six places", a race `ai-dev` fixed in httpd (`26017df`), not this track's to take. The fourth passed whole.

**The history.** `cli/lang` from `be8094d` was rewritten with `git filter-branch`, so that each commit has these forms from the commit where
its function first appears, and none of the recalled ones. The merges keep their second parents, `39f79ee` and `7ebf1ed`, and text's
commits are untouched. Every rewritten commit builds: natively with warnings on, and the four whose messages say "Not built yet"
against SurfOS's own headers. Each commit that has a program passes its own host tests. The last tree is byte for byte the tree that
passed. Where an earlier commit of this account records a run, that run was of the code as it was before the rewrite. Commit messages
that the rewrite made untrue were corrected.

Text's fixes change stdio's read-ahead, which `awk` does not use: it reads the descriptor itself and keeps its own buffer, so
`fflush(NULL)` has nothing of its to drop. The host's `popen` hides that, so three smoke steps read a command, a file and a pipe
across a `system()` and an `fflush()`. `MAX_WALK`'s `REG_ESIZE` reaches `awk` from `regcomp()`, as a message like any other.
