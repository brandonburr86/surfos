# The command-line suite, track sys: top, watch, man and the manual

The account of branch `cli/sys`, for the integrator and the review. The brief is
`2026-11-01-cli-suite.md`.

## What was built

- **`top`** (`user/bin/top.c`, model `user/bin/topmodel.h`): a live view on the alternate screen in raw mode, using
  only the ANSI subset of `include/surfos/tty.h`. Orders by CPU, memory and pid; `-d seconds`, `-n count`, `-s order`.
  Plain repeated reports when standard output is no terminal.
- **`watch`** (`user/bin/watch.c`, model `user/bin/watchtext.h`): `watch [-n seconds] [-t] command...` through
  `surfsh -c`, reading through a pipe, redrawing on the alternate screen, ending on `q` or Ctrl+C with the shell's
  screen back.
- **`user/bin/termview.h`:** what those two share: seconds as an option gives them, the keys a terminal sends, text made
  fit to show, one buffer and one write a screen, rows sent only when they changed.
- **`man`** (`user/bin/man.c`, table and pure logic `user/bin/manutil.h`): `man name` through `less` on a terminal, plain
  when piped; `man -k word` over the first lines.
- **The manual:** 113 pages in `rootfs/man/`, 2,613 lines, for every program (59), kernel command (83) and surfsh builtin
  (34) that exists today: 160 names, as some are two of those. 48 of them share a page through the table in `manutil.h`.
- **The kernel:** `struct procinfo` gains `cpu_ticks`, `switches` and `mem_pages`, appended (36 to 48 bytes, the checks in
  `kernel/syscall.c` and `user/libsurf/syscalls.c` follow); `SYS_sysinfo` is call 54 (`struct sysinfo` in
  `include/surfos/proc.h`, `sys_sysinfo()` in `kernel/proc.c`, `sysinfo()` in libsurf).
- **`help`:** one line pointing at `man`, in surfsh's `help` (`user/surfsh/builtins.c`). No check minded.
- **Tests:** `tests/host/test_top.c`, `test_watch.c`, `test_manutil.c`, `tests/test_man.py`, one block in `SMOKE`.

## Decisions a reviewer would ask about

**Headers rather than a `user/top/` directory.** The brief gives this track `user/bin/top.c` and `user/bin/watch.c`, and a
file there links alone. The logic that tests need went into headers beside them, as `lineutil.h` and `fileutil.h` do, so
the Makefile needed no new link rules and every host test compiles the code as it is.

**`mem_pages` counts what `task_mem_held()` counts:** a program's pages, its tables and, for a process, its kernel
stack. So the MEM column adds up to the `programs` total, and to memstat's "programs hold". A task on the list has its
address space or none: both ways of ending clear `t->uvm` with interrupts off before anything is freed, which is what
makes reading it in `fill_info()` safe. On riscv32 `t->uvm` is always NULL, and `kernel/proc.c` is not built there.

**A share is a sample.** `curTask->cpu_ticks++` in the timer tick is all the accounting the kernel has, so %CPU cannot be
finer than a tick, and a task that runs only between ticks shows nothing. The page says so. The clock for a share is
`sysinfo.ticks`, the counter the tasks' ticks are on, not `uptime()`, so no rate is converted. No 64-bit division: libsurf
has no `__udivdi3`, so `top_share()` stays in 32 bits.

**`procinfo()` is by position** in a list that can change between calls, so a task can be read twice or missed. `top_tidy()`
drops the second reading of a pid; a task missed shows at the next report. A task that was not in the reading before has
all its ticks counted as this interval's: pids are never reused (`next_pid++`).

**A kernel thread's name in brackets keys on `mem_pages`, not `PROC_USER`.** The flag is set only when a process first
enters ring 3, and the first boot showed `[cat]` for a program about to run. Every process counts its kernel stack from
the moment it is made, so `mem_pages == 0` is the right test.

**`-s cpu|mem|pid` is an addition to the brief's options.** The brief asks for the three orders; plain reports have no keys,
so without this they could only ever be by CPU.

