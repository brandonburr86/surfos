# Brief: everyday console and shell (track A)

## The request

The owner, 2026-10-25, asked for the highest-impact improvements to the kernel and its interface. They chose
everyday use on the laptop, a platform for new apps and robustness, with the console and the desktop
balanced. This brief is the console and shell part.

Two other tracks run at the same time:
- robustness: `2026-10-25-robustness.md`;
- windows for user programs: `2026-10-25-app-windows.md`.

**Where:** a worktree of its own, `../06-restoration-daily`, on branch `daily/main`. It starts from
`ai-dev` once storage milestone 1 has landed (its merge commit was ready on `storage/merge`, 3f27b61, when
this was written). It is merged into ai-dev only at the owner's word (protocol below), and nothing is pushed.

**What the scan found** (2026-10-25, ai-dev 36bb341):
- surfsh runs a kernel command before a program of the same name (`user/surfsh/exec.c:470-471, 670`):
  - `ls -l` runs the kernel's `ls`, which reads only its first argument;
  - `echo x | cat` is refused.
- TAB is dropped on F1–F4 (`kernel/console.c:1172-1174`) and on the serial mirror (`driver/serial.c:96-97`).
- There is no scrollback anywhere.
- Special keys lose Shift, Alt and Ctrl (`kernel/keyboard.c:162-190`).
- The framebuffer console is Spleen 8x16 at scale 2, 120x33 at 1920x1080 (`driver/fbcore.c:125-143`).
- Each scrolled line redraws nearly every cell, up to about 8 MB of pixel writes on the laptop
  (`console.c:236-253` → `driver/framebuffer.c:226-231`).
- surfsh has no Ctrl+R, no startup file and no saved history; there is no `less`, and `edit` has no undo.

## What gets built, stage by stage

Each stage is one or a few small commits that build and pass the checks below before the next begins, so
the branch can stop after any of them.

