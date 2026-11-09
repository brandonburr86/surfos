"""The guiapps scenario's checks of Chat (wchat, user/wchat/; the desktop applications, track chat:
docs/superpowers/plans/2026-11-05-desktop-apps.md), started in the Terminal, with no network: the guiapps scenario has
none, and holds the kernel's heap to the byte (tools/gui_apps_chatnet.py has a server, in qemu-usb.py's guichat boot).
wchat's window comes up without the focus on its connect form: its title, the form's first line, Server:, the status
bar's Not connected. A click on its title bar gives it the focus, and the server field, which has the toolkit's focus,
its teal border. Enter with no server says so in the form, in coral, and starts nothing. A server typed, TLS turned off
by a click (the port follows: 6667), and Enter: the window shows the rooms, the server's room says it is connecting,
and the helper it started, finding no network, says why in coral; the status bar says Not connected. Ctrl+N, the Chat
menu's Connect, brings the form back with the server in it. The close button closes the window at once, since nothing
is connected, and ps finds no wchat, the helper included. The checks leave the desktop as they found it, also when one
fails."""
import os

import gui_apps_term as term
from gui_apps_scenario import PAPER, INK, TEAL, MUTED, BAND
from gui_apps_win import answered

W, H = 760, 440                     # wchat's content (user/wchat/window.h)
TITLE_H, FRAME_W, FRAME_H = 28, 2, 29
CORAL = bytes.fromhex('ee906d')
SHEET = bytes.fromhex('fffdf5')
PARK = (900, 480)                   # the pointer clear of the windows and of the Terminal's text
HOST = (112, 55)                    # the server field's top left, in the content
TLS = (256, 85)                     # the TLS check box's
FORM_ERR = (16, 238)                # the form's message
STATUS = (10, H - 22 + 3)           # the status bar's text
ROW0 = 61                           # the messages' first row (the tab strip, the topic line, a line)
TEXT_X = 4 + 6 * 8                  # a line's start, after its time; the Chat's own lines' text after its mark and a space


def places(h=H):
    """where shell/guiclient.c opens the desktop's k-th program window, kept on a 960x540 screen (gui_place(): a window
    760 wide stops at x 198 from the seventh place on)"""
    for k in range(8):
        yield min(72 + 24 * k, 960 - (W + FRAME_W)), min(52 + 20 * k, 540 - 26 - 2 - (h + FRAME_H))


def titled_at(d, s, at, focused=None, title='Chat'):
    bars = (TEAL, MUTED) if focused is None else (TEAL,) if focused else (MUTED,)
    return any(d.text(s, at[0] + 30, at[1] + 6, title, PAPER, bar) for bar in bars)


def find(d, s, focused=None):
    for at in places():
        if titled_at(d, s, at, focused):
            return at
    return None


class Win:
    """wchat's window at at: its content's top left"""
    def __init__(self, d, at):
        self.d, self.at = d, at
        self.x, self.y = at[0] + 1, at[1] + TITLE_H

    def text(self, s, x, y, text, fg, bg=None):
        return self.d.text(s, self.x + x, self.y + y, text, fg, bg)

    def pixel(self, s, x, y):
        return self.d.pixel(s, self.x + x, self.y + y)

    def click(self, x, y):
        self.d.click(self.x + x, self.y + y)

    def status(self, text):
        """a predicate: the status bar's text, from its start"""
        return lambda s: self.text(s, *STATUS, text, INK, BAND)

    def close(self):
        self.d.click(self.at[0] + W + FRAME_W - 16, self.at[1] + 14)


