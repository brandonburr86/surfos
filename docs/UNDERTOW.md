# Undertow: the SurfOS web browser

> **Draft, 10/11/26.** Written from the approved design
> ([the spec](superpowers/specs/2026-10-10-undertow-design.md)) and the brief for the
> parallel tracks ([the brief](superpowers/plans/2026-10-11-undertow-tracks.md)) while the
> browser's parts (TLS, HTTP, the renderer, the screen and its commands) were still being
> built. It will be checked against the merged program and finished then. Anything marked
> *(to be confirmed)* has not been seen working yet. The HTTP messages quoted below are those of
> the finished HTTP part (track H); the rest describes the design.

Undertow is SurfOS's text web browser. It fetches pages over HTTP and HTTPS, shows them as
readable text one screen at a time, numbers their links so that you follow one by typing its
number, and keeps a short history for back and forward. It runs on every SurfOS console:
the VGA or framebuffer screen, the serial line and telnet sessions.

It reads pages; it does not run them. There is no JavaScript, and no forms, logins,
cookies, downloads, images or CSS layout (see [What pages work](#what-pages-work)).

## Before you start: a network

Undertow uses whatever network SurfOS has. In QEMU, `dhcp` is enough. On the laptop, use a
USB Ethernet adapter (`dhcp`; [USB-BOOT.md section 8](USB-BOOT.md)) or Wi-Fi
(`wifi up`, `wifi join NAME`, `dhcp wlan0`). DHCP also gives the DNS server that names are
looked up with. A `host=` line in `undertow.conf` gives a name a fixed address instead
([below](#undertowconf)).

HTTPS also needs two things from the machine. First, strong randomness: the CPU's RDRAND
instruction. The laptop's Ice Lake processor has it (to be confirmed with `nettest entropy`
there); in QEMU, give the guest `-cpu max`. Second, a clock that is roughly right
([HTTPS](#https)).

In QEMU on this Mac: `./devenv/run.sh -cpu max -m 256`. The extra options go after the
script's own, and the later `-m` wins, so the guest gets RDRAND and room for pages.

## Starting it

```
undertow                          the welcome screen: what to type, the clock, whether HTTPS can work
undertow example.com              a name without a scheme is HTTPS: https://example.com/
undertow http://host:8080/path    any http:// or https:// address
undertow --dump ADDRESS           the page's text, then its links' addresses, numbered; nothing asked
undertow --help
```

The interactive browser needs a terminal on both its input and its output. For
redirection (`undertow --dump ADDRESS > page.txt`), use `--dump`: the text and the numbered
list of links go to standard output, and the status and any error go to standard error.
Its exit status is 0 for a 2xx page, 1 for anything else that loaded or failed (a 404, an
untrusted certificate, a name that does not resolve), and 2 for bad usage.

The shell limits a command line to 255 bytes. For a longer address, start `undertow` and
type `open ADDRESS` at its prompt: the prompt takes lines of up to 4096 bytes, and an
address may have 2048.

## The screen

```
Undertow - Example Domain                              [HTTPS verified TLSv1.3]
https://example.com/
--------------------------------------------------------------------------------
Example Domain

This domain is for use in illustrative examples in documents.

More information...[1]
--------------------------------------------------------------------------------
lines 1-5 of 5 (100%)   Enter: next  b: back  NUMBER: follow a link  ?: help
undertow>
```

*(Layout as the brief describes it; the exact rows are to be confirmed.)* The first line
holds the page's title and, at the right, how it came: `[HTTPS verified TLSv1.3]` (or
`TLSv1.2`), `[HTTP unencrypted]`, and the status when it is not 200 (`[HTTP 404 Not Found]`).
An error page (404, 500) is still shown, with its status. A 204 or 304 is an empty page, and
a 300 is a page too: Undertow follows only 301, 302, 303, 307 and 308. The second line is
the page's full address after any redirects. A link's number follows its label: `More information...[1]`. Each
load starts with a line `Loading ADDRESS ...`.

A view fills the terminal less one row, so it never trips the shell's `--More--` pager.
Undertow asks the terminal for its size before every view, so a resized telnet window gets
views of its new size. On a colour terminal the frame, the headings and the link numbers
are in colour. A terminal without UTF-8 (the kernel word `ascii`) gets ASCII stand-ins:
`"` for curly quotes, `-` for dashes, `e` for `é`, and `?` for anything else.

## Commands

| Command | What it does |
| --- | --- |
| `open ADDRESS` or `o ADDRESS` | Load an address (a name without a scheme is HTTPS) |
| `NUMBER` or `go NUMBER` | Follow link NUMBER. A link to a place on the same page jumps there without loading anything |
| `back` or `b` | The previous page, where you were reading it (not loaded again) |
| `forward` or `f` | The next page in the history |
| `reload` or `r` | Load the current address again (if that fails, the page you had stays) |
| Enter or `next` | The next screen |
| `prev` | The screen before |
| `top` | The start of the page |
| `links` | The page's links: number, label and address, a screen at a time |
| `find WORDS` | Find text from where you are, going round to the top once. ASCII letters match in either case |
| `findnext` | The next place the same text is found |
| `history` | The pages in the history, the current one marked |
| `url` | The full current address |
| `clock` | The UTC time HTTPS checks certificates against |
| `clock YYYY-MM-DD HH:MM:SS` | Set that time (UTC) for this session ([HTTPS](#https)) |
| `help` or `?` | The commands |
| `quit` or `q` | Back to the shell |

At the `undertow>` prompt: Left and Right, Home and End, Backspace, Ctrl+U (clear the line),
and Up and Down (this session's earlier commands). Ctrl+C at the prompt and Ctrl+D on an
empty line also end Undertow. Ctrl+C while a page loads stops the load and returns to the
shell, as for any other command. The connection is closed, and that browsing session ends.

A line over 4096 bytes is refused as a whole: none of it runs.

History keeps 16 pages, and at most 8 MiB of them together. When it is full, the oldest
page goes first, never the one you are reading. A failed load changes nothing: after `back`,
a failed `open` keeps the page you went back from, so `forward` still reaches it. Only a
page that loads replaces the forward history.

## HTTPS

Every HTTPS connection is checked, and nothing turns the checks off:

- the server's certificate chain must lead to one of the roots in `/initrd/etc/ssl/cert.pem`;
- the certificate must be for the name in the address (the name is also sent as SNI);
- the certificate must be valid now: not expired and not yet to come, by the UTC clock below;
- only TLS 1.2 and 1.3 are used, with ALPN `http/1.1`.

A check that fails stops the load with a message that says which one: the issuer is not
trusted, the certificate is for another name, it has expired, it is not valid yet.
Undertow never tries `http://` after an HTTPS failure. A redirect from `https://` to
`http://` stops with the address it pointed to, and you can `open` that address yourself if
you want it.

**The roots.** `/initrd/etc/ssl/cert.pem` is Mozilla's root store as curl publishes it
(the extraction of 2026-09-25, 121 roots, licence MPL-2.0). Its source, date and SHA-256 are
in `rootfs/etc/ssl/README`. It is part of the build and is never downloaded while
browsing. *(The file and its README come with the TLS work; to be confirmed after the
merge.)*

**Strong randomness.** TLS keys come from a generator seeded by the CPU's RDRAND only. On a
CPU without RDRAND, HTTPS refuses to start, and the welcome screen says so. Plain HTTP works
there as usual. Nothing falls back to a weaker source.

**The clock.** SurfOS has no time zone: it reads the hardware (CMOS) clock as if it kept
UTC, and so does Undertow, less `rtc_offset_minutes` from `undertow.conf`:

    UTC = the clock's reading - rtc_offset_minutes

QEMU's clock keeps UTC (offset 0). A PC whose clock was last set by Windows usually keeps
local time. For US Central Daylight Time (UTC-5) the offset is `-300`; for Central Standard
Time it is `-360`, and it changes with daylight saving. Check the laptop's clock before
relying on HTTPS there: compare `clock` in Undertow (or `date` in the shell) with the real
UTC time. A clock that cannot be right, before 2026-09-25 as after a flat battery, stops
HTTPS with a message saying how to set the time.

`clock YYYY-MM-DD HH:MM:SS` sets UTC for this run of Undertow; it then advances with the
uptime. It never changes the hardware clock or any file's time, and the next `undertow`
starts from the hardware clock again.

## undertow.conf

`/initrd/etc/undertow.conf` is read when Undertow starts. Besides blank lines and `#`
comments it has two kinds of line:

```
rtc_offset_minutes=-300          the hardware clock's offset from UTC in minutes, -840 to 840 (default 0)
host=printer.home 10.0.0.20      a fixed IPv4 address for a name, used instead of DNS (at most 16 lines)
```

A file with a mistake (an unknown setting, a value out of range, a line over 512 bytes)
gets a warning that names the line, and Undertow carries on with the defaults. The initrd
is read-only. To change the file, edit `rootfs/etc/undertow.conf` in the source tree and
build the image again (`./devenv/build-usb.sh`, or `tools/devloop.py deploy` for the
development stick).

## What pages work

**Shown:** HTML and XHTML pages and plain text, in UTF-8, windows-1252, ISO-8859-1 or ASCII:

- headings, paragraphs, lists, preformatted text;
- tables, one row per line with cells separated by ` | `;
- link labels, and images' alternative text in brackets (`[Logo]`);
- places on a page (`#section` addresses and links).

A page in another character set shows its ASCII text with a notice naming the charset.

**Shown, but not usable:** a form's controls appear as placeholders (`[text field]`,
`[button: Search]`) with the notice "forms are not supported". Links to `mailto:` and
other schemes keep their number, but following them only says why not. `<noscript>`
content is shown, since no script ever runs.

**Not supported:** JavaScript, cookies, logins, forms, downloads, images, CSS layout, audio
and video, tabs and bookmarks. Also: compressed responses (Undertow asks for none, and a
gzip body is an error that names it), IPv6 addresses, and names in other alphabets (their
`xn--` form works). Pages that build their content with JavaScript show only the HTML
their server sends.

| Limit | Value |
| --- | --- |
| An address | 2048 bytes |
| A response's header lines (its interim responses and trailers included; each redirect counts on its own) | 32 KiB |
| A response body | 1 MiB (a bigger one is refused, and nothing of it is kept) |
| Text shown for one page | 512 KiB (the rest is left out, with a notice) |
| Links on a page | 256 (later ones keep their text, without a number) |
| Redirects in one load | 5 |
| Looking up a name / connecting / the TLS handshake | 5 / 10 / 20 seconds |
| A read or write with no progress | 15 seconds |
| One whole load, redirects included | 60 seconds |
| History | 16 pages, 8 MiB |

## Updating the CA bundle and Mbed TLS

*(The procedure is a draft until the TLS work is merged. Its files are
`third_party/README.md`, `third_party/undertow-lock.json`, `tools/fetch-webdeps.py` and
`rootfs/etc/ssl/README`.)*

**The CA bundle** is a deliberate build input. To update it:

1. Download `cacert.pem` and its `cacert.pem.sha256` from <https://curl.se/docs/caextract.html>.
2. Check the checksum.
3. Replace `rootfs/etc/ssl/cert.pem`, and update `rootfs/etc/ssl/README` with the date,
   the SHA-256 and the number of roots.
4. Run `make unittest` and `make test-web`, then the public HTTPS check on the laptop
   or in QEMU.
5. Commit the bundle with its README.

**Mbed TLS** (3.6 LTS) is pinned by version, download address and SHA-256 in
`third_party/undertow-lock.json`. `tools/fetch-webdeps.py` downloads the official release
archive, checks it, and extracts what the build needs into `third_party/mbedtls/`, which is
not committed. `devenv/build.sh` runs it before every build. To move to a new release:

1. Read the release notes and the security advisories since the pinned version.
2. Change the version, address and checksum in the lock file, and record the advisories
   you checked.
3. Run the fetch, then `make unittest`, `make test-all`, `make test-web` and the UEFI USB
   suite, at `-O0` and `-O2`.

The library's configuration (TLS 1.2 and 1.3, client only, the algorithms) is
`user/undertow/tls_config.h`.

## Testing it

- `make unittest`: the host tests of every part (URLs, HTML, HTTP framing, TLS against
  in-memory peers, history and views), plus the web fixtures and the end-to-end harness's
  own logic (`make test-web-host`).
- `make test-web`: the real browser in QEMU against the deterministic fixtures of
  `tools/web-fixtures.py`. These are HTTP and HTTPS servers on this computer, with a test CA
  made for each run. The guest boots a test initrd with that CA and a host table. Several
  guests run: one with RDRAND, one without; clocks in UTC, in 2004 and 5 hours off;
  a broken `undertow.conf`; the production initrd; simulated Wi-Fi; a USB Ethernet adapter.
  They cover the commands, both TLS versions, every certificate failure, redirects,
  framing, sizes, charsets, escape sequences in pages, timeouts and interrupts. Transcripts
  are in `build/web-test/O<level>/`. The production images never hold the test CA.
  `tools/qemu-web.py --list` lists the scenarios, and `--scenario NAME` runs one.
- `tools/qemu-web.py --kernel build/O0/surfos.bin --initrd build/images/initrd.tar --interactive`
  starts the fixtures and a guest on this terminal, to try the browser by hand (Ctrl-A x
  ends it). `tools/web-fixtures.py serve` runs the fixtures alone.
- The public check (separate from the fixtures): a few public HTTP and HTTPS pages loaded in
  the guest with the production roots, recording the final addresses and TLS versions.

## When something goes wrong

| What you see | What to do |
| --- | --- |
| HTTPS cannot work: no strong randomness | The CPU has no RDRAND. In QEMU add `-cpu max`. HTTP still works |
| The clock is not plausible / a certificate "not valid yet" or "expired" everywhere | Check `clock`, then set `rtc_offset_minutes` or type `clock YYYY-MM-DD HH:MM:SS` (UTC) |
| The issuer is not trusted | The site's root is not in the bundle (or the site sends an incomplete chain). There is no bypass |
| The certificate is for another name | The address's name is not one the certificate covers. Check the spelling, and try the `www.` name |
| A name does not resolve | No DNS server (run `dhcp`), or the name does not exist. A `host=` line can give it an address |
| Timed out, or "the page did not load within 60 seconds" | The server stopped sending for 15 seconds, or the whole load (redirects too) took over 60 |
| "image/png is not text: Undertow shows HTML and plain text" | Images, PDFs and archives are not downloaded |
| "sent the page compressed (Content-Encoding: gzip)" | The server sent a compressed body although Undertow asked for none |
| "over 1 MB" | The page is bigger than Undertow takes; nothing of it is kept |
| "closed the connection before the end of the page" | The server or the network cut the page short. `reload` tries again |
| "more than 5 redirects" or "the page redirects in a loop" | The site sends Undertow round in circles. The message names the next address |
| "redirected from HTTPS to unencrypted HTTP" | Undertow stopped rather than drop the encryption. `open` the address it shows if you trust it |
| "did not answer with an HTTP status line" | Not a web server at that port, or one that speaks TLS: try `https://` |
