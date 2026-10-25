# Brief: robustness (track B)

## The request

The owner, 2026-10-25, asked for the highest-impact improvements to the kernel and its interface. They chose
everyday use on the laptop, a platform for new apps and robustness. This brief is the robustness part:
- SurfOS should survive a misbehaving program;
- programs should be able to use floating point;
- a kernel stack overflow should be reported, not silent;
- the machine should shut down without leaving its sticks marked unclean.

The two other tracks run at the same time:
- everyday console and shell: `2026-10-25-daily-console.md`;
- windows for user programs: `2026-10-25-app-windows.md`.

Track C's app windows will want floating point (B2) and memory that fails cleanly (B1).

**Where:** a worktree of its own, `../06-restoration-robust`, on branch `robust/main`. It starts from
`ai-dev` once storage milestone 1 has landed (merge commit 3f27b61 on `storage/merge` when this was written);
B4 needs its `vfs_busy()` and `vfs_umount_lost()`. The order is B0, B2, B1, B3, B4. The branch may be
merged in stages, at the owner's word: B0 and B2, then B1, then B3 and B4. If the owner starts a second
session for this track, B2 moves to its own worktree, `../06-restoration-fpu` on `robust/fpu`. Nothing is
pushed.

**What the scan found** (ai-dev 36bb341, checked in the code):
- **Out of memory panics.** `page_fault_trap()` demand-maps the kernel heap and panics "out of memory: no
  frame for kernel heap page" when `pmm_alloc()` fails (`mm/paging.c:122-129`). Programs and the kernel
  share one pool of frames, and `uvm_sbrk()` maps eagerly (`mm/uvm.c:164-180`). So a program that mallocs
  until it fails drains the frames, and the next kernel heap touch panics.
- **Zombies keep memory.** A zombie keeps its user frames until it is reaped (`kernel/task.c:765-781`).
- **Floating point isn't switched.** Nothing saves floating-point state between tasks: no
  fxsave/fnsave/clts, and the device-not-available trap (`T_DEVICE`, `include/surfos/trap.h:38`) is
  unhandled. `set_cr0()` (`kernel/cpu.c:66-77`) clears TS once and runs `fninit`. CR4.OSFXSR/OSXMMEXCPT
  are left as the boot loader set them (`include/surfos/cpu.h:43`). User programs build with
  `-march=i486` (`Makefile:35,87-89`), so floats are x87 code. The kernel's `funky` demo uses floats
  (`shell/demos.c:184`).
- **Kernel threads can be killed.** `sys_kill` refuses only pid ≤ 1 (`kernel/syscall.c:291-296`).
  `kill_task()` ends a task that isn't inside a call at once (`kernel/task.c:405-420`), and kernel threads
  never are, so `kill` on the usb, net, iwl or touchpad thread ends it holding its locks.
- **Stacks have no guard.** Kernel stacks are 16 KB `kalloc` blocks with no guard page (`task.c:185-200`;
  audit M10 never fully closed). Every IDT entry is an interrupt gate (`kernel/interrupt.c:182`), and there
  is no double-fault task gate.
- **No clean shutdown.** `reboot` resets at once (`kernel/sys.c:24`) without writing mounts out, so a
  read-write volume stays marked unclean. `kernel/acpi.c:92-118` reads the FADT, MADT, HPET and the reset
  register only, so there is no power-off: on the laptop the way out is holding the power button.

## What gets built, stage by stage

| Stage | What | Where |
|---|---|---|
| B0. Kernel threads can't be killed | `TF_PROCESS`; `task_user_may_kill()`; -EPERM | `kernel/task.c`, `kernel/process.c`, `kernel/syscall.c`, `shell/main.c`, `shell/guisys.c` |
| B2. Floating point per task | lazy switching on #NM, fxsave/fnsave, CR4 bits, the kernel kept off the FPU, `%f` | new `kernel/fpu.c`, `kernel/cpu.c`, `kernel/task.c`, `Makefile`, `shell/demos.c`, `user/libsurf/` |
| B1. Out of memory | frame classes and a reserve, commit accounting for the heap, program memory that fails cleanly, memory freed at exit | `mm/pmm.c`, `mm/kalloc.c`, `mm/paging.c`, `mm/uvm.c`, `kernel/process.c`, `kernel/task.c` |
| B3. Stack guards | a double-fault task gate first, then a kernel stack window with guard pages | `kernel/gdt.c`, `kernel/interrupt.c`, `kernel/task.c`, `include/mm/memory.h` |
| B4. Clean reboot and power-off | ACPI S5 from the DSDT; a `shutdown` thread that ends programs and unmounts; `poweroff` | `kernel/acpi.c`, `driver/amlfield.c`, new `kernel/shutdown.c`, `kernel/keyboard.c`, `shell/main.c`, `tools/` |