**`-n` on a terminal stays for one more wait after the last report.** Otherwise `top -n 1` would flash and vanish, since the
view is on the alternate screen. `top -n 1 | cat` is the plain report on the main screen.

**`watch` refuses a standard output that is no terminal** (status 2). The brief asks for plain reports of `top` only, and a
screen has no use in a pipeline; repeating output there would have been more code for no asked-for use.

**A run of `watch` is a job of its own** (`SPAWN_JOB_NEW`, `SPAWN_BG`): nothing of it can read the keys, since a job that
is not the terminal's foreground stops when it reads, and `JOB_KILL` ends all of it, a pipeline's stages too. `q` never waits
for the command: `watch` polls the keys and the pipe together, and does not wait for the killed run either, since init
collects orphans and a kill inside a kernel command waits for the end of that command. A command asking a question stops
there until `q`, and the page says so.

**The run's end** is the first program's end plus an empty pipe, not the pipe's end of file alone: something left in the
background may hold the pipe open for good. Output arrives in `ws_feed()` until then, and is shown once a second during
a long run, so a command that never ends still shows.

**`watch` draws what a person reads, and no more.** Text goes through a small screen of cells (`watchtext.h`). `\r` and `\b`
move as they do; long lines wrap, as at the prompt; escape sequences are read and dropped, whole ones and ones in pieces
across reads; other control characters are dropped; what is not a character is a `?` a byte, as `less` shows it. A title
that never ends stops at the next control character rather than swallowing the rest. Past the last row the text is read
and dropped, not refused: the command then runs as it would anywhere.

**Text from outside never reaches the terminal as it came.** A task's name can be anything a file's name can, and may be
cut inside a character by the kernel's 16 bytes: `tv_clean()` makes it fit. The command's own text in `watch`'s title goes
through it too.

**A row is sent with `ESC[K` before its text, not after.** After a text of the row's whole width the cursor is past it, and
some terminals would erase its last character. The bottom row never reaches its last column, as `less` has it.

**The alias table is in `manutil.h`, in C**, so `man` needs no file for it; `tests/test_man.py` reads it from the source. A
page of a name's own comes before the table, so a table entry is never in the way of a new page. `gunzip` and `zcat` are in
the table for track archive, which cannot edit this file: a page of their own wins if it writes them.

**`man name` for a name that may not be a page** (`.`, `:`, `[`) goes through the table only; `man_name_ok()` takes letters,
digits and `- _ +`, so nothing leads out of `/initrd/man`.

**`man -k` searches first lines only**, as the brief says, so `man -k ptytest` finds nothing, though `man ptytest` works.
The test programs' page says what each does.

**Pages are generated once, then plain files.** A throwaway script did the wrapping; it is not in the tree. They are written
from each command's source and `help` entry, not from what such a command does elsewhere. Things worth a reader's notice
that this turned up: `sleep` counts milliseconds; `mv` replaces a file at the target, while `cp` empties a target before it
writes it, so a failed `cp` has lost it; `hanoi` ends only at 0.

**`tests/test_man.py` derives names from the sources** (the Makefile's `USER_PROGS`, the table in `shell/main.c`, the one in
`builtins.c`), so a new tool with no page fails by itself. Section headings beyond the brief's five are allowed (a `sed`
may want `ADDRESSES`); the five keep the brief's order.

## For the integrator

- **Remove `NOT_HERE_YET` in `tests/test_man.py`** once every track is merged. It is the one list of names that live on other
  branches. `SUITE` stays, and from then on every tool of the brief must have a page. Emptying it on this branch fails, as
  it should; that was tried.
- **`rootfs/man/grep.txt` describes today's `grep`**, and says it has no groups or intervals. Text's stage 2 makes that
  false: its `-E` needs adding to this page, and a page of that name on `cli/text` will be an add/add conflict to resolve.
- **`CLAUDE.md` is stale on one point**, which I may not edit: its Debugging tips say an unplugged device "stays in the list
  as gone". `bdev_unregister()` takes it off the list at once, so the name can be used again; only its mounts stay. The
  `lsblk` page follows the code.
