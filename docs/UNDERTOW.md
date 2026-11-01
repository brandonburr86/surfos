# Undertow: the SurfOS web browser

Undertow is SurfOS's text web browser. It fetches pages over HTTP and HTTPS, shows them as
readable text one screen at a time, numbers their links so that you follow one by typing its
number, and keeps a short history for back and forward. It runs on every SurfOS console:
the VGA or framebuffer screen, the serial line and telnet sessions.

It reads pages; it does not run them. There is no JavaScript, and no forms, logins,
cookies, downloads, images or CSS layout (see [What pages work](#what-pages-work)).

10/25/26: `wundertow` shows the same pages on the desktop, in a window, as a browser draws
them: headings, styles, links to click, lists, tables, scrolling (see
[In a window: wundertow](#in-a-window-wundertow)).

This guide describes the integrated browser (`user/undertow/`), including the 2026-10-15
review fixes (TLS time limits inside the TLS library's calls, a late `<base>`, random DNS
query IDs and ports, bare IPv4 addresses at the prompt). The messages
quoted are the program's own. What was tried on public sites is in
[Interoperability](#interoperability).

## Before you start: a network

Undertow uses whatever network SurfOS has. In QEMU, `dhcp` is enough. On the laptop, use a
USB Ethernet adapter (`dhcp`; [USB-BOOT.md section 8](USB-BOOT.md)) or Wi-Fi
(`wifi up`, `wifi join NAME`, `dhcp wlan0`). DHCP also gives the DNS server that names are
looked up with. A `host=` line in `undertow.conf` gives a name a fixed address instead
([below](#undertowconf)).

HTTPS also needs two things from the machine:

- **Strong randomness**: the CPU's RDRAND instruction. The laptop's Ice Lake processor has
  it (to be confirmed with `nettest entropy` there). In QEMU, give the guest `-cpu max`.
- **A clock that is roughly right** (see [HTTPS](#https)).

The welcome screen says whether both are there.

In QEMU on this Mac: `./devenv/run.sh -cpu max -m 256`. The extra options go after the
script's own, and the later `-m` wins, so the guest gets RDRAND and room for pages.

## Starting it

```
undertow                          the welcome screen: what to type, the clock, whether HTTPS can work
undertow example.com              a name without a scheme is HTTPS: https://example.com/
undertow http://host:8080/path    any http:// or https:// address
undertow --dump ADDRESS           the page's text and its links' addresses on stdout; nothing asked
undertow --help
```

The interactive browser needs a terminal on both its input and its output. Anywhere else it
says `undertow: reading pages needs a terminal; undertow --dump ADDRESS prints one` and
exits with 2.

`--dump` is for redirection (`undertow --dump ADDRESS > page.txt`):

- **Standard output:** the page's text, then a line `Links:` and one `[N] address` line for
  each link.
- **Standard error:** the status line, for example
  `undertow: https://example.com/: HTTP 200 OK, HTTPS verified TLSv1.3`, or the error.
- **Exit status:** 0 for a 2xx page, 1 for any other status (a 404) or a failure (an
  untrusted certificate, a name that does not resolve), and 2 for bad usage.

On a terminal, dump mode draws characters the way the browser does (see
[The screen](#the-screen)), without wrapping lines.

The shell limits a command line to 255 bytes. For a longer address, start `undertow` and
type `open ADDRESS` at its prompt: the prompt takes lines of up to 4096 bytes, and an
address may have 2048.

## The welcome screen

```
Undertow 1.0 - the SurfOS web browser
────────────────────────────────────────────────────────────────────────────────
To read a page, type open and its address:
  open https://example.com/
  open example.com     (a name alone means HTTPS)

On a page, a link's number follows it, Enter shows the next screen, back goes to the
page before and find WORDS searches it. help lists all the commands; quit leaves.

Clock  2026-10-11 18:02:11 UTC, from the hardware clock
HTTPS  is ready: strong random numbers, 121 trust roots
```

The `HTTPS` line uses the same readiness check as an actual load. It names a missing
RDRAND source, unusable roots or an implausible clock, and gives the relevant remedy.
HTTP does not need a certificate store or RDRAND. The `Clock` line shows the UTC value
and whether it came from the hardware clock, its configured offset or a session setting.

## The screen

```
Undertow - About the fixtures                           [HTTPS verified TLSv1.3]
https://fixture.test:56789/about
────────────────────────────────────────────────────────────────────────────────
About the fixtures

The fixture servers listen on 127.0.0.1 of the computer that runs QEMU; the
guest reaches them at 10.0.2.2, by the names its undertow.conf gives that
address.

Home[1] | The guide, part 1[2]
...
────────────────────────────────────────────────────────────────────────────────
lines 1-7 of 7 (100%)  end of the page  NUMBER: link  help: all commands
undertow>
```

**The title line.** It holds the page's title and, at the right, how the page came:

- `[HTTPS verified TLSv1.3]` (or `TLSv1.2`);
- `[HTTP unencrypted]`;
- in front of either, `[HTTP 404 Not Found]` and so on when the status is not 200.

**Statuses.** An error page (404, 500) is shown with its status. A 204 or 304 is an empty
page, and a 300 is a page too: Undertow follows only 301, 302, 303, 307 and 308.

**The rest of the screen:**

- the second line is the page's full address, after any redirects;
- a link's number follows its label: `Home[1]`;
- the position line gives the lines shown and where you are, then the keys, or a message
  from the last command, such as `"needle" is on the first line`;
- each load starts with a line `Loading ADDRESS ...`.

**Size.** A view fills the terminal less one row, so it never trips the shell's `--More--`
pager. The page gets the rows less 6 for its text, at least 1. Undertow asks the terminal
for its size before every view, so a resized telnet window gets views of its new size.
Lists (`links`, `history`, `help`) are paged by the shell's pager like any long output.

**Wrapping.** Text is wrapped to the terminal's width, at spaces, and inside a word longer
than a row. A list item's continuation rows are indented under its text.

**Colour.** On a colour terminal the frame, the headings and the link numbers are in
colour, never the page's own colours.

**Characters.** The SurfOS console draws what its character set holds: code page 437 (the
Latin-1 letters, box drawing, ½ ° ±, …) and its aliases (curly quotes and dashes, drawn as
ASCII). Everything else gets an ASCII stand-in: `(c)` for ©, `EUR` for €, `?` for
characters it has nothing for, such as Chinese or Japanese. A telnet session's terminal
gets every character as it is. With the kernel word `ascii`, every character but ASCII
gets its stand-in.

## Commands

| Command | What it does |
| --- | --- |
| `open ADDRESS`, `o ADDRESS` | Load a page (HTTPS unless the address says `http://`). An address typed alone (`example.com/news`, `10.0.2.2:8080`, anything with `://`) is opened too |
| `NUMBER`, `go NUMBER` | Follow link NUMBER. A link to a place on the same page jumps there without loading anything |
| Enter, `next` (PgDn) | The next screen |
| `prev` (PgUp), `top` | The screen before; the start of the page |
| `back`, `b` | The page before, where you left it (not loaded again) |
| `forward`, `f` | The page after it again |
| `reload`, `r` | Load this page again; if that fails, the page you had stays |
| `links` | The page's links, numbered, with their addresses |
| `find WORDS` | Find the words from this screen on, going round to the top once. ASCII letters match in either case |
| `findnext` | The next place they are |
| `history` | The pages kept for back and forward; `>` marks the one on the screen |
| `url` | The page's whole address, its status, how it came, its type and redirects |
| `clock` | The UTC time HTTPS checks certificates against |
| `clock YYYY-MM-DD HH:MM:SS` | Set it (UTC) for this session |
| `help`, `?` | The commands |
| `quit`, `q` | Back to the shell (Ctrl+C or Ctrl+D at the prompt too) |

**Editing keys at the `undertow>` prompt:**

- Left, Right, Home, End (Ctrl+A, Ctrl+E), Backspace, Delete;
- Ctrl+U clears the line, Ctrl+K deletes to its end, and Ctrl+W deletes the word before the
  cursor;
- Up and Down bring back the lines typed before.

A line longer than the row scrolls sideways rather than wrapping.

**Long and pasted lines.** A line over 4096 bytes is refused whole:
`undertow: the line is longer than 4096 bytes: it was not run`. Neither its start nor its
end runs. Keys typed while a page loads are dropped, so they never become part of the next
command.

**Ctrl+C during a load** stops it and returns to the shell, as for any other command. The
connection is closed, and that browsing session ends.

**History** keeps 16 pages, and at most 8 MiB of them together. When it is full, the oldest
page goes first, never the one you are reading. A failed load changes nothing: after
`back`, a failed `open` keeps the page you went back from, so `forward` still reaches it.
Only a page that loads replaces the forward history.

## HTTPS

Every HTTPS connection is checked, and nothing turns the checks off:

- the server's certificate chain must lead to one of the roots in `/initrd/etc/ssl/cert.pem`;
- the certificate must be for the name in the address (the name is also sent as SNI);
- the certificate must be valid now: not expired and not yet to come, by the UTC clock below;
- only TLS 1.2 and 1.3 are used, with ALPN `http/1.1`.

A check that fails stops the load with a message that says which one:

- `HTTPS refused: the certificate of HOST is not from a trusted issuer`
- `... is not for HOST but for another name`
- `... expired on DATE UTC (the clock says ...)`
- `... is not yet valid: it starts on DATE UTC ...`

Undertow never tries `http://` after an HTTPS failure. A redirect from `https://` to
`http://` stops with the address it pointed to:
`HOST redirected from HTTPS to unencrypted HTTP, which Undertow does not follow by itself; to read it unencrypted, open http://...`.

**The roots.** `/initrd/etc/ssl/cert.pem` is Mozilla's root store as curl publishes it: the
extraction of 2026-09-25, 121 roots, licence MPL-2.0. All 121 load. Its source, date and
SHA-256 are in `rootfs/etc/ssl/README`. It is part of the build and is never downloaded while
browsing.

**Strong randomness.** TLS keys come from a generator seeded by the CPU's RDRAND only.
Without RDRAND, HTTPS refuses to start: `HTTPS needs strong random numbers and this CPU has no
RDRAND instruction; http:// addresses still work (in QEMU, use -cpu max)`. Nothing falls back
to a weaker source.

**The clock.** SurfOS has no time zone: it reads the hardware (CMOS) clock as if it kept
UTC, and so does Undertow, less `rtc_offset_minutes` from `undertow.conf`:

    UTC = the clock's reading - rtc_offset_minutes

QEMU's clock keeps UTC (offset 0). A PC whose clock was last set by Windows usually keeps
local time. For US Central Daylight Time (UTC-5) the offset is `-300`; for Central Standard
Time it is `-360`, and it changes with daylight saving. Check the laptop's clock before
relying on HTTPS there: compare `clock` in Undertow (or `date` in the shell) with the real
UTC time. A clock that cannot be right, before 2026-09-25 as after a flat battery, stops
HTTPS: `HTTPS cannot check certificates: the clock says ..., which cannot be right`, followed
by how to set it.

`clock YYYY-MM-DD HH:MM:SS` sets UTC for this run of Undertow (`Clock set: ..., for this
session only.`); it then advances with the uptime. It never changes the hardware clock or any
file's time, and the next `undertow` starts from the hardware clock again. Bad times are
refused with the reason (`2026-02-30 is not a date`, `the month must be 01 to 12`).

## undertow.conf

`/initrd/etc/undertow.conf` is read when Undertow starts. Besides blank lines and `#`
comments it has two kinds of line:

```
rtc_offset_minutes=-300          the hardware clock's offset from UTC in minutes, -840 to 840 (default 0)
host=printer.home 10.0.0.20      a fixed IPv4 address for a name, used instead of DNS (at most 16 lines)
```

A file with a mistake (an unknown setting, a value out of range, a line over 512 bytes) gets
a warning that names the line:
`undertow: warning: undertow.conf line 3: unknown setting "colour"; the settings are the defaults`.
Undertow then carries on with the defaults. The initrd is read-only. To change the file, edit
`rootfs/etc/undertow.conf` in the source tree and build the image again (`./devenv/build-usb.sh`,
or `tools/devloop.py deploy` for the development stick).

## What pages work

**Shown:** HTML and XHTML pages and plain text, in UTF-8, windows-1252, ISO-8859-1 or ASCII:

- headings, paragraphs, lists, preformatted text;
- tables, one row per line with cells separated by ` | `;
- link labels, and images' alternative text in brackets (`[Logo]`);
- places on a page (`#section` addresses and links).

A page in another character set (Shift_JIS, say) shows its ASCII text, with a notice that
names the charset.

**Shown, but not usable:**

- A form's controls appear as placeholders: `[text field]`, `[button: Search]`,
  `[select: First option]`, `[checkbox]`, `[text area]`. The position line says `forms are
  not supported`.
- Links to `mailto:` and other schemes keep their number, and `links` marks them
  with the reason they cannot be opened. Following one says why it is not followed.
- `<noscript>` content is shown, since no script ever runs.

**Not supported:** JavaScript, cookies, logins, forms, downloads, images, CSS layout, audio
and video, tabs and bookmarks. Also:

- compressed responses: Undertow asks for none, and a compressed body is refused with
  `HOST sent the page compressed (Content-Encoding: gzip), ...`;
- IPv6 addresses;
- names in other alphabets (their `xn--` form works).

Pages that build their content with JavaScript show only the HTML their server sends.

| Limit | Value |
| --- | --- |
| An address | 2048 bytes |
| A response's header lines (its interim responses and trailers included; each redirect counts on its own) | 32 KiB |
| A response body | 1 MiB (a bigger one is refused, and nothing of it is kept) |
| Text shown for one page | 512 KiB (the rest is left out, with a notice at the end) |
| Links / named anchors on a page | 2048 each (later links keep their text, without a number) |
| Link labels, addresses and anchor names together | 1 MiB per page (later links keep their text, without a number; a `<base>` late in a page counts for the links before it too) |
| Redirects in one load | 5 |
| Looking up a name / connecting / the TLS handshake | 5 / 10 / 20 seconds |
| A read or write with no progress | 15 seconds |
| One whole load, redirects included | 60 seconds |
| History | 16 pages, 8 MiB |

## In a window: wundertow

`wundertow` (10/25/26) is Undertow on the desktop: the same pages, the same network and HTTPS, the same
settings, shown as a web browser draws them rather than as text. It is a program of its own
(`user/wundertow/`, `/initrd/bin/wundertow`): Undertow's HTTP, TLS, URLs and settings, and the same HTML
scanner (`user/undertow/html_scan.c`), with a page model, a layout and a painter of its own. The plan it
follows is `docs/superpowers/plans/2026-10-25-browser.md`.

**Starting it.** Its window is one of SurfOS's windows for programs (M12, `include/surfos/win.h`). On
the desktop (`gui`), type in its Terminal (Ctrl+T):

    wundertow &                         the start page
    wundertow example.com &             a page at once
    wundertow /initrd/www/index.html &  a file: the sample web site

or choose `wundertow` in Apps > Run program... (the launcher lists the programs in /initrd/bin). A window
started with `&` comes in front without the focus, as every program's does: a click on its title bar gives
it the focus. Typed without `&`, or started by the launcher, it has the focus at once.

A window opens only on the desktop of the terminal it was started from: on the serial console or over
telnet, `wundertow` says "no window". Anywhere, on any console, it does two things that need no window:

    wundertow --layout ADDRESS [WIDTH]        the page laid out WIDTH pixels wide (640), as text
    wundertow --render ADDRESS FILE [WIDTH]   the page painted as a BMP picture, for the Image Viewer

WIDTH is 64 to 4096 pixels (640); a picture is 8000 rows at most.

`wundertow --render https://example.com /data/example.bmp` on the laptop, then the picture opened in the
desktop's Image Viewer, shows the page as the window does.

**The window.** A toolbar (Back, Forward, Reload, the address field, Go), the page, a scroll bar and a
status bar, in the desktop's colours. The status bar names the link under the pointer, says Loading... while
a page loads, Done, or why a page failed, and at its right how the page came: "HTTPS verified TLSv1.3",
"HTTP: not encrypted" or "File". The window's title is the page's. Loading waits for the page, up to
Undertow's minute: the window says so first and goes on once the page is there.

**Keys and the pointer.**

| Key | |
|---|---|
| Ctrl+L | the address field, all of it chosen (typing replaces it) |
| Enter, Escape, Tab | in the field: load the address; give the page's back; return to the page |
| Up, Down | scroll 32 pixels |
| Page Up, Page Down, Space | scroll a screen |
| Home, End | the top, the bottom |
| Backspace | back |
| Ctrl+R | load the page again, at the same place |

A link is followed when the button is let go over it; the scroll bar's thumb can be dragged, and a click
above or below it scrolls a screen. The wheel scrolls once the desktop delivers it (track C's C8).

**Files.** An address that starts with `/` or `file:` is a file on this computer, read whole (1 MB at
most), HTML by its name (`.html`, `.htm`), text otherwise. A file page's links lead to files beside it; a
web page's links never lead to a file, whatever they say.

**What it shows.** Headings (h1 and h2 twice the size), paragraphs, bold, italic, underlined, struck-through
and monospaced text, sub- and superscripts, highlighted text, links in the link colour and underlined,
lists with bullets, numbers, letters or Roman numerals, quotations, preformatted text (its lines kept,
broken only at the window's edge), rules, line breaks, centred and right-aligned text, the page's colours
(`<body bgcolor text link>`, `<font color>`, the colours of tables, rows and cells), tables with their
columns sized to their content, a width, borders, padding and cells that span columns, and forms' fields as
boxes (shown, not used yet). Images are their alt text in a frame of their size until images are loaded.
The text is the console's font (code page 437); a character it does not have is shown as Undertow's ASCII
stand-in ("EUR", "(c)", "--"). What Undertow hides (scripts, styles, templates, SVG, MathML, objects), this
hides, and so the content of an element with the `hidden` attribute.

**Not yet** (the plan's later milestones): images, a Stop button and loading without waiting, finding in
a page, links by keyboard, forms that send, CSS, other fonts.

**Limits.** Undertow's (above), and 32,768 boxes and 131,072 runs of text a page; past any of them the page
stops there and the status bar says so. History: 16 pages, 8 MB.

**Its tests.** `make unittest` runs `test_wundertow_doc` (the page model: the fixtures in
`tests/fixtures/wundertow/`, `NAME.html` in, `NAME.tree` out, every page of Undertow's fixtures, the limits,
every allocation failing in turn), `test_wundertow_layout` (`NAME.layout` at 640 pixels, lines, margins,
lists, pre, tables, links under the pointer, painting and clipping, every page at widths from 64 to 1300)
and `test_wundertow_browser` (the window driven as a person drives it). The smoke test lays out a page over
HTTP and the sample site from a file, and paints a page to a BMP. `make test-web`'s scenarios `wundertow` and
`wundertow-production` lay out the fixture servers' pages over HTTPS (TLS 1.3 and 1.2, verified, a redirect
followed) and HTTP, and check that it refuses what Undertow refuses (an unknown root, another name, an expired
certificate, HTTPS to HTTP; the normal initrd trusts no fixture CA). `make test-usb`'s guiapps scenario
(`tools/gui_apps_web.py`) drives the window on the screen: started in the Terminal on pages of a data stick,
the focus given by a click on its title bar, the text, the link colour and the heading at twice the size
where the layout puts them, a link followed, Backspace, Page Down and Home, the corner dragged (the page
laid out again at the new width) and the close button. `test_wundertow_layout --render FILE OUT.ppm
[WIDTH]` paints any HTML file on the host.

## Updating the CA bundle and Mbed TLS

**The CA bundle** is a deliberate build input, never downloaded at run time. To update it
(`rootfs/etc/ssl/README`):

1. Download a newer dated `cacert-YYYY-MM-DD.pem` and its `.sha256` from
   <https://curl.se/docs/caextract.html>.
2. Check the file against its `.sha256`.
3. Replace `rootfs/etc/ssl/cert.pem`.
4. Update `rootfs/etc/ssl/README` (date, SHA-256, size, number of roots) and the `ca_bundle`
   entry of `third_party/undertow-lock.json`, in the same commit.
5. `tools/fetch-webdeps.py --check` compares the file with the lock.
6. Run `make unittest`, `make test-web`, and `tools/qemu-web.py --public` (below).

**Mbed TLS** (3.6.7, the long-term support branch) is never committed. It is pinned in
`third_party/undertow-lock.json` by the release archive's address, size and SHA-256.
`tools/fetch-webdeps.py` downloads it into `third_party/cache/`, checks it, and extracts
`include/`, `library/` and `LICENSE` into `third_party/mbedtls/`. `devenv/build.sh` runs it
before every build; offline, a checked archive in the cache or `--archive PATH` is enough. To
move to a new release (`third_party/README.md`):

1. Read its release notes and the security advisories since the pinned version.
2. Change the version, address, size and SHA-256 in the lock, and record the advisories
   checked.
3. Run the fetch.
4. Run `make unittest`, `make test-all`, `make test-web`, and the UEFI USB suite (`make
   test-usb`), at `-O0` and `-O2`.

The library's configuration is `user/undertow/tls_config.h`, and its platform code is
`user/undertow/tls_platform.c`. The extracted vendor files are unchanged: `fetch-webdeps.py --check` verifies every
one. A hash-checked build-directory copy of `ssl_tls13_keys.c` adds three cleanup calls
for partial TLS 1.3 transforms; the host and target compile the same copy. See
[the dependency notes](../third_party/README.md) for the patch and update procedure.

## Testing it

**`make unittest`:** the host tests of every part:

- URLs, the settings and the clock;
- HTML;
- HTTP framing and redirects against scripted servers;
- TLS against in-memory peers;
- history, views and the line editor;
- the web fixtures and the end-to-end harness's own logic (`make test-web-host`).

**`make test-web`:** the real browser in QEMU against the fixtures of
`tools/web-fixtures.py`:

- **The fixtures.** HTTP and HTTPS servers on this computer, with a test CA made for each run:
  good RSA and ECDSA chains, each bad certificate, servers that speak only TLS 1.2 or 1.3,
  and ones that break the handshake.
- **The test initrd.** Each guest boots a copy of the normal initrd with that CA and a host
  table. The production images never hold the test CA.
- **No public DNS.** Every guest's DNS server answers nothing, so no lookup leaves QEMU.
- **The guests:** with and without RDRAND; clocks in UTC, in 2004 and 5 hours off; a broken
  `undertow.conf`; the production initrd; simulated Wi-Fi with no route through Ethernet;
  a USB Ethernet adapter.
- **What it covers:** every command, both TLS versions, every certificate failure,
  redirects, framing, limits, charsets, escape sequences in pages, stalls and trickles,
  interrupts, 40 failures in a row, two consoles at once, an overlong line, and dump mode
  with its exit statuses.
- **Results.** Transcripts and the fixtures' log are in `build/web-test/O<level>/`.
  `tools/qemu-web.py --list` lists the scenarios, and `--scenario NAME` runs one.

**`make test`:** the smoke test, which loads a page from the harness's own HTTP server, by
`--dump` and in the browser. `make test-usb`'s USB networking scenario does the same over
the USB Ethernet adapter.

**Trying it by hand:**

- `tools/qemu-web.py --kernel build/O0/surfos.bin --initrd build/images/initrd.tar --interactive`
  starts the fixtures and a guest on this terminal (Ctrl-A x ends it);
- `tools/web-fixtures.py serve` runs the fixtures alone.

**`tools/qemu-web.py --public`:** the public interoperability check. It loads public pages
through the Internet with the production roots. It is never part of `make test-web`.

## When something goes wrong

| What you see | What to do |
| --- | --- |
| `HTTPS needs strong random numbers and this CPU has no RDRAND instruction` | In QEMU add `-cpu max`. HTTP still works |
| `HTTPS cannot check certificates: the clock says ..., which cannot be right` | Set `rtc_offset_minutes`, or type `clock YYYY-MM-DD HH:MM:SS` (UTC) |
| `... expired on ...` or `... is not yet valid ...` everywhere | The clock is off: check `clock` against the real UTC time |
| `HTTPS refused: ... is not from a trusted issuer` | The site's root is not in the bundle, or the site sends an incomplete chain. There is no bypass |
| `HTTPS refused: ... is not for HOST but for another name` | Check the spelling, and try the `www.` name |
| `the name server did not answer for NAME within 5 seconds` / `NAME was not found (DNS)` / `no network to look up NAME with (is it connected? try dhcp)` | No DNS server, or the name does not exist. A `host=` line can give it an address |
| `HOST (ADDRESS) refused the connection to port N` / `... did not answer on port N within 10 seconds` | Nothing listens there, or the machine is not reachable |
| `HOST sent nothing for 15 seconds` / `the 60-second limit for loading a page ran out ...` / `the page did not load within 60 seconds` | The server stopped sending, or the whole load (redirects too) took over 60 seconds. `reload` tries again |
| `the TLS handshake with HOST took more than 20 seconds` / `HOST does not answer in TLS (a broken handshake: is it an https server?)` | Not an HTTPS server at that port, or a very slow one. Try `http://` if the site serves it |
| `... is not text: Undertow shows HTML and plain text` | Images, PDFs and archives are not downloaded |
| `HOST sent the page compressed (Content-Encoding: gzip), ...` | The server ignored Undertow's request for an uncompressed page |
| `the page from HOST is over 1 MB, more than Undertow loads` | Nothing of it is kept |
| `HOST closed the connection before the end of the page` | The server or the network cut the page short. `reload` tries again |
| `more than 5 redirects (the next was to ...)` / `the page redirects in a loop (back to ...)` | The site sends Undertow round in circles |
| `... redirected from HTTPS to unencrypted HTTP ...; to read it unencrypted, open http://...` | Undertow stopped rather than drop the encryption. `open` the address shown if you trust it |
| `HOST did not answer with an HTTP status line` / `HOST reset the connection` | Not a web server at that port, or one that speaks TLS: try `https://` |
| `undertow: cannot open ADDRESS: URL contains a space` (and the other address errors) | Fix the address. Spaces go as `%20`; IPv6 and user names in addresses are not supported |
| `there is no link N: this page has M links, ...` | `links` lists them |

## Interoperability

`tools/qemu-web.py --public` loads public pages in QEMU. This is separate from the
deterministic suite: it is the one check that uses the Internet and public DNS.

**Setup.** The production initrd and roots, `-cpu max`, QEMU's user network with DHCP, and
`undertow --dump ADDRESS > /dev/null` for each page. The time runs from the command to the
shell's prompt.

The 2026-10-15 review fixes were validated with the deterministic suites only (see
[the tracks brief](superpowers/plans/2026-10-11-undertow-tracks.md)); the public check
below has not been repeated on that revision.

### 2026-10-13 03:46 UTC (completion validation)

The combined Undertow implementation was checked again using the frozen optimized
build and unchanged production initrd. 7 of 7 public addresses loaded successfully. Both unencrypted HTTP and verified HTTPS (TLS 1.2 and TLS 1.3) worked with the production roots.

| Address | What | Final address | Status line | Exit | Time |
| --- | --- | --- | --- | --- | --- |
| http://example.com/ | plain HTTP | http://example.com/ | HTTP 200 OK, HTTP unencrypted | 0 | 0.1 s |
| http://neverssl.com/ | plain HTTP, a site that never redirects to HTTPS | http://neverssl.com/ | HTTP 200 OK, HTTP unencrypted | 0 | 1.2 s |
| https://example.com/ | HTTPS | https://example.com/ | HTTP 200 OK, HTTPS verified TLSv1.3 | 0 | 0.3 s |
| https://en.wikipedia.org/wiki/Surfing | a large HTTPS page | https://en.wikipedia.org/wiki/Surfing | HTTP 200 OK, HTTPS verified TLSv1.3 | 0 | 1.2 s |
| https://tls-v1-2.badssl.com:1012/ | a server that speaks TLS 1.2 only | https://tls-v1-2.badssl.com:1012/ | HTTP 200 OK, HTTPS verified TLSv1.2 | 0 | 0.3 s |
| https://text.npr.org/ | a text-friendly news site | https://text.npr.org/ | HTTP 200 OK, HTTPS verified TLSv1.3 | 0 | 1.2 s |
| https://lite.cnn.com/ | another text-friendly news site | https://lite.cnn.com/ | HTTP 200 OK, HTTPS verified TLSv1.3 | 0 | 0.7 s |

The preceding local-date completion run was 2026-10-12 in America/Chicago.
The older observations below are retained as history, not current verification.

### 2026-10-11 18:54 UTC (-O2 build, QEMU 11.1 on the Mac)

| Address | Final address | Status line (stderr) | Exit | Time |
| --- | --- | --- | --- | --- |
| http://example.com/ | http://example.com/ | HTTP 200 OK, HTTP unencrypted | 0 | 0.1 s |
| http://neverssl.com/ | - | `neverssl.com (34.223.124.45) did not answer on port 80 within 10 seconds` | 1 | 10.1 s |
| https://example.com/ | https://example.com/ | HTTP 200 OK, HTTPS verified TLSv1.3 | 0 | 0.3 s |
| https://en.wikipedia.org/wiki/Surfing | https://en.wikipedia.org/wiki/Surfing | HTTP 200 OK, HTTPS verified TLSv1.3 | 0 | 1.1 s |
| https://tls-v1-2.badssl.com:1012/ (TLS 1.2 only) | https://tls-v1-2.badssl.com:1012/ | HTTP 200 OK, HTTPS verified TLSv1.2 | 0 | 0.5 s |
| https://text.npr.org/ | https://text.npr.org/ | HTTP 200 OK, HTTPS verified TLSv1.3 | 0 | 0.4 s |
| https://lite.cnn.com/ | https://lite.cnn.com/ | HTTP 200 OK, HTTPS verified TLSv1.3 | 0 | 0.8 s |

**Wikipedia.** The first lookup of en.wikipedia.org worked. An earlier run timed out once
after 5 s, when the resolver sent its query only once; `net/dns.c` now sends it again after
1 s and 3 s.

**neverssl.com (historical observations).** The failures also occurred from the Mac,
so this run did not isolate a SurfOS defect. These observations do not establish the
root cause: the server, network path and client remain possible contributors.

- From the Mac at the same time, 3 of 5 connections to 34.223.124.45:80 timed out at 15 s
  and the others took 7.1 s and 11.1 s. Two earlier ones took 4.1 s each.
- In the guest, the browser and the kernel's own `nettest connect` sometimes connected
  (one `--dump` gave `HTTP 200 OK, HTTP unencrypted`) and sometimes reached the 10-second
  bound.

**Not yet checked:** the physical laptop. HTTPS there also depends on its clock (see
[HTTPS](#https)).