| Stage | What | Where |
|---|---|---|
| A1. TAB | `\t` to the next multiple of 8 on the screen and the serial mirror | `kernel/console.c`, `driver/serial.c`, `tests/host/pager_bridge.c` |
| A2. Programs first | a program on PATH runs before a kernel command of the same name; `type`; `kernel NAME` as before | `user/surfsh/exec.c`, `builtins.c`, `user/bin/hexdump.c`, `user/bin/df.c`, `tools/qemu-run.py` |
| A3. A fast screen | -O2 for the drawing code, presents batched at most every 20 ms, cheap cursor moves, counters | `Makefile`, `driver/framebuffer.c`, `driver/fbcore.c`, `kernel/console.c`, `kernel/tty.c`, `user/surfsh/` |
| A4. Modifier keys | Shift/Alt/Ctrl with special keys, as xterm sequences in raw mode; F5–F12 | `kernel/keyboard.c`, `kernel/tty.c`, `include/surfos/keyboard.h`, `include/surfos/tty.h` |
| A5. Line editor | Ctrl+R, word moves, Alt+., `!!` and friends, 1,000 lines of history | `user/surfsh/editor.c`, `surfsh.h`, new `tests/host/test_editor.c` |
| A6. Scrollback | Shift+PgUp/PgDn/Home/End on F1–F4 (audit C9's last part) | `kernel/console.c`, `kernel/keyboard.c`, `include/surfos/console.h`, `driver/framebuffer.c` |
| A7. Fonts | Spleen 12x24 and 16x32 besides 8x16; `font=`; 12x24 the laptop's default | `driver/fonts/`, `tools/bdf2c.py`, `driver/fbcore.c`, `include/sys/framebuffer.h`, `tools/qemu-usb.py` |
| A8. Escapes | CSI G/d/s/u/X, ESC 7/8, more SGR, the alternate screen `?1049` | `kernel/console.c`, `include/surfos/tty.h`, `tests/host/vt_cases.h` |
| A9. Tools | `less`, `find`, `du`, `xargs`, `cut`, `sha256sum`; `edit` undo and UTF-8 | `user/bin/`, `Makefile`, `tests/host/test_edit.c` |
| A10. Startup file and history | `surfshrc` and saved history in `surfos-config/` on the stick SurfOS started from; `~`; `$HOSTNAME` in `≋@>` | `kernel/netman.c`, `fs/devfs.c`, `shell/settings.c`, `user/surfsh/` |

## Decisions

The owner's, 2026-10-25:
- **Programs first.** The order becomes: function, builtin, program on PATH, kernel command. `kernel NAME`
  still reaches the kernel's version.
- **Spleen 12x24 by default on the laptop:** 160x45 cells, drawn at the font's own size. It is the default
  only under `profile=laptop` at 1920x1080 or more. VGA text, 1280x800 and every boot without
  `profile=laptop` stay exactly as they are; `font=8x16|12x24|16x32` chooses another.
- **A startup file and saved history on the boot stick are approved.** They are a new writer on the volume
  SurfOS was started from, through the network manager's existing path and only for these two files.
  History saving is off until `settings history on`.

The integrator's:
- **The console's escape parser and `shell/guivt.c`'s are not merged.** They parse alike, but what they do
  with a sequence differs completely (grid, framebuffer, serial mirror and pager, against a ring of lines),
  and `guivt.c` belongs to track C now. Instead, `tests/host/vt_cases.h` holds cases that `test_pager` and
  `test_guiterm` both run.
- **Rows are never moved by reading the framebuffer back.** It is mapped write-combining (`VMM_WC`), so
  reads are slower than drawing again.
- **What the precedence change gives up:**
  - the kernel `ls`'s default long listing with its totals line (use `ls -l`, or `kernel ls`);
  - `cat`'s filtering of control characters;
  - `cp`'s "N bytes copied".

  Every program's output reaches pipes and files, and `type NAME` says which command runs and what it hides.
- **History never records** a line that starts with a space, as bash's `ignorespace`. The guide says the
  file is plain text on the stick. Wi-Fi passwords are asked by `shell_ask_secret()` and are never typed on
  a command line.

## The stages in detail

**A1. TAB (S).**
- In `kputch()`'s default case, `\t` moves to the next multiple of 8. It stops at the last column, does
  nothing at a pending wrap, and leaves cells alone: the same rules as `stream_put` (`console.c:663-710`)
  and `guivt.c`.
- `driver/serial.c:96` passes `\t` through; `tests/host/pager_bridge.c:42-49` mirrors serial's rules.
- No kernel message prints a TAB, so the boot log doesn't change.
- Checks:
  - `test_pager`: the cells and the serial record;
  - SMOKE `printf 'a\tb|\n'`;
  - the qemu-usb `fullhd` scenario reads `1       2` off the screen.

**A2. Programs before same-named kernel commands (M).**
- **The rule:** change `stage_kind()` (`exec.c:465-473`) and the simple-command path (`exec.c:670-680`). Add
  a lookup that says nothing next to `find_program()` (`exec.c:695`). Update `program_of()` (`exec.c:484`),
  the help sentence about the order, the `*` markers (`builtins.c:445,486`) and the header of `run.c`.
- **New builtin `type NAME`:** says whether a function, a builtin, a program (its path) or a kernel command
  runs, and what it hides.
- **What users keep:**
  - `user/bin/hexdump.c` calls `kcmd()` when its argument is not a file, so `hexdump usb0 0` keeps working
    on block devices.
  - `user/bin/df.c` takes over the kernel df's columns (human sizes, type, label).
- **Checks:**
  - The ~25 SMOKE steps that check kernel output formats (`3 entries`, `bytes copied`, df's columns,
    `hexdump hda 0`) get a `kernel ` prefix where they test the file systems rather than the format.
  - Add `ls -l /initrd`, `echo x | cat`, `cat < /initrd/motd`, `hexdump hda 0` and `type ls`.
  - Check the uses of cat, ls, cp and df in `tools/qemu-usb.py` and `tools/devloop.py` (devloop's `mkdir`
    "File exists" match).
- **Land it first** and tell the other tracks' sessions: it rewrites lines of the `SMOKE` table they edit
  nearby.

**A3. A fast screen (M, four commits).**
- **(a)** Target-specific `CFLAGS += -O2` for `driver/fbcore.o`, `driver/framebuffer.o`, `shell/guidraw.o`
  and `shell/gui.o`, as Mbed TLS has (`Makefile:772-776`). The laptop runs -O0 builds.
- **(b)** `framebuffer_present_lazy()`:
  - If the last present was 20 ms ago or more, present now. Otherwise mark the screen pending and start a
    one-shot `ktimer` that wakes an `fbflush` kthread. Timer callbacks run with interrupts off and must not
    draw.
  - The kthread is made in `init_console_grid()`, so VGA boots never have it.
  - While a present is pending, `framebuffer_draw_cell` and `set_cursor` only record.
  - `tty_getc()` flushes before it waits, so prompts and `--More--` appear at once.
  - `sscroll`, `ansi_blank` and `clearConsole` call the lazy version; panic output and graphics sessions
    keep the immediate path.
- **(c)** When only the cursor moved, redraw just the two underline rows. Today `cell_at()` draws the cell
  with the cursor, then `setpos()` redraws the old and new cursor cells: three glyphs per character.
- **(d)** Counters in `gfxinfo` (presents, glyphs drawn, presents saved, the slowest present in ms) and a
  `time` builtin in surfsh.
- **Targets** (on the laptop at -O0, and in the `fullhd` scenario):
  - `cat` of a 1,000-line file and `dmesg` in under 1 s (estimated 10–25 s today);
  - `time seq 1 3000` under 1 s;
  - at most about 50 presents a second.

**A4. Modifier keys end to end (M).**
- `kernel/keyboard.c` delivers a key code and its modifiers. Today Alt is used only for Ctrl-Alt-Del
  (`keyboard.c:142`), and Ctrl with a non-letter is dropped (`keyboard.c:181-185`).
- In raw mode `kernel/tty.c`'s `raw_keys[]` (`tty.c:240-253`) sends xterm sequences, with
  m = 1 + Shift + 2×Alt + 4×Ctrl:
  - arrows, Home, End: `ESC[1;<m>A`…`D`, `H`, `F`
  - Insert, Delete, PgUp, PgDn: `ESC[n;<m>~`
  - F5–F12: `ESC[15~`…`ESC[24~`
  - Shift+Tab: `ESC[Z`
  - Alt+key: ESC then the key
  - Ctrl+Space: NUL; Ctrl+/: 0x1F
- Line mode doesn't change. Ctrl-Alt-Del and F1–F4 (which switch consoles even in raw mode,
  `keyboard.c:115-121`) are still handled first.
