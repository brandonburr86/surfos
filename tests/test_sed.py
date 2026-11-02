#!/usr/bin/env python3
"""sed and grep (user/bin/sed.c, grep.c), built for the host, against the real ones, process against process: the same arguments,
input and files, then the same output, status and files afterwards. A corpus, and scripts made at random. GNU's tools in the
devenv are the reference for everything; BSD's, on the Mac, for what they agree on. What the real tools cannot be asked is in the
last part: bad scripts and input by the kilobyte. 11/01/26, the command-line suite, track text.

    tests/test_sed.py build/O0/host/sed-host build/O0/host/grep-host [rounds [seed]]

Where they differ on purpose it says so here: status 2 for every kind of trouble (GNU has 1 and 4), the messages' wording, w files that
appear when sed ends and not at the start, an empty match that steps over a whole UTF-8 character (GNU's goes by the byte), and what
own_rules() holds."""

import os
import random
import shutil
import subprocess
import sys
import tempfile

SED, GREP = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
ROUNDS = int(sys.argv[3]) if len(sys.argv) > 3 else 300
SEED = int(sys.argv[4]) if len(sys.argv) > 4 else 20261101
TIMEOUT = 20                                    # for ours, on a machine that may be busy
TIMEOUT_REAL = 5                                # a random script may well never end

checks = failures = skipped = 0


def fail(what):
    global failures
    failures += 1
    if failures <= 40:
        print('FAIL:', what)


