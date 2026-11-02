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
2. **The block has 30 rows, not about 25.** I merged it down from 48. It takes about 5 seconds, a fraction of a percent of
   `test-all`. Each remaining row is something only the real kernel shows.
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
- **Input from a pipe** is a file somebody wants delivered, not keys: it is read only while it fits, and waits in the pipe, so nothing is dropped. Nobody is there to press
  Ctrl+], so a server that takes nothing for a minute ends the session with status 2, as does one that closes with input still to go. It is not echoed. At its end a last line
  with no line end goes as it is, and the session ends with the server's close or after 2 seconds with no byte either way.

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
  then is printed like anything else it says. With no terminal the login fails. Anonymous never asks.
- **Limits I could not fix here:** the kernel's raw-mode input ring keeps the keys in kernel memory until they are overwritten, and TCP's send buffer holds the `PASS` line
  in the same way until it is sent and overwritten. `tty_readline_secret()` wipes the keys, but that is for kernel commands, and `kernel/tty.c` is outside this track. No program can
  read that memory. A small kernel change could close the first if the owner wants it closed.
- **The password's prompt comes after raw mode is on,** so a key in that gap is not echoed. `/dev/tty` always opens on SurfOS (`fs/devfs.c`), so a detached task could wait for ever: a key must
  come within 2 minutes, or the login fails.
- **Ctrl+C:** `TTY_INTR` is on only while a command runs. Left on at the prompt, a background `ftp` reading the terminal would not be stopped
  (a task with `TF_INTR` never stops). So Ctrl+C at the prompt ends ftp, and during a command it stops that command. From the moment its command
  has gone, a transfer sends ABOR **before** closing the data connection, since the end of the data alone would tell a server that an upload is whole. Any
  other command has no way back in step, so the connection closes. A second Ctrl+C gets out of a stuck ABOR. Scripts end with status 130. A script keeps `TTY_INTR` on from start to end, and
  the count is read after each command, so a Ctrl+C that came in no wait (an fsync, between commands, in `open`) still stops the next command.
- **After an abort, `NOOP`.** How many replies follow ABOR, and when, differs from server to server, and a late one taken for the next command's answer made a failed `delete` look done. A server answers
  in turn, so everything before `NOOP`'s answer belongs to the abort and is passed over. A server that does not get there within 5 s has its connection closed, and that is said.
- **A line cut by its length is refused,** not run: `delete` on what is left of a line could name another file.
- **What is bounded, and what is not.** A reply 120 s, a stalled data connection 60 s, a command the server will not take 30 s, a listing 5 minutes, a key of a password 2 minutes; replies 1000 lines
  and 64 KB, 16 preliminary replies; a listing kept for mget 4096 names and 256 KB, where it is stopped, and nothing is fetched from it. A flood with no line end is dropped as it comes and ends at the reply's
  time limit. **A file's transfer has no limit of its own:** a byte every 59 s keeps one alive for as long as the server likes. That is inherent: a client cannot tell that from a slow link, so Ctrl+C is the way out.
  Time a pager holds the output at `--More--` counts against none of these.
- **`reget` does not go on past 2 GB,** because `ftruncate()` takes 31 bits there and what a server added could not be cut off again. With no `SIZE`, a server that ignores `REST` has the whole file added to yours,
  and nothing here can tell: both are in the manual.
- **`get` in ascii mode** reports the bytes that came, not the bytes written.
- **`put` from a file that gets shorter** while it is read is aborted, as its end would tell the server the file is whole. One that grows is sent as it is.

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
| `test_telnetc` | 131202 | the module, telnetd's real module at every split, random bytes with exact buffers, the program |
| `test_ftpc` | 297 | `ftpclient.c`, with `fnmatch(3)` on random pairs |
| `test_ftpcs` | 27964 | the whole client against scripted servers, then 3000 rounds of hostile ones |
| the block in `SMOKE` | 30 rows | the real kernel: `telnet` to telnetd by the hairpin (NAWS as the session sees it, `status`, `send ayt`, `quit` from `less`, a kill from another session); to a server speaking no telnet; `ftp` to ftpd byte for byte; to `ScriptedFtp`: the password from a real pipeline with no echo, PASV naming another address, a low port, escape sequences, Ctrl+C, names that climb, a reset |

**The fix round.** An independent review of `69203a8` found no memory-safety bug reachable from a server, but silent data loss with status 0. Each finding got a test first, and each test was run on the unchanged client and
seen to fail, or to crash (the `strdup` use-after-free), before the fix. The review's own probes over real TCP then confirmed the fixes: its 240000 bytes through a pipe all arrive, `get x - > f` holds only the file, a failed `cd` sends no `DELE`. Two things in them needed
care: their `/dev/tty` is not pollable, so my copy of the shim was changed for that, and their server does not answer `NOOP`, so the probe of a late ABOR took the path that closes the connection until `NOOP` was added.

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

- *Mutation:* 52 defences were broken one at a time in a scratch copy (unsafe names, the short-file check, the scrub, the low port, PASV's address, reget's cut,
  backpressure, raw mode, every fix of the review, and so on). All 52 are caught. Two survived in the fix round at first, and got better tests. One survived at first: my flood test quit before any pressure built, and a new test closes that hole.
- *Fuzz depth:* the first fuzz was clean and nearly worthless, with data flowing in 263 of 3000 rounds. It now reaches 1637 of 3000, with uploads in 1000, and
  `test_ftpcs` fails if that falls. Its invariants: the user's file is as it was or whole, never a part; no `.tmp` left; no connection but the
  server's; no control character shown; the password nowhere. Three rounds in four run at a prompt, as a script now stops at its first failure and would get nowhere.
- *Line coverage* (clang, natively): `session.c` of ftp 95%, `commands.c` 97%, `main.c` 100%; telnet's `session.c` 97%, `tnclient.c` 99%. What is left is
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
