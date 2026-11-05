"""The guiapps scenario's checks of the desktop's side of the desktop applications (docs/superpowers/plans/
2026-11-05-desktop-apps.md, track desk), with user/bin/wintest.c's desk mode, which uses the window calls of 11/05/26
bare and says in its title what it got last.

Apps > Run program... starts `wintest desk`: its window opens with the focus, and its menus Test and View stand in the
bar in place of File and Edit. Ctrl+G, Right, Down and Enter choose Test > Checked; a click on View and one on Big
choose View > Big (WIN_EV_MENU); View > Size > Large is a submenu's item, Help > About wintest the program's item in
the desktop's Help (11/05/26, the review). Keys arrive with their modifiers: Ctrl+Shift+Left, Alt+X (no Esc before
it), F5, Ctrl+Shift+S, and since the review Ctrl+Tab (no Ctrl+I) and Ctrl+0. s and o ask for the Save as and Open
choosers (WIN_DIALOG), whose title bar names the window, and which Esc cancels (WIN_EV_DIALOG 0). SurfOS > Ocean / dusk
colors tells it the theme (WIN_EV_THEME) and back. Files mounts the data stick; its Copy path puts a path on the
clipboard, which wintest pastes (WIN_PASTE). Enter on desk/a b.wintest opens it in wintest (the registry's
rootfs/apps/wintest.app: the type wintest), a second window naming the file, path with its space; File > Open with
lists Window test. Print Screen: the Save as chooser, in the stick's folder, saves shot.bmp, which the host reads
back as a 960 x 540, 24-bit BMP. The windows are closed, the stick ejected and the theme ocean again, also after a
failure.
"""
import struct

import gui_scenario as base
from gui_apps_scenario import PAPER, INK, TEAL, MENU_H
from gui_apps_suite import files_front, files_status, files_row, select, EJECT
from gui_apps_win import find, titled_at, run_program, run_titled
from gui_apps_media import dialog_open

STICK = {'desk/a b.wintest': b'a file for wintest\n'}
PARK = (900, 480)
TEST_X, VIEW_X = 8 + 88 + 4, 8 + 88 + 56 + 4       # the program's menus' titles in the bar, after "\xf7 SurfOS"
CHOOSER = (220, 81)                                 # a chooser's top left at 960x540 (shell/guidialog.c's layout)
W = 322                                             # wintest desk's window: 320 wide and its border
FILE_TITLE = 'desk: file /usb1p1/desk/a b.'         # its title with the file, as far as its 29 cells show it


def titled(d, text):
    """a predicate: a program's window at any of the cascade's places titled text, in front"""
    return lambda s: find(d, s, text) is not None and titled_at(d, s, find(d, s, text), text, True)


def chooser(d, title, folder=None):
    """a predicate: the desktop's chooser is up, its title (and its folder)"""
    return lambda s: d.text(s, CHOOSER[0] + 28, CHOOSER[1] + 6, title, PAPER, TEAL) and \
        (folder is None or d.text(s, CHOOSER[0] + 80, CHOOSER[1] + 38, folder, INK))


def files_open(d):
    """Apps > Files by the keyboard (Ctrl+B would be the program's key while its window has the focus): the bar's
    fourth menu is Apps with the desktop's menus and with wintest's (÷ SurfOS, Test, View, Apps)"""
    for chord in ('ctrl-g', 'right', 'right', 'right', 'down', 'ret'):
        d.key(chord)


FILES_MIN = (24 + 600 - 74 + 10, 48 + 14)           # Files' minimize button (gui_init's place at 960x540)


def close_at(d, at):
    """the close button of a 320-wide program's window whose top left is at"""
    d.click(at[0] + W - 16, at[1] + 14)


def tidy(d, state):
    """after a failure: dialogs and menus dismissed, the theme ocean again, wintest's windows closed, the stick out"""
    for _ in range(3):
        if not dialog_open(d, d.vm.screen()):
            break
        d.key('esc')
        d.vm.pump(0.3)
    d.key('esc')
    if state['dusk']:
        for chord in ('ctrl-g', 'down', 'down', 'ret'):
            d.key(chord)
    if state['files'] or state['mounted']:
        files_open(d)
        d.vm.pump(0.5)
        if state['mounted']:
            d.click(*EJECT)
            d.vm.wait_screen(files_status(d, 'usb1: safe to remove'), 15, interval=0.2)
        d.move(*PARK)
        d.key('ctrl-w')
        d.vm.pump(0.5)
    for _ in range(3):                                  # wintest's windows, the one with a file too
        at = find(d, d.vm.screen(), 'desk: ')
        if not at:
            break
        close_at(d, at)
        d.move(*PARK)
        d.vm.pump(1.0)