## Decisions (the integrator's, stated to the owner)

- **The kernel heap stays demand-paged** (ROADMAP §1: "demand-paged kernel heaps" is architecture). It
  becomes safe through commit accounting: frames are promised when the heap grows and taken when a page
  faults in, so a heap fault always finds one. The heap panic remains only for real corruption.
- **Frame classes:** `PMM_USER` always leaves a reserve R free, `PMM_BULK` (large kernel buffers) leaves
  R/2, and `PMM_KERNEL` may take the last frame. R = clamp(frames/32, 2 MB, 16 MB). Programs fail long
  before the kernel does.
- **No force kill of kernel threads.** Killing one on the spot can't be made safe. A thread that users need
  to stop gets its own stop command (as `tcpecho` should). Ring-0 shells (`TF_SHELL`) stay killable: init
  restarts them, and their commands are calls that a kill waits for.
- **The kernel never uses the FPU.** `funky` is rewritten in integer math, and `-mno-80387` joins the kernel
  CFLAGS (filtered out of `USER_CFLAGS`, `Makefile:35`). The kernel doesn't link libgcc, so a float in
  kernel code fails to link. Interrupt handlers then can't touch FPU state, and the #NM handler panics on a
  ring-0 fault ("FPU used by the kernel").
- **SSE is an opt-in for each program** (`-march=pentium4 -mfpmath=sse`); libsurf stays i486. The stack is
  16-aligned at entry for it: `andl $-16,%esp` in crt0, and `sp &= ~15` in `process.c`.
- **The double-fault task gate lands before any guard page.** Without it, a guard page turns an overflow
  into a silent triple-fault reset.
- **`reboot` and `poweroff` are kernel commands** (surfsh reaches them through SYS_kcmd); no new system
  call. `reboot -f` keeps today's immediate reset.
- **Ctrl+Alt+Del asks init to shut down;** a second press resets. Today it resets inside the interrupt
  handler, and the handler may not create a task (audit T11).
