# The command-line suite: track archive

The brief is `2026-11-01-cli-suite.md`. This track built inflate and deflate with crc32, `gzip`, `gunzip`, `zcat`, `tar` and
`unzip`. None of it has run on the laptop: only host tests and QEMU, until the owner tries it.

## What was built

| Where | What |
|---|---|
| `user/libsurf/crc32.c`, `user/include/crc32.h` | CRC-32 of gzip and zip |
| `user/libsurf/inflate.c`, `user/include/inflate.h` | RFC 1951's decoder as a stream, resumable at any byte of input or output |
| `user/libsurf/deflate.c`, `user/include/deflate.h` | RFC 1951's encoder: LZ77 with hash chains, lazy matching from level 4, package-merge codes, the smallest of dynamic, fixed and stored per block |
| `user/gzip/gzfile.[ch]` | RFC 1952 over a source and a sink function: no system calls; gzip and tar share it |
| `user/gzip/outfile.h` | the one way gzip, tar and unzip make a file: `of_open`, `of_commit`, `of_drop`, `of_parents`, over saveutil.h |
| `user/gzip/tool.[ch]`, `gzip.c`, `gunzip.c`, `zcat.c` | the three programs: one function, three one-line mains |
| `user/tar/tarfile.[ch]` | reading an archive and making a header: no system calls |
| `user/tar/arpath.[ch]` | names from archives: `ar_safe_path`, `ar_show_name`, `ar_glob`, `ar_under`; unzip links this too |
| `user/tar/tar.c` | `tar` |
| `user/unzip/zipfile.[ch]` | the end record, central directory, local headers and names out of memory: no system calls |
| `user/unzip/unzip.c` | `unzip` |
| `rootfs/man/{gzip,gunzip,zcat,tar,unzip}.txt` | the manual |
| `tests/host/test_deflate.c`, `test_gztool.c`, `test_tar.c`, `test_unzip.c` | the host tests |
| `tests/host/ar_sim.[ch]`, `ar_names.h`, `ar_host.h`, `ar_crc.c`, `ar_string64.c` | their simulated FAT and helpers |
| `tests/archive-sanitize.sh` | the same tests under AddressSanitizer and UBSan, natively on the Mac |
| `Makefile`, `tools/qemu-run.py` | the insertions the brief allows: `USER_PROGS`, one block; one smoke block, `echo cli-archive` |

## Decisions a reviewer would ask about

### Inflate and deflate

- **The decoder is zlib's method, and came from my memory of zlib's design.** Only `make_table()`'s three steps are RFC 1951 section
  3.2.2, which stops at the codes. The RFC has no tables and no fast loop: the two-level table, `lookup()`, and `run_fast()`'s
  taking two bytes at a time and giving back what it did not use follow zlib's `inftrees.c` and `inffast.c` as I remember them. No
  zlib text is in it. `web/m2`'s `image_png.c` has another shape, one that could not stop part way. Its limits were read and kept:
  over-full and incomplete codes refused, length symbols 286 and 287 and distance symbols 30 and 31 refused, a distance checked
  against what the window really holds. None of its code was carried over.
- **It never reads past the last block.** Bytes are taken only as a code needs them, so gzip's trailer and a zip's next
  member are where `in` points. `run_fast()` takes that apart where there is plenty of input and room, taking two bytes at a
  time, and gives back any whole byte it did not use before it returns.
- **Every byte goes into the window and into `out`, and copies come only from the window.** Two stores a byte, but no
  history in the caller's buffer, and the distance check cannot be fooled by what `out` holds.
- **Decoding limits nothing.** A stream may make as much as it likes, as far as `out` goes: the caller holds the bound. unzip
  does, by the size it was told; gzip and tar need none, since the disk and the pipe are theirs.
- **Package-merge instead of a Huffman tree and a repair of what came out too long.** It gives the best code of a limited
  length directly, with no second pass. Its lists need no pointers: in each list the leaves in use are the first ones by
  frequency, so a count of leaves per list position says it all (`leaves[][]`, 17 KB of stack in the worst case).
- **Two symbols always get codes.** A code with one symbol is incomplete, which some decoders refuse, and the code of the code
  lengths must never be.
- **The longest distance is 32767, not 32768,** so that a distance fits 16 bits with 0 for a literal. This costs nothing that
  can be seen, and matches at the window's far edge are tested.
