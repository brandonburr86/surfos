# Undertow console browser design

Approved by the owner on 2026-10-10. Source baseline: `ai-dev`, `1dcf4f4`.

## Purpose

Undertow is SurfOS's text browser. Launch it from surfsh with an address, read
the page in the console, follow numbered links, and navigate back and forward.
The owner explicitly requested HTTPS in the first version. Both HTTP and HTTPS
are therefore acceptance requirements. JavaScript execution is excluded.

The first version is for reading public web pages. Forms, logins, cookies,
downloads, CSS layout, images, audio, video, tabs, and persistent bookmarks are
outside this version. Image alternative text and form labels may appear as text;
controls must be identified as unsupported. Sites whose content requires
JavaScript will only show the HTML their server supplies.

## Existing foundations

The tree contains a 32-bit x86 kernel, ring-3 user programs, libsurf, surfsh,
VGA and framebuffer consoles, serial and telnet terminals, and build and QEMU
test harnesses. The browser belongs alongside `user/surfsh/` and `user/telnetd/`.

Relevant code verified in this checkout:

| Foundation | Files | Consequence |
| --- | --- | --- |
| TCP descriptors, partial reads and writes, poll | `net/sock.c`, `include/surfos/sock.h`, `user/include/surf.h` | Reuse the existing network stack on Ethernet or Wi-Fi |
| DNS | `net/dns.c`, `include/net/dhcp.h` | Expose a bounded lookup to user programs; currently only kernel callers can use it |
| Basic HTTP diagnostic | `shell/main.c`, `user/bin/nettest.c` | Existing fetches demonstrate transport, but do not implement a browser or TLS |
| Terminal dimensions, line input and paging | `include/surfos/tty.h`, `user/libsurf/stdio.c` | Use a command prompt and page-sized output on each supported console |
| Random generator | `lib/crypto/random.c`, `include/crypto/crypto.h` | Existing Wi-Fi generator permits a timer-only fallback; HTTPS needs a separate strict contract |
| Calendar time | `kernel/syscall.c`, `user/libsurf/time.c` | RTC time is returned without a time zone; certificate validation needs an explicit UTC interpretation |
| Build and packaging | `Makefile`, `tools/mkimage.py`, `devenv/` | Include the browser and trust roots in both normal and USB initrds |

The source inventory covers every tracked directory. The detailed browser
integration review focuses on the interfaces above, not a new correctness audit
of every driver. Some summary documentation predates the latest Wi-Fi commits;
current code is authoritative for available interfaces.

## Approach

Build a native C application in `user/undertow/`, linked with libsurf and a
small client configuration of Mbed TLS. Keep HTML, URL and HTTP processing in
user space. Kernel changes are limited to services the application cannot
currently obtain safely: DNS lookup and strong random bytes.

Alternatives considered:

| Approach | Tradeoff |
| --- | --- |
| Native Undertow with an established TLS library, recommended | Fits SurfOS's small runtime and gives control over the console experience |
| Port an existing console browser | Adds substantially more library, terminal and operating-system compatibility work |
| Use a browser gateway on another computer | Adds a permanent external dependency and moves HTTPS trust outside SurfOS |

## Console interaction

Launch `undertow https://example.com/`, `undertow http://host:8080/path`, or
`undertow` for the welcome screen. A bare hostname defaults to HTTPS. Do not
retry with HTTP automatically after an HTTPS failure.

Each view shows the title, final address, transport state (`HTTPS verified` or
`HTTP unencrypted`), page position, readable text, and the `undertow>` prompt.
Links appear beside their labels as `[1]`, `[2]`, and so on.

