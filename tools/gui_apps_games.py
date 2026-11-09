"""The guiapps scenario's checks of the games (docs/superpowers/plans/2026-11-05-desktop-apps.md, track games): Mines,
Solitaire and Swell, each started in the Terminal (wmines &, wsolitaire &, wswell &); the Driver is
gui_apps_scenario.py's. Each window comes up without the focus, and a click on its title bar gives it the focus.
Mines: the small field's covered cells and the status bar's line; a click reveals an area, a right-click flags a
covered cell (its teal pennant); Esc during the game asks first and Enter (Cancel, the first button) keeps it; 3
during the game asks first too, and New field (Right, Enter) lays the large field out with 24-pixel cells; the close
button closes a field not yet begun at once. Solitaire: the stock's
back, a tableau's face up and the status bar; a click on the stock deals a face to the waste; Space deals another and
U takes it back; the close button during a game asks, and Close (Right, Enter) ends it. Swell: the band that says
Enter starts, the panel's Next; Enter lets a piece in at the top, Space drops it to the bottom, P pauses (the band
says so); the close button during a game asks, and Close ends it. Last, ps finds none of the three. The checks leave
the desktop as they found it (the games ended, the Terminal closed), also when one fails. The colours and places are
the programs' own (user/wmines/window.c, user/wsolitaire/window.c, user/wswell/window.c, their view.c and table.c),
in the desktop's logical pixels: each window's content at its place in the cascade."""
import os

import gui_apps_term as term
from gui_apps_scenario import PAPER, INK, TEAL, MUTED, BAND
from gui_apps_win import answered

TITLE_H, FRAME_W, FRAME_H = 28, 2, 29
PARK = (900, 480)                   # the pointer clear of the windows and of the Terminal's text
BAR_H, STATUS_H = 34, 22            # a game's toolbar (gk.h's buttons, padded 5) and status bar


def rgb(h):
    return bytes.fromhex(h)


# Mines (user/wmines/window.c)
COVER, OPEN, FLAG = rgb('dccb9e'), rgb('f7f3e8'), rgb('197d83')
# Solitaire (user/wsolitaire/window.c)
BACK, RIM, FACE = rgb('102e3b'), rgb('b8e9d9'), rgb('fffdf5')
# Swell (user/wswell/window.c)
WATER, GRID, FOAM_TEXT, BANNER = rgb('0f2f3b'), rgb('15394a'), rgb('e8f6f1'), rgb('0a2029')


