# The desktop applications: the integration's account

Branch `apps/integration`, worktree `06-restoration-apps`, volume `surfos-apps`, from the brief's commit e5f9a2e0
(`docs/superpowers/plans/2026-11-05-desktop-apps.md`, "After the tracks"). The tracks were merged in the order
their final hashes came, each with `git merge --no-ff --no-commit`, checked, and committed as `Merge apps/<track>
<hash>: ...`. Nothing went to `ai-dev` and nothing was pushed.

## The merges

| Track | Tip | Merge | Merge bases | Conflicts, and how they were resolved |
|---|---|---|---|---|
| desk | bba24dae | 35246763 | e5f9a2e0 | none |
| img | e9eb5b32 | cf1dc059 | desk 2c5afd30 | none (`shell/guicore.c`'s tick line and `include/sys/gui_media.h` merged by themselves) |
| sheet | 57d64db8 | 20d99281 | desk 2c5afd30 | `Makefile`, `tools/gui_apps_scenario.py`: sheet's hunks (and kit's, which came with sheet) re-applied from the merge base onto HEAD's copy (`cli_merge.py`) |
| chat | d2fd9a44 | 34f90407 | kit aac42b11, desk 2c5afd30 | `Makefile` (three regions), `TRACKS` |
| games | e5ce0b70 | 98c3a90a | desk 2c5afd30, kit 2ba18949 | `Makefile` (three regions), `TRACKS` |
| settings | bd0098b6 | 7f72bd4f | kit aac42b11, desk 2c5afd30 | `Makefile` (three regions), `TRACKS`, `tools/qemu-usb.py`'s `SCENARIOS` line |
| paint | f721b2e1 | ddda278f | kit aac42b11, desk 1447a4c2, img aea719bd | `Makefile` (four regions), `TRACKS`, `docs/GUI.md` |
| code | 9ae58991 | 35f2d971 | kit aac42b11, desk 2c5afd30 | `Makefile` (three regions), `TRACKS` |
| kit | 40156e90 | 8b7e870d | kit d38ea834 | none: the `Makefile`, the one file both sides changed, takes `test_gksecret` once |

Every conflict was in the files the tracks shared by insertion. How each was resolved:

- **Both sides' blocks, once each.** Where HEAD's side held the earlier tracks' program or host-test blocks and the
  track's side its own, both were kept, HEAD's first. `TRACKS` holds each track's line once, in the order merged
  (desk, kit, img, sheet, chat, games, settings, paint, code), and `SCENARIOS` guichat's line, then wsettings'.
