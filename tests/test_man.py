"""Tests of the manual, rootfs/man/ (11/01/26, the command-line suite, track sys): every name has a page, and every page has
the brief's shape.

Run from the repository: python3 -m unittest discover -s tests -p test_man.py   (make unittest)

The names come from where they are defined, not from a list here: the programs from the Makefile's USER_PROGS, the kernel's
commands from the table in shell/main.c, surfsh's own from the table in user/surfsh/builtins.c. A name has a page of its own,
or one through the table in user/bin/manutil.h, which is what man itself reads. Nothing here boots anything.
"""
import re
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
PAGES = REPO / 'rootfs' / 'man'
WIDTH = 72

# The brief's tools, which must have pages in the end whether or not the sources above name them yet.
SUITE = {
    'sed', 'awk', 'diff', 'patch', 'cmp', 'tar', 'gzip', 'gunzip', 'zcat', 'unzip', 'fsck', 'top', 'man', 'telnet', 'ftp',
    'basename', 'dirname', 'od', 'base64', 'strings', 'file', 'paste', 'join', 'comm', 'fold', 'nl', 'rev', 'cal', 'tree',
    'watch', 'bc',
}
# Those of them that are on other tracks' branches until the integration. A name here may lack a page and may be named
# under SEE ALSO. The integrator removes this list once every track is merged: everything in SUITE must then have its page,
# and this test says so.
NOT_HERE_YET = {
    'sed', 'awk', 'diff', 'patch', 'cmp', 'tar', 'gzip', 'gunzip', 'zcat', 'unzip', 'fsck', 'telnet', 'ftp', 'basename',
    'dirname', 'od', 'base64', 'strings', 'file', 'paste', 'join', 'comm', 'fold', 'nl', 'rev', 'cal', 'tree', 'bc',
}

# the brief's sections, which come in this order on any page that has them. A page may have others among them (KEYS,
# COMMANDS, a sed's ADDRESSES): a heading is any line of capitals at the left margin.
SECTIONS = ['SYNOPSIS', 'DESCRIPTION', 'OPTIONS', 'EXAMPLES', 'SEE ALSO']
HEADING = re.compile(r'^[A-Z][A-Z ]*$')


def programs():
    text = (REPO / 'Makefile').read_text(encoding='utf-8')
    names = set()
    for m in re.finditer(r'^USER_PROGS\s*\+?=\s*((?:[^#\n\\]*\\\n)*[^#\n]*)', text, re.M):
        names |= set(m.group(1).replace('\\', ' ').split())
    return names


def kernel_commands():
    text = (REPO / 'shell' / 'main.c').read_text(encoding='utf-8')
    table = text[text.index('static const struct command commands[] = {'):]
    table = table[:table.index('\n};')]
    return set(re.findall(r'^\s*\{ "([^"]+)",', table, re.M))


def builtins():
    text = (REPO / 'user' / 'surfsh' / 'builtins.c').read_text(encoding='utf-8')
    table = text[text.index('const struct builtin builtins[] = {'):]
    table = table[:table.index('\n};')]
    return set(re.findall(r'^\s*\{ "([^"]+)",', table, re.M))


def aliases():
    text = (REPO / 'user' / 'bin' / 'manutil.h').read_text(encoding='utf-8')
    table = text[text.index('man_aliases[] = {'):]
    table = table[:table.index('\n};')]
    return dict(re.findall(r'\{ "([^"]+)", "([^"]+)" \}', table))


def page_names():
    return {p.stem for p in PAGES.glob('*.txt')}


def resolves(name, pages, table):
    return name in pages or table.get(name) in pages


class Sources(unittest.TestCase):
    """the readers above, on what must be there: a table they misread must not pass quietly with nothing to check"""

    def test_each_table_is_read_whole(self):
        self.assertTrue({'hello', 'telnetd', 'nettest', 'grep', 'sha256sum', 'wundertow', 'top', 'watch', 'man'} <= programs())
        self.assertTrue({'help', 'clear', 'lsblk', 'crashstack', 'reboot', 'poweroff'} <= kernel_commands())
        self.assertTrue({'cd', 'kill', 'test', '[', ':', '.', 'source', 'jobs'} <= builtins())
        self.assertGreater(len(programs()), 55)
        self.assertGreater(len(kernel_commands()), 80)
        self.assertGreater(len(builtins()), 30)
        self.assertEqual(aliases()['crashdiv'], 'crash')