class Game:
    """a game's window: its title, its content's size, its place in the cascade once found"""
    def __init__(self, d, program, title, w, h):
        self.d, self.program, self.title, self.w, self.h = d, program, title, w, h
        self.at = None

    def places(self):
        """where shell/guiclient.c opens the desktop's k-th program window, kept on a 960x540 screen (gui_place())"""
        for k in range(8):
            yield 72 + 24 * k, min(52 + 20 * k, 540 - 26 - 2 - (self.h + FRAME_H))

    def titled_at(self, s, at, focused=None):
        bars = (TEAL, MUTED) if focused is None else (TEAL,) if focused else (MUTED,)
        return any(self.d.text(s, at[0] + 30, at[1] + 6, self.title, PAPER, bar) for bar in bars)

    def find(self, s, focused=None):
        for at in self.places():
            if self.titled_at(s, at, focused):
                return at
        return None

    # the content, from its top left
    def pixel(self, s, x, y):
        return self.d.pixel(s, self.at[0] + 1 + x, self.at[1] + TITLE_H + y)

    def text(self, s, x, y, text, fg, bg=None):
        return self.d.text(s, self.at[0] + 1 + x, self.at[1] + TITLE_H + y, text, fg, bg)

    def click(self, x, y):
        self.d.click(self.at[0] + 1 + x, self.at[1] + TITLE_H + y)

    def status(self, text):
        """a predicate: the status bar's text, from its start"""
        return lambda s: self.text(s, 10, self.h - STATUS_H + 3, text, INK, BAND)

    def asked(self, title):
        """a predicate: the toolkit's question over the window (gkbox.c: 470 x 146, or 16 less than a narrower
        window, in the middle of its content)"""
        bw = min(self.w - 16, 470)
        return lambda s: self.text(s, (self.w - bw) // 2 + 28, (self.h - 146) // 2 + 6, title, PAPER, TEAL)

    def close_button(self):
        self.d.click(self.at[0] + self.w + FRAME_W - 16, self.at[1] + 14)

    def gone(self, s):
        return self.find(s) is None


def start(d, grid, game, state):
    """typed in the Terminal: the window comes up without the focus; a click on its title bar gives it"""
    d.keys(game.program + ' &\n')
    s = d.visible(f'games: {game.program} & opens the {game.title} window, without the focus',
                  lambda s: game.find(s, False) is not None, timeout=20)
    state['open'] = game
    game.at = game.find(s)
    d.click(game.at[0] + 200, game.at[1] + 14)
    d.visible(f'games: a click on its title bar gives {game.title} the focus', lambda s: game.titled_at(s, game.at, True))
    d.move(*PARK)


def close_asked(d, game, state):
    """the close button during a game: the question, and Close (the second button) ends the program"""
    game.close_button()
    d.visible(f'games: {game.title}\'s close button during a game asks first', game.asked(f'Close {game.title}?'))
    d.key('right')
    d.key('ret')
    d.visible(f'games: Close ends {game.title}, and its window goes', game.gone, timeout=15)
    state['open'] = None
    d.move(*PARK)


def has(game, s, x, y, size, color):
    """color somewhere in the square of size at x, y of the content"""
    return any(game.pixel(s, x + i, y + j) == color for j in range(1, size - 1) for i in range(1, size - 1))


def mines(d, grid, state):
    g = Game(d, 'wmines', 'Mines', 692, 436)        # the large field's window, MV_CELL cells (view.c's mview_window())
    start(d, grid, g, state)
    # the small field in the board (the window less its toolbar and status bar): 40-pixel cells, in the middle
    cell, fx, fy = 40, (692 - 400) // 2, BAR_H + (436 - BAR_H - STATUS_H - 320) // 2
    d.visible('games: Mines shows the small field covered, and how to begin',
              lambda s: g.pixel(s, fx + 3, fy + 3) == COVER and g.pixel(s, fx + 9 * cell + 3, fy + 7 * cell + 3) == COVER
              and g.status('10 mines in 10 by 8: click a cell to begin')(s), 'guiapps-games-mines.png')
    g.click(fx + 4 * cell + 20, fy + 3 * cell + 20)
    d.move(*PARK)
    s = d.visible('games: a click reveals an area (the first reveal is always safe), and the clock runs',
                  lambda s: g.pixel(s, fx + 4 * cell + 3, fy + 3 * cell + 3) == OPEN and
                  g.status('Right-click flags; click a number to clear around')(s))
    covered = [(x, y) for y in range(8) for x in range(10) if g.pixel(s, fx + x * cell + 3, fy + y * cell + 3) == COVER]
    d.require('games: cells around the area are still covered', bool(covered))
    x, y = covered[0]
    d.move(g.at[0] + 1 + fx + x * cell + 20, g.at[1] + TITLE_H + fy + y * cell + 20)
    d.vm.mon.cmd('mouse_button 2')                  # the right button
    d.vm.pump(0.05)
    d.vm.mon.cmd('mouse_button 0')
    d.vm.pump(0.05)
    d.move(*PARK)
    d.visible('games: a right-click flags a covered cell: its teal pennant',
              lambda s: has(g, s, fx + x * cell, fy + y * cell, cell, FLAG), 'guiapps-games-mines-flag.png')
    d.key('esc')
    d.visible('games: Esc during a game asks first', g.asked('Close Mines?'))
    d.key('ret')
    d.visible('games: Enter keeps the game (Cancel is the first button)',
              lambda s: not g.asked('Close Mines?')(s) and g.pixel(s, fx + 4 * cell + 3, fy + 3 * cell + 3) == OPEN)
    d.keys('3')
    d.visible('games: 3 during a game asks first', g.asked('Start a new field?'))
    d.key('right')
    d.key('ret')
    lx, ly = (692 - 28 * 24) // 2, BAR_H + (436 - BAR_H - STATUS_H - 15 * 24) // 2
    d.visible('games: 3 lays the large field out, 28 by 15 with 24-pixel cells',
              lambda s: g.status('80 mines in 28 by 15: click a cell to begin')(s) and
              g.pixel(s, lx + 27 * 24 + 3, ly + 14 * 24 + 3) == COVER, 'guiapps-games-mines-large.png')
    g.close_button()
    d.visible('games: the close button closes a field not yet begun at once', g.gone, timeout=15)
    state['open'] = None
    d.move(*PARK)


def solitaire(d, grid, state):
    g = Game(d, 'wsolitaire', 'Solitaire', 600, 436)   # TB_W by TB_BAR_H + TB_H + TB_STATUS_H (table.h)
    start(d, grid, g, state)
    # 72-pixel cards: the columns 80 apart from x 24, the top row 10 into the table, the piles 120 into it
    col, top, tab = (lambda i: 24 + 80 * i), BAR_H + 10, BAR_H + 120
    d.visible('games: Solitaire deals: the stock a back, the last pile\'s top card up, and how to play',
              lambda s: g.pixel(s, col(0), top + 4) == RIM and g.pixel(s, col(0) + 5, top + 2) == BACK and
              g.pixel(s, col(6) + 68, tab + 6 * 8 + 30) == FACE and
              g.status('Drag a card, or click it; Space deals')(s), 'guiapps-games-solitaire.png')
    g.click(col(0) + 36, top + 48)
    d.move(*PARK)
    d.visible('games: a click on the stock deals a card face up to the waste',
              lambda s: g.pixel(s, col(1) + 68, top + 30) == FACE and g.pixel(s, col(1) + 5, top + 2) != BACK)
    d.keys(' ')
    d.keys('u')
    d.visible('games: Space deals another, and U takes it back: one card on the waste, the stock a back',
              lambda s: g.pixel(s, col(1) + 68, top + 30) == FACE and g.pixel(s, col(0) + 5, top + 2) == BACK,
              'guiapps-games-solitaire-dealt.png')
    close_asked(d, g, state)


def swell(d, grid, state):
    g = Game(d, 'wswell', 'Swell', 470, 438)        # 22-pixel cells, the panel, the status bar (view.c's sview_window())
    start(d, grid, g, state)
    cell, sx, sy = 22, 10, 10
    band_y = sy + 18 * cell // 2 - 20

    def band(text):
        return lambda s: g.text(s, sx + (12 * cell - len(text) * 8) // 2, band_y + 12, text, FOAM_TEXT, BANNER)

    d.visible('games: Swell waits: the band says Enter starts, the panel shows the next piece',
              lambda s: band('Enter starts')(s) and g.text(s, sx + 12 * cell + 10, sy, 'Next', MUTED, PAPER) and
              g.pixel(s, sx + 11, sy + 11) == WATER, 'guiapps-games-swell.png')

    def piece_at(rows):
        return lambda s: any(g.pixel(s, sx + x * cell + 11, sy + y * cell + 11) not in (WATER, GRID)
                             for y in rows for x in range(12))

    d.key('ret')
    d.visible('games: Enter lets the first piece in at the top', lambda s: not band('Enter starts')(s) and
              piece_at(range(0, 3))(s))
    d.key('spc')
    d.visible('games: Space drops it to the bottom', piece_at(range(15, 18)), 'guiapps-games-swell-dropped.png')
    d.keys('p')
    d.visible('games: P pauses: the band says so', band('Paused'))
    close_asked(d, g, state)


def checks(d):
    grid = term.Grid()
    state = {}
    try:
        d.move(*PARK)
        d.key('ctrl-t')
        d.visible('games: Ctrl+T opens the Terminal, for the games', d.title('Terminal'))
        d.require('games: the Terminal shows its prompt', term.wait_rows(d, grid, term.prompted, 30) is not None)
        mines(d, grid, state)
        solitaire(d, grid, state)
        swell(d, grid, state)
        for program in ('wmines', 'wsolitaire', 'wswell'):
            d.keys(f'ps | grep -c {program}\n')
            d.require(f'games: ps finds no {program}', term.wait_rows(
                d, grid, lambda rows: answered(rows, f'ps | grep -c {program}', '0')) is not None)
        state['done'] = True
    finally:
        if not state.get('done') and d.outdir:         # the screen as the failure left it, before the tidying
            d.vm.screen().png(os.path.join(d.outdir, 'guiapps-games-at-failure.png'))
        try:
            tidy(d, state)
        except AssertionError:                          # what failed first is what the results say
            pass


def tidy(d, state):
    """a game still open ended (its close button, Close in its question, or End when it does not answer), then the
    Terminal closed"""
    g = state.get('open')
    if g is not None and g.at is not None:
        for _ in range(3):      # 11/09/26 (the integration): the desktop asks "End the program?" at the third press
            if g.gone(d.vm.screen()):
                break
            g.close_button()
            if d.vm.wait_screen(g.asked(f'Close {g.title}?'), 3, interval=0.2) is not None:
                d.key('right')
                d.key('ret')
            elif term.dialog_up(d, d.vm.screen(), 'End the program?'):
                d.key('right')
                d.key('ret')
            d.vm.wait_screen(g.gone, 5, interval=0.2)
    term.close_terminal(d)