- **`CLAUDE.md`, docs:** `top`, `watch`, `man`, `rootfs/man/`, call 54 and the `make unittest` list are yours to add.
- **Files touched outside the brief's list, each as little as the work needs:** `include/surfos/syscall.h`, `sysimpl.h`,
  `kernel/syscall.c`, `kernel/task.c` (`fill_info()`), `user/libsurf/syscalls.c`, `user/include/surf.h`, and the line in
  `user/surfsh/builtins.c`. These are "what serves it" and `help`.

## Left out, and why

- **`help` in the ring-0 shell** has no pointer to `man`. A recovery console may have no initrd, and `man` with it, so the
  line could be false where it is printed.
- **A key to kill from `top`, a thread view, a `watch` option to highlight changes:** not asked for.
- **Other tools' `man -k` for shared names:** the brief says first lines.
- **`man` sections, `MANPATH`:** one manual in one place.

## Tests

| What | Checks |
|---|---|
| `test_top`: seconds, keys in pieces, characters, rows, readings, shares, orders, every row's text, 3,000 random task lists | 87,191 |
| `test_watch`: lines, characters and sequences cut at every byte, words, the title, random screens from 1x1 to 254x512, 16 MB with no line's end and 16 MB of nothing else | 34,873 |
| `test_manutil`: the table, names that lead out, page files, the word search | 1,380 |
| `tests/test_man.py`: every name's page, the page's shape, SEE ALSO | 15 tests |
| the smoke block (`echo cli-sys`) | 27 steps |

**Sanitizers.** The brief asked for `-fsanitize=address,undefined` where the host compiler has it. The coordinator
corrected that during the work: a `gcc -m32` sanitized binary hangs in the Docker image under qemu-i386, and it compiles
fine, so no probe can tell. No `-fsanitize` build is in `make unittest`. My first Makefile block did probe with one, and
that ran through one build in Docker without hanging before the correction came; it is gone. The three tests were run
natively on the Mac instead, all clean:

```
cc -g -O1 -Wall -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer \
   -Iuser/bin -idirafter user/include tests/host/test_top.c -o test_top
```

**Comparison with real tools: none.** `top` on Linux and macOS prints other columns of another machine's numbers, Linux's
`watch` draws with curses, and `man` here shares no format with roff. The brief's "where a real tool exists" does not reach
them, and nothing is compared.

**Checks shown to fail.** A test that cannot fail proves nothing, so each was broken on purpose: a page missing for a kernel
command, a program and a builtin; a line of 73 columns; a first line of the wrong shape; a SEE ALSO that leads nowhere; a new
program with no page; `NOT_HERE_YET` emptied early. Each failed the test. `JOB_KILL` taken out of `watch` left a `cat` running,
and the smoke step said so.

**By hand, once, not in the suite** (`tools/qemu-run.py`'s `Telnet` client against `telnetd`): `top`, its keys, `watch` and
`man` through `less` over a real telnet session at 80x24 and 40x10. No row went past a window. `ptytest run top` is in the
smoke block, and keeps a pseudo-terminal in it.

**On the finished tree:** `make unittest` passes (with `test_top`, `test_watch`, `test_manutil` and `test_man.py` in it),
`make test-all` passes through the lock with 545 checks at O0 and 545 at O2, and `make -C surfcpu test-os-qemu`, which the
change to `kernel/` calls for, passes 137 of 137 at O2 and at O0. This account's own results were written after those runs;
it is the only file that differs from the tree they ran on.

**`make test-usb` was not run:** nothing here touches the boot path, the console or the USB stack. Nothing has run on the
laptop, and nothing will until the owner tries it.

## Something unexplained

In the first run of the smoke block, the VGA dump printed after an unrelated failure showed the line
`SURF ≋> watch -t -n 0.2 uptime` on two rows, once. The serial transcript of the same run had it once, and that is every
byte the programs wrote. It never came back: 8 replays of the block, 4 of them side by side at a load of 10, and probes with
and without a full screen, with `q` and with Ctrl+C. No check looks at those rows, so it cannot make a test flaky. If it is
real it is in the console's path to VGA and not in these programs, which write one stream. It is recorded here rather than
left out; I did not find its cause.