- **A conflict the virtual base made.** With two merge bases (an older desk commit and an older kit commit, both
  merged into the app's branch), git merges the bases first, and their own conflict over the host tests' place
  surfaced as a region with desk's keyscan test and img's tests on HEAD's side and nothing on the track's. The
  track removed nothing there, so HEAD's side was kept (chat, games, settings, code).
- **Paint's three bases.** The regions overlapped, so the `Makefile` was made from HEAD's with Paint's two blocks
  inserted (its program after Settings', its host tests after Settings'): kit's `Makefile` changes from aac42b11 to
  d38ea834, which came with Paint, are desk's alone, which HEAD had, and the two lines of Paint's copy the result
  lacks are img's older ones, which HEAD has in img's final form. `docs/GUI.md` gained the blank line its merge
  lost before "### Paint".
- **`include/sys/gui_media.h`**, which the coordinator expected to conflict: desk did not change it after 2c5afd30,
  so img's two fields (`asked_pid`, `asked_at`) went in alone.

**How the merges were checked.** A script compared each line's count in the result with the three-way
expectation, count in HEAD + count in the track - count in the merge base (the virtual base's counts for several
bases): for the four shared files at every merge, and from chat on for every file both sides changed (70 to 93 a
merge; at kit's, the `Makefile` alone). Every file was as expected: no block twice, none lost. `docs/GUI.md` was
also read for one section a track and a blank line before each heading. The coordinator compared 582153be file by
file with the tips.

## What the integration broke, and the fixes

- **356cc86b, the Image Viewer.** img's `viewer_replace()` (`shell/guiview.c`) read `struct gui_client`'s
  `closing`, which desk's review replaced with `closes` (the CLOSEs sent) and `win_state.close_waiting` (one not
  read): the merged tree did not build. A viewer sent no CLOSE, with none waiting, is asked to close as before; one
  sent a CLOSE that is still there is ended at once, as img meant; `client_close()` is not called while a CLOSE
  waits unread, where it would now ask "End the program?".
- **63e69f38, Swell's window test, and 582153be, the Code Editor's.** kit's stage 3 (f87a3661, which came with
  Paint's merge of kit d38ea834) greys a program's menus while a box is open and drops a menu event that comes
  meanwhile. The two tests chose a menu item under a question to see it wait; on the fake desktop the greyed item
  can no longer be chosen, so they expect the refusal, and the checks after it are unchanged.
- **521efe0d, Paint's window test.** kit's last rounds made the fake desktop keep 64 unread events, as the kernel's
  window queue does, and say on stderr when it loses any. The test typed `$TMPDIR/wpaintw.XXXXXX/two.bmp` and Enter
  before reading: on the Mac's 49-character TMPDIR that is more than 64 keys, the Enter was lost and 8 checks failed
  ("9 events lost"). Each key is now read before the next; natively on the Mac with that TMPDIR, 162 checks and
  none failed, and in Docker in `make unittest`.
- **ae1e522f, the toolkit's `gk_unwatch()` (L1, review-kit's last reading).** A key and a watched descriptor ready
  in one turn queue the key first; a program that unwatched and closed the descriptor on that key still got its
  GK_EV_FD next, with `x` the closed number, which a new pipe may have by then. `gk_unwatch()` now drops the
  descriptor's queued events as `gk_timer_stop()` drops a timer's (one `drop_queued()` for both, the other events
  keeping their order and text round the ring's end). test_gk's `unwatched()` has the case (4 of its checks fail
  without the fix); natively test_gk passes, also under ASan and UBSan. No application was hit: Chat checks its
  own descriptor, and nothing reads `ev.x` yet.
- **Not changed: Settings' window test.** A trial merge of kit's first fix round (3e1551f3) failed its check of a
  held arrow on the theme's radio group (5 applies expected, 3 got: radio groups acted once a press). kit's last
  round (N1) made movement keys repeat again and only activation keys act once a press, so the test passes as
  written. The apps needed nothing else for kit's rounds: no caller uses `gk_set_text()`'s result, the event
  switches have no default (GK_EV_UNCOPIED is ignored), Chat's watch ends at `read() == 0` in `end_link()`, as the
  level-triggered watch asks, and the Code Editor's Ctrl+] and Ctrl+/ stay its own keys (no menu item has them).
- **c39e776e, guichat in the default list.** `make test-usb` and the gate run `tools/qemu-usb.py --test` with no
  `--scenario`, which runs the default list in `main()`; a scenario joins the gate by a line there, as fsck did
  and as Settings' wsettings did by its own insertion. Chat had left guichat to be run by name; it is in the list
  now, its docstring and its `SCENARIOS` line no longer say otherwise, and the default list has 26 scenarios.
- **7c3b5a87, the games' cleanup.** desk's second review: a second press of the close button sends CLOSE again to
  a program that reads its events, and the third asks "End the program?". The games' `tidy()` pressed twice, so a
  game that read both and stayed was left open after a failure; it presses three times now. The other checks:
  sheet's cleanup presses three times; Paint's presses twice and answers Right, Enter, which is "Don't save" in its
  question or End in the desktop's when it is stuck; the Code Editor's presses, quits, presses; Chat's and
  Settings' press once (their branch for "End the program?" is not reached by one press, under either rule); kit's
  presses the close button once, at one of two widths, and answers wkit's question, so the rule changes nothing
  there either.

## The docs

`CLAUDE.md`: the host tests, 26 scenarios, `kernel/netctl.c`, the toolkit, the pictures and the programs in the
layout, the desktop's files and the registry, the close button's rule, floating point in windowed programs (the
M12 paragraph and the Undertow section's note), the window calls the applications added, and `GUIAPPS_ONLY` a
track, and gk.h's rules as kit's last round left them (movement keys repeat, activation keys once a press; a
level-triggered watch until `gk_unwatch()`; menus grey under a box, a choice made just before one dropped; the
fake's 64 events). `docs/ARCHITECTURE.md`: section 24's close rule and floating point, and section 25, the desktop
applications. `docs/ROADMAP.md`: M14. `docs/README.md`: GUI.md's line, the brief and the accounts. `docs/USB-BOOT.md`:
26 scenarios, and the applications in the table of stages.

## The gate

(to come)

## Known lows

Each track's account has the detail.

- **desk:** a program started by an earlier desktop session can be reported "ended" in a later one, since the
  records are keyed by the shell's pid; the third press replaces a program's own question (by design); Help items
  that look like the desktop's beyond blanks and trailing punctuation are documented limits.
- **sheet:** a sheet from another program that opens inside the reader's headroom cannot be edited.
- **chat:** the ≋ test matches verbatim copies only (the reviewer's sentinel probe checks the property itself);
  `who_of()` should key the mark on `l->from[0]`.
- **games:** the docs say held keys never repeat a deal; that becomes true with kit's shortcut fix.
- **settings:** a running Wi-Fi join cannot be cancelled from the window. The way forward is in its account:
  `request_if()` returns `-EINTR` on `task_interrupted()`, so a cancel can interrupt the job's own thread, as Ctrl+C
  does a command's.
- **code:** a regex with a short branch and a long failing one (`x|x.*y`) can still make one step take seconds on
  a line of several MB, since only the clock bounds it (Esc works between steps); one pathological regexec
  (`.{1000}x`) can take seconds; with Regex and Words, a match that begins or ends with a non-word character
  (`-1`, `foo\(\)`, `café`) is not found, since the `\<( )\>` wrap needs word characters at the match's own edges
  (plain Words finds them); confirming a too-large Replace all also drops the earlier undo history, and the
  question does not say so; on lines over 8 KB auto-indent's whitespace can differ. The manual says the first and
  the last.
- **img:** the 32-scan limit of a progressive JPEG is kept.
- **paint:** none; its last round fixed the three it had.
- **kit:** while a box is up the program's menus are grey, and a choice made in the bar meanwhile is refused (F14,
  as gk.h documents it); a key held while another is tapped resumes as a new press, so one box can be answered that
  way (narrow: it comes from the keyboard's make and break codes); a lost break code costs one press; L1 (a
  descriptor's event after `gk_unwatch()`) is fixed at the integration (ae1e522f). Not done, optional: the
  applications do not say when a copy is refused (GK_EV_UNCOPIED), and the games could ask with
  `gk_confirm_default()` instead of putting Cancel first. Host-tested only: the keyboard's repeats (QEMU's sendkey
  never repeats) and a desktop dialog taking a program's chooser's place.

## On the laptop

None of this has run on the laptop, and nothing will until the owner tries it.