- **The S5 sleep type is parsed from `_S5_`, never assumed** (Intel's is usually 7).
- **What the storage track keeps:**
  - the busy check across every task's directory (`fs/vfs.c:~281`);
  - one cache identity for a disk and its partitions (`fs/bcache.c:49-56`);
  - kills inside the block cache or a FAT operation (`kill_escalate()`);
  - a mount-time scan for two names on one chain;
  - SurfCPU's B46–B82.

  This track only calls `fs/`.
- **New audit IDs** use a letter neither CODE-AUDIT nor the roadmap uses (L is free): L1–9 memory, L10–14
  FPU, L15–19 kills and stacks, L20–29 shutdown. The track also closes M10 and the out-of-memory half of M6.
- **The legacy boot's log and probing don't change.** No new boot lines; the legacy profile reads ACPI only
  when `poweroff` runs. Note that `find_rsdp_bios()` must not read 0x40E directly then: page 0 is unmapped
  after boot (`kernel/main.c:111`).

## The stages in detail

**B0. Kernel threads can't be killed (S; mergeable alone, early).**
- Add `TF_PROCESS` (0x100, `include/surfos/task.h`), set by `process_spawn_ex()`. Today's flags can't tell
  a process from a kernel thread: processes carry `TF_KTHREAD` too, and `TF_USER` is set only once loading
  starts.
- New `task_user_may_kill()` refuses idle, init and every task without `TF_PROCESS`, except `TF_SHELL`.
  - `sys_kill` returns -EPERM.
  - `cmd_kill` (`shell/main.c:189`) and End task's `kill()` (`shell/guisys.c:114`) say "is a kernel
    thread".
- Checks:
  - selftest: killing `net` is refused, killing your own child works;
  - SMOKE: `kill <net's pid>` is refused;
  - test_guisys: End task on a kernel thread.

**B2. Floating point per task (M).**
- **`kernel/cpu.c`:** read FXSR, SSE and SSE2 from CPUID. Set CR4.OSFXSR when FXSR is present and
  OSXMMEXCPT when SSE is. `cpuinfo` gains the FPU line (x87 only, FXSR, SSE2).
- **New `kernel/fpu.c`:**
  - `fpu_switch(next)`, called from `schedule()` (`kernel/task.c:948`), sets CR0.TS unless `next` owns the
    FPU. A shadow flag avoids rewriting CR0.
  - The handler, installed with `trap_set_handler(T_DEVICE, …)`, panics from ring 0. Otherwise it clears
    TS, saves the owner (fxsave, or fnsave without FXSR) and restores the current task. On a task's first
    use it runs fninit and loads MXCSR 0x1F80.
  - `become_zombie()` (`task.c:336`) clears the owner.
- **The save area:** 512 bytes, kalloc'd 16-aligned when a process is created, freed at reap. Kernel
  threads get none, and nothing is allocated inside the trap.
- **Then:**
  - the `funky` rewrite and `-mno-80387`;
  - the 16-byte stack alignment in crt0 and `process.c`;
  - libsurf `printf("%f")` (and `%e`/`%g` if small), in a commit of its own.
- **Checks:**
  - new `user/bin/fputest.c` prints exactly representable results from x87, and from SSE2 when present;
  - `fputest race` fills 8 x87 and 8 XMM registers, yields for 300 ms, and compares bit for bit;
  - selftest `test_fpu` runs two races at once, and both must exit 0;
  - SMOKE: `fputest`, and `cpuinfo`'s FPU line;
  - once by hand under `-cpu pentium`, for the fnsave path;
  - `make test-usb` at O0 and O2 (CR0/CR4 are in the boot path).

**B1. Out of memory: a program fails, the machine doesn't (L).**
- **Frame classes** in `mm/pmm.c`: `pmm_alloc_as(cls)`, with `pmm_alloc()` staying the kernel class. Count
  refusals per class and the lowest free count. The class check is a pure inline, so it can be host-tested.
- **The heap** (`mm/kalloc.c`, `heap_grow()` at `kalloc.c:88`, `GROW_CHUNK` 64 KB):
  - Growing commits frames: a request over `GROW_CHUNK` commits as BULK (desktop buffers, the DSDT copy),
    the rest as KERNEL.
  - If they can't be promised, the growth fails, and `kalloc` returns NULL.
  - `page_fault_trap()` keeps demand-mapping, but takes a committed frame.
  - A fault in the heap above its top is a stray pointer (fatal, as any other).
- **Program memory** (`mm/uvm.c`):
  - User frames are `PMM_USER`.
  - Page tables and directories cost 8 KB of kernel heap each today (`alloc_table_page()`,
    `uvm.c:36-44`): that is the route by which a program can drain the kernel. Take them from `PMM_USER`
    frames instead, identity-mapped or in a kernel window. 0x80000000–0xAFFFFFFF is unused; define it in
    `include/mm/memory.h` if a window is needed.
  - `uvm_sbrk()` unmaps a range it only partly mapped.
  - `uvm_grow_stack()` returns -ENOMEM, and the fault handler ends the program with `trap_fatal(tf, "out of
    memory: its stack could not grow")`.
- **Spawning** (`kernel/process.c`): -ENOMEM at once when fewer than about 16 frames are left for programs.
- **Freeing at exit:**
  - `task_exit()`, after `fd_close_all()`, with interrupts off: detach the uvm, switch to the kernel
    directory (`uvm_destroy` panics on the current one), then destroy it.
  - Kills that end a task on the spot (faults, Ctrl+C in ring 3, the 2 s escalation) hand the zombie's
    address space to a small `uvmreap` kthread, signalled from `become_zombie()`.
  - `schedule()` (`task.c:966`) compares against `uvm_current()` instead of `cur->uvm`.
- **`memstat`** gains a line: the reserve, pages refused, the lowest free count, and what programs and
  zombies hold.
- **Checks:**
  - New `user/bin/memhog.c`: allocates and touches 64 KB blocks until malloc fails, prints the total,
    exits 0. Modes: `stack` (recursion until the stack can't grow) and `hold <ms>`.
  - New `tests/host/test_kalloc.c`: a mapper stub that fails on demand, plus `heap_check()`.
  - SMOKE:
    - `memhog`;
    - `memhog stack` (the out-of-memory line, no panic);
    - `memhog hold 3000 &`, then `ls /initrd` (either it runs or it says out of memory), `ps`, and a telnet
      login;
    - afterwards memstat's free pages are back within 1%.
  - selftest: 64 `hello` at once, then wait; frames and heap bytes return to where they started.
  - qemu-usb: memhog in one 512 MB scenario, and guiapps's heap use after Ctrl+Q equals its use before
    `gui`.
- **Risks:**
  - Kernel objects made for programs (tasks, spawn arguments, pipes, sockets) still use kernel frames. The
    caps and the spawn check bound this; document it.
  - The heap never shrinks, so the desktop's 8.7 MB stays mapped after it exits. Trimming the heap's top
    is a follow-up.
  - An 8 MB growth commits about 2,000 frames with interrupts off. Measure it.

**B3. Kernel stack guards (M).**
- **First, the double-fault task gate:**
  - `kernel/gdt.c` gets a second TSS (`GDT_ENTRIES` 7, selector 0x30) with its own 8 KB stack, the kernel
    page directory in CR3, and EFLAGS 0x2.
  - IDT entry 8 becomes a task gate.
  - The handler reads the main TSS's saved registers. If ESP is in a guard page, it prints "kernel stack
    overflow in 'x' (pid n)" with a backtrace and halts. It doesn't try to recover: an interrupt may have
    been in progress, and locks may be held.
- **Then the kernel stack window:**
  - 32 KB slots, each a 16 KB stack (`KSTACK_SIZE`) mapped up front from frames, over an unmapped guard
    page. Processes' stacks come from `PMM_USER`.
  - `kstack_alloc()` and `kstack_free()` are used by `task_alloc()` and `reap()`. Freeing still happens
    after `kcmd_task_gone()`, which reads the stack.
  - Update `task_alloc()`'s pre-fault comment and CLAUDE.md's stack note.
  - `trap_dispatch()` panics with the task's name when a trap frame lands within 1 KB of a stack's bottom.
- **Checks:**
  - an `@boot` block runs a new `crashstack` command and expects "kernel stack overflow" and "SYSTEM
    HALTED";
  - selftest: the page below a new stack is unmapped.
  - Before merging, run usbnet, guiapps and the Wi-Fi blocks: guards will expose deep call paths that exist
    today, such as USB and Wi-Fi callbacks nesting on task stacks.

**B4. Clean reboot and power-off (M–L).**
- **ACPI** (`kernel/acpi.c`):
  - Read from the FADT: SMI_CMD (offset 48), ACPI_ENABLE (52), the PM1a/PM1b control blocks (64/68, or
    their extended forms at 172/184), and SLEEP_CONTROL (244) for hardware-reduced ACPI.
  - A new `aml_sleep_type()` in `driver/amlfield.c` finds `_S5_` with its existing bounded walker (root
    scope, no conditionals) over `acpi_aml_copy("DSDT", 0, …)`. Host-test it with OVMF's bytes
    (`08 _S5_ 12 06 04 00 00 00 00`) and variants.
  - `acpi_s5()`:
    1. `cli`; `wbinvd`.
    2. If SCI_EN is clear, enable ACPI mode through SMI_CMD, waiting at most 300 ms.
    3. Write SLP_TYP, then SLP_TYP|SLP_EN, to PM1a and then PM1b.
    4. Wait at most 3 s, then return failure.
- **New `kernel/shutdown.c`.** `reboot` and `poweroff` start a `shutdown` kthread, which:
  1. Sets its directory to "/". `in_use()` checks the asking task's directory, and `task_alloc()` copies
     the creator's.
  2. Stops init from restarting shells, and refuses new spawns (-EAGAIN).
  3. Ends every process and ring-0 shell with `kill_task_patient()`, waits at most 10 s, and names any
     left over.
  4. Unmounts the read-write volumes in a worker it waits for with `event_wait_timeout()`, so a held volume
     lock can't hang shutdown.
     - It reports `vfs_busy()`'s holder and `vfs_umount_lost()`'s count.
     - A volume it can't unmount gets `vfs_sync()` and the line "left marked not cleanly unmounted".
  5. Calls `reset()` or `acpi_s5()`. If neither works, it prints "It is now safe to turn off your
     computer." and halts.
- **Also:**
  - `poweroff -n`: a dry run listing programs, mounts and the S5 values.
  - Track A's history hook (A10) runs before the programs end.
  - The network manager's `/.netman` mount is retried within the time bound.
  - `tools/devloop.py run` refuses `poweroff`.
  - Ctrl+Alt+Del (`kernel/keyboard.c:142`), as decided above.
- **Checks:**
  - SMOKE `poweroff -n`.
  - An `@boot` block runs `reboot` (the harness passes `-no-reboot`, so QEMU exits) and checks the clean
    bit in hda1's FAT[1].
  - `poweroff` under `-no-shutdown`: the monitor's `info status` says shutdown. Repeat under
    `profile=laptop`.
  - qemu-usb scenario `shutdown`: with a read-write stick, `reboot`, and the next mount says "(FSInfo)";
    then `poweroff`, QEMU exits, and `fsck.fat` runs on the host.
- **On the laptop:** the owner checks that `poweroff` turns it off. Firmware may need `_PTS`, or trap the
  sleep write into SMM; record what happens in `docs/hardware/ideapad-3-15iil05.md`.

## Shared with the other tracks

- **`kernel/task.c`:** this track's single-line hooks in `schedule()`, `become_zombie()`,
  `task_alloc()`/`reap()`. The others don't touch it.
- **`kernel/process.c`:** small hunks from B1, B2 and B4. Track C's launcher only calls spawn.
- **`kernel/keyboard.c`:** track A changes it (modifiers, scrollback); B4 changes Ctrl+Alt+Del. Whoever
  lands second rebases.
- **`mm/`:** this track's alone. Track C's windows copy pixels into kernel buffers and change nothing in
  `mm/`. Their buffers are large kernel allocations, so B1 makes them commit as BULK; tell track C the
  budget numbers.
- **`tools/qemu-run.py`'s SMOKE table, `shell/selftest.c`'s `tests[]`, the Makefile's program lists and
  CODE-AUDIT:** everyone appends; textual conflicts only.
- **System calls:** none.

## Merging

The protocol of 2026-10-25:
1. Re-read `git log -1 ai-dev`.
2. Merge on `robust/merge` in this worktree (`git switch -c robust/merge ai-dev`,
   `git merge --no-ff robust/main`) and run the whole suite there.
3. Then `git merge --ff-only` in the main worktree, only if ai-dev hasn't moved.
4. Message the sessions waiting (ListAgents / SendMessage).

The track that lands first keeps its ARCHITECTURE.md section numbers. Pushing waits for the owner.

## Checks

- Every commit builds and passes `make unittest` and `make test-all` (-O0 and -O2). B1–B4 also pass
  `make test-usb` at -O0 and -O2 (memory setup, CR0/CR4, shutdown).
- On the Mac: `./devenv/build.sh …`, `O=0|2 ./devenv/build-usb.sh && ./devenv/test-usb.sh`. In this
  worktree, export the disk images first (`SURFOS_OUT=<worktree>/build/docker ./devenv/build.sh images`), or
  usbdisk and storage fail 2 checks and skip 67.
- **The acceptance test of the whole track:**
  - `memhog`, `memhog stack` and `memhog hold` leave the system up, with memstat back within 1%;
  - `fputest race` passes twice at once;
  - `crashstack` says "kernel stack overflow" instead of resetting;
  - `reboot` leaves a clean FAT bit;
  - `poweroff` makes QEMU report shutdown;
  - `kill` refuses kernel threads.
- **On the laptop** (when the integrator gives this track the laptop): `fputest`, `poweroff`, and memstat
  after a session with the desktop.
- At the end, an independent review; each finding gets a check.

## Rules (as for every track)

- **CLAUDE.md:**
  - the two profiles, where without `profile=laptop` the boot stays exactly as it was;
  - every hardware wait bounded;
  - kernel commands print on `curTask->con`, return CMD_OK, CMD_FAIL or CMD_USAGE, and ask through
    `shell_ask()`;
  - fix things as the audit describes and cite the IDs.
- **Small commits, one topic each.** Keep the 2004 headers; new files get the header block with the current
  date.
- **Docs:**
  - CODE-AUDIT.md (the L items, M10, M6);
  - ARCHITECTURE.md (memory, tasks, shutdown);
  - ROADMAP.md;
  - CLAUDE.md (the stack note, `poweroff`, `memhog`, `fputest`, `crashstack`);
  - USB-BOOT.md (`poweroff` on the laptop).
