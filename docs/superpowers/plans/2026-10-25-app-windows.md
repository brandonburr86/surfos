# Brief: windows for user programs (track C, milestone M12)

## The request

The owner, 2026-10-25, asked for the highest-impact improvements to the kernel and its interface. They chose
everyday use on the laptop, a platform for new apps and robustness, with the console and the desktop
balanced. This brief is the app platform: an ordinary ring-3 program opens a window on the desktop, draws in
it and takes its keys and pointer. After that comes the desktop's own polish.

The two other tracks run at the same time:
- everyday console and shell: `2026-10-25-daily-console.md`;
- robustness: `2026-10-25-robustness.md`.

**Where:** a worktree of its own, `../06-restoration-gui`, on branch `gui/win`. It starts from `ai-dev` once
storage milestone 1 has landed (merge commit 3f27b61 on `storage/merge` when this was written). It is merged
into ai-dev only at the owner's word, and nothing is pushed.

**Today** (ai-dev 36bb341):
- **Only kernel code draws.** The desktop's 13 apps are kernel code inside `cmd_gui` (`shell/gui.c:198-298`)
  and the model in `shell/gui*.c`, so a bug in any of them can panic the kernel. No program can draw.
- **This reverses a spec decision.** `docs/GRAPHICS.md:39` says "No new user-memory mapping or system call
  is provided", and the desktop spec chose "not a new userspace graphics ABI"
  (`docs/superpowers/specs/2026-10-10-desktop-design.md:17`). The owner's choice of "a platform for new
  apps" reverses that; the docs say so when this lands.
- **The window manager:**
  - fixed window ids, `GUI_WINDOWS` 15 (`include/sys/gui.h:21-38`);
  - one state per app inside `gui_state`;
  - `struct gui_app` hooks that get no window id (`shell/guipriv.h:141-172`);
  - apps listed in `suite[]` (`shell/guicore.c:39-42`).
- **The loop:** it repaints the whole scene into RAM when something changed, presents the changed bands
  (`gui.c:82-104`), then sleeps a fixed `sleep_intr(16)` (10–20 ms).
- **Scale:** the scene is 960x540 logical pixels at 2x on the laptop (`gui.c:39-49`).
- **The Terminal** (`shell/guiterm.c`, `guivt.c`, `guisys_term.c`) already hosts programs in a window
  through a pseudo-terminal: the pattern to follow.

## What gets built, stage by stage

Each stage is a series of small commits that build and pass the checks below before the next begins. The
platform is C1–C7 (milestone M12); C8–C13 follow it.

| Stage | What | Where |
|---|---|---|
| C1. Groundwork | `GUI_SLOTS`, with no behaviour change; the desktop waits on events instead of every 16 ms; frame timing; a fast present for 32-bit pixels | `include/sys/gui.h`, `shell/guicore.c`, `shell/gui.c`, `fs/poll.c`, `driver/pointer.c` |
| C2. Client windows | up to 8 program windows beside the 13 apps' fixed ones; pointer routing with grabs | new `shell/guiclient.c`, `include/sys/gui_client.h`, `include/sys/gui_sys.h`, `tests/host/gui_fake.c`, new `tests/host/test_guiclient.c` |
| C3. The window object | `SYS_winopen`, `SYS_winctl`; a window descriptor; events by `read()`; `poll()` | new `include/surfos/win.h`, new `kernel/win.c`, `include/surfos/syscall.h`, `kernel/syscall.c`, libsurf, new `user/bin/wintest.c`, new `tests/host/test_win.c` |
| C4. The desktop side | registration, the Terminal's pty adopted, windows drawn, closing | new `shell/guisys_win.c`, `shell/gui.c`, `shell/guisys_term.c`, `shell/guidraw.c` |
| C5. libgui | the drawing kit shared with programs; `gw_*` | new `include/sys/guidraw.h`, `user/libsurf/{fbcore,font8x16,guidraw,gui}.c`, new `user/include/gui.h`, new `user/bin/wdemo.c` |
| C6. A real app and a launcher | the Calculator as a program; Apps ▸ Programs, Run program… | new `shell/guicalc.c`, new `user/bin/wcalc.c`, `shell/guicore.c` |
| C7. Screen tests and docs | the full on-screen check; the reversal noted | new `tools/gui_apps_win.py`, docs |
| C8–C13 | wheel, right-click and modifiers; a sharp 1080p desktop; an xterm-level Terminal; touchpad scrolling; kernel apps moved to ring 3; graphical Undertow | below |