- Checks:
  - selftest `test_tty` (`shell/selftest.c:1335` expects F5 to be dropped today);
  - the qemu-usb keyboard scenario's bytes (`1b 5b 41 7f 0d 78 71` today);
  - a `rawkeys` SMOKE step with `ctrl-left` expecting `1b 5b 31 3b 35 44`;
  - `shift-`, `ctrl-` and `alt-` combinations added to the harnesses' key names.
- This encoding is an interface for track C: the desktop's key parser (`shell/gui.c:264`) and the window
  API's `mods` read it.

**A5. Line editor (M).**
- `sequence()` in `user/surfsh/editor.c:302-344` returns modifiers. ESC followed by another byte in the same
  read is Alt; a lone ESC is Esc.
- New keys: Ctrl/Alt+Left/Right, Alt+B/F/D, Alt+Backspace, Alt+. (the last word of the previous line).
- **Ctrl+R incremental search:** Ctrl+R again finds older matches, Enter runs, Esc or ^G restores the line.
- **History expansion:** `!!`, `!$`, `!n` and `!str`, expanded when the line is entered and echoed. Not
  inside single quotes, nor for `! ` and `!=`.
- History holds 1,000 lines (`HIST_MAX` in `surfsh.h:53`, 128 today).
- New `tests/host/test_editor.c` builds `editor.o` and `complete.o` with scripted keys, using the renames
  `test_edit` uses (`sim_names.h`). Today they are not host-tested (`Makefile:475`).

