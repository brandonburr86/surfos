#!/usr/bin/env python3
"""
SurfCPU open flow: the solver command of the LVS proof's z3cmd route
--------------------
File: z3cmd.py Date: 10/27/26 (SurfCPU, the flow track)
--------------------
lvs_equiv (fasm2netlist 0948889f, src/lvs/solver.cpp) asks its questions one of two ways. By
default (--solver libz3) it keeps one incremental session of the Z3 library it links, which keeps
what it learnt from one question to the next and whose memory grows with the run: on the DDR+GPU
design at bfaac48 it passed the 3.5 GB Docker cap at 41 % of its questions (docs/GPU.md 16.11).
With --solver CMD --format smt2 it writes each question to a file of its own (the whole network
in SMT-LIB 2, the miter asserted, check-sat), runs `CMD FILE 2>/dev/null` through the shell and
reads the answer off the standard output: "unsat" proves the point, "sat" is a difference,
anything else is unknown, never proved. The same questions in the same order, one process each,
so each question's memory is given back when it is answered.

The openxc7 image has the Z3 library (libz3-4, the one lvs_equiv links) but no solver program,
so this is the command: it hands the file's text to libz3's own SMT-LIB 2 front end
(Z3_eval_smtlib2_string, through ctypes) and prints Z3's answer, sat, unsat or unknown. Anything
else (the library missing, a parse error, an API error, no answer) prints nothing and exits 1,
which lvs_equiv counts as unknown, never as proved.

    python3 -I devenv/openxc7/z3cmd.py QUESTION.smt2        what lvs_equiv runs
    python3 -I devenv/openxc7/z3cmd.py --self-test [CMD]    before the proof (tools/openxc7-steps.sh)
    python3 -I devenv/openxc7/z3cmd.py --summary LOG        the questions' log, summed up

With Z3CMD_LOG set, each question appends a line to that file: the question's bytes, the seconds
it took, Z3's error code, its answer (or -; the first line of what Z3 printed, an error's text with
its spaces), and this process's largest resident set in KiB. --summary reads the answer as what lies
between the third field and the last, and counts a line it cannot read (cut short) as unreadable.

--self-test runs CMD (default: this file under this Python) exactly as lvs_equiv runs its solver,
`CMD FILE 2>/dev/null` through /bin/sh from the current directory, on three questions whose
answers are known: an unsatisfiable one must print unsat and exit 0, a satisfiable one sat and
exit 0, a malformed one nothing and exit 1. Each answer is read as lvs_equiv reads it (unsat
before sat, lower case). Exit 0 only if all three come back so. From G4's build/g4-lvs/z3cmd.py
and lvs-solver.sh, with which the DDR+GPU bitstream at bfaac48 was proved this way (20971 proved,
0 differ, 0 unknown in 13 h 52 min within the cap; docs/evidence/gpu-g4/lvs-ddr-gpu-bfaac48.txt).
Only the Python standard library.
"""

import ctypes
import os
import resource
import shlex
import subprocess
import sys
import tempfile
import time

# The three questions of the self-test, in the shape lvs_equiv writes (write_smtlib2: a Bool
# constant per input, a define-fun per node, the miter asserted, check-sat).
SELF_TEST = (
    ("unsat", "(set-logic QF_UF)\n(declare-const n1 Bool)\n(define-fun n2 () Bool (and n1 (not n1)))\n"
              "(assert n2)\n(check-sat)\n", "unsat", 0),
    ("sat", "(set-logic QF_UF)\n(declare-const n1 Bool)\n(declare-const n2 Bool)\n"
            "(define-fun n3 () Bool (and n1 (not n2)))\n(assert n3)\n(check-sat)\n", "sat", 0),
    ("error", "(set-logic QF_UF)\n(assert n9)\n(check-sat)\n", "", 1),
)


def solve(path: str) -> int:
    """one question: print Z3's answer and exit 0, or print nothing and exit 1"""
    t0 = time.monotonic()
    with open(path, "rb") as f:
        text = f.read()
    err, ans = -1, ""
    try:
        lib = ctypes.CDLL("libz3.so.4")
        lib.Z3_mk_config.restype = ctypes.c_void_p
        lib.Z3_del_config.argtypes = [ctypes.c_void_p]
        lib.Z3_mk_context.restype = ctypes.c_void_p
        lib.Z3_mk_context.argtypes = [ctypes.c_void_p]
        lib.Z3_set_error_handler.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
        lib.Z3_get_error_code.restype = ctypes.c_int
        lib.Z3_get_error_code.argtypes = [ctypes.c_void_p]
        lib.Z3_eval_smtlib2_string.restype = ctypes.c_char_p
        lib.Z3_eval_smtlib2_string.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
        cfg = lib.Z3_mk_config()
        ctx = lib.Z3_mk_context(cfg)
        lib.Z3_del_config(cfg)
        lib.Z3_set_error_handler(ctx, None)     # no handler: an error only sets the error code
        out = lib.Z3_eval_smtlib2_string(ctx, text)
        err = lib.Z3_get_error_code(ctx)
        ans = out.decode("utf-8", "replace").strip() if out is not None else ""
    except OSError:                             # no libz3.so.4: no answer
        pass
    log = os.environ.get("Z3CMD_LOG")
    if log:
        with open(log, "a") as f:
            f.write("%d %.3f %s %s %d\n" % (len(text), time.monotonic() - t0, err, ans.split("\n")[0][:60] or "-",
                                             resource.getrusage(resource.RUSAGE_SELF).ru_maxrss))
    if err != 0 or ans not in ("sat", "unsat", "unknown"):
        return 1
    print(ans)
    return 0