## Decisions (the integrator's, stated to the owner)

- **A window is a descriptor,** built like a pty master: a vnode of a path-less `winfs` (as ptyfs,
  `kernel/pty.c:84`).
  - `SYS_winopen` creates it, and `SYS_winctl` drives it (like `SYS_ptyctl`, since `vnode_ops` has no
    ioctl: `include/fs/vfs.h:126-142`).
  - `read()` returns 16-byte event records, and `poll()` works on it, so a program can poll its window, its
    sockets and its ptys together.
- **System call numbers:** the next two free ones when this merges. 52 and 53 today: the network tools took
  50 and 51, and `NR_SYSCALLS` is 52. The first track to land keeps its numbers.
- **Pixels are copied, not shared.** The program draws in its own memory, and `WIN_PRESENT` copies a
  rectangle into the window's kernel buffer during the program's own call: at most 1.75 MB per full update
  at 1080p, well under 1 ms.
  - Nothing in `mm/` changes (that is track B's area), and no kernel frame is ever mapped into ring 3.
  - No unmap-at-exit ordering is needed.
  - Shared ("lent") pages can come later as a new operation, without changing the rest.
- **Which desktop a window goes to:** the caller's terminal must be a console where `gui` registered a
  desktop, or a pty that this desktop's Terminal opened (`guisys_term.c` adopts and drops it). Anything else
  gets -ENODEV: text mode, another console, telnet's ptys, kernel threads. Arguments are checked first, so
  the text-mode smoke test still reaches EFAULT and EINVAL.
- **Budgets:**
  - 8 windows and 8 MB per desktop; 32 windows and 32 MB overall (-EAGAIN or -ENOMEM);
  - each buffer capped at the desktop's largest content area (958x457 logical on 960x540);
  - each window also takes one of the program's 16 descriptors.

  With track B's B1 these buffers commit as BULK and fail cleanly.
- **The 13 kernel apps keep their single-instance state;** their ~3,249 host checks don't change.
  `GUI_WINDOWS` stays 15, because the tests loop over it.
- **No floating point:** libgui, the drawing kit and the API are integer-only. Until track B's B2 lands,
  libgui's header says windowed programs must not use floating point.
- **Ending a window:**
  - The program's last close, or its reap after a kill, ends it.
  - The desktop's exit, or its death, hangs every window up. The registration is a descriptor in the
    desktop task, the trick `guisys_term.c` uses (its 10/18/26 notes), so reaping the desktop ends them.
    Programs aren't killed by that; those started in the Terminal end with the Terminal's own pty hangup.
  - A console switch changes nothing: presents keep arriving, and the kernel keeps the pixels.
- **Locking:** `kernel/win.c` uses only short irq-off sections and no mutex. It never calls the framebuffer,
  and nothing calls it from inside a `framebuffer_graphics_draw()` callback. Freeing happens outside those
  sections.

## The stages in detail

**C1. Groundwork (S–M, three commits).**
- **Slots:** `GUI_CLIENTS` 8, `GUI_FIRST_CLIENT = GUI_WINDOWS`, `GUI_SLOTS = 23`.
  - `GUI_SLOTS` sizes `win[]` and `order[]` (`gui.h:167-168`) and the window manager's loops: about 26 uses
    in `guicore.c`, the action ranges at `guipriv.h:127-128`, and `guiclock.c:393`, which walks `order[]`.
  - The suite loops in `gui_tick`, `gui_finish` and `gui_request_quit` (`guicore.c:657-680`) dereference
    `gui_app_of()` (`guicore.c:46`), so they stop at `GUI_FIRST_CLIENT`.
  - No behaviour change: every existing host check passes unchanged.
- **Waiting on events:**
  - `driver/pointer.c`'s `enqueue()` calls `poll_wake()`, as keys (`kernel/tty.c:139`) and pty output
    (`kernel/pty.c:130`) already do.
  - `fs/poll.c` exports `poll_generation()` and `poll_sleep(gen, ms)`, the no-lost-wakeup pattern of
    `sys_poll` (`poll.c:80-113`).
  - `gui.c` replaces `sleep_intr(16)` (`gui.c:256, 288`) with `poll_sleep(gen, dragging ? 16 : 100)`.
- **Frame time:** measure composing and presenting with `rdtsc` (`include/surfos/cpu.h:54`), shown in the
  About window or `gfxinfo`.
- **The fast path:** `present_rows` (`gui.c:50-65`) issues one `fb_rect_rgb` per run of one colour, which
  costs one call per pixel on photos and program windows at scale 2. Add a path for 32-bit framebuffers that
  expands a row once and copies it sequentially (the framebuffer is write-combining).
- Track A changes the Makefile so `gui.o` and `guidraw.o` build at -O2 (A3a). Don't duplicate it.

**C2. Client windows in the desktop (M).**
- New `include/sys/gui_client.h` (`struct gui_client client[GUI_CLIENTS]`, about 200 bytes each) and
  `shell/guiclient.c`.
- **Where the window manager calls an app, one extra branch for client ids:**
  - title, icon, resizable, minimum width;
  - a new `min_height()` replacing the fixed 200 (`guicore.c:973`);
  - open and close;
  - content press, wheel, release, pointer and keys (with "wants arrows");
  - drawing in `window()` (`guicore.c:1204`);
  - the Window menu's titles (`guicore.c:488`).
- **Pointer routing:** every pointer event over a client's visible content goes to that client: hover, and
  the right and middle buttons too (apps only ever see left presses today, `guicore.c:953`). A press inside
  starts a grab that lasts until every button is up.
- **`struct gui_sys`** (`include/sys/gui_sys.h:74-98`) gains five hooks at its end: `win_accept`,
  `win_update(h, &info, &view)`, `win_event`, `win_close`, and `run` (the launcher). The existing positional
  initializers stay valid, and `tests/host/gui_fake.c` fakes the new hooks.
- **New `tests/host/test_guiclient.c`:**
  - appearing on the first present, pixels at the content origin, clipping off-screen;
  - hover, a grab continuing outside, the right button;
  - one RESIZE per drag (the Terminal's rule, `guiterm.c:496`), close and End, removing a window whose
    program is gone;
  - 8 client windows plus every fixed window inside the guard pixels at 640x400, 960x540 and 1280x800.

**C3. The window object (L).**
- **The ABI** in new `include/surfos/win.h` (no SurfOS types; `user/include/surfos/win.h` includes it):

  ```c
  #define WIN_VERSION 1
  struct win_open    { unsigned version; unsigned short w, h, min_w, min_h, max_w, max_h;
                       unsigned flags /* WIN_RESIZABLE */, background; char title[64]; };
  struct win_present { const unsigned *pixels; unsigned stride; unsigned short x, y, w, h; };  /* 0x00RRGGBB */
  struct win_info    { unsigned version; unsigned short w, h, screen_w, screen_h, scale, focused; unsigned lost; };
  #define WIN_PRESENT 1   /* WIN_SETSIZE 2 ((w<<16)|h), WIN_TITLE 3, WIN_INFO 4 */
  struct win_event   { unsigned short type, mods; int x, y; unsigned value; };                 /* 16 bytes */
  /* WIN_EV_KEY 1, POINTER 2, WHEEL 3, FOCUS 4, RESIZE 5 (x, y = the new size), CLOSE 6 */
  ```

- **Calls:** `SYS_winopen (const struct win_open *)` returns a descriptor; `SYS_winctl (int fd, int op,
  void *arg)`.
- **`kernel/win.c`:**
  - **The buffer:** allocated by the kernel and filled with `background`, so old heap contents never reach
    the screen. A resize allocates a new one, and the old one lives while the desktop still holds a
    reference.
  - **Present:** checks the rectangle's whole span with `user_buf()` (64-bit arithmetic, stride at most
    4096), copies with interrupts on, bumps a counter and calls `poll_wake()`. A desktop frame drawn in the
    middle of a copy may show one torn frame, and the next fixes it.
  - **Events:** 64 queued per window. Moves with the same buttons coalesce, and so do resizes. Overflow is
    dropped and counted.
    - Keys are the bytes as typed, plus `WIN_KEY_*` codes equal to `GUI_UP` and the rest.
    - `mods` stays 0 until track A's modifier keys (A4) land.
    - Pointer coordinates are logical pixels relative to the content; a position outside it means the
      pointer left.
  - **read and poll:**
    - `read()` returns whole records only. It blocks until an event comes, can be cut short like any wait,
      and returns 0 once the window has ended.
    - `poll()` reports POLLIN for events or the end, POLLHUP once ended and drained, and POLLOUT always.
- **New `user/bin/wintest.c`** (raw calls, no library):
  - `nodesk`: EFAULT, EINVAL, ENODEV;
  - `pty`: runs `wintest nodesk` on a pty of its own and expects a refusal even while a desktop runs
    (telnet's case without a network);
  - `show`: colour quadrants, with the events echoed in its title.
- **New `tests/host/test_win.c`:**
  - the pure core: queueing, coalescing and drops, rectangle and stride overflow, sizes and budgets, titles
    cut on UTF-8 boundaries, the which-desktop rule;
  - `kernel/win.c`'s life cycle with stand-ins (as `guisys_host.c` does): both orders of ending, the
    desktop's exit, the owner's death, out of memory, a buffer outliving a resize.
- SMOKE: `run wintest nodesk`, and an `@telnet-win` step for the same over telnet.

**C4. The desktop side (M).**
- **New `shell/guisys_win.c`** implements the hooks. `shell/gui.c` registers the desktop after `gui_init`,
  with its logical size, largest content area and scale, and unregisters it after `gui_finish`.
  `guisys_term.c` adopts and drops its pty.
- **Every frame, `guiclient.c`:**
  - accepts waiting windows into free slots (a window appears at its first present, or after 250 ms);
  - applies title and size requests, and removes windows whose program has gone;
  - sends RESIZE when a corner drag ends or on maximize, and FOCUS changes.
- **`win_update`** hands over a buffer reference that the kernel keeps valid until the next update or close,
  so drawing stays read-only and makes no calls.
- **Drawing:** the background, then a new `gd_blit()` in `guidraw.c`. It copies rows directly into the
  0x00RRGGBB scene, as the Image Viewer does (`guiview.c:450-496`), and falls back to `fb_blit_rgb`.
- **Keys:** every key but Ctrl+G goes to the program, as in the Terminal.
- **Closing:**
  - The first click on the close button sends CLOSE. A second asks "End the program?", then kills it and
    drops the window.
  - Leaving the desktop with program windows open uses the existing "Leave the desktop?" dialog.

**C5. libgui (M).**
- Move `struct gui_draw`, the `gd_*` functions, the palette and the icons from `shell/guipriv.h` into new
  `include/sys/guidraw.h`.
- Add one-line wrapper files `user/libsurf/fbcore.c`, `font8x16.c` and `guidraw.c` that `#include` the
  kernel's sources, as `user/libsurf/cp437.c` and `prompt.c` already do. The library's source list is a
  wildcard.
- **`user/libsurf/gui.c` and new `user/include/gui.h`**, in standard types:
  - `gw_open`, `gw_present` (the changed rectangle), `gw_title`, `gw_close`;
  - `gw_next(win, &ev, ms)`: poll and read, handling RESIZE itself (a new image, then `WIN_SETSIZE`);
  - drawing: rectangles, lines, UTF-8 text in Spleen 8x16, blits, buttons.
- Wrappers for `winopen` and `winctl` go in `user/libsurf/syscalls.c`.
- **New `user/bin/wdemo.c`:** colour bars plus the last event and the size as text. `wdemo hang` ignores
  CLOSE; `wdemo crash` faults after its first present.

**C6. A real app and a launcher (M).**
- `gui_calc_key()` (`include/sys/gui.h:211`, `guicore.c:248-272`) moves to new `shell/guicalc.c`, shared by
  the kernel and libsurf.
- New `user/bin/wcalc.c` is the Calculator as a program, looking the same. It proves an app can move out,
  and that its crash costs only its window. It is the template for C12.
- **The launcher:** Apps ▸ Programs (a list) and "Run program…" spawn on the desktop's console with stdin
  from `/dev/null` (so a program never takes the desktop's keys), detached to init. A program started this
  way outlives the Terminal and ends with the desktop.

**C7. Screen tests and docs (M).**
- New `tools/gui_apps_win.py`, in `TRACKS` before `gui_apps_suite` (`tools/gui_apps_scenario.py:33`). It
  checks:
  - `wdemo &` typed in the Terminal shows its title and bar pixels at 2x;
  - clicks and keys are echoed (read with the Terminal track's glyph reader);
  - a corner resize shows the new size;
  - the close button ends it (`ps`);
  - `hang` is ended through the dialog, and `crash` loses only its window while the desktop goes on;
  - `wintest pty` is refused;
  - F2 and back to F1 keeps the content;
  - a launcher program outlives the Terminal and ends with the desktop.
- **Docs:**
  - GUI.md (programs' windows, libgui, the launcher);
  - GRAPHICS.md (the reversal);
  - a reversal note in the desktop spec;
  - ARCHITECTURE.md (a section; numbers as they are at merge);
  - CLAUDE.md (layout, `wintest`/`wdemo`/`wcalc`, the rules for programs' windows);
  - an M12 row in ROADMAP.md.

**After the platform** (each its own commits; the order is a recommendation):
- **C8. Input (S–M):**
  - wheel events from USB mice (the 4th byte of the boot report when 4 or more arrive,
    `driver/usb/mouse.c:57`) and PS/2 (the IntelliMouse handshake: rates 200/100/80, ID 3, 4-byte packets,
    in `kernel/keyboard.c` and `driver/ps2mouse.c`); the wheel hooks already exist (`guicore.c:922-937`);
  - right-click context menus reusing `menu_open`, `menu_hit` and `menu_run` (`guicore.c:530-576`);
  - modifiers kept by `gui.c`'s `sequence()` (it drops the `;mod` part, `gui.c:264`) once track A's A4 has
    landed; `win_event.mods` filled in.
  - Checks: test_usbmouse, test_ps2mouse, and the monitor's `mouse_move 0 0 1` in the gui scenario.
- **C9. A sharp 1080p desktop (M–L):** draw at the physical resolution with track A's `struct fb_font`
  (Spleen 16x32).
  - `scale` goes in `struct gui_draw`; only `guidraw.c` touches the font; layout stays in logical units.
  - 32 px icons and cursor shapes.
  - Two 8.3 MB buffers instead of two 2 MB ones.
  - `win_info.scale` tells programs.
- **C10. The Terminal up to an xterm subset (M), in `shell/guivt.c`:**
  - scroll regions, insert/delete line and character, the alternate screen;
  - 256 and true colour (parsed and dropped today, `guivt.c:192-193`);
  - mouse reporting, bracketed paste, OSC titles through `title_of`.
  - It runs track A's shared `tests/host/vt_cases.h`.
- **C11. Touchpad two-finger scroll (L).** InputMode 3 and the Precision Touchpad contacts in
  `driver/hidpointer.c` (it keeps X, Y and 3 buttons today, `include/sys/hidpointer.h:26-30`) and
  `driver/touchpad.c`; the gesture becomes wheel steps. It is host-tested with
  `tests/host/touchpad_06cb_ce2d.h`. It needs the owner at the laptop, and touches only drivers, so it can
  run any time.
- **C12. Kernel apps to ring 3, one by one,** on wcalc's template: Clock, Character Map, Hex, the Image
  Viewer… Each one takes ring-0 code out of the crash path.
- **C13. A graphical Undertow (XL),** on libgui after C9: proportional text layout, clickable links, and
  image decoders (only BMP exists, in the kernel: `shell/guibmp.c`).

## Shared with the other tracks

- **`shell/gui.c`'s loop:** this track's (C1, C4). Track A changes only its Makefile flags; C8 reads A4's
  key encoding.
- **`driver/framebuffer.c`, `driver/fbcore.c`:** track A's (the lazy present, `fb_font`). This track uses
  `fb_blit_rgb` and, in C9, `fb_font`.
- **`shell/guivt.c`:** this track's. Agree `tests/host/vt_cases.h` with track A (its A8).
- **`fs/poll.c`:** this track adds `poll_generation()`/`poll_sleep()`. Nobody else changes it.
- **`mm/`, `kernel/task.c`, `kernel/cpu.c`:** track B's. Not touched here.
- **System call numbers:** this track adds the only two new ones; take the next free numbers at merge.
- **Append-only places:** the SMOKE table, `shell/selftest.c`, the Makefile's program lists, CODE-AUDIT and
  the docs. Textual conflicts only.

## Merging

The protocol of 2026-10-25:
1. Re-read `git log -1 ai-dev`.
2. Merge on `gui/merge` in this worktree (`git switch -c gui/merge ai-dev`, `git merge --no-ff gui/win`) and
   run the whole suite there.
3. Then `git merge --ff-only` in the main worktree, only if ai-dev hasn't moved.
4. Message the sessions waiting (ListAgents / SendMessage).

The track that lands first keeps its system call and ARCHITECTURE.md section numbers; the later one
renumbers and says so in its merge message. Merge at the owner's word after C7 (M12), and again after the
follow-ups. Pushing waits for the owner.

## Checks

- Every commit builds and passes `make unittest`, `make test-all` (-O0 and -O2) and `make test-usb` at -O0
  and -O2. The desktop runs only on the UEFI framebuffer, so its `gui` and `guiapps` scenarios are the
  real check.
- On the Mac: `./devenv/build.sh …`, `O=0|2 ./devenv/build-usb.sh && ./devenv/test-usb.sh`. In this
  worktree, export the disk images first (`SURFOS_OUT=<worktree>/build/docker ./devenv/build.sh images`), or
  usbdisk and storage fail 2 checks and skip 67.
- **The acceptance test of the platform (M12):**
  - `test_win`, `test_guiclient` and every existing `test_gui*` check pass;
  - `gui_apps_win.py`: wdemo and wcalc draw, take keys and clicks, resize and close, and a crash loses only
    its window;
  - `run wintest nodesk` and the telnet case are refused.
- **On the laptop** (when the integrator gives this track the laptop): wcalc and wdemo in `gui`, with the
  touchpad.
- At the end, an independent review (the kernel side, and the desktop with libgui); each finding gets a
  check.

## Rules (as for every track)

- **CLAUDE.md:**
  - kernel commands print on `curTask->con` and return CMD_OK, CMD_FAIL or CMD_USAGE;
  - whatever changes a descriptor's readiness calls `poll_wake()`, and a vnode's poll op never does;
  - a kernel command never loops past its input's end;
  - the desktop's hooks never wait (`include/sys/gui_sys.h:8-10`).
- **The model stays host-testable:** no hardware access or allocation in `include/sys/gui.h`'s model, and
  nothing shared between consoles.
- **Small commits, one topic each.** Keep the 2004 headers; new files get the header block with the current
  date.
- **Nothing here starts by itself** or reaches the network: windows exist only on a desktop the owner
  started at the laptop.