**A6. Scrollback on F1–F4 (M/L; audit C9).**
- **The ring:** one per console, allocated at the first line that scrolls off, framebuffer consoles only.
  512 KB each (about 1,600 lines at 160 columns), 2 MB at most.
- `sscroll()` pushes the top row, and `ESC[2J` pushes the screen, so `clear` and Ctrl+L lose nothing. The
  alternate screen (A8) doesn't feed the ring.
- **Keys:** Shift+PgUp/PgDn and Shift+Home/End are caught in `keyboard.c` (after A4) and call
  `console_view()`. It composes ring rows and live rows into what the screen shows, hides the cursor, and
  puts a "−N" tag at the top right.
- **Output while scrolled back:** a `shows_live()` check replaces `isConsoleActive()` in `cell_at`,
  `ansi_blank`, `syncVideoConsole` and `setpos`, so output doesn't move the view. Any other key, or a
  console switch, snaps back to live.
- **Interactions:**
  - At `--More--` Shift+PgUp works, because it never reaches the tty.
  - Under the desktop the key goes to the desktop as `ESC[5;2~`.
  - Graphics sessions are not affected.
- Checks: `test_pager` (its bridge already runs `console.c` as an 80x25 framebuffer console) and a qemu-usb
  `<shift-pgup>` screen check.

**A7. Fonts (M).**
- Add `spleen-12x24.bdf` and `spleen-16x32.bdf` to `driver/fonts/`; `LICENSE-spleen` covers them.
- `tools/bdf2c.py`:
  - takes the size from `FONTBOUNDINGBOX`;
  - writes u16 rows;
  - keeps the `/* U+xxxx */` comments that the USB harness reads.
- The four SurfOS symbols (`surfos-symbols.bdf`) double exactly for 16x32. 12x24 needs a small hand-drawn
  BDF for whatever Spleen lacks (bdf2c lists those on stderr).
- `struct fb_font` goes in `struct fb_surface`, and `fb_setup()` takes it (`fbcore.c:125-143`, scale =
  min(w/640, h/400) today).
- **The default:** 12x24 under `profile=laptop` at 1920x1080 or more; `font=` on the kernel command line
  overrides it.
- `tools/qemu-usb.py`'s `Screen` (about lines 236–280) decodes pixels with `font8x16.c` and copies
  `fb_setup`'s geometry. It learns the chosen font, and the `fullhd` scenario's numbers change (160x45).
- The desktop keeps 8x16 through `shell/guidraw.c`; track C's sharp-desktop stage (C9) uses `fb_font`
  later. Record `struct fb_font` under "Interfaces fixed" when it is done.

**A8. Console escapes and the alternate screen (S–M).**
- Add to `console.c`, with the column tracking in `stream_csi` (`console.c:63`), the list in
  `include/surfos/tty.h:83-85` and the `ansi` command:
  - CSI G, d, s/u and X; ESC 7/8 (the telnet path already handles CSI G, `console.c:614`);
  - SGR 22/27/39/49/90–97/100–107, skipping 38/48 as `guivt.c` does;
  - `?1049h/l`: a saved grid per console and a new `TTY_CAP` bit, so `less` and `edit` give back the
    shell's screen.
- Leave L/M/@/P/r until a program needs them.
- `tests/host/vt_cases.h` is shared with `test_guiterm`. Track C owns `guivt.c`; agree the file with them.

**A9. Tools and `edit` (M overall).**
- `user/bin/less.c` (M):
  - a file or stdin, with keys from `/dev/tty`;
  - raw mode and the alternate screen;
  - `/` search with n/N, g/G;
  - long lines cut, UTF-8 drawn.
- Small programs (S each): `find`, `du`, `xargs`, `cut`, `sha256sum` (linking the Mbed TLS sha256 that is
  already built). `diff` later; no `sed`. Helpers: `user/bin/lineutil.h`, `fileutil.h`.
