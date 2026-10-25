# Brief: pipelines and scripting for surfsh

## The request

The owner, 2026-10-24: give surfsh pipelines and useful scripting. The parser refuses `|`
and `&&`, and there are no variables and no wildcards. Add bounded kernel pipes, then
pipelines, environment variables, script files, and foreground/background job control, in
stages; this makes the existing utilities much more useful together. The work is done in
a worktree of its own, `../06-restoration-surfsh`, on branch `surfsh/scripting` (from
ai-dev 0d641d4). It is merged into ai-dev only at the owner's word, and nothing is pushed.

## What gets built, stage by stage

Each stage is a series of small commits that build and pass the suites (below) before the
next stage begins, so the branch can stop after any of them.

| Stage | What | Where |
|---|---|---|
| 1. Pipes | `pipe()`: a bounded buffer in the kernel between two descriptors, 8 KB each, 32 pipes at most | `fs/pipe.c`, `include/surfos/pipe.h`, libsurf |
| 2. Pipelines and lists | `a \| b \| c`, `&&`, `\|\|`, `!`, `{ list; }`, `( list )`, `2>`, `2>&1`, `>&2`, `<` for surfsh's own commands too | `user/surfsh/` (a parser that builds a tree, an executor) |
| 3. Variables and expansions | `name=value`, `export`, `unset`, the environment of programs, `$name ${name} ${name:-x}` and the rest of POSIX's forms, `$1 $# $@ $* $$ $! $0`, `$(command)`, `$((arithmetic))`, wildcards `* ? [...]`, `PATH` | kernel: `SYS_spawnx` (an environment, four descriptors); libsurf: `environ`, `getenv`, `setenv`; surfsh |
| 4. Script files | `if`, `while`, `until`, `for`, `case`, functions, `break`, `continue`, `return`, `local`; `test` and `[`, `read`, `printf`, `eval`, `.`/`source`, `exec`, `true`, `false`, `:`, `kernel`; `set -e -u -x -f`; `#!` in the kernel; Ctrl+C that leaves a loop or a script (`SYS_waitx`, `TTY_INTR`) | surfsh; `kernel/process.c`, `kernel/task.c`, `kernel/tty.c` |
| 5. Job control | Ctrl+Z stops the job in the foreground; `jobs`, `fg`, `bg`, `wait`, `kill %1`; a job in the background that reads the terminal stops until it is brought to the foreground | kernel: jobs, stopped tasks, `SYS_job`, `SYS_waitx`; surfsh |
| 6. Filters | `grep`, `sort`, `uniq`, `tail`, `tee`, `tr`, `seq`: programs that pipelines want and SurfOS lacks | `user/bin/` |

As built: `surfsh -c TEXT`, `surfsh FILE ARGS`, here-documents and lines that continue came
with stage 2, `set --`, `shift` and `SPAWN_MAX_ARGS` 128 with stage 3, and `SYS_waitx` with
stage 4 (Ctrl+C in loops needs it), so it is number 48 and `SYS_job` 49. Stage 5's checks
are the smoke test's `@surfsh-jobs` over the serial console (the real surfsh, rather than a
`jobtest` program), Ctrl+D came with it, and `SPAWN_JOB_NONE` was added: what an interactive
shell starts outside a job (a `$( )`, a pipeline that ends in the shell) is in none, since a
surfsh typed at a surfsh's prompt is in a job itself and must keep it as the terminal's
foreground to read its prompt.

Stage 6 is the integrator's addition: of the programs a pipeline wants after `cat`,
`head` and `wc`, SurfOS has none, and `dmesg | grep usb` is the first thing anyone types.

## Decisions (the integrator's, stated to the owner)

* **Pipes are bounded twice**: each holds 8 KB (`PIPE_SIZE`), a writer waits while it is
  full, and a write of up to 4 KB (`PIPE_BUF`) is never mixed with another's; at most 32
  pipes exist at once (`PIPE_MAX`, -ENFILE beyond), so no program can take the kernel's
  memory with them. Every wait in a pipe is one that a kill, Ctrl+C or a hangup cuts short.
* **A broken pipe ends the writer**, as SIGPIPE does elsewhere: a program that writes to a
  pipe nobody reads any more ends with status 141 (128 + 13). libsurf does this (the kernel
  answers EPIPE, and only pipes answer EPIPE to a write); a program can ask for the error
  instead (`broken_pipe_exit(0)`), which surfsh does for its own writes. A kernel command
  whose reader went away (`dmesg | head`) is not a failure.
