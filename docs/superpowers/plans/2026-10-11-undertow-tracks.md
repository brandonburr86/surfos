# Undertow: the parallel tracks (tasks 4-8)

Brief for finishing Undertow, SurfOS's text web browser, on branch `codex/undertow`.
The approved design is [the spec](../specs/2026-10-10-undertow-design.md) and the task list
[the plan](2026-10-10-undertow.md); this brief splits what is left into tracks that run at
the same time and fixes the interfaces between them. Where this brief is more precise than
the plan, this brief wins; where it is silent, the spec and plan apply.

## Completion and next steps (2026-10-12, America/Chicago)

The implementation of tasks 1–8 is integrated on `codex/undertow`: DNS, strong
entropy, URL handling, verified TLS 1.2/1.3, HTTP, HTML rendering, history and
commands, fixtures, packaging, documentation and CI. The later 2048-link and
2048-anchor limits and separate 1 MiB string budget supersede the original 256
limits below. This section supersedes the work-in-progress state recorded below;
the original track briefs remain as the implementation history.

Final combined-source validation used separate frozen exports at both optimization
levels (the runs continued into 2026-10-13 UTC):

| Suite | O0 | O2 |
| --- | --- | --- |
| System smoke | 295 passed | 295 passed |
| Files integration | 51 passed | 51 passed |
| Deterministic browser | 59 passed | 59 passed |
| Full USB/UEFI | 261 passed | 261 passed |

All host unit suites passed. Browser host checks: URL 382, utility/configuration 72,
HTTP 835, HTML 610 (also 610 with 32-bit UBSan), session 78, view/editor 127, commands
75 and transport 376. Native ASan/UBSan passed for HTML, session, view and transport.
Transport fault injection covered every allocation in TLS 1.3 (9853), TLS 1.2
(12616), setup (13) and plain HTTP (1), with no leak allowance. The explicit
build-time Mbed TLS cleanup patch leaves the pinned vendor extraction unchanged;
see [dependency provenance](../../../third_party/README.md).