def checks(d):
    state = {'files': False, 'mounted': False, 'dusk': False}
    done = False
    try:
        d.move(*PARK)
        run_program(d)
        d.visible('desk: Apps > Run program... (its place kept)', lambda s: run_titled(d, s))
        d.keys('wintest desk\n')
        s = d.visible('desk: wintest desk opens with the focus', titled(d, 'desk: ready'), 'guiapps-desk-open.png')
        at = find(d, s, 'desk: ready')
        d.visible('desk: its menus Test and View in the bar, in place of File and Edit',
                  lambda s: d.text(s, TEST_X, 6, 'Test', INK, PAPER) and d.text(s, VIEW_X, 6, 'View', INK, PAPER))
        for chord in ('ctrl-g', 'right', 'down', 'ret'):
            d.key(chord)
        d.visible('desk: Ctrl+G, Right, Down, Enter: Test > Checked, its id and its menu\'s (WIN_EV_MENU)',
                  titled(d, 'desk: menu 12 of 1'))
        d.click(VIEW_X + 4, 14)
        d.visible('desk: a click on View opens it under its title, Big checked as one of a group (and chosen)',
                  lambda s: d.text(s, VIEW_X - 8 + 14, MENU_H + 4 + 4, 'Big', PAPER, TEAL) and
                  d.text(s, VIEW_X - 8 + 4, MENU_H + 4 + 4, '\x07', PAPER), 'guiapps-desk-menu.png')
        d.click(VIEW_X + 20, MENU_H + 4 + 12)
        d.move(*PARK)                                   # the pointer off the title it would cover
        d.visible('desk: a click on Big chooses it', titled(d, 'desk: menu 21 of 2'))
        for chord in ('ctrl-g', 'right', 'right', 'down', 'down', 'right', 'down', 'ret'):
            d.key(chord)
        d.visible('desk: View > Size > Large, an item of a submenu in View\'s place', titled(d, 'desk: menu 24 of 2'))
        for chord in ('ctrl-g', 'left', 'ret'):
            d.key(chord)
        d.visible('desk: Help > About wintest, the program\'s item first in the desktop\'s Help',
                  titled(d, 'desk: menu 31 of 3'))
        for keys, title in (('<ctrl-shift-left>', 'desk: key 0x102 mods 5'), ('<alt-x>', 'desk: key 0x78 mods 2'),
                            ('<f5>', 'desk: key 0x115 mods 0'), ('<ctrl-shift-s>', 'desk: key 0x13 mods 5')):
            d.keys(keys)
            d.visible(f'desk: {keys} reaches the program with its modifiers', titled(d, title))
        for chord, title in (('ctrl-tab', 'desk: key 0x10c mods 4'), ('ctrl-0', 'desk: key 0x30 mods 4'),
                             ('ctrl-ret', 'desk: key 0x10d mods 4')):
            d.key(chord)
            d.visible(f'desk: {chord} reaches the program as itself (the keyboard\'s keys with their modifiers)',
                      titled(d, title))
        d.keys('s')
        d.visible('desk: s asks for Save as: the desktop\'s chooser, its title naming the window',
                  chooser(d, 'desk: asking: Save as'), 'guiapps-desk-save.png')
        d.key('esc')
        d.visible('desk: Esc cancels it: WIN_EV_DIALOG 0, the request\'s id', titled(d, 'desk: cancelled 4'))
        d.keys('o')
        d.visible('desk: o asks for Open', chooser(d, 'desk: asking: Open a file'))
        d.key('esc')
        d.visible('desk: cancelled', titled(d, 'desk: cancelled 3'))
        for chord in ('ctrl-g', 'down', 'down', 'ret'):
            d.key(chord)
        state['dusk'] = True
        d.visible('desk: SurfOS > Ocean / dusk colors: the program hears of dusk (WIN_EV_THEME)', titled(d, 'desk: theme 1'),
                  'guiapps-desk-dusk.png')
        for chord in ('ctrl-g', 'down', 'down', 'ret'):
            d.key(chord)
        d.visible('desk: and of ocean again', titled(d, 'desk: theme 0'))
        state['dusk'] = False
        # the stick, in Files: its folder's path copied, then pasted in wintest
        files_open(d)
        s = d.visible('desk: Files, with the data stick usb1p1 not mounted',
                      lambda s: files_front(d)(s) and files_row(d, s, 'usb1p1', 1) is not None)
        state['files'] = True
        select(d, files_row(d, s, 'usb1p1', 1))
        d.key('ret')
        s = d.visible('desk: Enter mounts the stick, with its desk folder',
                      lambda s: files_status(d, 'usb1p1 on /usb1p1: fat32')(s) and files_row(d, s, 'desk', 2) is not None)
        state['mounted'] = True
        select(d, files_row(d, s, 'desk', 2))
        d.key('ctrl-c')
        d.visible('desk: Files copies the folder\'s path', files_status(d, 'Copied /usb1p1/desk'))
        d.click(*FILES_MIN)                             # Files tucked away: wintest's title bar to click
        d.visible('desk: Files tucked into the taskbar', lambda s: not files_front(d)(s))
        d.click(at[0] + 60, at[1] + 14)
        d.move(*PARK)
        d.visible('desk: a click on its title bar gives wintest the focus', titled(d, 'desk: '))
        d.keys('p')
        d.visible('desk: p pastes the clipboard in the program (WIN_PASTE)', titled(d, 'desk: paste 12 /usb1p1/desk'))
        # Files opens a .wintest file in wintest, the registry's program for its type
        files_open(d)
        d.visible('desk: Files back in front', files_front(d))
        d.key('ret')
        s = d.visible('desk: the desk folder opens', lambda s: files_row(d, s, 'a b.wintest', 3) is not None)
        select(d, files_row(d, s, 'a b.wintest', 3))
        for chord in ('ctrl-g', 'right', 'down', 'right'):
            d.key(chord)
        d.visible('desk: File > Open with lists Window test, the program its type belongs to',
                  lambda s: d.text(s, 8 + 88 - 4 + 14, MENU_H + 4 + 4, 'Window test', PAPER, TEAL), 'guiapps-desk-openwith.png')
        d.key('esc')
        d.key('ret')
        s = d.visible('desk: Enter opens a b.wintest in wintest, started with its path (space and all)',
                      titled(d, FILE_TITLE), 'guiapps-desk-file.png')
        close_at(d, find(d, s, 'desk: file'))
        d.move(*PARK)
        d.visible('desk: its close button ends it', lambda s: find(d, s, 'desk: file') is None)
        # a screenshot, into the stick's folder
        d.key('print')
        d.visible('desk: Print Screen asks where to save the screenshot, in the stick\'s folder',
                  chooser(d, 'Save the screenshot', '/usb1p1'), 'guiapps-desk-shot.png')
        d.keys('shot\n')
        d.visible('desk: saved: the chooser closes', lambda s: not dialog_open(d, s))
        files_open(d)
        d.visible('desk: Files in front again', files_front(d))
        d.click(*EJECT)
        d.visible('desk: Eject in Files flushes the stick', files_status(d, 'usb1: safe to remove'))
        state['mounted'] = False
        d.move(*PARK)
        d.key('ctrl-w')
        d.visible('desk: Files closes', lambda s: not files_front(d)(s))
        state['files'] = False
        s = d.vm.screen()
        close_at(d, find(d, s, 'desk: '))
        d.move(*PARK)
        d.visible('desk: wintest\'s close button ends it', lambda s: find(d, s, 'desk: ') is None)
        done = True
    finally:
        if not done:
            try:
                tidy(d, state)
            except AssertionError:
                pass


def readback(d, stick, helpers):
    """the screenshot read on the host: a 24-bit bottom-up BMP of the desktop's 960 x 540 pixels"""
    try:
        data = helpers['read_fat32'](stick, 'shot.bmp')
    except Exception as e:                              # the checks stopped before it was saved
        d.check(f'desk: the screenshot is on the stick ({e})', False)
        return
    row = 960 * 3
    ok = (len(data) == 54 + row * 540 and data[:2] == b'BM' and
          struct.unpack('<IIIiiHHI', data[2:6] + data[10:14] + data[14:18] + data[18:26] + data[26:30] + data[30:34]) ==
          (len(data), 54, 40, 960, 540, 1, 24, 0))
    d.check('desk: shot.bmp reads back on the host: 960 x 540, 24 bits, bottom-up, 1,555,254 bytes', ok)
    if ok:
        top = data[54 + 539 * row: 54 + 539 * row + 3]  # the top left pixel: the menu bar's paper
        d.check('desk: its top left pixel is the menu bar\'s', top == bytes(reversed(PAPER)))
