"""The guiapps scenario's checks of Settings (user/wsettings/; the desktop applications, track settings:
docs/superpowers/plans/2026-11-05-desktop-apps.md), started in the Terminal; the Driver is gui_apps_scenario.py's.
The guiapps boot has no network and no simulated Wi-Fi, and was started from the flashed USB image, whose settings
SurfOS only reads: so these are the checks of what such a boot has. Settings' window comes up with its Wi-Fi page:
the tab lit, "No Wi-Fi card", Ready in its band; a click on its title bar gives it the focus. F6 shows the Network page
with the host name the settings give (surfos); F8 the System page, where telnetd at startup asks first with the
settings command's warning, every line of it shown, and Esc keeps it off; the history switch goes through SYS_netctl
to the network manager, which keeps it in force without saving it on this stick, as the band says, and back. F7 shows
the Desktop page with desk's keys: Dusk is in force at once and turns the desktop's colours dusk within a second (the
desktop reads the settings each second), and is written once the steps stop; Ocean likewise back. F9 shows About
(SurfOS 1.0). Ctrl+W closes the window, surfsh tells the program is done, and ps finds no wsettings. Then
Apps > Programs lists Settings (rootfs/apps/wsettings.app), found by its title among the registry's programs, and
Enter starts it, in front; its close button ends it. The checks leave the desktop as they found it (the
theme ocean, history off, Settings ended, the Terminal closed), also when one fails."""
import os

import gui_apps_term as term
from gui_apps_scenario import PAPER, INK, TEAL, MUTED, BAND, OCEAN
from gui_apps_win import answered

W, H = 600, 400                     # Settings' content (user/wsettings/window.h: WSW_W, WSW_H)
TITLE_H, FRAME_W, FRAME_H = 28, 2, 29
DUSK = bytes.fromhex('142638')      # the desktop behind the windows at dusk (shell/guicore.c's gui_theme_colors())
DESKTOP_AT = (900, 400)             # a point of the desktop clear of the Terminal, Settings and the waves
PARK = (900, 480)
TABS = {'Wi-Fi': 8, 'Network': 66, 'Desktop': 140, 'System': 214, 'About': 280}   # each tab's left in the strip
BAND_AT = (10, H - 40 + 4)          # the band's first line (T_MSG: 40 high at the bottom, its text 10 in)
STATION_AT = (130, 38 + 2)          # the Wi-Fi page's line about the card (T_STATION)
TELNETD_SWITCH, HISTORY_SWITCH = (10 + 10, 40 + 11), (10 + 10, 127 + 11)        # the System page's switches
PANEL_TITLE, PANEL_TEXT = (16, 16 + 3), (16, 46 + 2)                            # the question panel's
OCEAN_OPTION, DUSK_OPTION = (156 + 8, 43 + 6), (232 + 8, 43 + 6)                # the Desktop page's theme options
HOST_AT = (138 + 4, 299 + 3)        # the Network page's host name field's text
VERSION_AT = (140, 42 + 3)          # About's first value
WARNING = ('telnetd has no password and no encryption: whoever reaches the laptop',     # as the panel wraps it at 600
           'over a network SurfOS is on gets a shell. It starts only once there is',
           'an address on Ethernet or a WPA2 network, and while it runs SurfOS',
           'joins no open Wi-Fi network by itself. But SurfOS cannot tell whose',
           'network it is: a cafe\'s WPA2 network, whose password others know,',
           'counts too. Save only your own networks.')


def places(h=H):
    """where shell/guiclient.c opens the desktop's k-th program window, kept on a 960x540 screen (gui_place())"""
    for k in range(8):
        yield 72 + 24 * k, min(52 + 20 * k, 540 - 26 - 2 - (h + FRAME_H))


def titled_at(d, s, at, title, focused=None):
    bars = (TEAL, MUTED) if focused is None else (TEAL,) if focused else (MUTED,)
    return any(d.text(s, at[0] + 30, at[1] + 6, title, PAPER, bar) for bar in bars)


def find(d, s, title='Settings', focused=None):
    for at in places():
        if titled_at(d, s, at, title, focused):
            return at
    return None


class Win:
    """Settings' window at at: its content's top left"""
    def __init__(self, d, at):
        self.d, self.at = d, at
        self.x, self.y = at[0] + 1, at[1] + TITLE_H

    def text(self, s, x, y, text, fg, bg=None):
        return self.d.text(s, self.x + x, self.y + y, text, fg, bg)

    def pixel(self, s, x, y):
        return self.d.pixel(s, self.x + x, self.y + y)

    def click(self, x, y):
        self.d.click(self.x + x, self.y + y)

    def band(self, text):
        """a predicate: the band at the window's bottom says text (its first line, from its start)"""
        return lambda s: self.text(s, BAND_AT[0], BAND_AT[1], text, INK, BAND)

    def titled(self, title):
        return lambda s: titled_at(self.d, s, self.at, title, True)