* **Kernel commands in pipelines.** A kernel command runs in a surfsh's own task (in a
  pipeline, a new surfsh's: below) and prints through the kernel's capture, which goes to
  descriptor 1 when the command is done: `lspci | grep Intel` works, and a long-running
  kernel command's output arrives at its end. They read only the console, so one after a
  `|` is refused, as `<` is for them.
* **How a pipeline runs.** Programs are processes. A stage that is surfsh's own (a builtin,
  a kernel command, a loop, a group, a function) runs in a new surfsh process that gets this
  shell's variables, functions and positional parameters through a pipe on its descriptor 3
  (as a script that sets them up); except the last stage, which runs in surfsh itself, so
  `... | read line` and `... | while read f; do ...; done` can set variables (as zsh and
  ksh do; bash runs it in a subshell). The same new surfsh runs `( list )`, a background
  job that is surfsh's own (`{ ...; } &`, `ping 10.0.2.2 100 &`), and `$(...)` (except a
  plain program, which `$(...)` starts itself). SurfOS has no fork(): this is how surfsh
  gets the copy of itself a subshell is.
* **The environment.** A program gets the exported variables (`SYS_spawnx`, up to 64 of
  them in 4 KB); libsurf's `spawn()` and `spawnfd()` pass a program's own environment on,
  so it reaches what programs start too. surfsh sets `PATH=/initrd/bin` when there is none,
  `PWD`, `OLDPWD` and `SHELL`. A name without a `/` is looked up in each directory of
  `PATH` after surfsh's own commands and the kernel's; `help` and Tab list them all.
  SurfOS has no users or home directories, so there is no `~` and no `HOME`.
* **Wildcards** match names as stored, case and all; a pattern that matches nothing stays
  as it is (POSIX). A command takes 128 words (it was 32; `SPAWN_MAX_ARGS`) and 8 KB of them
  (it was 2 KB), so `rm *.tmp` works on a folder of them.
* **Which command runs**: a special builtin (`:`, `.`, `break`, `continue`, `eval`, `exec`,
  `exit`, `export`, `readonly`, `return`, `set`, `shift`, `unset`), then a function, then
  surfsh's other commands, then the kernel's, then `PATH`. surfsh's `test` and `kill` come
  before the kernel's commands of the same names; `kernel NAME ARGS` runs the kernel's
  (`kernel test`, the DMA test the smoke test runs), and `kill` with a pid does what the
  kernel's does.
* **`set`** keeps its settings (`set prompt.unicode off`), and takes POSIX's options
  (`set -e`, `set -x`, `set +u`, `set -- a b c`). `set` alone lists the settings, then the
  variables.
* **Statuses**: 0, 1, 2 as before; 126 a program that could not be started, 127 no such
  command, 130 a command Ctrl+C stopped (a program killed by Ctrl+C too, which was 137), 137 a
  program killed otherwise, 141 a broken pipe, 148 a job stopped by Ctrl+Z (128 + 20, as
  bash). A pipeline's status is its last stage's; `!` turns 0 into 1 and the rest into 0.
* **Ctrl+C in a script or a loop**: when Ctrl+C stops a command that surfsh runs, surfsh
  leaves the rest of the line, loop or script (an interactive shell goes back to its prompt;
  a script ends with 130), as other shells do. The kernel's rule for whom Ctrl+C reaches
  (the innermost programs of the job in the foreground) is unchanged.
* **Job control** (stage 5) follows POSIX in SurfOS's terms. A job is the processes of one
  pipeline (and what they start); its number is `%1`, `%2`, ... A job in the foreground has
  the terminal: its programs read the keys, Ctrl+C and Ctrl+Z reach it. Ctrl+Z stops it, and
  surfsh says `[1] stopped  cmd` and shows its prompt; `fg` gives it the terminal again,
  `bg` lets it go on in the background. A job in the background that reads the terminal
  stops (`[1] stopped (reading the terminal)`) instead of taking the shell's keys. surfsh's
  own commands run in surfsh, which is never stopped: Ctrl+Z does nothing while a builtin or
  a kernel command runs (Ctrl+C still stops a kernel command), nor on a pipeline whose last
  stage runs in surfsh (above). When a program that a loop runs is stopped, surfsh leaves the
  loop, as bash does. A stopped task waits where it was: in its own code, at the end of a
  system call, or in a wait that a kill would cut short, which goes on afterwards (nothing is
  started again, nothing is lost). At `--More--` Ctrl+Z is a key, as Ctrl+C is.
  When an interactive surfsh ends, its stopped jobs are killed and its running ones go on
  (init collects them), out of any job.
