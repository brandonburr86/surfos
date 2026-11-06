"""Chat with a server, for qemu-usb.py's guichat scenario (the desktop applications, track chat): a boot of its own with
a USB Ethernet adapter and QEMU's CPU with RDRAND (-cpu max), as guiweb's is one, because the guiapps scenario has no
network and counts the kernel's heap to the byte. The server is this module's: a scripted IRC server in a thread, on
the Mac's loopback, which the guest reaches at 10.0.2.2 through QEMU's user network, so nothing leaves this computer.
Its only password is a made-up one, and it never answers anything but the script.

wchat is started in the Terminal. The form: the server 10.0.2.2, TLS off, the server's port, SASL, a password that
shows as dots. Connect: the server receives CAP LS 302, NICK and USER, asks for SASL's PLAIN login and gets the right
base64; the welcome comes and the status bar says who is where. /join #surf: the server receives the JOIN and answers
it with the topic, the names and a message that names us with a word in red; the window shows the channel's tab, the
topic, the nick list, the message (its nick in coral, the red word red). A message typed reaches the server. A
double-click on @bob opens a query, and a message there reaches bob. The close button asks first; Close says QUIT,
the server ends the connection, and the window goes. Then a second wchat asks for TLS on the same port, which is no
TLS server: the handshake fails and the room says so. ps finds no wchat at the end."""
import base64
import socket
import threading
import time

import gui_apps_term as term
import gui_apps_chat as chat
from gui_apps_scenario import INK, MUTED
from gui_apps_win import answered

PASSWORD = 'made-up-pass'           # made up, for the made-up server
NICK = 'surfer'
server = socket.socket()
server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
server.bind(('127.0.0.1', 0))
server.listen(4)
PORT = server.getsockname()[1]
received = []                       # every line from the client, in order
tls_tried = []                      # connections that began with a TLS record
RED = bytes.fromhex('e02020')       # colour 4 as Chat draws it (user/wchat/ircfmt.c)
NICKS_X = 600                       # the nick list's left, in the content
TABS_Y = 7


def send(conn, *lines):
    conn.sendall(''.join(line + '\r\n' for line in lines).encode())


def serve_one(conn):
    conn.settimeout(300)
    data = b''
    try:
        first = conn.recv(1)
        if not first:
            return
        if first == b'\x16':                        # a TLS handshake's record: this server speaks none
            tls_tried.append(True)
            return
        data = first
        while True:
            while b'\n' not in data:
                more = conn.recv(4096)
                if not more:
                    return
                data += more
            raw, data = data.split(b'\n', 1)
            line = raw.rstrip(b'\r').decode('utf-8', 'replace')
            received.append(line)
            if line == 'CAP LS 302':
                send(conn, ':irc.test CAP * LS :multi-prefix sasl=PLAIN')
            elif line == 'CAP REQ :sasl':
                send(conn, f':irc.test CAP {NICK} ACK :sasl')
            elif line == 'AUTHENTICATE PLAIN':
                send(conn, 'AUTHENTICATE +')
            elif line.startswith('AUTHENTICATE '):
                send(conn, f':irc.test 900 {NICK} {NICK}!s@10.0.2.15 {NICK} :You are now logged in as {NICK}',
                     f':irc.test 903 {NICK} :SASL authentication successful')
            elif line == 'CAP END':
                send(conn, f':irc.test 001 {NICK} :Welcome to the test network, {NICK}',
                     f':irc.test 005 {NICK} CHANTYPES=# PREFIX=(ov)@+ :are supported by this server',
                     f':irc.test 375 {NICK} :- irc.test Message of the Day -',
                     f':irc.test 372 {NICK} :- Waves and nothing else.', f':irc.test 376 {NICK} :End of /MOTD command.')
            elif line == 'JOIN #surf':
                send(conn, f':{NICK}!s@10.0.2.15 JOIN #surf', f':irc.test 332 {NICK} #surf :Waves today',
                     f':irc.test 353 {NICK} = #surf :{NICK} @bob +carol', f':irc.test 366 {NICK} #surf :End of /NAMES list.')
                time.sleep(0.5)
                send(conn, f':bob!b@surf.test PRIVMSG #surf :hi {NICK}, \x034red\x03 waves')
            elif line.startswith('PING '):
                send(conn, f':irc.test PONG irc.test :{line[5:].lstrip(":")}')
            elif line.startswith('QUIT'):
                send(conn, f'ERROR :Closing Link: {NICK} (Quit)')
                return
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


def waited(test, seconds):
    end = time.time() + seconds
    while time.time() < end:
        if test():
            return True
        time.sleep(0.1)
    return False


def got(line):
    return lambda: line in received


def start(d, grid, state, what):
    """wchat started in the Terminal, its window given the focus: the Win"""
    d.keys('wchat &\n')
    s = d.visible(f'chatnet: wchat opens its window ({what})', lambda s: chat.find(d, s, False) is not None, timeout=20)
    state['open'] = True
    w = chat.Win(d, chat.find(d, s))
    d.click(w.at[0] + 250, w.at[1] + 14)
    d.visible('chatnet: the focus, on the server field', lambda s: chat.titled_at(d, s, w.at, True))
    return w