def checks(d):
    grid = term.Grid()
    state = {}
    try:
        d.move(*PARK)
        d.key('ctrl-t')
        d.visible('chat: Ctrl+T opens the Terminal, for wchat', d.title('Terminal'))
        d.require('chat: the Terminal shows its prompt', term.wait_rows(d, grid, term.prompted, 30) is not None)
        d.keys('wchat &\n')
        s = d.visible('chat: wchat opens its window, without the focus: Chat', lambda s: find(d, s, False) is not None,
                      timeout=20)
        state['open'] = True
        w = Win(d, find(d, s))
        state['win'] = w
        d.require('chat: its connect form, its first line and Server:, and Not connected in the status bar',
                  w.text(s, 16, 19, 'Connect to an IRC server', INK, PAPER) and w.text(s, 16, 58, 'Server:', INK, PAPER)
                  and w.status('Not connected')(s))
        if d.outdir:
            s.png(os.path.join(d.outdir, 'guiapps-chat.png'))
        d.click(w.at[0] + 250, w.at[1] + 14)            # its title bar: the focus
        d.visible('chat: a click on its title bar gives it the focus; the server field, the toolkit\'s focus, is lit',
                  lambda s: titled_at(d, s, w.at, True) and w.pixel(s, *HOST) == TEAL)
        d.move(*PARK)
        d.key('ret')
        d.visible('chat: Enter with no server says so in the form, in coral',
                  lambda s: w.text(s, *FORM_ERR, 'Type the server\'s name', CORAL, PAPER), 'guiapps-chat-form.png')
        d.keys('irc.nowhere.test')
        w.click(TLS[0] + 5, TLS[1] + 11)                # TLS off: the port follows
        d.visible('chat: a click turns TLS off, and the port is 6667',
                  lambda s: w.text(s, HOST[0] + 4, TLS[1] + 3, '6667', INK))
        w.click(HOST[0] + 300, HOST[1] + 11)
        d.move(*PARK)
        d.key('ret')
        d.visible('chat: Enter connects: the rooms show, and the server\'s room says it is connecting',
                  lambda s: w.text(s, TEXT_X + 16, ROW0, 'Connecting to irc.nowhere.test, port 6667', MUTED, SHEET))
        d.visible('chat: its helper finds no network, and the room says why, in coral; the status bar Not connected',
                  lambda s: any(w.text(s, TEXT_X + 16, ROW0 + 16 * k, 'Not connected: ', CORAL, SHEET) for k in (1, 2)) and
                  w.status('Not connected')(s), 'guiapps-chat-no-network.png', timeout=30)
        d.key('ctrl-n')
        d.visible('chat: Ctrl+N, the Chat menu\'s Connect, brings the form back with the server in it',
                  lambda s: w.text(s, 16, 19, 'Connect to an IRC server', INK, PAPER) and
                  w.text(s, HOST[0] + 4, HOST[1] + 3, 'irc.nowhere.test', INK))
        w.close()
        d.visible('chat: with nothing connected the close button closes it at once', lambda s: find(d, s) is None,
                  timeout=15)
        state['open'] = False
        d.move(*PARK)
        d.keys('wait; ps | grep -c wchat\n')         # wait: the job's end, which comes just after its window's
        d.require('chat: ps finds no wchat, its helper included', term.wait_rows(
            d, grid, lambda rows: answered(rows, 'wait; ps | grep -c wchat', '0')) is not None)
        state['done'] = True
    finally:
        if not state.get('done') and d.outdir:         # the screen as the failure left it, before the tidying
            d.vm.screen().png(os.path.join(d.outdir, 'guiapps-chat-at-failure.png'))
        try:
            tidy(d, state)
        except AssertionError:                          # what failed first is what the results say
            pass


def tidy(d, state):
    """wchat ended (its close button, Close when it asks, or End when it does not answer), then the Terminal closed"""
    if state.get('open'):
        at = find(d, d.vm.screen())
        if at is not None:
            d.click(at[0] + W + FRAME_W - 16, at[1] + 14)
            if d.vm.wait_screen(lambda s: find(d, s) is None, 3, interval=0.2) is None:
                d.key('ret')                            # its question: Close
                if d.vm.wait_screen(lambda s: find(d, s) is None, 8, interval=0.2) is None and \
                        term.dialog_up(d, d.vm.screen(), 'End the program?'):
                    d.key('right')
                    d.key('ret')
    term.close_terminal(d)