* **`#!`**: the kernel runs a file whose first line is `#!INTERPRETER [ARG]` (an ELF
  program; no `#!` behind a `#!`) as INTERPRETER [ARG] FILE ARGS, as every Unix does, so a
  script runs by its name, from `run` too. surfsh also runs a file that is neither ELF nor
  `#!` as a surfsh script, as POSIX shells do.
* **Ctrl+D**: on a terminal in line mode Ctrl+D at the start of a line is the end of the
  input (read returns 0), so `cat > file`, `sort` and `read` can be ended from the keyboard
  (until now line mode ignored it).
* **Lines that continue.** At the prompt, a line that is not complete (an open quote, `if`
  without `fi`, a trailing `|` or `&&`, a `\` at its end, a here-document) is continued on
  the next line, after the continuation prompt `… ≋>` (ASCII `... ~>`), drawn by
  `shell/prompt.c` like the other prompts.
* **`exec`** with a command runs it and ends the shell with its status (SurfOS has no
  exec(), so surfsh waits for it); `exec` with only redirections (`exec > log`) changes
  surfsh's own descriptors for the commands that follow.
* **Not done**: aliases, `trap` (SurfOS has no signals), `getopts`, arrays, `~`, `select`,
  brace expansion `{a,b}`, process substitution, `[[ ]]`. A history line is one typed line.

## Interfaces fixed by the foundation commits

| What | Where |
|---|---|
| `SYS_pipe` 46 `(int fds[2])`: `fds[0]` reads, `fds[1]` writes; `PIPE_SIZE` 8192, `PIPE_BUF` 4096, `PIPE_MAX` 32 | `include/surfos/syscall.h`, `include/surfos/pipe.h`, `fs/pipe.c`; libsurf `pipe()` |
| `SYS_spawnx` 47 `(const char *path, char *const argv[], const struct spawnx *o)`: `struct spawnx { int fds[4]; char *const *envp; int job; unsigned int flags; }` (28 bytes); `fds[0..2]` -1 the console, `fds[3]` -1 none; `envp` NULL an empty environment; `SPAWN_ENV_MAX` 64 strings in `SPAWN_ENV_BYTES` 4096; `job` 0 the caller's, `SPAWN_JOB_NEW` (-1) a new one led by the child, or a job to join; `flags` `SPAWN_FG`: the new job is the terminal's foreground | `include/surfos/spawn.h`, `kernel/process.c`; libsurf `spawnx()`, `environ` (crt0: the third word on the stack), `spawn()` and `spawnfd()` over it with `environ` |
| `SPAWN_MAX_ARGS` 128, `SPAWN_ARG_BYTES` 8192 | `include/surfos/spawn.h`, `kernel/process.c` |
| `SYS_waitx` 48 (stage 4) `(int pid, struct waitx *w, int options)`: `struct waitx { int pid, how, status, job; }` (16 bytes); `how` `WX_EXITED` 0, `WX_KILLED` 1, `WX_INTERRUPTED` 2 (Ctrl+C), `WX_STOPPED` (stage 5); options `WNOHANG` 1, `WUNTRACED` 4 (stage 5) | `include/surfos/job.h`, `kernel/task.c`, `kernel/syscall.c`; libsurf `waitx()` |
| `TTY_INTR` 6 (stage 4): the Ctrl+Cs that reached the caller's job since it last asked; 1: the caller (a shell) looks for Ctrl+C itself, is told instead of killed, and as the pager's owner takes one that reaches nothing else | `include/surfos/tty.h`, `kernel/tty.c`, `kernel/task.c` (`task_interrupt_job()`) |
| `#!` (stage 4): `process_spawn_ex()` runs `#!INTERPRETER [ARG]` scripts; a file neither ELF nor `#!` is `-ENOEXEC` at once | `kernel/process.c` |
| `SYS_job` 49 (stage 5) `(int op, int arg)`: `JOB_FG` 1 (the terminal's foreground job; the previous one), `JOB_BG` 2, `JOB_KILL` 3, `JOB_ID` 4; spawnx's `job`: 0 the caller's, `SPAWN_JOB_NEW` (-1), a job of the caller's, `SPAWN_JOB_NONE` (-2); `WUNTRACED` 4, `WX_STOPPED` 3 (status `WX_STOP_KEY` 0, `WX_STOP_READ` 1) | `include/surfos/job.h`, `include/surfos/spawn.h`, `kernel/task.c`, `kernel/tty.c`, `kernel/syscall.c`; libsurf `jobctl()` |
| `PROC_STOPPED` 5 in `struct procinfo`'s state; `ps` says `stopped`; `TS_STOPPED` | `include/surfos/proc.h`, `include/surfos/task.h`, `kernel/task.c` |
| `broken_pipe_exit(int on)` | `user/include/surf.h`, `user/libsurf/syscalls.c` |
| `struct prompt_ctx.continuation`: the continuation prompt | `include/surfos/prompt.h`, `shell/prompt.c`, `tests/host/test_prompt.c` |

## surfsh's parts afterwards

    main.c      the start (-l, -c, a file), the settings, the input: the editor, a file, a pipe
    editor.c    the line editor (unchanged but for the continuation prompt)
    complete.c  Tab: the commands of PATH too
    parse.c     text becomes a tree (the scanner too): lists, and-or lists, pipelines,
                commands, compound commands, functions' definitions
    unparse.c   a tree becomes text again: what a new surfsh runs, a function's definition
    expand.c    words become fields: parameters, $(...), $((...)), splitting, wildcards, quotes
    arith.c     $((...)): C's integer operators on 32-bit numbers
    glob.c      patterns and wildcards
    vars.c      variables, the environment, positional parameters, locals, options, functions
    exec.c      the tree runs: pipelines, redirections, subshells, compound commands,
                functions, the commands' order, Ctrl+C
    run.c       a command: the kernel's (kcmd), a program (and a script without #!)
    jobs.c      the jobs: their table, fg, bg, wait, the reports before a prompt
    builtins.c  the builtins of before, export, readonly, unset, shift, set's options
    script.c    : true false break continue return local eval . source read kernel
    test.c      test and [;  printf.c  printf

`tests/host/test_surfsh.c` runs the parser, the expansions (on `tests/host/sim_fs.c` for
wildcards), arithmetic, `test` and `printf` on the host.

## Checks

* `make unittest`: `test_surfsh` (above) and `test_prompt` (the continuation prompt).
* `user/bin/pipetest.c`: pipes from user space: data in order and whole, a full pipe's
  writer waits, the end of the input, a broken pipe (EPIPE, and status 141 by default),
  writes of up to `PIPE_BUF` never mixed, `poll()`, `PIPE_MAX`, the descriptors' limit, a kill
  and Ctrl+C cut its waits short. Job control: the smoke test's `@surfsh-jobs` over the
  serial line (Ctrl+Z, `fg`, `bg`, a background reader stopped, a stopped job killed, the
  shell's end, Ctrl+D) in place of the `jobtest` program first planned.
* The smoke test (`tools/qemu-run.py`): a block of pipelines, variables, scripts and jobs in
  surfsh over the serial line, and the two test programs; `make test-all`, `make test-tcp`,
  `make test-usb` at O0 and O2 (the terminal changes: Ctrl+Z, Ctrl+D), then an independent
  review.
* 10/25/26, the review (two reviewers: the kernel side, and surfsh with the new programs).
  Kernel: a reader Ctrl+Z stopped kept its place in the terminal's queue of readers; a stop
  asked for in a call that then waited again was never counted; a job was inherited onto
  another terminal and by what a kernel command starts; `JOB_FG` was open to any task, and
  the foreground job's shell was the last caller rather than the job's own (a surfsh inside a
  surfsh inside a surfsh lost the terminal); Ctrl+Z was swallowed in escape sequences; the
  job was set after the task could run; `exit(-1)` read as a kill. surfsh: a stack overflow
  in `${#name}` and a use after free in `${x#pattern}`; a script's own file on descriptor 3;
  a pipeline's status when its last command could not start; Ctrl+Z in an `until` loop;
  long here-documents that hung; `eval` and `.` losing `$?` and `set -e`'s conditions; a
  script's jobs dropped before `wait`; reports written into a redirected group; `exit` alone
  being 0; `tail -n` of a huge count; and smaller ones (`docs/ARCHITECTURE.md` section 21,
  "What a review found"). Each has a check in the smoke test or `test_surfsh`.