| Command | Behavior |
| --- | --- |
| `open ADDRESS` or `o ADDRESS` | Load an address |
| `NUMBER` or `go NUMBER` | Follow a numbered link |
| `back` or `b` | Restore the previous history entry and reading position |
| `forward` or `f` | Restore the next history entry |
| `reload` or `r` | Fetch the current address again |
| Enter or `next` | Show the next screen of the current page |
| `prev` | Show the preceding screen |
| `top` | Return to the start of the current page |
| `links` | Show a paginated list of link numbers, labels and addresses |
| `find WORDS` | Find text from the current position, wrapping once |
| `findnext` | Find the next occurrence of the same text |
| `history` | Show a paginated history list with the current entry marked |
| `url` | Show the full current address |
| `clock` | Show the UTC time used for certificate validation |
| `clock YYYY-MM-DD HH:MM:SS` | Set UTC for this browser session, advanced by monotonic uptime |
| `help` or `?` | Show the commands |
| `quit` or `q` | Return to surfsh |

EOF exits cleanly. Ctrl+C follows the shell's existing interrupt behavior and
returns control to surfsh, releasing sockets and application memory. There is
no promise that Ctrl+C preserves the browser session.

The browser renders one screen at a time, leaving space for status and prompt,
so it coexists with the shell's inherited pager. It uses terminal dimensions
and avoids dependence on a full-screen terminal library. Text has an ASCII
fallback where the console cannot display UTF-8. Redirected use requires
`undertow --dump ADDRESS`, which outputs the rendered text and link references
without interaction, with diagnostics on stderr and a meaningful exit status.

## Browsing and rendering

Handle absolute URLs, relative paths, root-relative and scheme-relative links,
query-only references, fragments, dot segments, and the first usable HTML base
address. Preserve percent-encoded delimiters. Only HTTP and HTTPS addresses
are fetched; unsupported schemes are identified without executing anything.
Support IPv4 and DNS hostnames; IPv6 and internationalized hostname conversion
are deferred, with clear errors rather than incorrect interpretation.

Use a tolerant, bounded HTML tokenizer. Keep headings, paragraphs, lists,
line breaks, preformatted text, basic table row separation, image alternative
text and link labels. Decode numeric character references and a documented set
of common named entities. Ignore comments, scripts, styles and templates.
Support UTF-8, ASCII and Windows-1252/Latin-1 input with replacement for malformed
characters. Advertised unsupported encodings produce an explanation.

Strip terminal control characters from every untrusted field, including page
text, titles, URLs and error descriptions. A page must not emit ANSI commands.
Fragment links jump to the corresponding anchor when available and never send
the fragment to the server.

Keep up to 16 history entries, with an 8 MiB aggregate budget for rendered text,
links and metadata. Back and forward use saved pages without refetching. Opening
a new address after going back discards forward history only once loading
succeeds. Evict the oldest entries as needed, never the current page. Failed
loads and failed reloads preserve the current page and navigation state.

Initial limits: 2,048 bytes per URL, 32 KiB of response headers, 1 MiB of received
body, 512 KiB of rendered text, and 256 links per page. Reject oversized headers
and bodies; a rendering or link limit produces an explicit truncation notice.
Drain overlong command input and reject it as one command, never execute its tail.
The existing shell argument limit applies to launch arguments; long addresses
can be entered using `open` inside Undertow.

## HTTP and HTTPS

Implement HTTP/1.1 GET over one connection at a time, with HTTP/1.0 response
support. Send Host, an Undertow user agent, `Connection: close`, and
`Accept-Encoding: identity`. Support content-length, chunked and close-delimited
response bodies. Unsupported compression is a readable error. Process bounded
informational responses and at most five redirects, resolving relative Location
values. An HTTPS-to-HTTP redirect stops and displays its destination; the user
may explicitly open that HTTP URL. HTTP error pages can still be read with their
status displayed. Binary content is identified without printing it as text.

Use Mbed TLS 3.6.7 from the maintained 3.6 LTS branch as the initial dependency
candidate. Pin the official release archive and its checksum during dependency
integration, retain its license, and check advisories again then. Enable TLS 1.2
and 1.3, SNI, certificate-chain verification, hostname verification, certificate
validity dates, and HTTP/1.1 ALPN. Exclude obsolete TLS versions, early data and
session resumption from this version. Do not offer an insecure verification
bypass. These are implementation requirements, not claims of an existing port.