class EveryName(unittest.TestCase):
    def setUp(self):
        self.pages = page_names()
        self.table = aliases()

    def missing(self, names):
        return sorted(n for n in names if n not in NOT_HERE_YET and not resolves(n, self.pages, self.table))

    def test_every_program(self):
        self.assertEqual(self.missing(programs()), [])

    def test_every_kernel_command(self):
        self.assertEqual(self.missing(kernel_commands()), [])

    def test_every_surfsh_builtin(self):
        self.assertEqual(self.missing(builtins()), [])

    def test_every_tool_of_the_suite(self):
        self.assertEqual(self.missing(SUITE), [])

    def test_the_tables_page_exists(self):
        for name, page in self.table.items():
            self.assertTrue(page in self.pages or name in NOT_HERE_YET, f'{name} points at {page}, which has no page')

    def test_the_table_names_only_what_exists(self):
        real = programs() | kernel_commands() | builtins() | SUITE
        self.assertEqual(sorted(set(self.table) - real), [])

    def test_a_page_is_for_something(self):
        real = programs() | kernel_commands() | builtins() | SUITE
        shared = set(self.table.values())
        self.assertEqual(sorted(self.pages - real - shared), [])

    def test_the_list_of_tools_elsewhere_is_true(self):
        """a name that came here by a merge has to leave the list, or a page missing later would pass"""
        here = programs() | kernel_commands() | builtins()
        for name in sorted(NOT_HERE_YET & here):
            self.assertTrue(resolves(name, self.pages, self.table), f'{name} is here now and has no page')


class TheSuite(unittest.TestCase):
    """the review of 11/01/26: SUITE was NOT_HERE_YET and three names, and those three were all it ever checked: with the
    list gone it would have held only them, so a tool with no page would have passed"""

    def test_it_is_the_briefs_list(self):
        brief = (REPO / 'docs' / 'superpowers' / 'plans' / '2026-11-01-cli-suite.md').read_text(encoding='utf-8')
        scope = brief[brief.index('**In scope:**'):brief.index('`grep -E`')]
        self.assertEqual(sorted(set(re.findall(r'`([a-z0-9]+)`', scope))), sorted(SUITE))

    def test_the_list_is_part_of_it(self):
        self.assertLessEqual(NOT_HERE_YET, SUITE)

    def test_with_the_list_gone_a_missing_page_fails(self):
        """this file as the integrator will leave it: the list empty, and every other tool's page there but tar's and fold's"""
        source = Path(__file__).read_text(encoding='utf-8')
        emptied = re.sub(r'^NOT_HERE_YET = \{.*?^\}', 'NOT_HERE_YET = set()', source, flags=re.M | re.S)
        self.assertNotEqual(emptied, source)
        later = {'__name__': 'test_man_later', '__file__': __file__}
        exec(compile(emptied, 'test_man_later', 'exec'), later)
        check = later['EveryName']('test_every_tool_of_the_suite')
        check.setUp()
        check.pages = set(later['SUITE']) - {'tar', 'fold'}
        self.assertEqual(check.missing(later['SUITE']), ['fold', 'tar'])
        with self.assertRaises(AssertionError):
            check.test_every_tool_of_the_suite()
        check.pages = set(later['SUITE'])
        check.test_every_tool_of_the_suite()


