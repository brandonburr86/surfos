"""The guiapps scenario's checks of the suite as one desktop (docs/superpowers/plans/2026-10-17-gui-suite.md): Files'
Open takes each kind of file to the application that shows it. They run after the tracks' (gui_apps_scenario.TRACKS)
and use track MEDIA's files on the data stick (usb1, APPSTICK).

Files mounts the stick (Enter on its row) and opens its media folder. Enter on sea24.bmp opens it in the Image Viewer,
and Enter on bytes.bin (a NUL byte in its first bytes: Notepad would refuse it) opens it in Hex; each window's title
names the file. Files then ejects the stick. The desktop is left as it was found, also after a failure: dialogs
dismissed, the windows closed, the stick ejected.
"""
import gui_scenario as base
from gui_apps_media import dialog_open
from gui_apps_scenario import window, PAPER, INK, TEAL, BAND, MENU_H

FILES = (24, 48, 600, 380)                      # gui_init's place for Files at 960x540
STATUS = (FILES[0] + 10, FILES[1] + FILES[3] - 23 + 3)
EJECT = (FILES[0] + 502 + 27, FILES[1] + 32 + 11)
PARK = (900, 480)                               # the pointer out of the way of what is read


def files_front(d):
    return lambda s: d.text(s, *base._title(*FILES[:2]), 'Files', PAPER, TEAL)


def files_status(d, text):
    return lambda s: d.text(s, *STATUS, text, INK, BAND)


def files_row(d, s, name, depth):
    """the row (from the top of Files' list) where name is at depth, or None"""
    for r in range(18):
        if d.text(s, *base._files_row(r, depth), name, INK):
            return r
    return None


def titled(d, name, text):
    """application name's window in front with text as its title, at its first place or maximized"""
    x, y, _, _ = window(name)
    return lambda s: any(d.text(s, *base._title(px, py), text, PAPER, TEAL) for px, py in ((x, y), (0, MENU_H)))


def select(d, row):
    """Files' selection to row, by the keyboard"""
    for chord in ['home'] + ['down'] * row:
        d.key(chord)


def opens(d, rows, name, app, title, shot):
    """Enter on name in Files opens it in app; Ctrl+W closes app, and Ctrl+B brings Files back"""
    select(d, rows[name])
    d.key('ret')
    d.visible(f'suite: Files opens {name} in {app}, its title naming the file', titled(d, app, title), shot)
    d.key('ctrl-w')
    d.key('ctrl-b')
    d.visible(f'suite: {app} closes, and Files is in front again', files_front(d))


def tidy(d, state):
    """after a failure: what the checks opened is closed again, and the stick ejected"""
    for _ in range(3):
        if not dialog_open(d, d.vm.screen()):
            break
        d.key('esc')
        d.vm.pump(0.3)
    for name in ('Hex', 'Image Viewer'):
        d.open_app(name)
        d.vm.pump(0.3)
        d.key('ctrl-w')
    if state['files'] or state['mounted']:
        d.key('ctrl-b')
        d.vm.pump(0.5)
        if state['mounted']:
            d.click(*EJECT)
            d.vm.wait_screen(files_status(d, 'usb1: safe to remove'), 15, interval=0.2)
        d.key('ctrl-w')


def checks(d):
    state = {'files': False, 'mounted': False}
    done = False
    try:
        d.move(*PARK)
        d.key('ctrl-b')
        s = d.visible('suite: Files opens, with the data stick usb1p1 not mounted',
                      lambda s: files_front(d)(s) and files_row(d, s, 'usb1p1', 1) is not None)
        state['files'] = True
        select(d, files_row(d, s, 'usb1p1', 1))
        d.key('ret')
        s = d.visible('suite: Enter on the stick mounts it and opens it, with its media folder',
                      lambda s: files_status(d, 'usb1p1 on /usb1p1: fat32')(s) and
                      files_row(d, s, 'media', 2) is not None)
        state['mounted'] = True
        select(d, files_row(d, s, 'media', 2))
        d.key('ret')
        s = d.visible('suite: Enter opens the media folder',
                      lambda s: all(files_row(d, s, name, 3) is not None for name in ('bytes.bin', 'sea24.bmp')))
        rows = {name: files_row(d, s, name, 3) for name in ('bytes.bin', 'sea24.bmp')}
        opens(d, rows, 'sea24.bmp', 'Image Viewer', 'sea24.bmp - Image Viewer', 'guiapps-suite-viewer.png')
        opens(d, rows, 'bytes.bin', 'Hex', 'bytes.bin - Hex', 'guiapps-suite-hex.png')
        d.click(*EJECT)
        d.visible('suite: Eject in Files flushes the stick', files_status(d, 'usb1: safe to remove'))
        state['mounted'] = False
        d.move(*PARK)
        d.key('ctrl-w')
        d.visible('suite: Files closes', lambda s: not files_front(d)(s))
        state['files'] = False
        done = True
    finally:
        if not done:
            try:
                tidy(d, state)
            except AssertionError:
                pass
