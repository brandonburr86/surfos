"""What the web browser in a window does with a server, for qemu-usb.py's guiweb scenario (a boot of its own, with a USB
Ethernet adapter, because the guiapps scenario has no network and counts the kernel's heap to the byte). The rest of
the acceptance test of milestone 2 (docs/superpowers/plans/2026-11-01-browser-m2-m3.md): Stop ends a load that never
finishes while the window answers; a GET form typed and submitted reaches the test server with the right query; a POST
form and a password form do not reach it, and say so. The server is this module's, in a thread, on the Mac's loopback,
which the guest reaches at 10.0.2.2 through QEMU's user network: nothing leaves this computer. The window is the one
gui_apps_web.py knows, on a page of the data stick."""
import socket
import threading
import time

import gui_apps_term as term
import gui_apps_web as web
from gui_apps_scenario import MUTED

server = socket.socket()
server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
server.bind(('127.0.0.1', 0))
server.listen(16)
PORT = server.getsockname()[1]
HOST = f'http://10.0.2.2:{PORT}'
requests = []                       # request lines, as they came
closed = []                         # the paths of connections the client closed without an answer

STICK = {
    'webnet/net.html': (f'''<!DOCTYPE html><title>Net</title><p><a href="{HOST}/never">Never ends</a>
<form action="{HOST}/search"><p>Words <input name=q size=30>
<input type=checkbox name=box value=yes checked> <select name=pick><option>One<option selected>Two</select>
<input type=hidden name=h value=hid><input type=submit name=go value=Search></form>
<form method=post action="{HOST}/posted"><p><input name=a value=1><input type=submit value=Post></form>
<form action="{HOST}/login"><p><input name=user value=me size=8> <input type=password name=pw size=8>
<input type=submit value=Login></form>''').encode(),
}
# on the page, 760 wide less the scroll bar: a paragraph 16 apart from y 24, fields 24 high, their text 4 down:
# see user/wundertow/layout.c. Each y is the top of its line.
LINK_Y = 24
WORDS_Y = LINK_Y + 16 + 16
POST_Y = WORDS_Y + 24 + 16
LOGIN_Y = POST_Y + 24 + 16
FIELD_X = 8 + len('Words ') * 8
RELOAD_X = 7 + 62 + 4 + 86 + 4              # the toolbar: 7, Back of 6 characters, Forward of 9, 14 wider than their text
# a field of size n is n * 8 + 8 wide; after it a space
SEARCH_LABEL = 'Search'


def serve_one(conn):
    conn.settimeout(120)
    data = b''
    try:
        while b'\r\n\r\n' not in data:
            more = conn.recv(4096)
            if not more:
                return
            data += more
        line = data.split(b'\r\n')[0].decode('latin-1')
        requests.append(line)
        path = line.split(' ')[1]
        if path.startswith('/never'):     # never answers: until the client goes
            while conn.recv(4096):
                pass
            closed.append(path)
            return
        body = b'<!DOCTYPE html><title>Results</title><p>The server has it.'
        conn.sendall(b'HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: %d\r\nConnection: close\r\n\r\n' % len(body) + body)
    except OSError:
        pass
    finally:
        conn.close()


def serve():
    while True:
        try:
            conn, _ = server.accept()
        except OSError:
            return
        threading.Thread(target=serve_one, args=(conn,), daemon=True).start()


def status_text(d, s, at, text):
    return d.text(s, at[0] + 1 + 9, at[1] + web.TITLE_H + web.CONTENT_H - web.STATUS + 3, text, web.INK, web.BAND)


def field_text(d, s, at, y, text, x):
    """what a field shows, on its own background and with the caret at its end: only what is lit is looked at"""
    return d.text(s, at[0] + 1 + x, at[1] + web.TITLE_H + web.TOOLBAR + y, text, web.INK)


def page_click(d, at, x, y):
    d.click(at[0] + 1 + x, at[1] + web.TITLE_H + web.TOOLBAR + y)


def waited(test, seconds):
    end = time.time() + seconds
    while time.time() < end:
        if test():
            return True
        time.sleep(0.1)
    return False


