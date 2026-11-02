# Account: the net track of the command-line suite

Branch `cli/net`. The brief is `2026-11-01-cli-suite.md`. Nothing here has run on the laptop or on a real network: only
on the host, and in QEMU against SurfOS's own servers and scripted ones. How `make test-all` went is in the track's report,
not here, so that this file is part of the tree that was tested.

## What was built

| Files | What |
|---|---|
| `user/telnet/tnclient.c`, `.h` | the client's side of RFC 854: no system calls, host-tested |
| `user/telnet/session.c`, `tnsession.h` | the session's poll loop, Ctrl+]'s prompt |
| `user/telnet/main.c` | the command line, the connection, raw mode |
| `user/ftp/ftpclient.c`, `.h` | replies, commands, passive replies, what may be shown, names, wildcards, a typed password: no system calls, host-tested |
| `user/ftp/session.c`, `ftpsession.h` | the control connection, data connections, local files |
| `user/ftp/commands.c`, `ftpcommands.h` | the command table, mget, mput |
| `user/ftp/main.c` | the command line and the input loop |
| `tests/host/test_telnetc.c`, `telnetc/` | the module alone, against telnetd's module, and the program on a simulated kernel |
| `tests/host/test_ftpc.c` | `ftpclient.c` alone, against the host's `fnmatch(3)` |
| `tests/host/test_ftpcs.c`, `ftpc/` | the whole program on a simulated kernel: a scripted server, a terminal, a failing file system, a clock that jumps |
| `rootfs/man/telnet.txt`, `ftp.txt` | the manual |
| `Makefile`, `tools/qemu-run.py` | insertions only (`git diff` shows no line removed) |

`ftp` links `user/ftpd/ftp.c` as it is, for `ftp_ctl_feed` (the control connection's lines, Telnet taken out),
`ftp_parse_port`, and the text-mode line ends. `tnclient.h` takes its numbers and `struct tn_buf` from `telnet.h`.

## Decisions a reviewer would ask about

### Where this departs from the brief

1. **`reget` keeps a partial file.** The brief's rule is that a file that did not exist is deleted if the tool fails partway. `get`
   obeys it. `reget` does not, also when it started the file: `reget` exists to go on after a failure, and deleting what came
   would leave it nothing to go on with. So `get` is all or nothing and `reget` is the explicit request to keep what comes, which
   the manual tells people to use on a connection that may break. The one thing `reget` undoes: if the server sends more than
   `SIZE` left room for, it ignored `REST`, and what it added is cut off with `ftruncate`.
2. **The block has 32 rows, not about 25.** I merged it down from 48, and the third review's detached script added two. It takes
   a few seconds, a fraction of a percent of `test-all`. Each remaining row is something only the real kernel shows.
3. **One line outside `SMOKE`:** `run_smoke` gained `if callable(line): line = line(q)`, so a follow line can be a function, as
   `boot_step` already allows. Without it a block's lines cannot name a forwarded port. It is an insertion, and no other block changes.
4. **Sanitizers are not in `make unittest`,** by the coordinator's correction of the brief: a `-m32 -fsanitize` binary hung under
   the Docker image's qemu-i386. They run by hand with clang on the Mac (below). Before that correction this track ran one such
   probe in Docker, with no timeout; it returned, and left nothing behind.

### telnet

- **Who speaks first.** On port 23 the client offers its options at once. On another port it is silent until the server
  negotiates, so it can talk to a mail server by hand; `test_telnetc` checks that a server speaking no telnet gets only the line typed.
- **Line mode.** Until the server agrees to SUPPRESS-GO-AHEAD, keys wait in a line edited here (Backspace, Ctrl+U), as RFC 854's
  terminal does; otherwise a mail server would see DEL bytes. While the server does not ECHO, this end shows what is typed,
  as raw mode shows nothing.
- **ECHO is refused on this end, always:** two ends echoing each other would never stop. The terminal type (24) is refused too: it is
  not in the brief. A real Linux telnetd will therefore not learn `TERM`.
- **A hostile server cannot stop the loop.** Writes to it wait in a buffer and leave on POLLOUT. Nothing more is read from it while
  that buffer might not hold the answers (`TNC_ROOM`), so a flood waits in the connection and no answer is lost. Keys are always
  read, because Ctrl+] is the way out of exactly such a session: while the server takes nothing, keys are dropped (said once)
  and only Ctrl+] counts. POLLHUP with answers waiting lets them go, or `poll()` would answer at once for ever.