- `edit` (`user/bin/edit.c`), M each:
  - undo and redo on Ctrl+Z/Ctrl+Y, grouping runs of typing;
  - UTF-8 as characters in `render_line`, `col_of` and `index_of`, with the shadow screen at 4 bytes a
    cell (libsurf has `utf8_feed()`);
  - checks in `tests/host/test_edit.c`.

**A10. A startup file and saved history (M).**
- **`kernel/netman.c`:** a generic `netman_file_read(name)` / `netman_file_write(name)` that reuses its path:
  1. the stick lock;
  2. the short mount of the volume SurfOS was started from (`bdev_boot_vol()`), or the owner's own mount
     held with `vfs_hold_mount()`;
  3. a write to a `.new` file, sync, rename, read back;
  4. `netman_can_save()`;
  5. unmount.

  Only `surfshrc` (read) and `history` (written) are allowed.
- **No new system call:** devfs files `/dev/config/surfshrc` and `/dev/config/history` are read when opened
  and written when closed.
- **surfsh:**
  - An interactive surfsh runs `surfshrc`, after `/initrd/etc/surfsh.conf` (`main.c:128-160, 424`).
  - History is loaded at start. With `settings history on` it is merged, capped at 1,000 lines, and written
    at exit, on `history -w`, every 25 commands and at shutdown (track B's B4 calls a hook).
  - `~` expands to HOME (`HOME=/`, or a `home` setting).
  - telnetd exports `$HOSTNAME`, which fills the remote prompt `≋@>` (`shell/prompt.c:135-137`;
    `editor.c:174-188` never sets it today).
- Check: SMOKE `@boot settingsdisk=` twice; the history survives the reboot.
- Update the storage rule's text in CLAUDE.md: name the two files.

## Interfaces fixed

Recorded as each stage landed on `daily/main` (10/25/26); fields and functions are added, not changed.

- **A3, the screen's presents** (`include/sys/framebuffer.h`): `framebuffer_present_lazy()` (at once when the last
  present was 20 ms ago or more, otherwise by the `fbflush` thread), `framebuffer_flush()` (a pending present now;
  `tty_getc()` calls it through `console_flush()`), and from A6 `framebuffer_present_soon()` (by the thread at once,
  for an interrupt handler). Graphics sessions keep `framebuffer_present()`.
- **A4, keys in raw mode** (`kernel/tty.c`, `include/surfos/tty.h`): xterm's, with m = 1 + Shift + 2·Alt + 4·Ctrl:
  `ESC[1;mA`-`D`, `H`, `F`; `ESC[n;m~` for Insert, Delete, PgUp, PgDn and F5-F12 (`ESC[15~`-`ESC[24~` without
  modifiers); Shift+Tab `ESC[Z`; Alt+key ESC then the key; Ctrl+Space 0x00, Ctrl+/ 0x1F. `tty_key(t, c, mods)` with
  `KEY_SHIFT` 1, `KEY_ALT` 2, `KEY_CTRL` 4 (`include/surfos/keyboard.h`). Track C's `win_event.mods` is m - 1.
- **A6, scrollback** (`include/surfos/console.h`): `console_scroll_key(key)` takes Shift+PgUp, PgDn, Home and End
  from `keyboard.c` (1: the view's; 0: the tty's, in VGA text mode and under a graphics session, where the desktop
  gets `ESC[5;2~` and the like: `framebuffer_graphics_on()`); `console_view_live()` before any other key.
- **A7, fonts** (`include/sys/framebuffer.h`): `struct fb_font { name, width, height, rows8, rows16 }`, the 256 code
  page 437 glyphs a row each (`u8` up to 8 pixels wide, `u16` up to 16, the most significant bit leftmost);
  `fb_font_8x16`, `fb_font_12x24`, `fb_font_16x32` (`driver/font*.c`, from `tools/bdf2c.py`); `struct fb_surface`'s
  `font`; `fb_setup_font(s, base, mode, font)` (`fb_setup()` is 8x16); `fb_font_named()` and `fb_font_choose()`
  (`driver/fbfont.c`). The desktop keeps `font8x16`; track C's C9 draws its sharp text with these.