Package a pinned Mozilla-derived CA bundle with its provenance and license in
`/initrd/etc/ssl/cert.pem`. Bundle updates are deliberate build inputs, never
unverified runtime downloads. Test-only roots belong exclusively to test images.

Provide a strict random service that returns an error if an approved entropy
source is unavailable or fails. On the target laptop this can use checked
RDRAND output to seed the TLS generator. Do not expose the Wi-Fi generator's
timer-only fallback as cryptographically strong randomness. Keep its existing
callers compatible. The HTTPS tests must cover both successful entropy and
failure; QEMU HTTPS runs need an explicitly capable CPU configuration.

By default, interpret the RTC reading as UTC and document that assumption.
Allow a configured RTC offset in `undertow.conf` and an explicit session UTC
override through `clock`. The session override never changes the hardware clock
or file timestamps. Invalid dates stop HTTPS with a corrective message. Verify
the laptop's actual RTC interpretation before claiming hardware HTTPS works.

All network stages must finish or fail: DNS within 5 seconds, TCP connection
within the existing 10-second bound, TLS handshake within 20 seconds, reads and
writes after 15 seconds without progress, and a 60-second total load deadline
across redirects. Use monotonic uptime and poll, handle partial I/O, and close
connections on every failure path. Check deadlines even when small amounts of
data keep arriving. DNS hardening includes reply-source and question validation,
compression bounds and an absolute deadline when unrelated packets arrive.

## Component boundaries

| Component | Responsibility |
| --- | --- |
| `main.c` and session module | Command loop, views, history, search and lifecycle |
| URL module | Parse, normalize and resolve addresses without I/O |
| HTML module | Tokenize, decode and render text and links without network access |
| HTTP module | Request formatting, response framing, limits and redirect policy |
| Transport module | TCP, TLS, deadlines, trust roots and UTC conversion |
| Kernel and libsurf additions | Validated DNS and strong-random system calls |

Pure parsers accept explicit buffer lengths and expose testable results. TLS
objects and large buffers live on the heap. Allocation failures return errors
without corrupting the session. Keep the TLS port's configuration and platform
adapter separate from the vendor source.

## Acceptance evidence

1. Host tests exercise URL resolution, malformed HTML, entities, link numbering,
   control-character filtering, response framing, truncation and history rules.
2. QEMU boots the actual SurfOS browser and fetches deterministic HTTP and HTTPS
   fixtures. Test both TLS versions, relative links, redirects, pagination,
   search, back/forward, reload, EOF, interrupts and socket cleanup.
3. Negative HTTPS fixtures cover an unknown root, wrong hostname, expired and
   future certificates, a broken handshake, incorrect clock and unavailable
   strong entropy. None may display `HTTPS verified` or silently fall back.
4. Failure fixtures cover DNS failure, connection refusal, stalls, slow trickles,
   oversized and malformed responses, binary content and unsupported encodings.
5. Run `make unittest`, `make test-all`, and UEFI USB tests at O0 and O2 through
   the repository's development environment. Include a browser check over the
   simulated Wi-Fi path and USB Ethernet path. Verify the packaged executable
   and CA bundle in both initrd variants.
6. Verify public HTTPS separately from deterministic fixtures. Report build,
   emulator, Internet interoperability and physical-laptop results distinctly.
   A physical installation or reboot requires coordination with the owner;
   creating the USB image never writes a physical disk.

## Protocol and dependency references

- [HTTP/1.1 framing](https://www.rfc-editor.org/rfc/rfc9112.html)
- [URI resolution](https://www.rfc-editor.org/rfc/rfc3986.html)
- [Mbed TLS 3.6.7 release](https://github.com/Mbed-TLS/mbedtls/releases/tag/mbedtls-3.6.7)
- [Mbed TLS random generation](https://mbed-tls.readthedocs.io/en/latest/kb/how-to/add-a-random-generator/)
- [Mozilla CA extraction provided by curl](https://curl.se/docs/caextract.html)