def checks(d):
    threading.Thread(target=serve, daemon=True).start()
    grid = term.Grid()
    mounted = False
    at = None
    try:
        d.move(*web.PARK)
        d.key('ctrl-t')
        d.visible('Ctrl+T opens the Terminal, for the browser', d.title('Terminal'))
        d.require('the Terminal shows its prompt', term.wait_rows(d, grid, term.prompted, 30) is not None)
        d.keys('mount usb1p1\n')
        mounted = d.check('the data stick is mounted in the Terminal',
                          term.wait_rows(d, grid, lambda rows: any('usb1p1 on /usb1p1' in r for r in rows), 30) is not None)
        d.keys('wundertow /usb1p1/webnet/net.html &\n')
        s = d.visible('wundertow opens the page of forms', lambda s: web.find(d, s, 'Net - Undertow') is not None, timeout=40)
        at = web.find(d, s, 'Net - Undertow')
        d.click(at[0] + 150, at[1] + 14)
        d.move(*web.PARK)
        d.visible('the focus', lambda s: web.titled(d, s, at, 'Net - Undertow', True), timeout=20)

        # Stop: a load that never finishes
        page_click(d, at, 8 + 24, LINK_Y + 8)
        d.move(*web.PARK)
        d.require('the link goes to the server, which does not answer', waited(lambda: any('/never' in r for r in requests), 30))
        stop = at[0] + 1 + RELOAD_X + 7, at[1] + web.TITLE_H + 4 + 3
        s = d.visible('Reload is Stop while it loads, and the status bar says so',
                      lambda s: d.text(s, *stop, 'Stop', web.INK) and status_text(d, s, at, 'Loading'),
                      'guiapps-web-loading.png', timeout=20)
        d.key('ctrl-l')
        d.visible('the window answers meanwhile: Ctrl+L takes the address field',
                  lambda s: d.pixel(s, at[0] + 1 + RELOAD_X + 62 + 4 + 6, at[1] + web.TITLE_H + 4) == web.LINK, timeout=20)
        d.key('esc')
        d.click(stop[0] + 8, stop[1] + 6)
        d.move(*web.PARK)
        d.visible('Stop ends it', lambda s: status_text(d, s, at, 'Stopped'), 'guiapps-web-stopped.png', timeout=20)
        d.check('and the helper has gone: the server saw the connection close',
                waited(lambda: any('/never' in p for p in closed), 15))
        d.check('Stop gives Reload back', d.text(d.vm.screen(), *stop, 'Reload', web.INK))

        # a GET form, typed and submitted
        page_click(d, at, FIELD_X + 16, WORDS_Y + 8)
        d.move(*web.PARK)
        d.keys('big waves')
        d.visible('a text field: what is typed shows', lambda s: field_text(d, s, at, WORDS_Y + 4, 'big waves', FIELD_X + 4),
                  'guiapps-web-typed.png', timeout=20)
        d.key('ret')
        d.require('Enter submits it: the query reaches the server', waited(lambda: any('/search' in r for r in requests), 30))
        line = next(r for r in requests if '/search' in r)
        d.check(f'the right query, the standard\'s encoding ({line})',
                line == 'GET /search?q=big+waves&box=yes&pick=Two&h=hid&go=Search HTTP/1.1')
        d.visible('the answer shows', lambda s: web.titled(d, s, at, 'Results - Undertow'), timeout=30)
        d.key('backspace')
        d.visible('back at the forms', lambda s: web.titled(d, s, at, 'Net - Undertow'), timeout=20)

        # a POST form: nothing goes, and it says so
        before = len(requests)
        post_x = 8 + 20 * 8 + 8 + 8 + 24            # its button, after a text field of 20 cells and a space
        page_click(d, at, post_x, POST_Y + 12)
        d.move(*web.PARK)
        d.visible('a POST form says it was not sent', lambda s: status_text(d, s, at, 'This form sends by POST'),
                  'guiapps-web-post.png', timeout=20)
        time.sleep(2)
        d.check('and nothing reached the server', len(requests) == before)

        # a form with a password: not over GET, whatever the scheme; its stars, nothing else
        pw_x = 8 + (8 * 8 + 8) + 8 + 16             # the password field, after the user's of 8 cells and a space
        page_click(d, at, pw_x, LOGIN_Y + 12)
        d.move(*web.PARK)
        d.keys('hunter2')
        d.visible('a password shows as stars', lambda s: field_text(d, s, at, LOGIN_Y + 4, '*******', pw_x - 16 + 4), timeout=20)
        d.key('ret')
        d.visible('a password form says it was not sent', lambda s: status_text(d, s, at, 'This form has a password field'),
                  'guiapps-web-password.png', timeout=20)
        time.sleep(2)
        d.check('and nothing reached the server', len(requests) == before)
        d.check('the address holds no password', not any('hunter2' in r for r in requests))
    except AssertionError:
        d.vm.screen('guiapps-webnet-at-failure.png')
        raise
    finally:
        if at:
            there = web.find(d, d.vm.screen(), 'Net - Undertow') or web.find(d, d.vm.screen(), 'Results - Undertow')
            if there:
                d.click(there[0] + web.FRAME_W - 16, there[1] + 14)
                d.vm.wait_screen(lambda s: web.find(d, s, 'Net - Undertow') is None, 10, interval=0.2)
        if mounted:
            d.move(*web.PARK)
            d.keys('umount /usb1p1\n')
            term.wait_rows(d, grid, term.prompted, 20)
        term.close_terminal(d)
        server.close()