def form(d, w, tls, sasl):
    """the form filled in: 10.0.2.2, TLS as asked, PORT, SASL with PASSWORD or none; then Enter"""
    d.move(*chat.PARK)
    d.keys('10.0.2.2')
    if not tls:
        w.click(chat.TLS[0] + 5, chat.TLS[1] + 11)
    w.click(chat.HOST[0] + 20, chat.TLS[1] + 11)    # the port field
    d.key('ctrl-a')
    d.keys(str(PORT))
    if sasl:
        w.click(chat.HOST[0] + 5, 145 + 11)         # the password's kind: None, then Right twice, SASL
        d.key('right')
        d.key('right')
        w.click(chat.HOST[0] + 100, 175 + 11)       # the password field
        d.move(*chat.PARK)                          # the pointer off the dots it would hide
        d.keys(PASSWORD)
        d.visible('chatnet: the password shows as dots (code page 437\'s 7), never as itself',
                  lambda s: w.text(s, chat.HOST[0] + 4, 175 + 3, '\x07' * (len(PASSWORD) - 1), INK, chat.SHEET))
        # (the last dot shares its cell's edge with the caret)
    d.move(*chat.PARK)
    d.key('ret')


def checks(d):
    threading.Thread(target=serve, daemon=True).start()
    grid = term.Grid()
    state = {}
    try:
        d.move(*chat.PARK)
        d.key('ctrl-t')
        d.visible('chatnet: Ctrl+T opens the Terminal, for wchat', d.title('Terminal'))
        d.require('chatnet: the Terminal shows its prompt', term.wait_rows(d, grid, term.prompted, 30) is not None)
        w = start(d, grid, state, 'to talk to the test server')
        form(d, w, False, True)
        d.require('chatnet: Connect: the server receives CAP LS 302, NICK and USER', waited(got('CAP LS 302'), 30) and
                  waited(got(f'USER {NICK} 0 * :{NICK}'), 10) and got(f'NICK {NICK}')())
        plain = base64.b64encode(b'\0' + NICK.encode() + b'\0' + PASSWORD.encode()).decode()
        d.require('chatnet: SASL PLAIN: the login it asked for, in base64, then CAP END',
                  waited(got(f'AUTHENTICATE {plain}'), 20) and waited(got('CAP END'), 10) and got('CAP REQ :sasl')())
        d.visible('chatnet: welcomed: the status bar says who is where',
                  w.status(f'{NICK} on 10.0.2.2, port {PORT} (not encrypted)'), 'guiapps-chatnet-welcome.png',
                  timeout=20)
        d.keys('/join #surf\n')
        d.require('chatnet: /join #surf reaches the server', waited(got('JOIN #surf'), 20))
        d.visible('chatnet: the channel\'s tab, its topic, its nick list and the message that names us (its nick in '
                  'coral, the red word red)',
                  lambda s: w.text(s, 111, TABS_Y, '#surf', chat.PAPER) and w.text(s, 4, 37, 'Waves today', INK) and
                  w.text(s, NICKS_X + 6, 63, '@bob', INK) and w.text(s, chat.TEXT_X, chat.ROW0 + 32, '<bob>', chat.CORAL)
                  and w.text(s, chat.TEXT_X + 17 * 8, chat.ROW0 + 32, 'red', RED), 'guiapps-chatnet-channel.png',
                  timeout=20)
        d.keys('hello bob\n')
        d.require('chatnet: a message typed reaches the server', waited(got('PRIVMSG #surf :hello bob'), 20))
        w.click(NICKS_X + 30, 61 + 1 + 8)               # @bob, twice: a query with him
        w.click(NICKS_X + 30, 61 + 1 + 8)
        d.visible('chatnet: a double-click on @bob opens a query with him',
                  lambda s: w.text(s, 4, 37, 'Talking with bob', MUTED), timeout=15)
        d.move(*chat.PARK)
        d.keys('psst\n')
        d.require('chatnet: a message in the query reaches bob', waited(got('PRIVMSG bob :psst'), 20))
        w.close()
        d.visible('chatnet: connected, the close button asks first', lambda s: w.text(
            s, (chat.W - 470) // 2 + 28, (chat.H - 146) // 2 + 6, 'Leave the server and close Chat?', chat.PAPER), timeout=15)
        d.key('ret')
        d.require('chatnet: Close says QUIT to the server', waited(lambda: any(r.startswith('QUIT') for r in received), 20))
        d.visible('chatnet: and the window goes when the connection has ended', lambda s: chat.find(d, s) is None,
                  timeout=20)
        state['open'] = False
        # TLS to a server that has none: refused, and said
        w = start(d, grid, state, 'for TLS')
        form(d, w, True, False)
        d.require('chatnet: TLS on the port: a handshake reaches the server', waited(lambda: bool(tls_tried), 30))
        d.visible('chatnet: the handshake fails, and the room says so', lambda s: w.text(
            s, chat.TEXT_X + 16, chat.ROW0 + 16, 'Not connected: ', chat.CORAL, chat.SHEET), 'guiapps-chatnet-tls.png',
                  timeout=30)
        w.close()
        d.visible('chatnet: not connected, the close button closes it', lambda s: chat.find(d, s) is None, timeout=15)
        state['open'] = False
        d.move(*chat.PARK)
        d.keys('wait; ps | grep -c wchat\n')         # wait: the jobs' ends, which come just after their windows'
        d.require('chatnet: ps finds no wchat', term.wait_rows(
            d, grid, lambda rows: answered(rows, 'wait; ps | grep -c wchat', '0')) is not None)
        d.check('chatnet: the password never reached the server as itself',
                not any(PASSWORD in r for r in received))
        state['done'] = True
    except AssertionError:
        d.vm.screen('guiapps-chatnet-at-failure.png')
        raise
    finally:
        try:
            chat.tidy(d, state)
        except AssertionError:
            pass
        server.close()
