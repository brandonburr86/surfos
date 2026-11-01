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

# The command-line suite's tools that are on other tracks' branches until the integration. A name here may lack a page and
# may be named under SEE ALSO. The integrator removes this list once every track is merged: everything in SUITE must then
# have its page, and this test says so.
NOT_HERE_YET = {
    'sed', 'awk', 'diff', 'patch', 'cmp', 'tar', 'gzip', 'gunzip', 'zcat', 'unzip', 'fsck', 'telnet', 'ftp', 'basename',
    'dirname', 'od', 'base64', 'strings', 'file', 'paste', 'join', 'comm', 'fold', 'nl', 'rev', 'cal', 'tree', 'bc',
}
# the brief's tools, which must have pages in the end whether or not the sources above name them yet
SUITE = NOT_HERE_YET | {'top', 'watch', 'man'}

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