The browser suites verify production/test-root isolation, certificate refusal,
clock and entropy failures, limits, repeated failure cleanup, cancellation,
simultaneous consoles, and HTTP/HTTPS over simulated Wi-Fi and USB Ethernet.
The separate current public interoperability results are in the
[user guide](../../UNDERTOW.md#interoperability). Machine-readable checks,
transcripts, source hashes and a current console screenshot are retained locally
under `build/takeover-evidence/`; this is generated evidence, not tracked source.

The remaining handoff steps are independent review of the complete browser branch,
integration into `ai-dev`, and a separately authorized physical laptop boot. The
release image is built with `devenv/build-usb.sh` after the final code commit;
its manifest, build ID, SHA-256, 121-root bundle, browser binary, pinned Wi-Fi
firmware and licenses must match that revision. Physical laptop browsing remains
unverified; QEMU/OVMF results do not establish physical-device acceptance.

## Where things stand (2026-10-11)

| Task | State |
| --- | --- |
| 1 DNS for programs (`net_resolve`, SYS_resolve 44) | Done, reviewed (f1fe0d1) |
| 2 Strict entropy (`getentropy`, SYS_getentropy 45) | Done, reviewed (5e0f0ed) |
| 3 URL parsing and resolution (`user/undertow/url.c`) | Done (aee6d30) |
| `ai-dev` (file browser, framebuffer graphics) | Merged into this branch |
| Foundation: shared headers, util.c, config.c, stand-ins, Makefile | This commit |
| 4 TLS and transport | Track T |
| 5 HTTP | Track H |
| 6 HTML rendering | Track R |
| 7 Session, screen, commands | Track S |
| 8 Fixtures, QEMU browser tests, packaging, docs | Track F (fixtures and harness now; the end-to-end suite after the merge) |

## Rules for every track

- Work only in your own worktree and branch (`ut/<track>`), created from the foundation
  commit. Build with your own Docker volume: `SURFOS_VOLUME=surfos-ut-<track>`; never the
  default volume, never another track's. Run guests with the host's QEMU
  (`/opt/homebrew/bin/qemu-system-i386`, `qemu-system-x86_64`) against exported images.
- Touch only the files your track owns (table below). Shared headers (`undertow.h`,
  `url.h`, `config.h`, `transport.h`, `http.h`, `html.h`) are fixed: if one must change,
  make the smallest additive change, say so in your report, and never change a meaning
  another track relies on. In the Makefile, `tests/host/undertow/surf.h` and
  `tools/qemu-run.py`, add your lines under your own marker comment only.
- Follow `CLAUDE.md`: the 2004 header block with the current date on new files; small
  commits, one topic each; every commit builds, passes `make unittest`, and passes the full
  smoke test at -O0 and -O2 (`make test-all`, or its native equivalent below) before it is
  committed. A track that changes libsurf, the kernel, or anything the laptop profile uses
  also runs the UEFI USB suite at -O0 and -O2. End commit messages with the attribution
  line in use on this branch.
- Never write a disk device, never touch the laptop (10.0.0.4) or the owner's sticks, never
  push, never use public DNS or Internet sites in a test (the one public interoperability
  check in track F is separate and labelled as such). Never put a real password anywhere.
- Code: C89-ish GNU C as in `user/`, 4-space indent, `if(`, comments that say why. Every
  function that can fail returns -1 with a bounded, terminal-safe message (`ut_error()`),
  leaves its outputs unchanged on failure, and frees what it took. Big objects go on the
  heap. Untrusted bytes (anything from the network) are never printed raw: `ut_safe_text()`
  or the renderer's safe text first.
- Tests come first (show the failing run), then the code. Host tests also run clean under
  `-fsanitize=address,undefined` on the Mac (clang, native, not -m32) where they can.
- Your final message is your report: what you built, the commits (hashes), every test run
  with its counts, anything you could not do, and every change you made outside your files.

### Building and testing natively

```sh
SP=/private/tmp/claude-501/-Users-brandonburr-Documents-SurfOS-Restoration-Project/f605e446-4d25-4727-a175-82270540f717/scratchpad
$SP/bin/ut-build.sh $PWD surfos-ut-<track> 0 <tag> all images unittest   # export: build/<tag>-O0/
$SP/bin/ut-build.sh $PWD surfos-ut-<track> 2 <tag> all images            # export: build/<tag>-O2/
$SP/bin/ut-smoke.sh $PWD <tag> 0     # make test's suites (smoke + file browser) at -O0: build/<tag>-O0-smoke.log
$SP/bin/ut-smoke.sh $PWD <tag> 2
$SP/bin/ut-build.sh $PWD surfos-ut-<track> 0 <tag> usb-image && $SP/bin/ut-usb.sh $PWD <tag> 0   # when required
```

`ut-build.sh` runs `devenv/build.sh -j8` with your volume and exports to `build/<tag>-O<n>/`
(the volume keeps both levels; `build/images/` in an export is the level built last, which
is why each level has its own export). A smoke run takes about five minutes; run the two
levels one after the other, not at the same time. Host tests alone:
`$SP/bin/ut-build.sh $PWD surfos-ut-<track> 0 <tag> test-undertow-<x>`.

## Who owns what

| Track | Files (new unless marked) |
| --- | --- |
| T | `user/undertow/transport.c` (replaces the stand-in), `tls_config.h`, `tls_platform.h`, `tls_platform.c`; `third_party/README.md`, `third_party/undertow-lock.json`; `tools/fetch-webdeps.py`, `tests/test_webdeps.py`; `tests/host/test_undertow_transport.c` and its certificates; `rootfs/etc/ssl/cert.pem`, `rootfs/etc/ssl/README`; `user/include/assert.h`, `user/include/inttypes.h`, additions to `user/include/stdint.h` (and libsurf if a function is missing); `.gitignore`; `devenv/build.sh`, `devenv/build-usb.sh` (the fetch step); its Makefile block; the transport section of `tests/host/undertow/surf.h` |
| H | `user/undertow/http.c` (replaces the stand-in); `tests/host/test_undertow_http.c`; its Makefile block |
| R | `user/undertow/html.c` (replaces the stand-in); `tests/host/test_undertow_html.c`; `tests/fixtures/undertow/` (HTML in, text out); its Makefile block |
| S | `user/undertow/session.h`, `session.c`, `view.h`, `view.c`, `main.c` (replaces the stand-in), and a line editor if you split it out (`edit.c`/`edit.h`); `tests/host/test_undertow_session.c`, `test_undertow_view.c`; `rootfs/README` (one line); its Makefile block; its section of `tests/host/undertow/surf.h` |
| F | `tools/web-fixtures.py`, `tools/qemu-web.py`, `tests/test_web_fixtures.py`, `tests/test_qemu_web.py`; `docs/UNDERTOW.md`; its Makefile block; later `tools/qemu-run.py` / `tools/qemu-usb.py` steps and the docs listed in task 8 |
| Integrator | the foundation files, merges, `CLAUDE.md`, `docs/README.md`, `docs/ARCHITECTURE.md`, `docs/USB-BOOT.md`, `.github/workflows/ci.yml` |

## The foundation (read before you start)

- `user/undertow/undertow.h`: limits (`UT_*_MAX`), deadlines (`UT_*_MS`), paths, and the
  helpers in `util.c`: `ut_now()`, `ut_expired()`, `ut_remaining()` (uptime milliseconds,
  wrap-safe), `ut_error()`, UTF-8 (`ut_utf8_decode/encode`), `ut_unsafe_char()`,
  `ut_ascii_for()`, `ut_safe_text()`.
- `user/undertow/config.h`, `config.c`: `struct ut_config` (the clock and the host table) read
  from `/initrd/etc/undertow.conf`; `ut_clock_now()`, `ut_clock_set()`, `ut_clock_plausible()`,
  `ut_clock_format()`. `rootfs/etc/undertow.conf` is the production file.
- `url.h` (task 3), `transport.h`, `http.h`, `html.h`: the contracts between the tracks. Read
  their comments; they are the specification.
- Stand-ins: `transport.c`, `http.c`, `html.c` and `main.c` compile and link, so
  `/initrd/bin/undertow` exists from the start (`undertow --help` is in the smoke test).
- Host tests: `tests/host/undertow/surf.h` replaces `<surf.h>`; each test defines the SurfOS
  calls its modules use (at least `uptime()`, `ut_sim_time()` and `ttyctl()`, see
  `test_undertow_util.c`). The modules are compiled with `malloc`, `calloc`, `realloc`,
  `free`, `strdup`, `strndup` renamed to `tests/host/undertow/alloc.c`'s counting versions:
  `ut_alloc_live` must be 0 after a test frees everything, and `ut_alloc_fail_after = n`
  lets n more allocations succeed and fails the rest (-1: none fails).
- Makefile: `UNDERTOW_SRC` is every `user/undertow/*.c`; `$(BUILD)/host/undertow/%.o` builds
  a module for the host; each test target depends on `$(BUILD)/user/bin/undertow` too, so
  every module also compiles against SurfOS's headers. Add your test target under your
  marker: a `test-undertow-<x>` target, `unittest: test-undertow-<x>`, `.PHONY`.

## Track T: TLS (Mbed TLS 3.6.7) and the transport

Task 4 of the plan, with `tls-port-research.md` and `tls-dependency-research.txt` in
`/Users/brandonburr/.codex/worktrees/undertow/06-restoration/.superpowers/sdd/2026-10-10-undertow/`
(read them first; the archive `mbedtls-3.6.7.tar.bz2`, its extracted copy and the CA bundle
`cacert-2026-09-25.pem` are there too; their SHA-256 values are in the research notes, and
anything you take from there is checked against them).

1. **Dependencies.** `tools/fetch-webdeps.py` downloads the official release asset
   (`https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.7/mbedtls-3.6.7.tar.bz2`)
   into an ignored cache, checks its SHA-256 from `third_party/undertow-lock.json`, and
   extracts only what the build needs (`include/`, `library/`, `LICENSE`, and anything else
   the chosen sources need) into ignored `third_party/mbedtls/`, refusing absolute paths,
   `..`, links and devices, and publishing atomically (a bad download never replaces a good
   tree). Offline with a verified cache it works; with neither it fails with a message that
   says what to do. `--archive PATH` uses a local copy (checked the same way).
   `tests/test_webdeps.py` (Python unittest, local fixture archives only) covers checksum
   mismatch, traversal, links, offline cache, no cache. `devenv/build.sh` and
   `devenv/build-usb.sh` run the fetch on the Mac before the source is streamed into Docker;
   the Makefile runs it too when `third_party/mbedtls` is missing (Linux and CI), with an
   explicit source list so a clean build works in one pass (no `$(wildcard)` over files that
   do not exist yet). Record the advisories you checked in the lock file and
   `third_party/README.md`.
2. **Trust roots.** `rootfs/etc/ssl/cert.pem` is curl's Mozilla extraction `cacert-2026-09-25.pem`
   (SHA-256 `a41b5d356aea97a529fe27e0f7316d2f9d946d75927476cf9cf1b90637d00505`; confirm it
   against curl's published checksum if you can), committed, with `rootfs/etc/ssl/README`
   giving the source, date, checksum and licence (MPL-2.0). All 121 roots must load:
   enable P-521 signature verification (not as a key-exchange group) or explain in the
   report why one does not. A bundle where some roots fail to parse reports how many loaded;
   none loading is an error for HTTPS only (HTTP still works).
3. **Configuration** (`tls_config.h`, one file for every Mbed TLS translation unit and for
   transport.c): client only; TLS 1.2 and 1.3; SNI; ALPN; X.509 with RSA (PKCS#1 v1.5 and
   PSS) and ECDSA verification; ECDHE with X25519, P-256, P-384; AES-GCM and
   ChaCha20-Poly1305; no OS sockets, OS entropy, file system helpers, threading, assembly or
   SIMD, DTLS, renegotiation, session tickets or resumption, early data, PSK. `mbedtls_time_t`
   64-bit (certificates run past 2038). Hooks in `tls_platform.c`: calloc/free, snprintf,
   `mbedtls_hardware_poll` from `getentropy()` (chunked to 256 bytes, failing when it fails:
   never a weaker source), `mbedtls_ms_time` from uptime extended to 64 bits, the UTC clock
   from `ut_clock_now()` of the config passed to `ut_transport_open()` (the vendor callback has
   no context argument: a process-local pointer set around TLS calls is fine), gmtime_r.
   Add `<assert.h>` and `<inttypes.h>` to libsurf's headers (real ones, small) rather than
   private shims. Build the library with `-O2` at every level (debugging Mbed TLS is not
   what -O0 is for, and -O0 crypto under QEMU is slow) and the target's other flags; link
   `undertow` with it and the cross-compiler's 32-bit libgcc (`$(CC) -m32
   -print-libgcc-file-name`), never the host's libc. Check `nm -u` of the final link is empty.
4. **Transport** (`transport.c`, contract in `transport.h`): host table, then IPv4 literal,
   then `net_resolve()`; `tcp_connect()`; for TLS, `psa_crypto_init()`, the DRBG seeded from
   the hardware hook, `MBEDTLS_SSL_VERIFY_REQUIRED`, the roots (parsed once per process),
   `mbedtls_ssl_set_hostname(url->host)`, ALPN `http/1.1`, groups X25519/P-256/P-384, a
   handshake loop driven by `poll()` on WANT_READ/WANT_WRITE. Check `ut_clock_plausible()`
   first and say how to fix the clock when it is not. Map verification failures to plain
   messages (untrusted issuer, wrong name, expired, not yet valid). Check the deadline
   before every stage and every wait; never start DNS (5 s) or connect (10 s) unless that
   much remains; reset the idle timer only on progress; a peer that sends one byte just
   before each idle timeout still ends at the deadline. Read TLS's buffered bytes before
   polling. EOF and close_notify are 0; fatal alerts are -1. Close the socket on every
   failure path.
5. **Tests.** `tests/host/test_undertow_transport.c`: the real Mbed TLS (host build, with the
   server side for the peer) against in-memory peers through simulated sockets, `poll()`
   and uptime (your section of `tests/host/undertow/surf.h` maps the calls): TLS 1.2 and
   1.3 handshakes with the right name; wrong name; unknown issuer; expired; not yet valid;
   a clock that is not plausible; entropy failure; no roots; allocation failure at each
   step (no leak, no crash); a broken handshake; partial writes; bytes buffered by TLS;
   EOF against a fatal alert; the 20-second handshake bound; the 15-second idle bound; a
   trickle past the 60-second deadline; uptime wrapping during a connection. Test
   certificates are made by a script (or committed with that script) and never reach a
   production image.

## Track H: HTTP

Task 5 of the plan; the contract is `http.h`. `http.c` writes the request (target is
`url->target`, never the fragment; `Host` with the port when not the default; `User-Agent:
UT_USER_AGENT`; `Accept: text/html, application/xhtml+xml, text/plain;q=0.9, */*;q=0.1`;
`Accept-Encoding: identity`; `Connection: close`), handling partial writes, then reads the
response through `ut_transport_read()`: the status line (HTTP/1.0 and 1.1), header lines
(obsolete line folding is refused), interim responses (100-199 except 101, at most
`UT_INTERIM_MAX`; 101 is an error), `Content-Length` (digits only; the same value twice is
fine, two values are not), `Transfer-Encoding: chunked` (extensions ignored, trailers read
and counted against `UT_HEADERS_MAX`; any other transfer coding, or chunked with
Content-Length, is refused), and close-delimited bodies; no body for 1xx, 204, 304 (and
none is read for them). Early EOF in any framed body is an error. Bodies over
`UT_BODY_MAX` fail without a partial result. `Content-Encoding` other than identity is an
error naming it. Media type and charset come from `Content-Type`; decide "is it text" from
the headers before reading the body when they say (an `image/png` body is not
downloaded), and by sniffing when there is no Content-Type. Redirects: 301, 302, 303,
307, 308 with a Location resolved against the current address by `ut_url_resolve()`; a
Location without a fragment keeps the original request's fragment; at most
`UT_REDIRECTS_MAX`; an address seen before in the chain is a loop; https to http stops with
a message naming the address. The whole load, redirects included, has one deadline
`ut_now() + UT_LOAD_MS`. Status lines' reason phrases go through `ut_safe_text()`.

`tests/host/test_undertow_http.c` provides its own `ut_transport_*` (scripted connections:
the bytes a server sends, split at every boundary; errors; EOF at any point; what was
written), so `http.c` is tested without TLS. Cover: Content-Length, chunked with
extensions and trailers, close-delimited, interim responses, 204 and 304, early EOF,
malformed and overflowing sizes (`ffffffffffffffff` chunks, 20-digit lengths), duplicate
and conflicting lengths, Transfer-Encoding ambiguity, header and body limits exactly at and
one past the limit (1 MiB succeeds, 1 MiB + 1 fails with nothing kept), relative
redirects, five hops and six, loops, downgrade refusal, a redirect that keeps the fragment,
the request bytes (Host with a port, no fragment), allocation failure at every step, no
leaks.

## Track R: HTML to readable text

Task 6 of the plan; the contract is `html.h`. A bounded tokenizer (no recursion over the
input, no DOM), entities, charsets, then layout into lines:

- Ignore comments, `<!DOCTYPE>`, processing instructions, `head` content except `title`,
  `base` and `meta` charset, and the content of `script`, `style`, `template`, `svg`,
  `math`, `iframe`, `object`, `noembed`; show `noscript` content (no script ever runs).
  Raw text elements (`script`, `style`, `textarea`, `title`) end only at their own end tag.
  Attribute values may be quoted with `>` inside; tags may be unclosed or mismatched.
- Blocks (`p`, `div`, `section`, `article`, `header`, `footer`, `nav`, `main`, `aside`,
  `blockquote`, `figure`, `figcaption`, `form`, `fieldset`, `address`, `details`, `summary`,
  `dl`, `dt`, `dd`, `table`, `tr`, `ul`, `ol`, `li`, `h1`-`h6`, `hr`, `pre`, `center`, ...)
  start new lines; paragraphs and headings are separated by one blank line; `br` breaks a
  line; whitespace collapses outside `pre`; `pre` keeps it (tabs to 8-column stops).
- Lists: `* ` items for `ul`, `1. ` for `ol` (honour `start`), two more spaces of indent per
  nesting level; `blockquote` and `dd` indent by two spaces. The view indents a wrapped
  line's continuation under the text after these markers, so keep them exactly this shape.
- Tables: one line per row, cells separated by ` | `; a table inside a table flattens.
- Links: every `<a href>` with a non-empty href gets the next number, written right after
  its label as `[N]` (a `UT_SPAN_LINK` span); its label is the link's text (or an image's
  alt, or `[N]` alone when empty). Fragment-only links resolve to the page's own address
  with the fragment. Non-http(s) schemes keep their number, `usable` 0, and the safe text
  of the reference. Links past `UT_LINKS_MAX` keep their text but get no number.
- Images: `alt` text in brackets (`[Logo]`), or nothing without alt; inside a link the alt
  text is the label, without brackets.
- Forms: each control is a bracketed placeholder (`[text field]`, `[button: Search]`,
  `[select: first option]`, `[checkbox]`, `[text area]`) and the first form adds the notice
  "forms are not supported".
- Headings: blank lines around them and a `UT_SPAN_HEADING` span; no other markup.
- `hr`: a line of 40 `-` (the view does not stretch it).
- Entities: decimal and hex references (invalid scalars, surrogates and 0 become U+FFFD,
  128-159 map through windows-1252 as browsers do), the named ones in the plan plus every
  Latin-1 name (`&nbsp;` to `&yuml;`), `&euro; &bull; &middot; &laquo; &raquo; &times;
  &divide; &deg; &plusmn; &para; &sect; &frac12; &frac14; &frac34; &larr; &rarr; &uarr;
  &darr; &harr; &minus; &le; &ge; &ne; &prime; &Prime; &lsaquo; &rsaquo; &sbquo; &bdquo;
  &dagger; &Dagger; &permil; &thinsp; &ensp; &emsp; &zwj; &zwnj; &shy;`. The Latin-1 names
  and `amp lt gt quot` also work without the semicolon in text (not in attributes before
  `=` or an alphanumeric). Unknown names stay as written. `&shy;` is dropped.
- Charsets: as `html.h` says. UTF-8 with replacement; windows-1252 (and iso-8859-1,
  us-ascii, latin1, cp1252 and the WHATWG labels for them) through its table; anything else
  gives its ASCII bytes, `?` for the rest, and a notice naming the charset (safe text).
- Output safety: `ut_unsafe_char()` characters never appear (a page cannot send ESC or C1);
  `U+00A0` becomes a space; text lines have no trailing spaces; at most one blank line in a
  row; none at either end. `title` is the first `<title>`, whitespace collapsed, safe,
  bounded by `UT_FIELD_MAX`.
- Limits: `UT_TEXT_MAX` (stop with a final notice line "[Undertow: the rest of this page is
  not shown: it is over 512 KB]" and `UT_TRUNC_TEXT`), `UT_LINKS_MAX`, `UT_ANCHORS_MAX`,
  `UT_FIELD_MAX` per title, label and anchor name, `UT_SPANS_MAX` spans. `bytes` counts every
  allocation the document owns. Anchors record the text offset where their element's text
  begins (first one wins for a duplicate name).

`tests/host/test_undertow_html.c` with fixtures in `tests/fixtures/undertow/` (`NAME.html`
in, `NAME.txt` the expected text, links and anchors in a form you choose): everything the
plan lists (paragraphs, headings, lists, pre, tables, alt text, forms, comments,
script/style/template, quoted `>`, mismatched and missing end tags, nested link labels,
duplicate anchors, late `<base>`, entities, split and invalid UTF-8, windows-1252, Latin-1,
unknown charset, plain text, embedded NUL, 256 and 257 links, the text limit), ESC and C1
bytes in text, titles, labels and hrefs never reaching the output, allocation failure at
every allocation, no leaks, and the ASan/UBSan run.

## Track S: session, screen and commands

Task 7 of the plan. You own the interfaces of `session.h` and `view.h`; only `main.c`
uses them. The user-facing behaviour is the spec's command table plus:

- **Screen.** Before each page view, `ttyctl(TTY_PAGE, 1)` (a new screenful for the
  shell's pager). A view is at most rows - 1 lines and then the prompt: a title line
  (`Undertow - TITLE` and, at the right, `[HTTPS verified TLSv1.3]`, `[HTTP unencrypted]`
  or `[HTTP 404 Not Found]`-style status when not 200), the address line, a rule, the
  page's rows, a rule, a position line (`lines 1-27 of 240 (11%)`, then the most useful
  keys), and `undertow> `. Colours (TTY_CAP_COLOR) only for the chrome, link markers and
  headings (the document's spans); without UTF-8 (TTY_CAP_UTF8 off) everything goes
  through `ut_ascii_for()`. Use TTY_GETSIZE every view (the terminal may change; telnet
  sessions are often 80x24); handle 40 columns and 25 rows without zero-width loops.
- **Wrapping.** Word-wrap each text line to the width in display columns (one per code
  point, as the console draws them), breaking words longer than a row; a continuation row
  is indented under the text after a line's leading spaces and list marker (`* `, `1. `),
  unless that would leave fewer than 20 columns. Never split a UTF-8 sequence. Reading
  positions are byte offsets at the start of a row, so a resize keeps the reader's place.
- **Input.** Read commands with your own small line editor in raw mode (the console's line
  discipline stops at 255 bytes and the spec allows 4096): printable ASCII, Backspace,
  Left/Right, Home/End, ^U, Up/Down through this session's commands; Enter; ^C at the
  prompt quits Undertow (in raw mode it is a key, not a kill); ^D on an empty line is EOF.
  Leave raw mode before loading or printing anything (so Ctrl+C during a load interrupts as
  the shell's other commands do). A line over `UT_COMMAND_MAX` is refused whole: nothing of
  it runs. `user/surfsh/editor.c` shows how the terminal behaves (VT100 wrap, escape
  sequences from serial and telnet terminals, paste handling with TTY_SKIP).
- **Commands.** All of the spec's table and aliases, `links` and `history` paginated a
  screen at a time, `find`/`findnext` case-insensitive for ASCII and exact for the rest,
  wrapping once, `clock` and `clock YYYY-MM-DD HH:MM:SS` through config.h, `help`/`?`,
  `quit`/`q`. A link to another page loads it; a link with only a fragment of the current
  page jumps without fetching; `NUMBER` past the last link, or an unusable link, says why.
  `open` of an address without a scheme takes it as a new address (`ut_url_parse()`), not
  relative to the page. Before each load, print `Loading ADDRESS ...` on one line.
- **History.** As the plan says: 16 entries, `UT_HISTORY_BYTES`, the current page never
  evicted, new pages committed only on success, forward history dropped only when a new
  page loads, reload replacing the entry and keeping the reading position when the page
  still has that many bytes, positions per entry.
- **Start and modes.** `undertow` (the welcome screen: what to type, the clock, and whether
  HTTPS can work: strong randomness and the roots), `undertow ADDRESS`, `undertow --help`,
  `undertow --dump ADDRESS` (the text and then a numbered list of links' addresses on
  stdout, status and errors on stderr, exit 0 for 2xx, 1 for anything else that loaded or
  failed, 2 for bad usage). Interactive mode needs a terminal on stdin and stdout
  (`isatty`); otherwise it says to use `--dump`. Read `/initrd/etc/undertow.conf` at start
  (a bad file is a warning on stderr and the defaults). Free everything on exit
  (`ut_transport_cleanup()` too).

Tests: `test_undertow_session.c` links `session.c` (and your other modules) with its own
`ut_fetch()` and `ut_render()` (fake pages), never `http.o` or `html.o`: open A, open B,
back, a failed open C keeps B's forward entry, forward to B; a successful C drops it;
failed reload keeps text and position; back and forward never fetch; the 17th entry evicts
the oldest; more than 8 MiB evicts the oldest but never the current page; fragment jumps
and missing anchors; find with wrap and case; allocation failure at every step.
`test_undertow_view.c`: 40/80/120 columns, 25 rows, long words, multibyte text, list
indentation, ASCII mode, resize keeping the position, `prev` from any position, the last
screen, empty pages, colours only with the capability. In QEMU, `undertow --help` and the
welcome screen with `quit`; the smoke-test line for `undertow --help` exists already
(adjust its expected text if you change the usage).

## Track F: fixtures and the browser in QEMU

Task 8 of the plan. Now: the fixture servers and the harness, testable without a working
browser; after the merge, the end-to-end suite (you will be resumed for that).

- `tools/web-fixtures.py`: HTTP and HTTPS servers on 127.0.0.1 (ephemeral ports, threads,
  clean shutdown even after an exception) that the guest reaches through QEMU's user network
  at 10.0.2.2. A per-run test CA and leaf certificates made with the `openssl` command
  (OpenSSL 3 on the Mac and in the toolchain image): a good leaf for `fixture.test`,
  `www.fixture.test` and IP 10.0.2.2; a wrong-name leaf; expired and not-yet-valid leaves
  (`-not_before`/`-not_after`, or a fixed QEMU `-rtc base=`); a leaf from an unknown CA;
  ECDSA P-256 and RSA 2048 chains with an intermediate. Servers limited to TLS 1.2 and to TLS
  1.3, one that sends garbage instead of a handshake, and recording of SNI, Host and the
  request line. Routes for everything tracks H and R handle (Content-Length, chunked with
  extensions and trailers, close-delimited, 1xx, 204, early EOF, 1 MiB and 1 MiB + 1, 6
  redirects, a loop, https to http, relative redirects, a stall, a byte every 10 seconds,
  `image/png`, gzip, windows-1252, Shift_JIS, 257 links, anchors, ESC in title and text,
  404 with HTML) and a few readable pages with links between them.
- `tests/test_web_fixtures.py`: the fixtures tested with Python's own client (`ssl`,
  `http.client`, raw sockets): every route, the TLS versions, the certificate cases, SNI and
  Host recording, cleanup after an exception, no fixed ports.
- `tools/qemu-web.py`: builds a test initrd from a normal one (a copy: the fixture CA as
  `etc/ssl/cert.pem`, an `etc/undertow.conf` with `host=fixture.test 10.0.2.2` lines; never
  the normal image files themselves), boots the guest (`-cpu max` for RDRAND; the default CPU
  for the entropy-failure case), runs scripted browser sessions over the serial console
  (`tools/qemu-run.py`'s `Qemu` class and `PROMPT`), and reports PASS/FAIL lines and a
  summary like the other harnesses, with transcripts under `build/web-test/O<n>/`.
  `tests/test_qemu_web.py`: the test initrd is built correctly and the production initrd
  never contains the fixture CA; nothing opens a disk device.
- Makefile: `test-web` (`$(KERNEL)` and `images`, then `qemu-web.py --test`).
- `docs/UNDERTOW.md`: the user guide (draft now; final after the merge).

Until the merge, `undertow` is a stand-in, so the end-to-end scenarios cannot pass yet:
write them against the spec's command table and screen, make the harness and fixtures
solid, and say which scenarios are written but not yet run.

## Merging

The integrator merges tracks into `codex/undertow` with `--no-ff` in this order: R, H, T, S,
then F, running the suites after each merge; then the end-to-end suite, an independent review
of each track, fix rounds on fresh branches from the integrated state, and the final
validation (unit tests, smoke and file-browser suites at -O0 and -O2, `test-web` at -O0 and
-O2, the UEFI USB suite at -O0 and -O2, the public interoperability check, the USB image).