- **A block ends at 16383 symbols.** `pending` holds a whole block because the form chosen is never larger than fixed and a
  fixed symbol is at most 31 bits, so 63.5 KB, and a stored block is chosen only when smaller still. `test_deflate` fills it with
  the worst symbols it can make.
- **The level table's numbers are this file's own,** tuned by hand. On 1.4 MB of this tree's text, level 6 gives 461304 bytes to
  GNU-compatible gzip's 461635, and level 9 459140 to 459396. On the kernel image, level 6 gives 2138530 to 2139790.

### gzip, gunzip, zcat

- **Zero bytes after the last member are allowed, which the brief's "trailing garbage refused" does not say.** GNU tar pads what
  `tar czf -` writes to a pipe up to its record size, and GNU gzip takes that silently; refusing it would refuse archives from
  Linux, which the brief wants read. Zeros then anything, and a member after zeros, are still refused. The test takes a real
  piped `tar czf -` from the host.
- **Three programs, not one with three names.** There are no links on FAT, and nothing has to look at `argv[0]`. 250 KB each.
- **The input goes after `of_commit()` with `fsync`, never before.** A rename left half done keeps it. So does a twin
  (`su_twin_beside()`): deleting a file that shares its clusters with another would free that one's data. saveutil.h protects
  replacing; this is the same care for deleting.
- **What Ctrl+C leaves.** A program is killed between writes, so nothing here can clean up: a short output can stay beside the
  whole input. No data is lost, and the next run says the output exists. The page says so.
- **A name and a time go into the header, and are not used on the way back.** `-N` is not in the brief, and no time can be set.
- **Compressed data is not written to, or read from, a terminal without `-f`,** as GNU gzip. `isatty()` knows only the console, so
  this does not reach a telnet session's pty.

### tar