## Shared with the other tracks

- **`tools/qemu-run.py`'s SMOKE table, the Makefile's program lists, `shell/selftest.c`'s `tests[]` and
  CODE-AUDIT:** every track appends to them. Textual conflicts only.
- **`kernel/keyboard.c`:** track B's shutdown (B4) changes Ctrl+Alt+Del at `keyboard.c:142`. Whoever lands
  second rebases.
- **`shell/gui.c`, `shell/guicore.c`, `shell/guivt.c`:** track C's. This track changes only the Makefile
  flags of `gui.o` and `guidraw.o` (A3a) and agrees `vt_cases.h` with them.
- **`driver/framebuffer.c`, `driver/fbcore.c`:** this track's (A3, A7). Track C's C9 uses `fb_font` later;
  `framebuffer_present_lazy()` and `fb_font` are interfaces to agree early.
- **`kernel/tty.c`:** this track's. Track B doesn't touch it.
- **System calls:** none. The A10 files are devfs files.

## Merging

The protocol of 2026-10-25:
1. Re-read `git log -1 ai-dev`.
2. Merge on `daily/merge` in this worktree (`git switch -c daily/merge ai-dev`,
   `git merge --no-ff daily/main`) and run the whole suite there.
3. Then `git merge --ff-only` in the main worktree, only if ai-dev hasn't moved.
4. Message the sessions waiting their turn (ListAgents / SendMessage).

The track that lands first keeps its ARCHITECTURE.md section numbers; the later one renumbers and says so in
its merge message. Merge at the owner's word, after A2 (early, for the SMOKE churn), after A7, and at the
end; pushing waits for the owner.

## Checks

- Every commit builds and passes `make unittest` and `make test-all` (-O0 and -O2). Every commit also passes
  `make test-usb` at -O0 and -O2, because the USB scenarios drive surfsh, the console and the keyboard.
- On the Mac: `./devenv/build.sh`, `./devenv/build.sh test-all`, `./devenv/build.sh unittest`, then
  `O=0 ./devenv/build-usb.sh && ./devenv/test-usb.sh` and the same with `O=2`. In this worktree, export the
  disk images first (`SURFOS_OUT=<worktree>/build/docker ./devenv/build.sh images`); without them the
  usbdisk and storage scenarios fail 2 checks and skip 67, which is not a regression.
- **The acceptance test of the whole track:**
  - `ls -l /initrd`, `echo x | cat`, `printf 'a\tb\n'` on F1 and over telnet;
  - `time cat` of a 1,000-line file under 1 s in `fullhd`;
  - Shift+PgUp shows earlier output;
  - Ctrl+R finds an earlier command;
  - `font=12x24` gives 160x45 (qemu-usb reads it);
  - `less` and `edit` give back the shell's screen;
  - history survives a reboot with `settings history on`.
- **On the laptop** (when the integrator gives this track the laptop; `tools/devloop.py deploy`): the speed,
  the 12x24 font, scrollback, the keys. The owner looks at the screen.
- At the end, an independent review, as the earlier tracks had. Each finding gets a check.

## Rules (as for every track)

- **CLAUDE.md:**
  - kernel commands print on `curTask->con` and return CMD_OK, CMD_FAIL or CMD_USAGE;
  - the prompt is drawn only by `shell/prompt.c`;
  - without `profile=laptop` the boot stays exactly as it was;
  - every hardware wait is bounded.
- Keep the 2004 file headers where old code is touched. New files get the same header block with the
  current date.
- Small commits, one topic each, citing audit IDs where an item closes one (C9: scrollback).
- **Docs:**
  - ARCHITECTURE.md (console, surfsh);
  - USB-BOOT.md (the guide: fonts, keys, history);
  - CLAUDE.md (new commands, the `font=` word, the A10 files under the storage rule);
  - CODE-AUDIT.md;
  - ROADMAP.md §7 (what this track closed);
  - the row of USB-BOOT.md's table that still lists "Pipes, variables, wildcards, job control" as missing.
