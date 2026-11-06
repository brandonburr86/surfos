# Account: the chat track of the desktop applications

Branch `apps/chat`. The brief is `2026-11-05-desktop-apps.md`. Nothing here has run on the laptop or on a real network: only
on the host, and in QEMU against a scripted server of the test harness. Passwords in the tests are made up.

The track works in three phases, as the integrator asked: the model alone (new files only), then the window against kit's
toolkit once kit's core and desk's window ABI are merged, then the wiring into the Makefile, the screen checks and the
documents once desk's implementation is merged. This account grows with each.

## What was built

### Phase 1: the model

| Files | What |
|---|---|
| `user/wchat/irc.c`, `irc.h` | IRC on the wire: lines cut from a server's bytes, parsed in place; case mappings; names; lines put together for sending, and refused; base64; CTCP |
| `user/wchat/ircfmt.c`, `ircfmt.h` | text for the screen: UTF-8 or windows-1252, controls and invisible marks out; the formatting codes as runs of one style, or left out; colours; rows |
| `user/wchat/chat.c`, `chat.h`, `chatint.h` | the session: rooms, their lines and nick lists, the connection's life, registration, what goes out and when |
| `user/wchat/recv.c` | everything a server sends, read as hostile |
| `user/wchat/input.c` | what is typed: messages, the eight commands |
| `user/wchat/link.c`, `link.h` | the frames between the window and its helper process (below), read as hostile on the window's side |
| `tests/host/test_wchat.c`, `tests/host/wchat_alloc.h` | the host tests (below) |

None of these files makes a system call or draws: the window hands in the time and the server's bytes and sends what the
model gives it, as `user/wclock/`'s does.

### Phase 2: the window, on kit's toolkit (merged: apps/kit 2ba18949, with desk's ABI)

| Files | What |
|---|---|
| `user/wchat/window.c`, `window.h` | the window on `gk.h`: the connect form, the rooms as tabs, the messages (an area the program draws), the nick list, the input line, the status bar, the menus; the helper's pipes watched with `gk_watch()` |
| `user/wchat/proc.c` | what is SurfOS's own: the helper started with `spawnx()` and two pipes, killed, collected; the clock's minute |
| `user/wchat/helper.c` | the helper: Undertow's transport, `poll()` on its descriptor and the window's pipe, the bytes relayed |
| `user/wchat/main.c` | the loop: `gk_next()` for as long as the model may wait, each event, then the window's tick; `--helper` |
| `tests/host/test_wchatwin.c` | the window on kit's fake desktop, the helper played by the test at the other ends of real pipes |

### Phase 3: the wiring (merged: apps/desk 2c5afd30)