def as_lvs_equiv_reads(output: str) -> str:
    """the answer as lvs_equiv classifies a solver's standard output (solver.cpp run_solver)"""
    o = output.lower()
    return "unsat" if "unsat" in o else "sat" if "sat" in o else "unknown"


def self_test(cmd: str) -> int:
    print("z3cmd self-test: the command %r, run as lvs_equiv runs it (CMD FILE 2>/dev/null, through /bin/sh)" % cmd)
    try:
        lib = ctypes.CDLL("libz3.so.4")
        v = [ctypes.c_uint() for _ in range(4)]
        lib.Z3_get_version(*[ctypes.byref(x) for x in v])
        print("libz3 %d.%d.%d.%d (this Python's)" % tuple(x.value for x in v))
    except OSError as e:
        print("libz3: not found by this Python (%s)" % e)
    env = {k: v for k, v in os.environ.items() if k != "Z3CMD_LOG"}
    fails = 0
    with tempfile.TemporaryDirectory(prefix="z3cmd-self-test-") as d:
        for name, text, want, want_rc in SELF_TEST:
            path = os.path.join(d, name + ".smt2")
            with open(path, "w") as f:
                f.write(text)
            r = subprocess.run(["/bin/sh", "-c", "%s %s 2>/dev/null" % (cmd, path)], stdout=subprocess.PIPE,
                               stdin=subprocess.DEVNULL, env=env, text=True)
            got = r.stdout.strip()
            ok = got == want and r.returncode == want_rc and (want == "" or as_lvs_equiv_reads(got) == want)
            fails += not ok
            print("self-test %-5s answer %r, exit %d (wants %r, exit %d; lvs_equiv reads %s): %s"
                  % (name, got, r.returncode, want, want_rc, as_lvs_equiv_reads(got), "ok" if ok else "WRONG"))
    print("z3cmd self-test: " + ("PASS" if not fails else "FAIL (%d of %d wrong)" % (fails, len(SELF_TEST))))
    return 0 if not fails else 1


def summary(path: str) -> int:
    """the questions' log (Z3CMD_LOG) summed up, one line. A line is bytes, seconds, Z3's error code,
    its answer and the largest resident set; the answer is the first line of what Z3 printed, and an
    error's text has spaces, so it is whatever lies between the third field and the last. A line that
    does not read so (cut short when its process was killed while writing it) is counted, never a crash."""
    rows, unread = [], 0
    with open(path, errors="replace") as f:
        for i, line in enumerate(f, 1):
            p = line.split()
            try:
                if len(p) < 4:
                    raise ValueError("too few fields")
                size, secs, err = int(p[0]), float(p[1]), p[2]
                int(err)
                rss, answer = (int(p[-1]), " ".join(p[3:-1])) if len(p) >= 5 else (0, p[3])
            except ValueError:
                unread += 1
                continue
            rows.append((i, size, secs, err, answer, rss))
    more = "; %d line%s unreadable" % (unread, "" if unread == 1 else "s") if unread else ""
    if not rows:
        print("z3cmd: no question answered (%s %s)" % (path, "has only unreadable lines" if unread else "is empty"))
        return 0
    answers = {}
    for r in rows:
        key = r[4] if r[3] == "0" else "error %s" % r[3]
        answers[key] = answers.get(key, 0) + 1
    longest = max(rows, key=lambda r: r[2])
    largest = max(rows, key=lambda r: r[5])
    total = sum(r[2] for r in rows)
    print("z3cmd: %d questions (%s); question files of %d to %d bytes; %.1f s in the solver, %.2f s a question, "
          "the longest %.2f s (question %d); the largest solver process %.0f MiB (question %d)%s"
          % (len(rows), ", ".join("%d %s" % (n, a) for a, n in sorted(answers.items())), min(r[1] for r in rows),
             max(r[1] for r in rows), total, total / len(rows), longest[2], longest[0], largest[5] / 1024.0,
             largest[0], more))
    return 0


def main(argv: list) -> int:
    if len(argv) >= 2 and argv[1] == "--self-test":
        default = "%s -I %s" % (shlex.quote(sys.executable), shlex.quote(os.path.abspath(__file__)))
        return self_test(argv[2] if len(argv) > 2 else default)
    if len(argv) == 3 and argv[1] == "--summary":
        return summary(argv[2])
    if len(argv) == 2 and not argv[1].startswith("-"):
        return solve(argv[1])
    print(__doc__.split("--------------------")[0].strip() + "\nusage: z3cmd.py QUESTION | --self-test [CMD] | "
          "--summary LOG", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