- **`ar_safe_path()` is the only way a name becomes a path.** It takes `/` and `\` both as separators when it looks for `..`,
  though only `/` is one here: a `\` is refused anyway, but a Windows name that climbs out should say so. The rules are
  fs/fat.c's and the kernel enforces them too; here they hold before a system call sees the name, with a reason for the message.
- **Refused means skipped.** A bad name, a link, a device: said, status 2, and the entries around it are taken. The brief's
  word is refuse, so nothing is stripped on the way in. On the way out, `c` does strip a leading `/` and everything up to a
  `..`, as GNU tar does, because `x` would refuse them; it says so.
- **`ar_show_name()` is the only way a name reaches the terminal.** An archive's name could otherwise be commands to it. C1's
  CSI as well-formed UTF-8 (U+009B) is a legal FAT name and must not go out bare; it is escaped too. Refused names are shown the
  same way, and t lists everything.
- **A pax header is read a record at a time, with only the first 4 KB of a record kept.** A Mac's extended attributes can be
  megabytes around the two records that matter. Nothing is allocated, and no length is believed further than the input goes.
- **A pax record that cannot be read is reported at the end and the entry keeps its own name.** It is not fatal: the ustar
  name is checked like any other, and a size that went wrong shows as a bad header next.
- **`GNU.sparse.*` in a pax header, and type `S`, are skipped.** The data is not the file's, and writing it out would be wrong in
  a way nobody would notice. An `S` header's extension blocks are read past.
- **Links, folders and devices have no data, whatever their size field says.** POSIX says so, and it stops a header hidden in
  "data" from being reached one way by tar and another by a scanner. A file by type and a folder by name (v7) still has its size
  read past, as GNU tar does.
- **An archive with no end marker is status 2,** though GNU tar is silent. Every tar writes the two zero blocks, so their absence
  means a cut at a block boundary, which nothing else can show. Files before it stand.
- **After the end marker the rest of the input is read.** Through gzip that checks the CRC of all that was read, and a pipe's
  writer is not left without a reader. A `.tgz` whose CRC is wrong is found though the tar end marker came first.
- **The archive is named by its absolute path once opened.** `-C` moves the program, and `unlink(archive)` on the way out
  would otherwise have deleted a file of that name where `-C` went. This was a bug in the first draft, and it has a test.
- **A folder whose own name will not fit ustar loses its entry, not its contents.** `x` makes the folders above a file.
- **Names that will not fit are left out, with a message and status 2.** The brief says tar writes ustar, so there is no pax
  header on the way out. A name of 101 to 255 bytes with no `/` near its end is the case; such names are rare on FAT.
- **No gzip auto-detection on `x`.** Not in the brief. Without `z` the message says "not a tar archive (a gzip one needs z)".
- **Named members and `--exclude` ignore ASCII case,** FAT's rule. Named members take a folder's contents; `--exclude` matches any
  run of whole components, as GNU tar's does by default, and `*` crosses `/`.
- **Files under `/dev` are left out of `c`:** `tar cf x /` would otherwise block forever reading the console.
- **New files are not fsynced one by one on `x`; one `sync()` ends the run.** Replaced ones are, through saveutil. A
  thousand fsyncs would be a thousand flushes of the same cache, and `sync()` gives "finished" the same meaning.

### unzip

- **The central directory is read whole, up to 16 MB, and checked before anything else.** The format lets entries come in any
  order, and past 2 GB this kernel can only read forward (below), so reading it in one pass is what keeps a large archive
  workable. 16 MB is 65535 entries of 200 bytes.
- **`zip_check_directory()` is the bound on the archive:** each entry inside the file, before the directory, and the sum of all of
  them at most the directory's offset. It is a sum, not a sort of intervals: it takes no memory, and it is enough, because then
  what can be read overlapping is limited by the archive's size, and output by deflate's ratio. Fifield's overlapping bomb is
  refused before anything is written.
- **The member is bounded both ways.** The reader never asks for more than `csize`, and `out` never gets more than `usize`; one byte
  over is the end of it. Data that ends before its compressed size says is refused too. A claim of 4 GB costs nothing.
- **The local header must agree on the name, byte for byte, and the method.** This is the old trick of one name for a scanner
  and another for the extractor. Info-ZIP warns; here the member is skipped.
- **Names.** With the flag, UTF-8. Without it, code page 437, except that one made on Unix or a Mac and valid as UTF-8 is that:
  both write UTF-8 without saying so, and `ditto` is what Finder uses. The test runs `ditto` on the Mac. Info-ZIP's Unicode Path
  extra field is not read.
- **Without `-o` or `-n` a file that exists stays, and that is trouble (status 2).** Info-ZIP asks. Nothing else here prompts, and a
  prompt in a script would hang.
- **`-p` and `-t` do not check names:** nothing is made of them.
- **Zip64 is refused only where needed:** a saturated field. A Zip64 extra field an archive did not need is ignored, and Python's
  `force_zip64` is tested. FAT stops at 4 GB, so nothing past the limits could be held anyway.
- **lseek.** `vfs_lseek()` keeps 32 signed bits everywhere, so nothing reaches past 2 GB, not even by `SEEK_CUR`. `seek_to()` seeks
  as far as it can and reads forward. A member past 2 GB costs a read of what lies between. `test_unzip` runs a 2.2 GB archive on
  a simulated `lseek` with the kernel's rules.
- **Not supported: an archive with a prefix** (a self-extractor). Its offsets fail the local header's check, with that message.

### The tests

- **`ar_sim.c` is new, rather than an extension of `sim_fs.c`.** That one has 40 flat names and belongs to `test_edit`; this needed
  real folders, `lseek`, `chdir`, descriptors counted to 16, quotas, a failure at the nth change, the disk pulled and sparse
  files, and the tools' own memory from the host's `malloc` so a sanitizer sees each allocation.
- **Fixtures are made byte by byte in the test,** with a CRC of the tests' own, so none depends on the code under test.
- **Sanitizers run on the Mac, not in Docker.** The brief's first version asked for `-fsanitize=address,undefined` wherever the
  host compiler has it; a correction during the work said otherwise: a `gcc -m32 -fsanitize=address,undefined` binary hung at 99%
  CPU in the Docker image under qemu-i386, though it compiles fine. So no `-fsanitize` build is anywhere in the `Makefile`, and
  `tests/archive-sanitize.sh`, for Darwin only, runs all four tests natively with clang. One such probe of mine ran in Docker before
  the correction: it finished, left nothing, and nothing depends on it.
- **The native build is 64-bit, and uses two stand-ins.** libsurf's `printf.c` has x86's `divl`, so `ar_sim.c` gives the host's
  formatter there (`AR_HOST_PRINTF`); `make unittest`, at `-m32`, uses libsurf's own. `string.c` takes a `long` for four bytes, so
  `ar_string64.c` builds it with `long` as `int`, and the real code still runs under ASan. So the two runs cover each other: one has
  sanitizers, the other SurfOS's ABI and formatter. The `-m32` run has already earned its keep: it found four evaluation-order
  bugs in the tests that clang hid (a call that sets a length, and that length, among one call's arguments).
- **Each comparison skips itself, and says so.** Docker has GNU gzip, GNU tar and Python; the Mac has Apple gzip, bsdtar, Info-ZIP
  and `ditto`, which only the sanitizer script reaches.

## What was left out, and why

- `gzip -r -v -l -N`, `tar -r -u -A`, and any tar option the brief does not list: not asked for.
- Bracket patterns in `--exclude` and unzip's names: `*` and `?` only. The page says so.
- Writing pax or GNU long names: the brief says ustar.
- A level 0 for gzip, and flush points in deflate: nothing here needs them.
- Cleaning up after Ctrl+C: a program has no way to be told in time.
- Self-extracting zips, Info-ZIP's Unicode extra field, and encrypted members.
- `tests/test_man.py` is sys's: these five pages wait for it.

## Tests

Host tests, in `make unittest` (target `unittest-archive`), at `-m32` in Docker, under a minute in all:

| Test | Checks | What |
|---|---|---|
| `test_deflate` | 645 | CRC-32's check values; inflate on hand-made streams, every refusal, cut at each byte, one byte of input or room at a time; deflate at each level over a corpus, its worst symbols included; 9000 mutated and 3000 random streams; a bomb that stops where `out` ends |
| `test_gztool` | 707 | the options; each header flag; each kind of damage; every cut; 400 mutated files; a change failing, and the disk pulled, at every step; a full disk; a half-done rename; twins; GNU gzip both ways; Python's zlib for fixed, RLE, Huffman-only, level 0 and sync-flushed streams |
| `test_tar` | 614 | `ar_safe_path` and friends, each rule alone; c, x, t and their options; every header form, byte by byte; 30 hostile names as files and as folders; every cut; 3000 mutated archives, half with their sums put right; a change failing at every step; full disks; the host's tar both ways; Python's tarfile in three formats, both ways |
| `test_unzip` | 309 | the options; names in UTF-8 and code page 437; refusals with the entries around them taken; some 20 ways of lying about a size, offset or name; the overlap bomb; each field of the end record; every cut; 4000 mutated archives, each extracted, listed, tested and piped; a change failing at every step; a 2.2 GB archive; Python's zipfile |

`tests/archive-sanitize.sh`, on the Mac, runs the same four natively under AddressSanitizer and UBSan, with what Docker skips:
Apple gzip, bsdtar, Info-ZIP's zip, `ditto`, and a member read across 2 GB with its CRC. All four pass, 25 s, 315 checks for
`test_unzip` there. Under `make unittest`, `test_unzip` says what it skips: Info-ZIP, `ditto` and that read.

**A mutation pass.** Tests that cannot fail prove nothing, so on a scratch copy 66 safety properties were broken one at a time
(a path rule gone, a CRC unchecked, a bound removed, a failed write committed) and the test that should notice was run under
sanitizers. 62 were caught at once. The other four were looked at, and each now has a test of its own:

- `..` was still refused, as a name that ends in a dot. Not a hole, but the rule's own reason is now checked.
- A negative base-256 size was only ever all ones, which an overflow check also stops. Small low bytes are now tested.
- Nothing ran `tar c ..` from a subfolder, which is the one way to reach a member that would have started with `/`.
- A member holding more than it claims was refused in the end whether or not the bound was there, by the check that it holds
  less: bytes past the claim could have gone out first. `-p` now shows that what goes out stops at the claim.

All 66 are caught now. The script is not in the tree: it is a pass that was made, not a test that runs.

**On QEMU.** The smoke block, `echo cli-archive`, has 26 steps: gzip, zcat and gunzip on FAT; a cut file and an output that exists;
tar with z, -C, --exclude, x over a file, and a pipe; unzip by -l, -t, -p and unpacking of a zip written out with `printf`; and a
zip and a tar each with a member named `../up`, refused on the real FAT with the members around it taken. It is a little over the
brief's "about 25" because its two fixtures take 11 lines: a typed line loses bytes on the serial path past about 250 characters.

**`make unittest` and `make test-all`** passed on the tree these commits hold: `test-all` through the lock, 545 checks at -O0 and
545 at -O2. No kernel, file system or shell file changed, so `make -C surfcpu test-os-qemu` and the storage scenario do not apply.

None of this has been on the laptop or on a stick.