| Files | What |
|---|---|
| `user/undertow/transport.c`, `transport.h` | one read-only accessor, `ut_transport_fd()`, added as fetch added `ut_transport_eof_clean()`: no line changed |
| `Makefile` | insertions only: the program (with Undertow's transport and Mbed TLS, as fetch), `unittest-chat` |
| `rootfs/apps/wchat.app` | Chat in Apps > Programs and on the desktop; its icon, two speech bubbles, drawn for it |
| `rootfs/man/wchat.txt` | the manual page |
| `docs/GUI.md` | Chat's section, before "Programs' windows" |
| `tools/gui_apps_chat.py`, `tools/gui_apps_scenario.py` | the guiapps scenario's checks (no network there), and the line that adds them |
| `tools/gui_apps_chatnet.py`, `tools/qemu-usb.py` | a scripted IRC server and the checks against it, in a boot of their own (`--scenario guichat`, inserted as `fsck`'s was) |

## Decisions a reviewer would ask about

### The connection is a helper's, not the window's

Undertow's transport (`user/undertow/transport.c`), which the brief says to use as `user/fetch/` links it, waits inside its calls:
up to 5 seconds to look a name up, 10 to connect and 20 for the TLS handshake. A window that called it would not answer its
close button for 35 seconds. So the connection belongs to a helper, `wchat --helper HOST PORT tls|tcp`, started by the window
with `spawnx()`, its descriptor 0 a pipe from the window and 1 a pipe to it, as Wundertow's helpers are. The helper relays bytes
and nothing else; the window parses IRC. What the helper sends is framed (`link.h`) and read as hostile, since its process has
taken a server's TLS records. A fault in TLS costs the connection, never the window and its rooms. The password goes through the
pipe inside the line that carries it (PASS or AUTHENTICATE), never in the helper's arguments or environment.

Undertow's transport fails a read that waits 15 seconds for a byte, which an IRC connection does all the time, so the helper reads
only when `poll()` says the connection has something. The transport kept its descriptor to itself; the integrator allowed one
read-only accessor, `ut_transport_fd()`, added to `transport.c` and `transport.h` without a line changed, as fetch added
`ut_transport_eof_clean()`. The helper reads with room for a whole TLS record (16 KB, Mbed TLS's default here), so nothing is left
inside TLS where `poll()` cannot see it; a record that comes in pieces is waited for within the transport's 15 seconds. A TLS 1.3
session ticket that comes alone makes the transport wait for the data after it, up to 15 seconds: servers send them right after
the handshake, when the IRC server's first lines follow at once. The transport's deadline is 20 days (its clock's differences hold
24): a connection that lasts longer ends and says so. Undertow's sentences about web pages are put in the Chat's words by
`link_words()` ("TLS refused" for "HTTPS refused", without the advice about `http://` addresses and Undertow's `clock` command).

The helper has its two pipes and nothing else: `spawnx()` gives it descriptors 0 and 1 and closes the rest, so it never touches the
desktop's console (no `ttyctl()`; what it has to say goes up the pipe as a frame). The window never waits on
it: it reads the pipe when `poll()` says so, writes at most a `PIPE_BUF` when `poll()` says that much fits (a pipe's POLLOUT means
4096 bytes of room), and stops watching for room as soon as nothing waits (the watch is level-triggered). A helper killed by a
fault keeps its pipe until it is collected, so while one runs the window looks every 250 ms whether it was, reads what it sent
before it died, and says the helper stopped. A write to a helper that has gone says EPIPE (`broken_pipe_exit(0)`) instead of ending
the window.

### The window

- **Two pages over a status bar.** The connect form, then the chat: the rooms as tabs (kit's `gk_tabs`, closable, their pages empty:
  the messages, the nick list and the input line are shared by all the rooms), the topic line, the messages, the nick list
  (`gk_list`), the input line (`gk_field` with `gk_field_history()`). The form's password field is a `GK_PASSWORD` field, wiped
  (`gk_field_wipe()`) once the model has the password, and when the kind of password goes back to None or the form is cancelled.
- **The history** of the input line is the field's (`gk_field_history()`, which kit built for this), not the model's: the model's
  own, written in phase 1 before the toolkit existed, did the same and was taken out, so that there is one.
- **The messages are an area the program draws** (`gk_area`), in rows of 16 pixels: the time, the nick in its form (`<nick>`,
  `* nick`, `-nick-`), the text wrapped at spaces under itself after the time. A line's rows are counted once, at the area's width
  less the scroll bar's room, so a bar coming or going never wraps them again, and the scroll bar is in rows. While the view is at
  the newest line it follows new ones; scrolled back it stays on the line at its top, also when the oldest lines go or the window's
  width wraps them anew. Switching rooms shows the newest lines of the other.
- **Colours:** the theme's (`gk_theme()`), the nicks in teal, ours muted, one that names us in coral on the band colour, events and
  the Chat's own lines muted, errors coral. A sender's colours are drawn as asked, and a sender's colour that cannot be read on what
  is behind it (BT.601 brightness less than 90 apart) in the theme's ink, never the theme's own colours.
- **The nick list** follows the room's by inserting and removing only the lines that changed (a merge of the two sorted lists), so
  what is selected and how it is scrolled stay. A double-click or Enter on a nick opens a query with it.
- **Tabs** follow the rooms by their ids (rooms are only added at the end or closed), never by a pointer, which a closed room's
  successor could share. A tab's label says how many lines came since it was shown, with `!` for one that names us.
- **Keys:** Enter sends; Page Up and Page Down scroll the messages (the field does not take them); Ctrl+Page Up and Ctrl+Page Down
  go round the rooms; Alt+1 to Alt+9 (desk's keys come with WIN_MOD_ALT, no Esc first) go to a room by its place; Ctrl+W, Ctrl+N and
  Ctrl+Q are the Chat menu's. Esc does nothing: it would be too easy to lose a connection by it.
- **Menus,** three short titles, for the bar's 80 cells at 640x400: Chat (Connect, Disconnect, Close room, Close Chat), View (the room before and
  after, Formatting), and About Chat in the desktop's Help. Connect is greyed while connected, Disconnect while not, Close room in
  the server's room.
- **Closing:** with nothing connected the close button closes at once; connected, it asks first (Close, Cancel), and Close sends
  QUIT and lets the window go when the connection has ended, or after the model's three seconds.
- **The window's size:** 760 x 440, at least 480 x 300, inside the laptop's 958 x 457. Its pixels are the desktop's budget's.

### Lines from a server

- **A line ends at LF.** A CR right before it is part of the line end; a CR anywhere else stays in the line and is taken out with
  the other controls. Ending lines at a lone CR as well would let anyone whose text a lax server passes on make one line read as
  two here, the second as if the server had sent it.
- **IRC_IN_MAX, 8703 bytes:** RFC 2812's 512 with IRCv3's 8191 bytes of message tags in front. A longer line is dropped whole, up to
  its LF, and so is a line with a NUL (no message may hold one). The server's room says how many were dropped, at most every 10
  seconds.
- **Parsing:** tags, prefix, a command of letters or three digits, 15 parameters, the 15th taking the rest with or without its
  `:` (RFC 2812 section 2.3.1). Spaces between the parts may be more than one, as RFC 1459's grammar has it.
- **Case mappings:** rfc1459, strict-rfc1459 and ascii, as the ISUPPORT draft defines them (`^` is the upper case of `~` under
  rfc1459; RFC 2812's text says the reverse, and servers follow the draft). Any other mapping is taken as rfc1459.
- **Nicks are ASCII here.** A nick from a server is kept in a list or names a room only when `irc_valid_nick()` takes it: no
  control, space, comma or `! @ * ?`, no `:` or channel type first, 64 bytes at most, nothing above 0x7e (RFC 2812's nicks are
  ASCII). A message from a nick that fails is shown in the server's room under its cleaned name, and changes no list.

### Text

- **UTF-8 or windows-1252, a whole text at a time.** IRC has no character set. A text that is all valid UTF-8 (RFC 3629: no
  overlong forms or surrogates) stays as it is; any other is read as windows-1252 byte by byte, as older clients send it. Mixing the
  two within one line would turn the halves of a character into two.
- **What goes:** C0 controls but the formatting codes, DEL, C1 controls, and the invisible marks that reorder or join what is shown
  (U+200B-U+200F, U+202A-U+202E, U+2060-U+2064, U+2066-U+2069, U+FEFF). A TAB is a space. ANSI escapes lose their ESC, so they
  show as text and do nothing.
- **Formatting codes** (bold, italics, underline, strikethrough, monospace, reverse, reset, colours by number and in hex) are kept
  in the lines, and the window shows them or leaves them out (`show_codes`). Colours 0-15 have the formatting document's names and
  our shades; 16-98 are six rows of twelve hues and eleven greys, computed. How the window draws them is under "The window".

### Registration

- **CAP LS 302 first,** then PASS when there is a server password, NICK and USER. The user name is the nick's letters and digits in
  lower case, the real name the nick. A server that knows no CAP answers 421 or 451, or registers at once; either is fine. A server
  that says nothing to CAP LS for 30 seconds gets CAP END.
- **SASL PLAIN only when asked for** in the connect dialog, and only when CAP LS offers `sasl` with PLAIN (or without a list of
  mechanisms). The account is the nick typed in the dialog, the authorization identity empty (RFC 4616). A server that says
  AUTHENTICATE + before PLAIN was asked for, or a second time, gets nothing; one that asks for anything else gets `AUTHENTICATE *`.
- **When SASL fails or is not offered, registration goes on** without the login, and the server's room says so as an error. The other
  choice, ending the connection, would leave a user on a network whose services are down with no way in at all; the line says
  plainly that they are not logged in.
- **Taken nicks:** at registration `_`, `__`, then `2` to `8` after the nick, within nine characters or the nick's own length. A 432
  (a nick the server will not have) is not guessed around: the room says to choose one with `/nick NAME`, and so it does when the
  guesses run out. A `/nick` typed during registration ends the guessing.

### The password

The password is used once. PASS goes in the first lines; SASL's goes when the server asks for it. Then the model's copy is wiped,
the queue slot that held the line is wiped as it is sent, and so are the buffers SASL built it in. Anything left of it goes when
the connection ends. A reconnection asks for it again. It is never shown, logged or written anywhere. A test looks for the
password's bytes in the whole model after each step. Without TLS the server's room says that the password goes unencrypted; the
connection is the user's choice (a bouncer on a home network), so it is allowed.

### What a server's lines do

- **Where lines go:** a channel's message to its room; a private message to its sender's query, made for it; a notice to the
  sender's query when there is one, else to the room in front (a services' answer to a command typed there); what the server says
  itself to the server's room; a numeric error (400-599) to the room in front, which is where the command that caused it was typed.
  A message for a channel with no room, or to a target that is neither a channel nor us, goes to the server's room with the target
  in brackets.
- **Highlights:** a channel message or action that names our nick as a word, under the case mapping, and every private message.
- **CTCP:** ACTION is an action. Every other request is shown in the server's room and never answered, so that nobody can make the
  client tell its version or time, or answer for them in a flood. A DCC offer is shown and nothing more: no DCC connection is ever
  made. A CTCP reply is shown as a notice.
- **PING:** answered at once, ahead of everything waiting; at most two PONGs wait at a time, so a flood of PINGs is answered twice.
- **Keepalive:** 90 seconds with no byte from the server, a PING; 60 more and the connection is lost ("the server has not answered
  for 150 seconds").
- **ISUPPORT (005)** is read, not shown: PREFIX, CHANTYPES, CHANMODES, STATUSMSG, NICKLEN, CASEMAPPING, NETWORK (with the
  draft's `\xHH` escapes), each checked; a value that fails leaves the one before. RFC 2812's RPL_BOUNCE, which has the same
  number, is shown.
- **MODE** changes the nick list's ranks; CHANMODES says which other modes take a parameter (A and B always, C when set), so a
  `+lh 10 bob` gives bob his half-op.
- **Rooms of channels:** our /part closes the room when the server's PART comes back; a PART we did not ask for, or a KICK, keeps the
  room and its lines, not joined. A JOIN nobody asked for (a server can make one) makes a room that does not come in front.

### What goes out

- **Pacing:** RFC 1459 section 8.10 describes how a server counts a client's lines: two seconds each, nothing more while the count
  is ten seconds ahead of the clock. The model counts the same way with eight, so a burst of four goes at once and then one every
  two seconds, and a long paste never gets the client thrown out for flooding. Registration's lines are not counted (servers count
  from the welcome; the first `/join` should not wait for them). PONG, QUIT and registration's lines jump the queue.
- **Every line going out** is made by `irc_compose()`, which refuses a CR, LF or NUL anywhere, a space or a leading `:` in any
  parameter but the last, and a line over 512 bytes. Names typed are checked as those from a server are, so `/join #a b` cannot
  smuggle a key and `/msg x\r\nQUIT` cannot exist (line ends typed become spaces first).
- **Long messages** are cut into PRIVMSGs that fit once the server puts `:nick!user@host` in front: our user and host as our JOIN
  showed them, or 10 and 63 bytes until then. Cuts are at spaces when that leaves half a line or more, and never inside a character.
- **The queue** holds 128 lines; a line typed beyond that is refused with a message. `/quit` drops what waits (the room says how
  many) so that QUIT goes at once.

### What is typed

- A paste of several lines is one message: line ends and tabs become spaces. Controls go but the formatting codes; `\x01` goes too,
  as CTCP is `/me`'s alone. 2048 bytes at most, cut between characters.
- `/join name` adds the server's first channel type when there is none. `/join` of a channel we are in brings its room in front.
- `/msg target text` shows the message in the target's room when there is one, else in the room in front as `→ target: text`.
- `/topic` with no text asks the server, whose answer is shown.
- `/quit` while a connection is being made cancels it.
- Up and Down bring back the lines sent, 64 of them: the input field's history (`gk_field_history()`), which keeps the line being
  typed when Up first leaves it and gives it back after the newest.

### Limits

| What | Limit | Past it |
|---|---|---|
| A line from the server | 8703 bytes | dropped, counted, said |
| A text kept | 2048 bytes | cut between characters |
| Rooms | 64 (the server's, channels, queries) | the server's room shows what would have gone there |
| A room's lines | 2000 | the oldest go |
| Every room's lines | 8 MB | the oldest of the room holding the most go |
| A channel's nick list | 8192 | not added; said once |
| Every nick list | 65,536 | not added |
| A nick, a channel's name | 64, 200 bytes | not taken |
| Lines waiting to go | 128, and 16 urgent | refused, said |
| History | 64 lines | the oldest goes |

Memory: at most about 8 MB of lines, 5 MB of nicks (65,536 of 64 bytes) and the model's own 100 KB, well inside the brief's 64 MB.

## What is left out, and why

- **DCC, scripts, opening links, keeping a password, logs:** the brief's "never". There are no files at all.
- **CTCP replies:** none, not even VERSION or PING (above).
- **IRCv3 beyond CAP and SASL PLAIN:** no capability is asked for but `sasl`. Message tags are taken and passed over, as is
  multi-prefix's NAMES form, should a server send them anyway. No server-time: a line's time is the PC's when it came.
- **Commands beyond the brief's eight:** no `/whois`, `/names`, `/mode`, `/raw`. What a server sends for them unasked is shown.
- **No automatic rejoin** after a reconnection: the rooms stay for the same server, not joined, and `/join` takes them up again. A
  connection to another server starts with the server's room alone.
- **No copying from the messages, no nick completion with Tab, no saved servers:** not in the brief. The input line copies and
  pastes as every field does (Ctrl+C, Ctrl+V: its first line).
- **The guichat scenario is not in qemu-usb.py's default list:** adding it there would change an existing line, which the brief's
  rule of pure insertion does not allow. It runs by name (`--scenario guichat`); the integrator may add it to the list.
- **TLS against a server the test trusts:** the screen checks' boots carry the production roots only, so TLS is shown refused (by
  a server that speaks none), not succeeding. TLS that succeeds is Undertow's transport's, which `make test-web` checks against its
  fixture servers with a test CA; the helper adds nothing to the handshake.

## The tests

`tests/host/test_wchat.c` (72,343 checks), built with the host's C library and the model's memory through `wchat_alloc.h`:

- lines cut from the server's bytes at every split; at the longest length and one byte over, with and without CR; a megabyte
  with no line end; a NUL;
- messages parsed, with tags, prefixes of every form, fifteen parameters and malformed ones; the case mappings; names; lines
  composed and refused at 512 bytes; RFC 4648's base64 vectors; CTCP;
- text: UTF-8 kept, windows-1252, holes, overlong forms and surrogates, controls and invisible marks, cuts at every size; the
  formatting codes as runs, stripped, the colours, readability, rows; 400 random texts wrapped at every width from 1 to 39
  (every character in a row, none over its width);
- registration: the dialog's checks, a server with no CAP, taken nicks through every guess, a refused nick, CAP in several lines,
  CAP not answered;
- SASL PLAIN to the byte, the longest password, not offered, no PLAIN, refused, failed, a server asking for something else, a
  server password with and without a space; after each, the password's bytes are looked for in all of the model's memory;
- channels, lists, ranks by MODE, topics, kicks, parts, quits, nicks, a case mapping that changes under the lists; messages,
  highlights, queries, notices, CTCP and DCC, STATUSMSG, bad nicks, escapes and windows-1252;
- PING and a flood of them; the keepalive; what is typed, every command, a paste of lines, long messages cut to fit (with our host
  known and not), not connected, not registered, a line over 2048 bytes; the pacing; quitting; the status, the title, the labels;
- a hostile server: 200 queries, a nick list longer than kept, 3000 lines in a room, 9000 long lines in 60 rooms, ISUPPORT that is
  nothing of the sort, every command short of its parameters; one session's bytes given whole, a byte at a time and cut at random
  (the same rooms every way); 300 rounds of 20 KB of random words and bytes cut at random, with the clock jumping, every list
  checked after each;
- a whole session with every allocation in it failing in turn: never a crash, nothing left allocated;
- the link's frames cut at every byte, and every way a frame can be wrong.

`tests/host/test_wchatwin.c` (263 checks): the window on kit's fake desktop, with the model as it is and the helper played by the
test at the other ends of real pipes (the test's `chat_spawn()`, `chat_kill()`, `chat_reaped()`, `chat_minute()` stand in for
`proc.c`):

- the form: its checks said in coral, TLS and the port following each other, SASL opening the password field, its dots, the
  password gone from the field, the window and the model once it was sent (looked for in all of their memory);
- the lines going out only as the pipe takes them; the status bar, the title, the menus greyed as the state says;
- a channel: its tab, topic, messages with their times, the nick list kept in place as nicks come, change and get modes; a
  highlight in coral on the band colour; a red word red, and ink with View > Formatting off;
- a query from a double-click on a nick; Alt+1; Ctrl+Page Down; a tab's count and `!`; a tab's `x` leaving the channel;
- scrollback: following the newest lines; Page Up; staying put while lines come; a resize wrapping anew; 2000 more lines with the
  view scrolled back (its lines gone: the oldest kept at the top);
- a helper that says nonsense, one that dies (its last words read first), one that says why it ended (Undertow's words made the
  Chat's), one that does not start, and the model's own ending (no answer to its PING);
- closing: at once when not connected; asking first when connected, Cancel, Close, QUIT, the window going with the connection's
  end; and going after three seconds when the server never closes.

**The screen** (`tools/gui_apps_chat.py`, in the guiapps scenario, which has no network): wchat started in the Terminal, its form,
the focus, an empty server refused in coral, TLS off moving the port, a connection that its helper cannot make (no network) said
in coral in the server's room, Ctrl+N bringing the form back, the close button, ps finding no wchat.
**With a server** (`tools/gui_apps_chatnet.py`, `--scenario guichat`: a boot with a USB Ethernet adapter, `dhcp` and `-cpu max`,
and a scripted IRC server on the Mac's loopback, which the guest reaches at 10.0.2.2; nothing leaves the computer): the form with
SASL and a made-up password shown as dots; CAP LS 302, NICK, USER, CAP REQ and the exact base64 of the PLAIN login at the
server; the welcome in the status bar; /join and the server's answer: the tab, the topic, the nick list, a message that names us
with a red word; a message typed reaching the server; a query from the nick list, and a message in it reaching bob; the close
button asking, QUIT reaching the server, the window going; then TLS to the same port, which is no TLS server: the handshake
reaches the server, fails, and the room says so; ps finding no wchat; the password never at the server as itself.

`helper.c`, `proc.c` and `main.c` are SurfOS's calls (spawnx, the pipes, Undertow's transport) and run only on SurfOS: the
screen checks exercise them (a connection made, one refused by TLS, one with no network, the helper collected), not the host
tests.

**Sanitizers:** by hand on the Mac with clang, `-fsanitize=address,undefined -fno-sanitize-recover=all`: test_wchat passes, and
`test_wchat 4000 SEED` (4000 rounds of the hostile server) passes for seeds 1, 2, 3, 0xdeadbeef and 12345, about 400 MB of hostile
input; test_wchatwin passes, built x86_64 (under Rosetta, as kit built the toolkit's: libgui's printf has an x86 `divl`) with kit's
toolkit and fake desktop. None of it is in `make unittest`, as the brief says. In Docker the same files built with `gcc -m32` pass
(`make unittest`), and the model builds for SurfOS at -O0 and -O2 with no warning.

## Where the code comes from

Written for this track from these documents, not from any program's source:

- RFC 1459 (sections 2.2, 2.3, 4, 6 and 8.10: messages, case, commands, numerics, a server's flood control) and RFC 2812
  (sections 2.3.1 and 5: the message grammar, the numerics);
- IRCv3: capability negotiation (CAP LS 302, REQ, ACK, NAK, END), SASL 3.1 (AUTHENTICATE, the 400-byte lines, numerics 900-908),
  message tags (their length);
- draft-brocklesby-irc-isupport (RPL_ISUPPORT, its tokens, the case mappings, `\xHH`);
- RFC 4616 (SASL PLAIN), RFC 4648 (base64), RFC 3629 (UTF-8);
- the WHATWG Encoding Standard's index of windows-1252;
- the IRC formatting document of ircdocs (modern.ircdocs.horse/formatting) for the codes and the colours' numbers and names, and
  the CTCP draft of ircdocs for `\x01` framing and ACTION;
- ITU-R BT.601's brightness weights, and the textbook HSL-to-RGB formula for the extended colours.

The window is built on kit's toolkit (`gk.h`) and its fake desktop, the helper's process on SurfOS's `spawnx()` and pipes as
Wundertow's loader uses them, and the connection on Undertow's transport: all of this tree. The icon is two speech bubbles drawn
here in the registry's palette.

Behaviour matched, not code: appending `_` to a taken nick is what IRC clients have long done.