class Facts(unittest.TestCase):
    """what a review found the pages had wrong, kept right: each is a fact of the code, which the words must say"""

    def says(self, page, *words):
        text = ' '.join((PAGES / (page + '.txt')).read_text(encoding='utf-8').split())
        for w in words:
            self.assertIn(w, text, page)

    def never_says(self, page, *words):
        text = ' '.join((PAGES / (page + '.txt')).read_text(encoding='utf-8').split())
        for w in words:
            self.assertNotIn(w, text, page)

    def test_which_surfsh_has_no_job_control(self):
        """may_take_terminal(), kernel/syscall.c: one the ring-0 shell started has it; audit T14"""
        for page in ('jobs', 'run'):
            self.never_says(page, 'A surfsh that run started in the ring-0 shell has no job control',
                            'A surfsh that run starts has no job control')
            self.says(page, 'run surfsh at a surfsh prompt', 'typed at the ring-0 shell')

    def test_the_last_stage_of_a_pipeline_is_this_shell(self):
        """exec_pipe's here, user/surfsh/exec.c"""
        self.says('surfsh', 'echo a | read x; echo $x prints a')

    def test_what_q_cannot_reach(self):
        """kernel/process.c: what a kernel command starts is in no job"""
        self.says('watch', 'in no job', 'run memhog hold 100000')

    def test_the_end_of_a_run_ends_its_jobs(self):
        self.says('watch', 'in the background end with it')

    def test_a_full_screen_program_in_the_background(self):
        self.says('top', 'draws over the prompt')
        self.says('watch', 'draws over the prompt')


class Shape(unittest.TestCase):
    def setUp(self):
        self.table = aliases()
        self.pages = page_names()
        self.files = sorted(PAGES.glob('*.txt'))
        self.assertGreater(len(self.files), 100)

    def each(self, check):
        for path in self.files:
            with self.subTest(page=path.name):
                check(path, path.read_bytes().decode('utf-8'))

    def test_the_name_may_be_a_file_of_the_initrd(self):
        self.each(lambda path, text: self.assertRegex(path.stem, r'^[A-Za-z0-9][A-Za-z0-9_+-]{0,63}$'))

    def test_the_first_line(self):
        def check(path, text):
            first = text.split('\n', 1)[0]
            self.assertTrue(first.startswith(path.stem + ' - '), first)
            self.assertGreater(len(first), len(path.stem) + 3 + 3)
        self.each(check)

    def test_lines(self):
        def check(path, text):
            self.assertTrue(text.endswith('\n'))
            self.assertFalse(text.endswith('\n\n'))
            for i, line in enumerate(text.split('\n')):
                self.assertLessEqual(len(line), WIDTH, f'line {i + 1}')
                self.assertEqual(line, line.rstrip(), f'line {i + 1}')
                self.assertFalse(any(ord(c) < 32 or 0x7f <= ord(c) < 0xa0 for c in line), f'line {i + 1}: a control character')
        self.each(check)

    def test_sections(self):
        def check(path, text):
            lines = text.split('\n')
            found = [l for l in lines[1:] if l and not l.startswith(' ')]
            for name in found:
                self.assertRegex(name, HEADING)
            self.assertIn('DESCRIPTION', found)
            known = [n for n in found if n in SECTIONS]
            self.assertEqual(known, sorted(known, key=SECTIONS.index), 'out of order')
            self.assertEqual(len(found), len(set(found)), 'twice')
            if 'SEE ALSO' in found:
                self.assertEqual(found[-1], 'SEE ALSO')
            for i, line in enumerate(lines):
                if line and not line.startswith(' ') and i:
                    self.assertEqual(lines[i - 1], '', line)
                    self.assertTrue(lines[i + 1].startswith('    '), line)
        self.each(check)

    def test_see_also_leads_somewhere(self):
        def check(path, text):
            lines = text.split('\n')
            if 'SEE ALSO' not in lines:
                return
            words = ' '.join(lines[lines.index('SEE ALSO') + 1:]).split(',')
            for word in (w.strip() for w in words if w.strip()):
                self.assertTrue(resolves(word, self.pages, self.table) or word in NOT_HERE_YET, word)
                self.assertNotEqual(word, path.stem)
        self.each(check)

    def test_synopsis_names_the_command(self):
        """some line of a synopsis is about what the page is called, or a name that shares it"""
        def check(path, text):
            lines = text.split('\n')
            if 'SYNOPSIS' not in lines:
                return
            names = {path.stem} | {n for n, p in self.table.items() if p == path.stem}
            synopsis = []
            for line in lines[lines.index('SYNOPSIS') + 1:]:
                if not line:
                    break
                synopsis.append(re.split(r'[\s|]', line.strip())[0])
            self.assertTrue(names & set(synopsis), synopsis)
        self.each(check)


if __name__ == '__main__':
    unittest.main()