def chosen_left(d, s, y=28 + 4 + 12):
    """the left of a menu's chosen row on the row y crosses (a menu box's ink border, its paper, then the row's teal, 3
    pixels in), or None: a teal title bar behind the menu has no border before it"""
    for x in range(100, 900):
        if d.pixel(s, x, y) == INK and d.pixel(s, x + 3, y) == TEAL:
            return x + 3
    return None


def programs_settings(d):
    """Apps > Programs by the keyboard, then down its list until the chosen row is Settings: Enter. False when it is not
    there. The Programs menu stands in the Apps menu's place, its rows 24 high from the bar's bottom, the chosen one
    teal (shell/guicore.c's menu_render())"""
    d.key('ctrl-g')
    for _ in range(3):
        d.key('right')
    for _ in range(14):                         # Notepad .. Clear sketch, the suite's eight, Run program..., Programs
        d.key('down')
    d.key('right')
    s = d.vm.wait_screen(lambda s: chosen_left(d, s) is not None, 5, interval=0.1)
    if s is None:
        return False
    left = chosen_left(d, s)
    for row in range(32):
        y = 28 + 4 + 24 * row
        s = d.vm.wait_screen(lambda s: d.pixel(s, left + 1, y + 12) == TEAL, 3, interval=0.1)
        if s is None:
            return False
        if d.text(s, left - 3 + 14, y + 4, 'Settings', PAPER, TEAL):
            d.key('ret')
            return True
        d.key('down')
    return False


