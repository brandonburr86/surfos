# third_party: what Undertow is built from

Undertow, the SurfOS web browser (`user/undertow/`), does HTTPS with **Mbed TLS 3.6.7**, the
current release of the 3.6 long-term support branch. Its source is never committed: it is pinned
by SHA-256 in `undertow-lock.json`, fetched and checked by `tools/fetch-webdeps.py`, and extracted
into `third_party/mbedtls/` (ignored by git). The trust roots it checks certificates against are
committed instead: `rootfs/etc/ssl/cert.pem` (see `rootfs/etc/ssl/README`), recorded in the same lock.

## What is pinned

| | |
| --- | --- |
| Archive | <https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.7/mbedtls-3.6.7.tar.bz2> (the official release asset, not GitHub's generated source snapshot: the asset carries the generated files the build needs) |
| SHA-256 | `a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6`, 5473689 bytes |
| Checked | 2026-10-11 against the release notes, the release's `mbedtls-3.6.7-sha256sum.txt` and GitHub's digest of the asset; all three agree with the archive |
| Extracted | `include/`, `library/` and `LICENSE` (277 files); nothing else of the archive is used |
| Licence | Apache-2.0 OR GPL-2.0-or-later (`third_party/mbedtls/LICENSE`) |

## How a build gets it

* `devenv/build.sh` (and so `devenv/build-usb.sh`) runs `tools/fetch-webdeps.py` on the Mac before the
  tree is copied into the Docker volume. The volume's copy is replaced by the Mac's on every build,
  so the files have to be on the Mac: something fetched only inside Docker would vanish.
* The Makefile runs it too, whenever `third_party/mbedtls/UNDERTOW-LOCK` is missing or older than the
  lock (a clean Linux checkout, CI), so one `make` builds everything. The Mbed TLS sources are listed
  in the Makefile rather than found with `$(wildcard)`, which would see nothing before the fetch.
* The tool downloads into `third_party/cache/` (ignored), checks the SHA-256, and only then extracts:
  into a new directory next to `third_party/mbedtls/`, which takes the old one's place only when every
  file is there, so a bad download or archive never replaces a good tree. An archive with a member that
  is a link, a device or a FIFO, has an absolute path or `..`, or lies outside its top directory is
  refused whole. `third_party/mbedtls/UNDERTOW-LOCK` records what was extracted, with each file's
  SHA-256; while it matches the lock the tool does nothing (cheap enough to run before every build).
* Without a network: a verified archive in `third_party/cache/` is enough, or
  `tools/fetch-webdeps.py --archive PATH` (checked the same way). With neither, the tool says so and
  what to do. `--offline` never downloads.
* `tools/fetch-webdeps.py --check` changes nothing and checks everything: each extracted file against
  its recorded SHA-256 (no vendor file may be edited: the configuration and the platform code are
  Undertow's own files), and `rootfs/etc/ssl/cert.pem` against the lock.
* `make test-webdeps` (part of `make unittest`) tests the tool with archives it makes itself, then
  checks the real tree with `--check`.

## How Undertow uses it

`user/undertow/tls_config.h` is the whole configuration (`MBEDTLS_CONFIG_FILE`): a TLS 1.2 and 1.3
client with SNI and ALPN, ECDHE over X25519, P-256 and P-384, ChaCha20-Poly1305 and AES-GCM,
certificates signed with RSA (PKCS#1 v1.5, PSS) or ECDSA; no server side, DTLS, renegotiation, tickets,
resumption, early data, PSK, sockets, files, threads, assembly or SIMD. P-521, SHA-512 and SHA-1 are
there only so that every root of the bundle can be read; `transport.c` accepts only SHA-256, SHA-384 and
SHA-512 in certificate chains and handshakes (an explicit `mbedtls_x509_crt_profile` and signature
list). `user/undertow/tls_platform.c` is the platform: memory and formatting from libsurf, randomness
only from `getentropy()` (RDRAND; a CPU without it has no HTTPS, and nothing weaker stands in), the
monotonic clock from `uptime()`, and certificate dates checked against the browser's UTC clock
(`config.h`). The library is built with `-O2` and without debug information at every level, from the
listed sources, and linked into `/initrd/bin/undertow` with the cross-compiler's 32-bit `libgcc`; the
host tests build it separately, with the server side added (`tests/host/undertow/tls_host_config.h`).
`platform_util.c` keeps one empty `asm volatile("")` in `mbedtls_platform_zeroize()`: a compiler
barrier that emits no instruction, which `MBEDTLS_HAVE_ASM` does not control.

## Security advisories (checked 2026-10-11)

The index <https://mbed-tls.readthedocs.io/en/latest/security-advisories/> lists nothing published
after 3.6.7 (2026-07-07), which is still the newest 3.6 release. Every advisory of 2026 gives 3.6.7 as
not affected: the July ones were fixed in 3.6.7 (among them CVE-2026-49300, CVE-2026-25832,
CVE-2026-50580, CVE-2026-50581, CVE-2026-54435, CVE-2026-54441), the March ones in 3.6.6. One
advisory names no fixed version: compiler-induced constant-time violations (CVE-2025-66442), seen with
Clang's select-optimize pass; SurfOS is built with GCC, and the operations it affects (RSA PKCS#1 v1.5
decryption, CBC padding) are not used by this client. The lock lists the identifiers. Check again
whenever the pin changes.

## Local cleanup patch and remaining 3.6.7 workarounds

Found by `tests/host/test_undertow_transport.c`, which makes every allocation of a connection fail in
turn (`UT_TRANSPORT_EXHAUSTIVE=1` for all of them):

1. `mbedtls_ssl_tls13_compute_handshake_transform()` and `..._compute_application_transform()`
   (`library/ssl_tls13_keys.c`) free a half-made transform with `mbedtls_free()` rather than
   `mbedtls_ssl_transform_free()`. When the second `mbedtls_cipher_setup()` in
   `mbedtls_ssl_tls13_populate_transform()` fails for lack of memory, the first cipher context (one
   small block) is lost, and the handshake fails with `MBEDTLS_ERR_SSL_HANDSHAKE_FAILURE`. Only an
   allocation failing at that point leaked in the original release. The build now adds
   `mbedtls_ssl_transform_free()` before each raw free in the early-data, handshake and
   application-transform error paths. `third_party/patches/mbedtls-3.6.7-tls13-cleanup.json`
   records the three exact insertions and the input/output SHA-256. `tools/patch-webdeps.py`
   refuses different input or output and atomically writes only a build-directory copy.
   Target and host use that same source. The extracted vendor tree is unchanged and
   `fetch-webdeps.py --check` still verifies all its original files. The transport tests
   require zero live allocations after failure, with no vendor-leak allowance.
2. `mbedtls_entropy_func()` (`library/entropy.c`) frees its SHA-512 accumulator and sets it up again;
   when that allocation fails, the context keeps the hash's type without its state while
   `accumulator_started` stays 1, and the next entropy call (the generator's next reseed) dereferences
   NULL. `transport.c` therefore makes PSA, its random generator and the roots anew, once no TLS
   connection is open, after any handshake that failed inside Mbed TLS (such a failure often surfaces
   only as `MBEDTLS_ERR_SSL_HANDSHAKE_FAILURE`).

Both require allocation failure. On an upstream update, inspect these cleanup paths first.
If upstream includes the three frees, remove the local patch, its generator/tests and the
special Makefile source rules together; otherwise re-review the patch against the new
source and record new input/output hashes. Do not simply relax the hash check.

The official release list and advisory index were rechecked on 2026-10-12: 3.6.7 remains
the newest listed 3.6 release. This is a dependency-status check, not an independent
security audit: https://github.com/Mbed-TLS/mbedtls/releases and
https://mbed-tls.readthedocs.io/en/latest/security-advisories/.

Also: Mbed TLS sends the host name as SNI even when it is an IPv4 address (RFC 6066 says it should not);
servers ignore it, and the address is still checked against the certificate's IP names.

## Updating

1. Read the release notes and the advisories; pick the newest 3.6 release (or decide on a move to 4.x,
   which changes the configuration and the PSA interfaces).
2. Change `version`, `url`, `sha256`, `size` and `root` in `undertow-lock.json` (the checksum from the
   release notes and the release's `sha256sum.txt`, compared with the downloaded file), and the
   advisory record.
3. Run `tools/fetch-webdeps.py`, then `make unittest` and the smoke tests at -O0 and -O2; a source file
   the configuration newly needs shows up as an undefined symbol in the link of `undertow`, and is
   added to `MBEDTLS_SRC` in the Makefile.
4. Commit the lock, this file and any configuration change together.