def tool_kind(name):
    if not shutil.which(name):
        return None
    try:
        out = subprocess.run([name, '--version'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=TIMEOUT).stdout
    except subprocess.TimeoutExpired:
        return None
    return 'gnu' if b'GNU' in out and b'BSD' not in out else 'bsd'


SED_KIND, GREP_KIND = tool_kind('sed'), tool_kind('grep')


def run(prog, args, stdin, files, utf8, timeout=TIMEOUT):
    """in a fresh directory: stdout, status, and the files there afterwards"""
    d = tempfile.mkdtemp(prefix='sedtest')
    try:
        for name, data in files.items():
            os.makedirs(os.path.dirname(os.path.join(d, name)), exist_ok=True)
            with open(os.path.join(d, name), 'wb') as f:
                f.write(data)
        env = dict(os.environ)
        env['LC_ALL'] = 'C.UTF-8' if utf8 else 'C'
        p = subprocess.run([prog] + args, input=stdin, stdout=subprocess.PIPE, stderr=subprocess.PIPE, cwd=d, env=env, timeout=timeout)
        after = {}
        for name in sorted(os.listdir(d)):
            path = os.path.join(d, name)
            if os.path.isfile(path):
                with open(path, 'rb') as f:
                    after[name] = f.read()
        if b'Sanitizer' in p.stderr or b'runtime error' in p.stderr:    # built that way on the Mac
            fail('%s %r: %r' % (os.path.basename(prog), args, p.stderr[:300]))
        return p.stdout, p.returncode, after, p.stderr
    finally:
        shutil.rmtree(d, ignore_errors=True)


def same_status(real, mine):
    """GNU's 1 and 4 are trouble, as 2 is; every other number is q's"""
    if real in (1, 2, 4) and mine == 2:
        return True
    return real == mine


def compare(real, mine, args, stdin=b'', files=None, utf8=False, what='sed'):
    global checks
    files = files or {}
    checks += 1
    try:
        r = run(real, args, stdin, files, utf8, TIMEOUT_REAL)
    except subprocess.TimeoutExpired:
        return False                            # a script that does not end, in the real one too: no comparison
    try:
        m = run(mine, args, stdin, files, utf8)
    except subprocess.TimeoutExpired:
        fail('%s %r on %r: did not end' % (what, args, stdin))
        return True
    if r[1] in (1, 4) and m[1] == 2:            # a mistake: both refuse, and the files there are as they were (GNU has made w's by then)
        ok = all(r[2].get(k) == m[2].get(k) for k in files)
    else:
        ok = r[0] == m[0] and same_status(r[1], m[1]) and r[2] == m[2]
    if m[1] < 0:
        ok = False
    if not ok:
        fail('%s %r on %r: real %r %d, ours %r %d %r' % (what, args, stdin[:60], r[0][:80], r[1], m[0][:80], m[1], m[3][:80]))
        if r[2] != m[2]:
            print('   files: real', r[2], 'ours', m[2])
    return True


def expect(prog, args, stdin=b'', files=None, status=0, out=None, after=None, said=None):
    """what no real tool can be asked: ours against what is written here"""
    global checks
    checks += 1
    m = run(prog, args, stdin, files or {}, False)
    if m[1] != status or (out is not None and m[0] != out) or (after is not None and m[2] != after) or (said is not None and said not in m[3]):
        fail('%s %r on %r: status %d, %r, files %r, said %r' % (os.path.basename(prog), args, stdin[:60], m[1], m[0][:80], {k: v[:40] for k, v in m[2].items()}, m[3][:120]))


TEXT = b'one\ntwo\nthree\nfour\nfive\nsix\nseven\neight\nnine\nten\n'
AB = b'a\nb\nc\nd\ne\n'

# (arguments, input, files): POSIX's, which BSD's sed agrees on with GNU's
COMMON = [
    (['p'], AB), (['-n', 'p'], AB), (['-n', '2p'], AB), (['-n', '$p'], AB), (['-n', '2,4p'], AB), (['2d'], AB), (['2,3d'], AB), (['$d'], AB),
    (['1!d'], AB), (['2,3!d'], AB), (['/b/d'], AB), (['/b/,/d/d'], AB), (['/b/!d'], AB), (['/b/,3d'], AB), (['2,/d/d'], AB),
    (['-n', '/t/p'], TEXT), (['-n', '/^t/p'], TEXT), (['-n', '/e$/p'], TEXT), (['-n', '/^.\\{3\\}$/p'], TEXT), (['-n', '/[aeiou]\\{2\\}/p'], TEXT),
    (['s/o/0/'], TEXT), (['s/e/E/g'], TEXT), (['s/e/E/2'], TEXT), (['s/\\(.\\)\\(.\\)/\\2\\1/'], TEXT), (['s/.*/[&]/'], TEXT), (['s/.*/&&/'], AB),
    (['s/a/\\&/'], AB), (['s,a,/,'], AB), (['s|a|b|'], AB), (['s#a#b#'], AB), (['s/x*/-/g'], b'abc\n'), (['s/b*/X/2'], b'abc\n'), (['s/b*/X/g'], b'abbc\n'),
    (['-E', 's/(a|b)+/<&>/g'], b'aabbcab\n'), (['-E', 's/a{2,}/X/'], b'caaad\n'), (['-E', 's/^(.)(.*)$/\\2\\1/'], TEXT), (['-E', '/one|two/d'], TEXT),
    (['y/abc/xyz/'], b'aabbcc\n'), (['y/a-c/x-z/'], b'a-c\n'), (['y/\\//x/'], b'a/b\n'), (['y,a\\,b,xyz,'], b'a,b\n'),
    (['='], AB), (['-n', '='], AB), (['-n', '$='], AB), (['-n', '/c/='], AB),
    (['n;d'], AB), (['$!N;s/\\n/ /'], AB), (['N;N;s/\\n/+/g'], b'1\n2\n3\n4\n5\n6\n'), (['$!N;P;D'], AB), 
    (['-n', 'h;n;G;p'], b'1\n2\n3\n4\n'), (['-n', 'x;p'], AB), (['x;$G'], AB), (['1!G;h;$!d'], AB), (['G'], AB), (['H;$!d;x;s/\\n/,/g'], AB),
    (['-n', '$!{h;d;};x;G;p'], AB), (['/c/{s/c/X/;p;}'], AB), (['/b/,/d/{/c/d;}'], AB), (['/b/,/d/{/b/d;/d/d;}'], AB), (['2{p;p;}'], AB),
    (['b'], AB),  (['/b/bx\ns/$/!/\n:x'], AB),  (['s/a/X/;ty\ns/$/!/\n:y'], AB),
    ([':a\ns/^.\\{1,3\\}$/ &/\nta'], AB), ([':a\n$!N;s/\\n//;ta'], AB), (['a\\\nappended\n'], AB), (['i\\\ninserted\n'], AB), (['c\\\nchanged\n'], AB),
    (['2a\\\none\\\ntwo\n'], AB), (['2i\\\none\\\ntwo\n'], AB), (['2c\\\none\\\ntwo\n'], AB), (['2,3c\\\nX\n'], AB), (['/b/,/c/c\\\nX\n'], AB), (['$a\\\nend\n'], AB),
    (['1i\\\nstart\n'], AB), (['1q'], AB), (['3q'], AB), (['/c/q'], AB), (['$q'], AB), (['-n', '2{p;q;}'], AB),  (['-n', 'l'], b'tab\there\n'),
    (['-n', 'l'], b'\x01\x7f\xff\n'),  (['-e', 'p', '-e', 'p'], AB), (['-e', 's/a/b/', '-e', 's/b/c/'], AB),
    (['-n', '-e', '/a/p', '-e', '/b/p'], AB), (['s/a/1\\\n2/'], AB), (['s/$/\\\n/'], AB), (['s/^/>/'], AB), (['s/$/</'], AB),
    (['\\,a,d'], AB), (['\\%b%d'], AB), (['/a\\/b/d'], b'a/b\nc\n'), (['s/a\\/b/X/'], b'a/b\n'), (['s/[/]/X/'], b'a/b\n'), (['s/[[:digit:]]\\{1,\\}/N/g'], b'a1b22c333\n'),
    (['s/[^[:alpha:]]//g'], b'a1 b2\n'), (['s/a/X/'], AB), (['#comment\np'], AB),
     (['-n', 'p'], b''), (['$d'], b''), (['$a\\\nx'], b''), (['s/a/b/'], b'\n'), (['/^$/d'], b'a\n\n\nb\n'), (['$!{$!N;};P;D'], b'1\n2\n3\n'),
    (['2,1p'], AB), (['-n', '3,1p'], AB), (['-n', '/c/,1p'], AB), (['-n', '/b/,/nomatch/p'], AB), (['-n', '1,/a/p'], AB),
    (['w out\n'], AB, {}), (['-n', '/b/w out'], AB, {}), (['s/a/X/w out'], AB, {}), (['-n', 's/a/X/w out'], AB, {}), (['-n', '/a/w x\n/b/w y'], AB, {}), (['w f\nw f'], AB, {}),
    (['r in'], AB, {'in': b'R\n'}), (['2r in'], AB, {'in': b'R1\nR2\n'}), (['$r in'], AB, {'in': b'R\n'}), (['r nosuch'], AB), (['2r in\n2a\\\nA\n'], AB, {'in': b'R\n'}),
    (['-n', 'p', 'x'], b'', {'x': b'1\n2\n'}), (['-n', '$p', 'x', 'y'], b'', {'x': b'1\n', 'y': b'2\n'}), (['-n', '2p', 'x', 'y'], b'', {'x': b'1\n', 'y': b'2\n'}),
    (['-n', '$=', 'x', 'y'], b'', {'x': b'1\n2\n', 'y': b'3\n'}), (['-f', 'script'], AB, {'script': b's/a/X/\ns/b/Y/\n'}),
    (['-f', 'script', 'x'], b'', {'script': b'p\n', 'x': b'q\n'}), (['-n', '-f', 'script'], AB, {'script': b'# c\n/a/p\n\n\n/b/p'}),
    (['$!d'], AB), ([''], AB), (['-e', ''], AB), (['-n', '-e', ''], AB), (['s/./X/3'], b'abc\nab\n'), (['s/a/b/g;s/b/c/g'], b'aabb\n'),
    (['s/\\([ab]*\\)c/\\1\\1/'], b'abcabc\n'), (['s/\\(a\\)\\(b\\)\\(c\\)\\(d\\)\\(e\\)\\(f\\)\\(g\\)\\(h\\)\\(i\\)/\\9\\8\\7\\6\\5\\4\\3\\2\\1/'], b'abcdefghi\n'),
    (['-n', '/\\(.\\)\\1/p'], b'aa\nab\n'), (['s/\\(.\\)\\1/X/g'], b'aabbab\n'), (['N;s/^\\(.*\\)\\n\\1$/same/'], b'x\nx\n'),
]

# what is GNU's alone, or where BSD's differs
GNU_ONLY = [
    (['p', '-'], b'a\n'), (['-e', 'a\\', '-e', 'text'], AB), (['-e', 'i\\', '-e', 'text'], AB), (['N;P;D'], b'1\n2\n3\n4\n'), (['/b/b;s/$/!/'], AB), (['s/a/X/;t\ns/$/!/'], AB), (['l'], b'a\tb\\c\n'), (['-n', 'l'], b'x' * 200 + b'\n'), (['p'], b''),
    (['-n', '1~2p'], TEXT), (['-n', '0~3p'], TEXT), (['-n', '2~0p'], TEXT), (['-n', '2,+2p'], TEXT), (['-n', '/t/,+1p'], TEXT), (['-n', '2,~4p'], TEXT), (['-n', '4,~4p'], TEXT),
    (['-n', '5,~0p'], TEXT), (['-n', '/t/,~3p'], TEXT), (['-n', '0,/one/p'], TEXT), (['-n', '1,/one/p'], TEXT), (['0,/e/d'], TEXT), (['0,/e/s/e/E/'], TEXT), (['0,/x/d'], TEXT),
    (['-n', '$!{n;p}'], AB), (['-n', '/ONE/Ip'], TEXT), (['/T/I,/F/Id'], TEXT), (['s/ONE/x/I'], TEXT), (['s/one/x/i'], b'ONE\n'), (['s/o/0/2g'], b'ooooo\n'), (['s/o/0/3g'], b'oo oo\n'),
    (['s/a*/x/g'], b'baaac\n'), (['s/a*/x/2'], b'baaac\n'), (['s/a*/x/3'], b'baaac\n'), (['s/a*/x/2g'], b'baaac\n'), (['s/\\b/|/g'], b'one two\n'), (['s/\\B/-/g'], b'abc de\n'),
    (['s/\\</</g;s/\\>/>/g'], b'one two\n'), (['s/\\w\\+/W/g'], b'a1 b_2 -c\n'), (['s/\\W\\+/-/g'], b'a, b; c\n'), (['s/\\s\\+/ /g'], b'a \t b\n'), (['s/\\S\\+/X/g'], b'ab cd\n'),
    (['s/a\\+/X/'], b'caab\n'), (['s/ca\\?b/X/'], b'cb cab\n'), (['s/a\\|b/X/g'], b'abc\n'), (['s/\\`a/X/g'], b'aaa\n'), (["s/a\\'/X/g"], b'aaa\n'), (['N;s/^b/X/'], b'a\nb\n'),
    (['s/a/\\n/'], AB), (['s/a/x\\ty/'], AB), (['s/\\t/T/'], b'a\tb\n'), (['-E', 's/\\t/T/'], b'a\tb\n'), (['s/[\\t]/T/'], b'a\tb\n'), (['N;s/[\\n]/T/'], b'a\nb\n'), (['N;s/a\\nb/X/'], b'a\nb\n'),
    (['s/\\x41/b/'], b'A\n'), (['s/\\o101/b/'], b'A\n'), (['s/\\d065/b/'], b'A\n'), (['s/\\cA/b/'], b'\x01\n'), (['s/a/\\x26/'], AB), (['s/a/\\o101\\d066\\cA/'], AB), (['s/\\f/x/;s/\\v/y/;s/\\a/z/;s/\\r/w/'], b'\x0c\x0b\x07\x0d\n'),
    (['a hello'], AB), (['1a hello'], AB), (['i hello'], AB), (['c hello'], AB), (['2c hello'], AB), (['a\\  hello'], AB), (['a   hello'], AB), (['i\\   hello'], AB), (['a\\text'], AB),
    (['a foo\\bar'], AB), (['a foo\\\\bar'], AB), (['a x\\ty'], AB), (['a x\\\ny'], AB), (['a x;p'], AB), (['1{a x\n}'], AB), (['-e', 'a x', '-e', 'p'], AB), (['$a x'], b''), (['a\\'], AB), (['a'], AB),
    (['2,3c X'], AB), (['2,3!c X'], AB), (['$!N;c X'], AB), (['/b/,/nomatch/c X'], AB), (['c\\  x'], AB),
    (['{p}'], AB), (['{p;p}'], AB), (['/b/{p}'], AB), (['{p};p'], AB), (['{p}#c'], AB), (['p#c'], AB), (['p # c'], AB), (['s/a/b/ # c'], AB), (['{;p;}'], AB), (['{ p ; p ; }'], AB), (['/a/ { p ; } ; p'], AB),
    ([':a;N;$!ba;s/\\n/ /g'], AB), (['{:a;N;ba}'], AB), (['{bx};:x;p'], AB), ([':a b\nb'], AB), ([': x'], AB), (['bx ;p;:x'], AB), (['t;p'], AB), (['T;p'], AB), (['s/a/x/;T;p'], AB), (['s/a/x/;Tz;p;:z'], AB), (['s/a/b/;n;ty;s/$/!/;:y'], AB),
    (['q5'], AB), (['q 5'], AB), (['Q'], AB), (['Q7'], AB), (['2Q'], AB), (['3q 9;p'], AB), (['q5 ;p'], AB), (['$!N;q'], AB), (['a x\nq'], AB), (['a x\nQ'], AB), (['r in\nq'], AB, {'in': b'R\n'}),
    (['-n', 'l 5'], b'abcdefghij\n'), (['-n', 'l 1'], b'abc\n'), (['-n', 'l 2'], b'abc\n'), (['-n', 'l 0'], b'x' * 200 + b'\n'), (['-n', 'l;l'], b'a\n'), (['l 5;p'], b'a\n'), (['l5'], b'a\n'),
    (['#n\np'], AB), (['#n'], AB), (['#x\np'], AB), (['-e', '#n', '-e', 'p'], AB), (['-e', 'p', '-e', '#n'], AB), (['-f', 's'], AB, {'s': b'#n\np\n'}), (['-f', 's'], AB, {'s': b'#n'}), (['-e', 'p', '-f', 's'], AB, {'s': b'#n\n'}),
    (['-s', '-n', '$p', 'x', 'y'], b'', {'x': b'1\n2\n', 'y': b'3\n4\n'}), (['-s', '-n', '1p', 'x', 'y'], b'', {'x': b'1\n2\n', 'y': b'3\n4\n'}), (['-s', '-n', '2,3p', 'x', 'y'], b'', {'x': b'1\n2\n', 'y': b'3\n4\n'}),
    (['-n', '2,3p', 'x', 'y'], b'', {'x': b'1\n2\n', 'y': b'3\n4\n'}), (['-s', 'N', 'x', 'y'], b'', {'x': b'1\n2\n', 'y': b'3\n4\n'}), (['-s', 'n;d', 'x', 'y'], b'', {'x': b'1\n2\n', 'y': b'3\n4\n'}), (['-s', 'p', 'x'], b'', {'x': b'1'}),
    (['-s', 'p', 'x', 'y'], b'', {'x': b'1', 'y': b'2'}), (['p', 'x', 'y'], b'', {'x': b'1', 'y': b'2'}),
    (['p', '-n'], AB), (['--', 'p'], AB), (['-ne', 'p'], AB), (['-nf', 's'], AB, {'s': b'p\n'}), (['p', 'nosuch', '-n'], b''), (['-n', '$p', 'x', 'nosuch'], b'', {'x': b'a\n'}), (['-n', '$=', 'x', 'nosuch', 'y'], b'', {'x': b'a\n', 'y': b'b\n'}), (['p', 'nosuch'], b''),
    # the missing last newline
    (['p'], b'a'), (['-n', 'p;p'], b'a'), (['x'], b'a\nb'), (['G'], b'a'), (['h;G'], b'a'), (['H;x'], b'a'), (['x;G'], b'a'), (['N'], b'1\n2'), (['h;x'], b'a'), (['g'], b'a'), (['x;g'], b'a'), (['p;x'], b'a\nb'),
    (['s/$/x/'], b'a'), (['y/a/b/'], b'a'), (['D'], b'a'), (['$!N;P;D'], b'1\n2'), (['P'], b'a'), (['l'], b'a'), (['n'], b'a'), (['w /dev/stdout'], b'a'), (['$a x'], b'a'), (['i x'], b'a'), (['='], b'a'), (['r in'], b'a', {'in': b'R'}),
    (['r in'], b'1\n2\n', {'in': b'R'}), (['w out'], b'a\nb'), (['-n', 'w out'], b'a'), (['c x'], b'a'), (['d'], b'a'), (['q'], b'a'), (['$!d'], b'a\nb'), (['-n', '$p'], b'a\nb'), (['-s', '-n', '$p', 'x', 'y'], b'', {'x': b'1', 'y': b'2'}),
    # w's special files, and the same file twice
    (['w /dev/null'], AB), (['-n', 's/a/X/w /dev/null'], AB), (['-n', 'w /dev/stdout'], AB), (['w /dev/stdout'], AB), (['-n', '1w /dev/stdout\n2w /dev/stderr'], AB), (['-n', 's/a/b/w /dev/stdout'], AB), (['-n', '/a/w f\n/b/w f'], AB), (['s/a/X/w f\ns/b/Y/w f'], AB),
    # w over a file that is there
    (['w out'], AB, {'out': b'old\n'}), (['-n', '/nomatch/w out'], AB, {'out': b'old\n'}),
    # -i
    (['-i', 's/a/X/', 'f'], b'', {'f': AB}), (['-i.bak', 's/a/X/', 'f'], b'', {'f': AB}), (['-ibak_*', 's/a/X/', 'f'], b'', {'f': AB}), (['-i', 's/a/X/', 'f', 'g'], b'', {'f': AB, 'g': b'a\n'}), (['-i', '2q', 'f'], b'', {'f': AB}), (['-i', '2Q', 'f'], b'', {'f': AB}),
    (['-i', '1d', 'f'], b'', {'f': AB}), (['-i', '$d', 'f'], b'', {'f': AB}), (['-i', 'd', 'f'], b'', {'f': AB}), (['-n', '-i', '2p', 'f'], b'', {'f': AB}), (['-i', '1i\\\nnew', 'f'], b'', {'f': AB}), (['-i', '$a end', 'f'], b'', {'f': AB}),
    (['-i', '$a end', 'f', 'g'], b'', {'f': b'1\n', 'g': b'2\n'}), (['-i', 'w out', 'f'], b'', {'f': AB}), (['-i', 'w /dev/stdout', 'f'], b'', {'f': AB}), (['-i', 's/a/b/', 'f', 'nosuch', 'g'], b'', {'f': b'a\n', 'g': b'a\n'}), (['-i', 'p', 'f'], b'', {'f': b'a'}),
    (['-i', 's/a/b/'], b'x\n'), (['-i', '$=', 'f', 'g'], b'', {'f': AB, 'g': AB}), (['-s', '-i', '1d', 'f', 'g'], b'', {'f': AB, 'g': AB}), (['-i.bak', '1d', 'f'], b'', {'f': AB, 'f.bak': b'old backup\n'}), (['-in', 's/a/b/', 'f'], b'', {'f': b'a\n'}), (['-i', 'p', 'e'], b'', {'e': b''}),
    # bad scripts: both refuse, and nothing changes
    (['k'], AB), (['p p'], AB), ([':'], AB), (['b nowhere'], AB), (['{p'], AB), (['p}'], AB), (['1}'], AB), (['}'], AB), (['1,'], AB), (['1,p'], AB), (['/a'], AB), (['s/a'], AB), (['s/a/b'], AB), (['s/\\(/b/'], AB), (['s/a/\\1/'], AB), (['s/x/y/gg'], AB), (['s/x/y/pp'], AB),
    (['s/x/y/0'], AB), (['s/x/y/2 3'], AB), (['s/a/b/x'], AB), (['y/x/ab/'], AB), (['y/a/b/x'], AB), (['0p'], AB), (['0,5p'], AB), (['2!!p'], AB), (['1#c'], AB), (['1,2q'], AB), (['qx'], AB), (['l x'], AB), (['s/a/b/w'], AB), (['w'], AB), (['r'], AB), (['s//y/'], AB), (['/a/s//\\1/'], AB),
    (['-e'], AB), (['-x', 'p'], AB), ([], AB), (['-E'], AB), (['-n'], AB), (['-i'], AB), (['-f', 'nosuch'], AB), (['s/a/b/I;s//c/I'], AB), (['s/[/x/'], b'[/\n'), (['1{a x}'], AB),
    # NUL bytes
    (['s/a/b/'], b'\x00a\n'), (['s/./X/g'], b'a\x00b\n'), (['p'], b'a\x00b'), (['y/a/b/'], b'a\x00a\n'), (['l'], b'a\x00b\n'),
    # empty regular expressions
    (['/a/s//X/'], AB), (['s/a/X/;s//Y/'], b'aa\n'), (['/a/{//d}'], AB), (['/\\(a\\)/s//\\1\\1/'], AB), (['-n', '/b/,//p'], AB), (['/a/,/c/{//d}'], AB),
    # a backslash before itself, and a delimiter that is one of case conversion's letters: no conversion is asked for
    (['s/b/\\\\U/'], b'abc\n'), (['s/b/\\\\l\\\\E/'], b'abc\n'), (['sUbU\\UU'], b'abc\n'), (['sEbE\\EE'], b'abc\n'),
    # delimiters
    (['s.a\\.b.X.'], b'axb\n'), (['s.a\\.b.X.'], b'a.b\n'), (['s|a\\|b|X|'], b'a|b\n'), (['-E', 's|a\\|b|X|g'], b'ab\n'), (['snanxn'], b'a\n'), (['s,\\,,x,'], b',\n'), (['s&a&\\&&'], b'a\n'), (['s&a&\\n&'], b'a\n'), (['s1a1\\11'], b'a\n'), (['s(a(\\(('], b'a\n'), (['s\\a\\b\\'], b'a\n'),
    (['\\aba p'], b'a\nb\n'), (['\\a\\aa p'], b'a\n'), (['\\/a\\/b/p'], b'a/b\n'), (['/[/]/p'], b'/\n'), (['/[]/]/p'], b'/\n'), (['s/[]/]/x/'], b'/\n'), (['s/[[:alpha:]/]/x/'], b'/\n'), (['s/[^/]/x/'], b'/a\n'), (['s/\\[/x/'], b'[/\n'), (['s/[\\/]/x/'], b'\\/\n'),
    (['s/a/b/;;'], AB), (['p;;p'], AB), (['b;p'], AB), (['b a;:a;'], AB), (['t;:a'], AB), (['1 !p'], AB), (['1! p'], AB), (['1,2 p'], AB), (['1 , 2 p'], AB), (['1, 2p'], AB), (['/a/ , /b/ p'], AB), (['2 ~ 2 p'], AB), (['2, + 1p'], AB), (['-n', '$,2p'], AB), (['-n', '2,$p'], AB), (['-n', '1~2,+1p'], AB), (['-n', '2,2p'], AB), (['2,4{N;N;N;d}'], TEXT), (['1{n;n};2,4d'], TEXT), (['-n', '1{n;n};2,4p'], TEXT), (['-n', '2,3{n;n;p}'], TEXT),
    (['-n', 'n;2,3p'], TEXT), (['-n', 'N;N;N;2,4p'], TEXT), (['-n', '/three/{N;N;};/four/,/six/p'], TEXT), (['1{N;N;};2,+1d'], TEXT), (['1{N;N;};2,~4d'], TEXT), (['N;N;2,3d'], TEXT),
    (['y/\\a/x/'], b'a\x07\n'), (['y/a/\\n/'], b'a\n'), (['y/a/\\t/'], b'a\n'), (['y/ab/\\\\x/'], b'ab\n'), (['y/\\\\/x/'], b'a\\b\n'), (['y/a\\nb/xyz/'], b'abc\n'), (['y/aa/bc/'], b'a\n'), (['y/a/\\//'], b'a\n'),
]

UTF8 = [
    (['s/./x/g'], 'héllo€𝄞\n'.encode()), (['s/é*/x/g'], 'aééb\n'.encode()), (['y/é€/e$/'], 'é€a\n'.encode()), (['y/eb/é€/'], 'abe\n'.encode()), (['s/[é€]/x/g'], 'aé€b\n'.encode()), (['s/[^a]/x/g'], 'aé€\n'.encode()),
    (['s/\\(.\\)\\(.\\)/\\2\\1/'], 'é€z\n'.encode()), (['-n', '/^.$/p'], 'é\nab\n€\n'.encode()), (['s/.$/x/'], 'aé\n'.encode()), (['-E', 's/.{2}/x/'], 'é€z\n'.encode()), (['l'], 'é\n'.encode()),
]

# a script made of pieces at random. Jumps go forward only, and D stays away from the hold space: both could go round for ever.
LINES = [b'a', b'b', b'ab', b'aab', b'', b'abc', b'cba', b'x y', b'aaa', b'bb']


def random_address(r):
    k = r.randrange(12)
    if k == 0: return str(r.randrange(1, 7))
    if k == 1: return '$'
    if k == 2: return '/%s/' % r.choice(['a', 'b', 'ab', 'a*', '^a', 'b$', '^$', '.', 'c'])
    if k == 3: return '%d~%d' % (r.randrange(0, 4), r.randrange(1, 4))
    if k == 4: return '%d,%d' % (r.randrange(1, 6), r.randrange(1, 6))
    if k == 5: return '%d,$' % r.randrange(1, 6)
    if k == 6: return '/%s/,/%s/' % (r.choice(['a', 'b', '^$']), r.choice(['a', 'b', 'c']))
    if k == 7: return '%d,/%s/' % (r.randrange(1, 5), r.choice(['a', 'b']))
    if k == 8: return '/%s/,%d' % (r.choice(['a', 'b']), r.randrange(1, 6))
    if k == 9: return '/%s/,+%d' % (r.choice(['a', 'b']), r.randrange(0, 3))
    if k == 10: return '%d,~%d' % (r.randrange(1, 5), r.randrange(0, 4))
    return '0,/%s/' % r.choice(['a', 'b'])


def random_command(r, hold):
    simple = ['p', 'd', 'P', 'n', 'N', '=', 'l', 'x', 'g', 'G', 'h', 'H'] if hold else ['p', 'd', 'D', 'P', 'n', 'N', '=', 'l']
    k = r.randrange(16)
    if k < 6: return r.choice(simple)
    if k < 10:
        pat = r.choice(['a', 'b', 'a*', 'b*', '.', '^', '$', '\\n', 'x*', '[ab]', 'a\\|b', '\\(a\\)\\(b\\)', '\\(a*\\)b'])
        rep = r.choice(['X', '&&', '[&]', '', '\\n', '\\1' if '\\(' in pat else 'Y'])
        return 's/%s/%s/%s' % (pat, rep, r.choice(['', 'g', 'p', '2', '2g', 'gp']))
    if k == 10: return 'y/ab/ba/'
    if k == 11: return r.choice(['a added', 'i inserted', 'c changed', 'r nosuchfile'])
    if k == 12: return r.choice(['q', 'Q', 'q3'])
    if k == 13: return 'bend'
    if k == 14: return r.choice(['tend', 'Tend'])
    return 'wout'


def random_script(r):
    hold = r.randrange(2)
    n = r.randrange(1, 6)
    parts = []
    for i in range(n):
        c = random_command(r, hold)
        if r.randrange(2):
            c = random_address(r) + (r.choice(['', '!']) if r.randrange(3) == 0 else '') + c
        if r.randrange(6) == 0:
            c = '%s{\n%s\n}' % (random_address(r), c)
        parts.append(c)
    parts.append(':end')
    return '\n'.join(parts)


def random_input(r):
    n = r.randrange(0, 8)
    text = b'\n'.join(r.choice(LINES) for i in range(n))
    if n and r.randrange(4):
        text += b'\n'
    return text


def malformed():
    """what no real tool is compared on: it must end by itself, with a status, whatever it is given"""
    global checks
    r = random.Random(4242)
    pieces = ['s', 'y', '/', '\\', '{', '}', ';', '\n', 'a', 'i', 'c', 'b', 't', ':', '!', ',', '$', '1', '0', '~', '+', 'p', 'd', 'n', 'N', '[', ']', '*', '(', ')', '&',
              'w', 'r', 'l', 'q', 'g', 'I', ' ', '#', '=', '\\n', '\\1', '[[:', '\\x', '\\c', '\\d', '\\o', 's/a/b/', '/a/', 'x', 'D', 'G']
    for i in range(ROUNDS * 3):
        script = ''.join(r.choice(pieces) for k in range(r.randrange(1, 14)))
        if 'D' in script or ':' in script or 'G' in script:     # these could go round for ever, as they may
            continue
        checks += 1
        try:
            m = run(SED, ['--', script] if script.startswith('-') else [script], random_input(r), {}, False)
        except subprocess.TimeoutExpired:
            continue
        if m[1] not in range(0, 256):           # a signal
            fail('sed %r: status %d, %r' % (script, m[1], m[3][:100]))
    # a line of a megabyte, a script as long, deep braces, many of them; truncated in the middle of a sequence
    big = b'ab' * 500000
    for args, text in ((['s/a/X/g'], big + b'\n'), (['s/b*$/!/'], big), (['y/ab/ba/'], big + b'\n'), (['-n', 'l'], big[:20000] + b'\n'), (['h;G;G;G'], big + b'\n'),
                       (['-E', 's/(a|b)*/<&>/'], big[:100000] + b'\n'), (['s/\\(.*\\)b/\\1/'], big[:100000] + b'\n')):
        checks += 1
        m = run(SED, args, text, {}, False)
        if SED_KIND == 'gnu':
            r2 = run('sed', args, text, {}, False)
            if r2[0] != m[0]:
                fail('big input %r differs' % (args,))
        elif m[1] != 0:
            fail('big input %r: status %d' % (args, m[1]))
    checks += 1
    deep = '{' * 200 + 'p' + '}' * 200
    m = run(SED, [deep], AB, {}, False)
    if m[1] or m[0].count(b'\n') != 10:
        fail('{ inside { 200 times: status %d' % m[1])
    checks += 1
    m = run(SED, ['{' * 300 + 'p' + '}' * 300], AB, {}, False)
    if m[1] != 2 or not m[3]:
        fail('{ inside { 300 times: status %d' % m[1])
    checks += 1
    long_script = ';'.join('s/a%d/b/' % i for i in range(3000))
    m = run(SED, ['-f', 'scr'], AB, {'scr': long_script.encode()}, False)
    if m[1] or m[0] != AB:
        fail('a long script: status %d' % m[1])
    checks += 1
    m = run(SED, ['s/./x/g'], b'\xe2\x82', {}, False)
    if m[0] != b'xx':
        fail('half a sequence: %r' % m[0])
    checks += 1
    m = run(SED, ['y/\xc3\xa9/e/'.encode('latin1').decode('utf-8', 'surrogateescape')], b'\xc3\xa9\xff\xc3\n', {}, False)
    if m[0] != b'e\xff\xc3\n':
        fail('y and bad bytes: %r' % m[0])
    # a delimiter of more than a byte would cut a character in two
    checks += 1
    m = run(SED, ['s\u00a7a\u00a7b\u00a7'], b'a\n', {}, False)
    if m[1] != 2:
        fail('a delimiter of two bytes: status %d' % m[1])
    # GNU's sed takes an empty match byte by byte, in the middle of a sequence; ours does not
    checks += 1
    m = run(SED, ['s/x*/-/g'], 'é€\n'.encode(), {}, False)
    if m[0] != '-é-€-\n'.encode():
        fail('an empty match beside a sequence: %r' % m[0])
    checks += 1
    m = run(SED, ['s/x*/-/g'], b'\xe2\x82\xff\n', {}, False)
    if m[0] != b'-\xe2-\x82-\xff-\n':
        fail('an empty match beside bad bytes: %r' % m[0])
    # a reader that goes
    checks += 1
    p = subprocess.Popen([SED, 'p'], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    p.stdout.close()
    try:
        p.stdin.write(b'line\n' * 200000)
        p.stdin.close()
    except BrokenPipeError:
        pass
    rc = p.wait(timeout=TIMEOUT)
    if rc not in (141, -13):
        fail('the reader went: status %d' % rc)
    # messages: tool: what: why
    checks += 1
    m = run(SED, ['p', 'nosuch'], b'', {}, False)
    if not m[3].startswith(b'sed: nosuch: '):
        fail('a message: %r' % m[3])
    checks += 1
    m = run(SED, ['p\nk'], b'', {}, False)
    if not m[3].startswith(b'sed: script line 2: '):
        fail('a message: %r' % m[3])


def own_rules():
    """where sed differs from the real one on purpose, or the real one cannot be run on what matters"""
    # #n alone on its line is -n. GNU's 4.9 takes every script that starts with those two characters so, and -i then empties the file
    twice = b'a\na\n'
    for script in ('#no\np', '#n x\np', '#nx\np', '#n \np', '#normalise\np'):
        expect(SED, [script], b'a\n', out=twice)
    expect(SED, ['-e', '#nope', '-e', 'p'], b'a\n', out=twice)
    expect(SED, ['-f', 's'], b'a\n', {'s': b'#normalise\np\n'}, out=twice)
    expect(SED, ['-i', '-f', 's', 'f'], b'', {'s': b'#normalise line ends\ns/x/y/\n', 'f': b'axb\n'}, after={'s': b'#normalise line ends\ns/x/y/\n', 'f': b'ayb\n'})
    # two names for one file are one output. GNU's opens it twice, each with its own place, and one overwrites the other
    expect(SED, ['-n', '1w ./o\n2w o'], AB, after={'o': b'a\nb\n'})
    expect(SED, ['-n', '1w o\n2w ./o\n3w .//o'], AB, after={'o': b'a\nb\nc\n'})
    expect(SED, ['-n', '1w d/../o\n2w o'], AB, {'d/x': b''}, after={'o': b'a\nb\n'})
    expect(SED, ['-n', '1w d/o\n2w d/./o\n3w o'], AB, {'d/x': b''}, after={'o': b'c\n'})     # and d/o is not o
    expect(SED, ['-n', '1w o\n2w ./o'], AB, {'o': b'old\n'}, after={'o': b'a\nb\n'})
    # a name that is the working name of another: one rename would undo the other
    expect(SED, ['-n', '1w o\n2w o.tmp'], AB, status=2, after={}, said=b'o.tmp')
    expect(SED, ['-n', '1w o.tmp\n2w o'], AB, after={'o': b'b\n', 'o.tmp': b'a\n'})
    # with a backup, one file twice would leave the first pass's output in the backup. GNU's does that
    expect(SED, ['-i.bak', 's/$/x/', 'f', './f'], b'', {'f': b'a\n'}, status=2, after={'f': b'a\n'}, said=b'one file')
    expect(SED, ['-i.bak', 's/$/x/', 'f', 'd/../f'], b'', {'f': b'a\n', 'd/x': b''}, status=2, after={'f': b'a\n'})
    expect(SED, ['-i.bak', 's/$/x/', 'f', 'd/f'], b'', {'f': b'a\n', 'd/f': b'b\n'}, after={'f': b'ax\n', 'f.bak': b'a\n'})
    # a suffix that makes the backup the working name. GNU's has no such name
    expect(SED, ['-i.tmp', 's/a/bb/', 'f'], b'', {'f': b'a\n'}, status=2, after={'f': b'a\n'}, said=b'f.tmp')
    # no case conversion: a sentence, not a letter in the file
    for replacement in ('\\U&', '\\L&', '\\u&', '\\l&', '&\\E'):
        expect(SED, ['-i', 's/b/%s/' % replacement, 'f'], b'', {'f': b'abc\n'}, status=2, after={'f': b'abc\n'}, said=b'case conversion')


def undecided():
    """a line on which the search gave up matches no more than it fails to, whatever the options: status 2"""
    big = b'abcdefghij' * 300000 + b'\n'
    pattern = '\\(.\\).*\\1'
    for options in ([], ['-q'], ['-v'], ['-qv'], ['-c'], ['-cv'], ['-l'], ['-lv'], ['-n']):
        expect(GREP, options + [pattern, 'f'], files={'f': big}, status=2, said=b'grep: ')
    expect(GREP, ['-cv', pattern, 'f'], files={'f': big}, status=2, out=b'0\n')
    expect(GREP, [pattern, 'f'], files={'f': big + b'xx\n'}, status=2, out=b'xx\n')    # the lines that were decided still count
    expect(GREP, ['-v', pattern, 'f'], files={'f': big + b'xy\n'}, status=2, out=b'xy\n')
    expect(GREP, ['-q', pattern, 'f'], files={'f': big + b'xx\n'}, status=2)
    expect(GREP, ['-q', pattern, 'f'], files={'f': b'xx\n' + big}, status=0)            # it stopped before that line
    expect(GREP, ['-q', pattern, 'f', 'nosuch'], files={'f': b'xx\n'}, status=0)        # a file not there, as POSIX says for -q


GREP_CASES = [
    (['a'], AB), (['^a'], AB), (['a$'], b'a\nba\nab\n'), (['-i', 'A'], AB), (['-v', 'a'], AB), (['-n', 'c'], AB), (['-c', '[ab]'], AB), (['-l', 'a'], AB), (['-q', 'a'], AB), (['-q', 'x'], AB), (['x'], AB), (['-c', 'x'], AB), (['-F', 'a.b'], b'a.b\naxb\n'),
    (['-E', 'a|b'], AB), (['-E', '(a|b)c'], b'ac\nbc\nc\n'), (['-E', 'a+'], b'aa\nb\n'), (['-E', 'a{2}'], b'aa\na\n'), (['a\\{2\\}'], b'aa\na\n'), (['\\(a\\)\\1'], b'aa\nab\n'), (['-E', '(a)\\1'], b'aa\nab\n'), (['-e', 'a'], AB), (['-e', '-x'], b'-x\ny\n'), (['--', '-x'], b'-x\ny\n'),
    (['-in', 'A'], AB), (['-vc', 'a'], AB), (['-iv', 'A'], AB), (['a'], b'no newline a'), (['-v', 'a'], b'b'), ([''], AB), (['-c', ''], AB), (['-F', ''], AB), (['[[:digit:]]'], b'1\na\n'), (['.*'], b''), (['-c', '.*'], b''), (['^$'], b'\na\n\n'),
    (['a', 'f'], b'', {'f': AB}), (['a', 'f', 'g'], b'', {'f': AB, 'g': b'ba\n'}), (['-n', 'a', 'f', 'g'], b'', {'f': AB, 'g': b'ba\n'}), (['-c', 'a', 'f', 'g'], b'', {'f': AB, 'g': b'b\n'}), (['-l', 'a', 'f', 'g'], b'', {'f': AB, 'g': b'b\n'}),
    (['a', 'nosuch'], b''), (['a', 'f', 'nosuch'], b'', {'f': AB}), (['-q', 'a', 'f', 'nosuch'], b'', {'f': AB}), (['x', 'f', 'nosuch'], b'', {'f': AB}), (['a', '-'], AB),
]
GREP_GNU_ONLY = [
    (['\\w\\+'], b'ab\n--\n'), (['\\bfoo\\b'], b'foo\nfoobar\n'), (['a\\|b'], AB), (['a\\+'], b'aa\nb\n'), (['\\(a\\|b\\)c'], b'ac\nc\n'), (['-E', '(a'], AB), (['-E', 'a{2'], b'a{2\n'), (['-E', '{'], b'{\n'), (['\\('], AB),
]


def main():
    global skipped
    if SED_KIND is None:
        print('test_sed: no sed here: nothing to compare with')
    else:
        for case in COMMON + (GNU_ONLY if SED_KIND == 'gnu' else []):
            args, stdin = case[0], case[1]
            files = case[2] if len(case) > 2 else {}
            compare('sed', SED, args, stdin, files)
        if SED_KIND == 'gnu':
            for args, stdin in UTF8:
                compare('sed', SED, args, stdin, {}, True)
            r = random.Random(SEED)
            for i in range(ROUNDS):
                script, text = random_script(r), random_input(r)
                compare('sed', SED, ['-n', script] if r.randrange(3) == 0 else [script], text)
                if not compare('sed', SED, ['-s', script, 'f', 'g'], b'', {'f': text, 'g': random_input(r)}):
                    skipped += 1
        else:
            print('test_sed: %s sed here: the corpus GNU shares, no random scripts' % SED_KIND)
    malformed()
    if GREP_KIND is None:
        print('test_sed: no grep here: nothing to compare with')
    else:
        for case in GREP_CASES + (GREP_GNU_ONLY if GREP_KIND == 'gnu' else []):
            args, stdin = case[0], case[1]
            files = case[2] if len(case) > 2 else {}
            compare('grep', GREP, args, stdin, files, what='grep')
    own_rules()
    undecided()
    print('test_sed: %d checks, %d failed%s' % (checks, failures, ' (%d scripts did not end in the real sed)' % skipped if skipped else ''))
    return 1 if failures else 0


sys.exit(main())