def checks(d):
    grid = term.Grid()
    state = {}
    try:
        d.move(*PARK)
        d.key('ctrl-t')
        d.visible('settings: Ctrl+T opens the Terminal, for wsettings', d.title('Terminal'))
        d.require('settings: the Terminal shows its prompt', term.wait_rows(d, grid, term.prompted, 30) is not None)
        d.keys('wsettings &\n')
        s = d.visible('settings: wsettings opens its window: Settings, its Wi-Fi page',
                      lambda s: find(d, s, 'Settings - Wi-Fi') is not None, timeout=20)
        state['open'] = True
        w = Win(d, find(d, s, 'Settings - Wi-Fi'))
        state['win'] = w
        d.require('settings: its Wi-Fi tab lit, "No Wi-Fi card" (this boot has none), Ready in its band',
                  w.text(s, TABS['Wi-Fi'] + 7, 7, 'Wi-Fi', PAPER, TEAL) and
                  w.text(s, TABS['Network'] + 7, 7, 'Network', INK) and
                  w.text(s, STATION_AT[0], STATION_AT[1], 'No Wi-Fi card (no driver found one)', INK, PAPER) and
                  w.band('Ready')(s))
        if d.outdir:
            s.png(os.path.join(d.outdir, 'guiapps-settings.png'))
        d.click(w.at[0] + 300, w.at[1] + 14)           # its title bar: the focus
        d.visible('settings: a click on its title bar gives it the focus', w.titled('Settings - Wi-Fi'))
        d.move(*PARK)
        d.key('f6')
        d.visible('settings: F6, View > Network: the Network page, the host name from the settings in its field',
                  lambda s: w.titled('Settings - Network')(s) and w.text(s, 10, 78, 'Interfaces', INK, PAPER) and
                  w.text(s, HOST_AT[0], HOST_AT[1], 'surfos', INK), 'guiapps-settings-network.png')
        d.key('f8')
        d.visible('settings: F8: the System page', lambda s: w.titled('Settings - System')(s) and
                  w.text(s, 42, 43, 'Start the telnet server at startup (telnetd)', INK, PAPER))
        w.click(*TELNETD_SWITCH)
        d.move(*PARK)                                   # off the panel's text, which opens where the switch was
        d.visible('settings: telnetd at startup asks first, with the settings command\'s warning, every line of it',
                  lambda s: w.text(s, PANEL_TITLE[0], PANEL_TITLE[1], 'The telnet server at startup', INK, PAPER) and
                  all(w.text(s, PANEL_TEXT[0], PANEL_TEXT[1] + 16 * i, line, INK, PAPER)
                      for i, line in enumerate(WARNING)), 'guiapps-settings-telnetd.png')
        d.key('esc')
        d.visible('settings: Esc answers Cancel: the System page again, telnetd still off, nothing changed',
                  lambda s: w.text(s, 42, 43, 'Start the telnet server', INK, PAPER) and
                  w.band('Ready')(s))
        w.click(*HISTORY_SWITCH)
        state['history'] = True
        d.visible('settings: history on goes to the network manager, which keeps it in force but cannot save it on this '
                  'stick, as the band says', w.band('history = on is in force until SurfOS restarts'),
                  'guiapps-settings-history.png')
        w.click(*HISTORY_SWITCH)
        d.visible('settings: and off again', w.band('history = off is in force until SurfOS restarts'))
        state['history'] = False
        d.move(*PARK)
        d.key('f7')
        d.visible('settings: F7: the Desktop page, desk\'s keys as choices, Ocean chosen',
                  lambda s: w.titled('Settings - Desktop')(s) and w.text(s, 10, 40 + 3, 'Theme:', INK, PAPER) and
                  w.text(s, 156, 43, 'Ocean', INK, PAPER) and w.text(s, 232, 43, 'Dusk', INK, PAPER) and
                  d.pixel(s, *DESKTOP_AT) == OCEAN)
        w.click(*DUSK_OPTION)
        state['dusk'] = True
        d.visible('settings: Dusk: in force at once (nothing written yet), and the desktop, which reads it each second, '
                  'turns dusk', lambda s: d.pixel(s, *DESKTOP_AT) == DUSK and
                  w.band('desktop.theme = dusk: in force now')(s), 'guiapps-settings-dusk.png')
        d.visible('settings: once the steps stop it is written, through the network manager, which keeps it in force but '
                  'cannot save it on this stick', w.band('desktop.theme = dusk is in force until SurfOS restarts'))
        w.click(*OCEAN_OPTION)
        d.visible('settings: Ocean: and back', lambda s: d.pixel(s, *DESKTOP_AT) == OCEAN and
                  w.band('desktop.theme = ocean: in force now')(s))
        d.visible('settings: and written once', w.band('desktop.theme = ocean is in force until SurfOS restarts'))
        state['dusk'] = False
        d.move(*PARK)
        d.key('f9')
        d.visible('settings: F9: About, the version first', lambda s: w.titled('Settings - About')(s) and
                  w.text(s, VERSION_AT[0], VERSION_AT[1], 'SurfOS 1.0', INK, PAPER) and
                  w.text(s, 12, VERSION_AT[1], 'Version', MUTED, PAPER), 'guiapps-settings-about.png')
        d.key('ctrl-w')
        d.visible('settings: Ctrl+W, File > Close: its window goes', lambda s: find(d, s) is None, timeout=15)
        state['open'] = False
        for _ in range(10):         # its window goes as it ends: surfsh collects it and tells so at a prompt after that
            d.keys('\n')
            if term.wait_rows(d, grid, lambda rows: term.then_prompt(rows, 'done  wsettings'), 2) is not None:
                break
        d.require('settings: surfsh tells that wsettings is done (and collects it)', term.wait_rows(
            d, grid, lambda rows: term.then_prompt(rows, 'done  wsettings'), 2) is not None)
        d.keys('ps | grep -c wsettings\n')
        d.require('settings: ps finds no wsettings', term.wait_rows(
            d, grid, lambda rows: answered(rows, 'ps | grep -c wsettings', '0')) is not None)
        # the registry: Apps > Programs > Settings starts it, in front
        d.require('settings: Apps > Programs lists Settings (rootfs/apps/wsettings.app)', programs_settings(d))
        s = d.visible('settings: chosen there, it starts with the focus',
                      lambda s: find(d, s, 'Settings - Wi-Fi', True) is not None, timeout=20)
        state['open'] = True
        at = find(d, s, 'Settings - Wi-Fi', True)
        d.click(at[0] + W + FRAME_W - 16, at[1] + 14)
        d.visible('settings: its close button ends it', lambda s: find(d, s) is None, timeout=15)
        state['open'] = False
        d.move(*PARK)
        state['done'] = True
    finally:
        if not state.get('done') and d.outdir:         # the screen as the failure left it, before the tidying
            d.vm.screen().png(os.path.join(d.outdir, 'guiapps-settings-at-failure.png'))
        try:
            tidy(d, state)
        except AssertionError:                          # what failed first is what the results say
            pass


def tidy(d, state):
    """the theme ocean and history off again (through Settings, still open), Settings ended (its close button, or End
    when it does not answer), then the Terminal closed"""
    w = state.get('win')
    if state.get('open') and w is not None:
        d.key('esc')                                    # a question open: cancelled
        if state.get('dusk'):
            d.key('f7')
            w.click(*OCEAN_OPTION)
            d.vm.wait_screen(lambda s: d.pixel(s, *DESKTOP_AT) == OCEAN, 5, interval=0.2)
        if state.get('history'):
            d.key('f8')
            w.click(*HISTORY_SWITCH)
        s = d.vm.screen()
        at = find(d, s)
        if at is not None:
            d.click(at[0] + W + FRAME_W - 16, at[1] + 14)
            if d.vm.wait_screen(lambda s: find(d, s) is None, 3, interval=0.2) is None:
                d.key('ret')                            # its question: Close
                if d.vm.wait_screen(lambda s: find(d, s) is None, 5, interval=0.2) is None and \
                        term.dialog_up(d, d.vm.screen(), 'End the program?'):
                    d.key('right')
                    d.key('ret')
    term.close_terminal(d)