- **The terminal is put back twice.** The kernel ends raw mode, colours and the alternate screen when a task dies, and a kill relies on that. Every other way
  out ends with one fixed sequence, so that the last words land on the main screen: `ESC \` ends any string or sequence left open (it reaches the idle state from every
  state of `ansi_byte()` in `kernel/console.c`, read for this), then colours, the cursor and the main screen are set. An earlier version watched the stream for what
  to undo; the review showed forms it missed (`ESC ]` left open, `ESC ( [?1049h`, `ESC[?10490h`), and there will always be others. `ESC 7` before `?1049l` is there because xterm restores a
  saved cursor at `?1049l` even with the main screen showing, which with nothing saved is the top left. It goes only to a terminal, never into a file or a pipe.
- **The window has no signal,** so it is looked at once a second while NAWS is on.
- **Input from a pipe** is a file somebody wants delivered, not keys: it is read only while it fits with room for a read of the server's to spare
  (`TNC_KEYS_ROOM(PIECE) + TNC_ROOM(PIECE)`), and waits in the pipe, so nothing is dropped and the pipe never keeps the server from being read:
  only the server itself can, with negotiations whose answers it does not take (`TNC_ROOM`, above). That matters: a server whose
  output waits for us (a shell behind a telnetd, a router's command line, a long banner) may read nothing more until it has gone, and if we stopped
  reading it in turn the two would wait for each other. The first fix round read the pipe until 12358 bytes waited but the server only while 8231 did,
  and the review of `856c4d8` found that deadlock. Nobody is there to press Ctrl+], so a server that takes nothing for a minute ends the session with
  status 2, as does one that closes with input still to go. It is not echoed. At its end a last line with no line end goes as it is, and the session
  ends with the server's close or after 2 seconds with no byte either way.

### ftp

- **Standard output** holds only listings and the bytes of `get remote -`. Replies, `pwd`, `size`, `status`, `help`, the prompt and every message go to standard error, or `ftp host > file`
  would hold them mixed with the file. If standard output cannot be written, the run ends with status 2, also at a terminal.
- **A script stops at its first failed command,** with that command's status, and quits: a failed `cd` must not leave the next `delete` to run in the wrong folder, nor a failed `lcd` the next `mget`.
  `mget` leaving a file counts as a failure. At a terminal the next command is the user's.
- **What makes a file good:** a 2xx after the last byte and, in binary mode, at least as many bytes as `SIZE` said. Fewer is a failure even
  with a 226. More is accepted for `get` (a log that grew) and refused for `reget`. A 2xx is needed: end of the data alone is not.
- **Order:** the local file is opened only after the server's 1xx, so a 550 leaves nothing behind. A file that is there goes through
  `saveutil.h`, a new one is `O_CREAT | O_EXCL`. A device (`/dev/null`) is written, not replaced.
- **PASV's address is ignored, not refused.** The data connection always goes to the control connection's peer. Refusing would break
  servers behind NAT and the hairpin itself, where ftpd names 10.0.2.15 to a client that reached 10.0.2.2. curl does the same.
- **Ports below 1024 are refused** in a passive reply. Not in the brief; it mirrors ftpd's rule for PORT, so a reply cannot aim the
  client at a service.
- **mget** lists the pattern's folder and matches here, so the server's idea of wildcards does not count, and a name that does not
  match what was typed is never taken. Only a listed name's last part is used, for the remote path too, so `../../x` asks for `x`. `ftpc_safe_name()`
  goes past the brief: no leading `-` (it would be an option to the next tool), no trailing dot or blank (FAT drops them, so `.. ` could
  turn into something else), valid UTF-8 and no C1 controls. `*` and `?` do not match a leading dot.
- **mget never replaces a file unasked:** it asks at a terminal, and in a script leaves the file and says so (status 1). A server's listing
  should not be able to overwrite `surfshrc`-like files in a script. `get`, whose name the user typed, replaces.
- **The password** is read in raw mode from `/dev/tty`, also when the commands come from a pipe: the next line of a script is never taken
  for it. It goes straight to the socket from a buffer that is wiped, never through stdio, and is scrubbed from the server's answer in
  case it says it back. That covers the answer to PASS only: by a later reply the password has been wiped here, so a server that repeats it
  then is printed like anything else it says. Anonymous never asks.
- **Limits I could not fix here:** the kernel's raw-mode input ring keeps the keys in kernel memory until they are overwritten, and TCP's send buffer holds the `PASS` line
  in the same way until it is sent and overwritten. `tty_readline_secret()` wipes the keys, but that is for kernel commands, and `kernel/tty.c` is outside this track. No program can
  read that memory. A small kernel change could close the first if the owner wants it closed.
- **The password's prompt comes after raw mode is on,** so a key in that gap is not echoed.
- **A password is waited for as long as it takes,** and asked for only by a process with a terminal of its own. The first fix round gave it 2 minutes,
  which was wrong: at the limit raw mode ended, so keys typed later were echoed and read as a command (`no command` and the password, on the screen),
  or, from a script, read by the shell. The review's rule, which `own_terminal()` follows: `procinfo()` for this process's own pid, whose tty is -1
  without a terminal. I added a second condition, and this is why: on SurfOS as it is, `procinfo()` gives every live task a tty (`fill_info()` in
  `kernel/task.c`; `tty_for_console()` falls back to F1), so -1 alone never fires, while a detached task (the desktop's Run program..., a ring-0
  shell's `&`) still opens `/dev/tty` as its console's, whose keys go to someone else, and would wait in vain. So a detached task has no terminal
  of its own either, nor has a program that such a task runs: the third review found that no spawn passes `TF_DETACHED` on (`kernel/syscall.c`,
  `process.c`), so a script started by Run program..., or by the ring-0 shell's `spawn` or `&`, ran a `user` that opened the console's `/dev/tty`
  and waited for ever. `own_terminal()` now walks up the parents while the tty stays the same, and any detached one means no terminal. It stops
  where the tty changes, as telnetd is detached and its sessions' shells are on ptys of their own; a parent that ended has had its children given
  to init (`orphan_children()`), so the walk always reaches the top. Either way the login fails at once and says why; nothing is read and no
  `PASS` goes. `procinfo()` counts tasks from the
  list's head at each call, so a task collected during the scan moves the rest up one and the scan can pass over its own process (the kernel's
  comment on `task_info_all()` says so); then it would refuse a password for want of a terminal, as a `printf ... | ftp` whose `printf` is
  being reaped might. It scans again, four times at most.
- **Ctrl+C:** `TTY_INTR` is on only while a command runs. Left on at the prompt, a background `ftp` reading the terminal would not be stopped
  (a task with `TF_INTR` never stops). So Ctrl+C at the prompt ends ftp, and during a command it stops that command. From the moment its command
  has gone, a transfer sends ABOR **before** closing the data connection, since the end of the data alone would tell a server that an upload is whole. Any
  other command has no way back in step, so the connection closes. A Ctrl+C that comes before a command's first byte went stops it there, and the
  session stays as it was, in step (`NOT_SENT`): nothing is sent and nothing is closed. That includes one that `drain()` meets while it reads what the
  server said on its own before a command; the first fix round used that one up and sent the command anyway. A second Ctrl+C gets out of a stuck
  ABOR; one before ABOR or `NOOP` has gone closes the connection, the control connection first, as the data connection's end alone would tell a
  server taking an upload that the file is whole. One exception to `NOT_SENT`: after reget's REST has been answered, a Ctrl+C before the
  transfer's command goes closes the connection after all, as a server keeps REST for its next transfer (RFC 3659 5.4; SurfOS's ftpd does), and a
  `put` after it would go on from the reget's offset: the third review's probe left the server 1000 old bytes and 3000 new, said as 3000 bytes with
  status 0. Closing it is, rather than sending `REST 0`, because what a restart marker of 0 does to the next `STOR` differs from server to server.
  Scripts end with status 130. A script keeps `TTY_INTR` on from start to end, and
  the count is read after each command, so a Ctrl+C that came in no wait (an fsync, between commands, in `open`) still stops the next command.
- **After an abort, `NOOP`.** How many replies follow ABOR, and when, differs from server to server, and a late one taken for the next command's answer
  made a failed `delete` look done. A server answers in turn, so every reply before `NOOP`'s 200 belongs to the abort and is passed over: a 150 on its
  way, 426, 225, 226, and the 500 or 502 of a server with no ABOR. The first fix round took 500 and 502 for `NOOP`'s answer, so `NOOP`'s 200 went to the
  next command, and a `delete` of a missing file said `200 NOOP ok`. A 421 among them closes the connection. A server that does not answer `NOOP`
  within 5 s of its last reply, or sends 16 replies without it, has its connection closed, and that is said ("no answer to NOOP after ABOR").
- **What `NOOP` cannot do,** settled as the review asked: a server that answers `NOOP` before the abort's last reply (its 426 or 226 comes after the
  200), or answers `NOOP` twice, still hands a stale reply on to the next command, which then reports that reply as its own: the review's probes show
  `delete x` answered with the late 426, and with a second `200 NOOP again`. No `NOOP` scheme guards against that, this one included: replies carry no
  tag, and no wait that a correct server never needs could tell a late reply from the next command's answer. Nor can it tell a server that answers
  ABOR itself with 200, which is no reply RFC 959 gives ABOR: that 200 is taken for `NOOP`'s, and `NOOP`'s own 200 goes to the next command. A server
  that knows no `NOOP` (RFC 959 makes it part of the minimum) loses its connection after an abort.
- **A line cut by its length is refused,** not run: `delete` on what is left of a line could name another file.
- **What is bounded, and what is not.** A reply 120 s, a stalled data connection 60 s, a command the server will not take 30 s, mget's listing 5 minutes; replies 1000 lines
  and 64 KB, 16 preliminary replies; a listing kept for mget 4096 names and 256 KB, where it is stopped, and nothing is fetched from it. Nothing is fetched
  either from a listing that failed part-way: reset, stalled, over its time, or ended by a 4xx or 5xx after some names (`FS_CUT_SHORT`; the first fix round
  fetched from those). A flood with no line end is dropped as it comes and ends at the reply's time limit. **A file's transfer has no limit of its own,
  nor has a listing shown by `ls` or `dir`,** which may be going into a file: a byte every 59 s keeps one alive for as long as the server likes. That is
  inherent: a client cannot tell that from a slow link, so Ctrl+C is the way out. A password has no limit either (above). Time a pager holds the output
  at `--More--` counts against none of these.
- **`reget` does not go on past 2 GB,** because `ftruncate()` takes 31 bits there and what a server added could not be cut off again. With no `SIZE`, a server that ignores `REST` has the whole file added to yours,
  and nothing here can tell: both are in the manual.
- **`get` in ascii mode** reports the bytes that came, not the bytes written.
- **`put` from a file that gets shorter** while it is read is aborted, as its end would tell the server the file is whole. One that grows is sent as it is.
  A server that refuses part-way (552, its disk full) is reported as its refusal, with status 1: the size is looked at only when the file's own end
  stopped the upload. The first fix round looked first, and said "the file got shorter" with status 2. A server may also end the data connection
  first and say why only then (vsftpd's order on a full disk), so when the data connection ends early its reply is waited for, 5 s at most, before
  anything else: a 4xx or 5xx is its refusal, status 1, and no ABOR follows; a 2xx there is no success, as the server has less than the whole file
  (status 2); with no reply in time ABOR follows as before, and passes a later refusal over.

## Did any of this follow a specific program's source?

No function or table was copied or transcribed from another program, and I read no outside source: I had the repository and nothing else. Behaviour is another matter, and these come from my memory of other
programs. The code is mine in each case, but the rule is not new:

- **Line mode and local echo in `tnclient.c`:** keys are line-edited until the server agrees to SUPPRESS-GO-AHEAD, and shown here until it agrees to ECHO. That is the rule of netkit/BSD telnet's "kludge linemode", and I recalled
  its `getconnmode()` while designing this. Mine is two one-line tests (`tnc_by_key()`, `tnc_remote_echo()`) and an editor of my own (`tnc_edit_key()`): the same rule, not the same code.
- **Silence on a port other than 23** until the server negotiates: BSD telnet's rule, as its manual says.
- **`ls` as NLST and `dir` as LIST:** netkit ftp's split; the command names are the brief's. **Ignoring PASV's address:** what curl does by default, taken as a design choice. Neither shaped any code beyond that choice.

`option()` in `tnclient.c` is adapted from this repository's `user/telnetd/telnet.c`. `ftpc_glob()`'s retry from the last star is the common way to do it without recursion, and `ftpc_wipe()` the common volatile loop; neither is from a program I can name.
The standards used: RFC 854, 856, 857, 858, 1073, 1143 and 1123 for telnet; RFC 959, 1123, 2428 and 3659 for FTP; RFC 3629 for UTF-8; ECMA-48 for the escape sequences.

## Left out, and why

- Not in the brief, so not added: `close`, `bye`, `prompt`, `hash`, `!`, `.netrc`, active mode, TLS, `put -` (standard input holds the commands).
- A password in a script: the brief says it is asked for, so a script can only log in with a terminal at hand.
- telnet: the terminal type, LINEMODE, `send` of anything but `ayt`, and a way to send a literal Ctrl+].

## Tests

| Test | Checks | What |
|---|---|---|
| `test_telnetc` | 131206 | the module, telnetd's real module at every split, random bytes with exact buffers, the program |
| `test_ftpc` | 297 | `ftpclient.c`, with `fnmatch(3)` on random pairs |
| `test_ftpcs` | 28144 | the whole client against scripted servers, then 3000 rounds of hostile ones |
| the block in `SMOKE` | 32 rows | the real kernel: `telnet` to telnetd by the hairpin (NAWS as the session sees it, `status`, `send ayt`, `quit` from `less`, a kill from another session); to a server speaking no telnet; `ftp` to ftpd byte for byte; to `ScriptedFtp`: the password from a real pipeline with no echo, a script run by a detached task that must not ask, PASV naming another address, a low port, escape sequences, Ctrl+C, names that climb, a reset |

**The fix round.** An independent review of `69203a8` found no memory-safety bug reachable from a server, but silent data loss with status 0. Each finding got a test first, and each test was run on the unchanged client and
seen to fail, or to crash (the `strdup` use-after-free), before the fix. The review's own probes over real TCP then confirmed the fixes: its 240000 bytes through a pipe all arrive, `get x - > f` holds only the file, a failed `cd` sends no `DELE`. Two things in them needed
care: their `/dev/tty` is not pollable, so my copy of the shim was changed for that, and their server does not answer `NOOP`, so the probe of a late ABOR took the path that closes the connection until `NOOP` was added.

**The second fix round.** The review of `856c4d8` found no memory-safety bug, but three of the first round's fixes had brought regressions: the
pipe's deadlock, `NOOP`'s answer taken from ABOR's 500 or 502, and the 2-minute password. Three more: a 552 during an upload said as a short file,
mget fetching from a listing cut by a stall, a reset, its time or a 451, and `drain()` using up a Ctrl+C; and two nits, the message that named ABOR
for `NOOP` and the 5-minute limit on `ls` and `dir`. Each got tests first, run over connections whose writes queue only part of what is given, as
SurfOS's `tcp_write` does (3 to 1000 bytes at a time, mostly 5; 512 for telnet). On the unchanged client of
`856c4d8`, 20 checks of `test_ftpcs` and 2 of `test_telnetc` fail; on this tree, none. Tests that guard the new code without failing on `856c4d8`:
ABOR answered late, and twice (the reviewer asked for both), a second Ctrl+C before ABOR or `NOOP` went, and tasks collected while
`procinfo()` is read. That last is a race in this round's own `own_terminal()`, found while I looked into a failed run of `test-all`:
the scan could pass over its own process, and a single scan fails the test. The review's probes over real TCP agree: 3000 piped
lines of 1000 bytes to a server answering 30 KB each stopped after 29 on `856c4d8` (status 2) and all go now, 90 MB back, the server read at every
wait; `delete` after an abort on a server with no ABOR got `NOOP`'s 200 in 10 of 10 runs before and its own 550 in 10 of 10 now, with `NOOP` answered
after 0 to 3 s; a password typed after 140 simulated seconds logs in and the next line is the next command. My copy of the probes' shim gained
`procinfo()` and `getpid()`. A mutant found that one check had gone empty with the first round's `NOOP`: it looked for `NLST ABOR EPSV NLST` in a
row to prove that mget's next pattern was not listed after a Ctrl+C. It counts the `NLST`s now.

**The third fix round.** The review of `790203e` found three more, one of them a narrow race in the second round's own `NOT_SENT`:
a Ctrl+C as reget's REST was answered left REST at the server, so the next `put` went on from the reget's offset (the connection now
closes); an upload whose data connection the server ended before it said why (vsftpd on a full disk) was reported as a lost data
connection, ABOR and `NOOP` passing the 552 over (the reply is now awaited first); and a script run by a detached task asked for a
password on the console and waited for ever (`own_terminal()` now walks up the parents). Three nits too: ABOR answered with 200 among
the stale replies, the words "the server is always read", and the fuzz's upload floor with 9 rounds of slack. Each finding has tests
that fail on `790203e`, 8 checks of `test_ftpcs` over partial writes, beside guards that pass on both: a Ctrl+C before REST went or
while its answer was awaited; a 552 at once, none, or one past the bound; a telnetd session, whose detached server is on another
terminal. In QEMU the block now spawns a script with the kernel's `spawn`, detached as Run program... would, and its `user` must fail
at once: 12 of 12 in Docker, and with `790203e`'s ftp put into the initrd that check fails, and no other. The review's probes agree:
reget, that Ctrl+C, then `put` or `get` leave the connection closed and the server's file as it was; the four orders of a data
connection ended before a 552 all report the 552 with status 1 and no ABOR (`856c4d8`: status 2, ABOR sent); detached, tty -1 and four
scans that pass over this process ask nothing, and the rest ask.

**What the tests found earlier.** All of these were real bugs, fixed: `fs_put` closed its descriptors on a 150 (no upload ever worked); a reply read in one
piece with the 150 was not seen during a transfer; one Ctrl+C also stopped the ABOR that followed it; keys typed after the prompt's line were lost;
a too-long password said the wrong thing; `get x -` could put the file before what was said to it on a pipe; mget went on from a listing cut by Ctrl+C, and
past it to the next pattern; a Ctrl+C landing just before a transfer read the control connection was used up there, and the transfer went on (found by reading the code, and
then tested). And one the host tests could not find: `ftpc_parse_size` divided 64 bits, which links at O2 and not at O0, as programs have no libgcc. `make test-all` found that,
because I had built the programs only at O2; both levels are built now. Reading the code for timing after `test-all`'s next failure found two more: the password's prompt came
before raw mode, so a key in that gap would have been echoed (raw mode now comes first), and a Ctrl+C before the 150 dropped the connection instead of aborting the transfer.

**The Mac's QEMU does not predict `make test-all`,** which runs QEMU in the Docker image, under emulation. My block passed 12 of 12 on the Mac and failed `test-all` at O2. Under Linux, QEMU's user
network sometimes hands the guest a server's reset as a plain end of the data; the client then rightly waited for a final reply while `ScriptedFtp` waited for an ABOR, each for the other. That
was the harness, not the client: the server now sends its 426 after 3 seconds, and the check takes either path and still finds nothing left on the disk. The block then passed 12 of 12 in Docker, 6 at O2
and 6 at O0 with the O0 programs put into the initrd by hand, as `build/images` holds only the last level's. Anyone changing this block should try it there first.

**How far to believe them.**

- *Mutation:* 82 defences were broken one at a time in a scratch copy (unsafe names, the short-file check, the scrub, the low port, PASV's address, reget's cut,
  backpressure, raw mode, every fix of the three reviews, and so on). All 82 are caught. Two survived in the first fix round at first, and got better tests;
  one in the second (above). One survived before that: my flood test quit before any pressure built, and a new test closes that hole.
- *Fuzz depth:* the first fuzz was clean and nearly worthless, with data flowing in 263 of 3000 rounds. It now reaches 1625 of 3000, with uploads tried in 759
  (the same as at `856c4d8`, whose own build gives these figures; this file said 1637 and 1000 until this round, which were stale), and
  `test_ftpcs` fails if data flows in fewer than a quarter of the rounds, or uploads are tried in fewer than a fifth (a quarter left them 9 rounds
  of slack, as the third review pointed out). Its invariants: the user's file is as it was or whole, never a part; no `.tmp` left; no connection but the
  server's; no control character shown; the password nowhere. Three rounds in four run at a prompt, as a script now stops at its first failure and would get nowhere.
- *Line coverage* (clang, natively): `session.c` of ftp 96%, `commands.c` 99%, `main.c` 99%; telnet's `session.c` 97%, `tnclient.c` 99%. What is left is
  mostly a failed `read` of a local file, or an allocation failing in `names_add`.

**Sanitizers,** by hand on the Mac, from the worktree (ASan and UBSan, all three clean on the tree committed):

```
F="-g -O1 -w -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
clang $F -Iuser/ftp tests/host/test_ftpc.c user/ftp/ftpclient.c user/ftpd/ftp.c -o /tmp/test_ftpc && /tmp/test_ftpc
clang $F -Itests/host/ftpc -Dmain=ftp_main -c user/ftp/main.c -o /tmp/ftp_main.o
clang $F -Itests/host/ftpc tests/host/test_ftpcs.c user/ftp/ftpclient.c user/ftp/session.c user/ftp/commands.c user/ftpd/ftp.c /tmp/ftp_main.o -o /tmp/test_ftpcs && /tmp/test_ftpcs
clang $F -Itests/host/telnetc -Dmain=telnet_main -c user/telnet/main.c -o /tmp/tn_main.o
clang $F -Iuser/telnet -Itests/host/telnetc tests/host/test_telnetc.c user/telnet/tnclient.c user/telnet/session.c user/telnetd/telnet.c /tmp/tn_main.o -o /tmp/test_telnetc && /tmp/test_telnetc
```

`test_ftpcs` and `test_telnetc` take a number of fuzz rounds; 5000 is clean too. macOS has no `timeout`: `perl -e 'alarm 120; exec @ARGV' ./test` does.

**Comparison with real tools, where the brief asks for it.** No real ftp client or server is on the Mac or in the Docker image, and none is
in Python's standard library but `ftplib`, a client, so ftp is compared with nothing real but SurfOS's own ftpd. `fnmatch(3)` was compared, and two corners differ on purpose:
a backslash inside brackets quotes (as in surfsh and glibc; macOS's takes it as itself), and there are no `[.x.]` symbols (as in surfsh). Those patterns are left out of the
random comparison and pinned by a table. Homebrew's `telnet` is on the Mac, not in Docker. I tried it against a local listener, by hand, under a pty: it connected and
sent no negotiation at all, with or without a minus sign before the port, so **that gave no comparison, and I claim none.** I did not find out why, and stopped there as it
is optional. telnet's evidence is telnetd's real module and the real telnetd in QEMU.

**Not tested:** a real foreign server of either kind; a real network, DNS names (`net_resolve` leaves QEMU, so tests use addresses only); the
desktop's Terminal; a console's own keyboard for Ctrl+] (the harness types on the serial line; `keyboard.c` makes 0x1d from it by reading, not by a test here).
