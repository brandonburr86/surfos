"""qemu-usb.py's wsettings scenario (11/05/26, the desktop applications, track settings, after its review): Settings
(user/wsettings/) on the desktop against the real kernel's simulated Wi-Fi, the one place a join through SYS_netctl
runs end to end. The stick folder of the USB build is made into a one-stick image as the settings scenario makes it,
GRUB's SurfOS entry given the word wlansim; the boot has a USB Ethernet adapter, onto which wlansim bridges its access
points.

Settings is started in the Terminal and driven through the screen: the card on and the scan after it; SurfSimWPA
joined with tools/qemu-run.py's test passphrase (wlansim's made-up one), typed into the password field, which shows
dots, and saved by ticking Save it (unticked by default); networks.conf read here holds its key and never the
passphrase. The review's M1: the card off and on again (which joins the saved network, as wifi connect does),
Disconnect at once, and no join comes back by itself in the 45 s after, the guest's own time (sleep in the Terminal:
the keeper looks again 30 s after a connect); the same for `wifi connect; wifi leave` typed in the Terminal; and the
other order, a leave and then a connect that finds the saved network gone (switched off in wlansim), after which the
keeper joins it by itself once it is back. The review's M2: with telnetd started on the serial console before the
desktop (a Terminal's pseudo-terminal is no place for one), an open network's Join only asks, with the warning, and
nothing is joined until the person answers Join; the band then says the network is open while the server runs. M3: the
password panel's Save it, unticked, says that telnetd starts on a network saved so. At the end: the kernel log has a
line for each job; the passphrase is nowhere on the serial line and nowhere new on the stick (the kernel image holds
wlansim's own copy); fsck_msdos and fsck.fat find no error on the stick."""
import hashlib
import os
import shutil
import tempfile
import time

import gui_scenario as base
import gui_apps_scenario as gas
import gui_apps_term as term
import gui_apps_settings as gs
from gui_apps_scenario import PAPER, INK, BAND, FOAM, MUTED

PLAIN = b'multiboot2 /boot/surfos.bin profile=laptop bootvol=$surfos_bootvol\n'
WLANSIM = b'multiboot2 /boot/surfos.bin profile=laptop wlansim bootvol=$surfos_bootvol\n'
CARD = (8 + 10, 45 + 11)            # Settings' Wi-Fi page, in its content (window.c's layout at 600x400)
JOIN, SCAN = (528 + 32, 78 + 12), (364 + 32, 78 + 12)
LEAVE = (505, 44 + 12)              # in Disconnect whether Save stands beside it (a network not saved) or not
ROW0 = (8 + 40, 106 + 1 + 9)        # the networks' first row; a row is 18 high
SAVE_BOX = (16 + 8, 216 + 11)       # the password panel's Save it
ASK_YES, ASK_NO = (520 + 32, 320 + 12), (444 + 34, 320 + 12)
QUIET_MS = 45000                    # the keeper's look after a connect comes 30 s later (netman.c's WIFI_RETRY_MS)
AGAIN_MS = 75000                    # and its next 30 s after that: a join by the keeper comes within two looks


def taskbar(d, s, title):
    """where to click the taskbar button whose title begins with title (high in it: the pointer drawn whole, which the
    driver looks for), or None (gui_apps_tools.py's way of finding it)"""
    for n in range(1, 16):
        w = min(156, (960 - 96) // n - 4)
        for i in range(n):
            x = 6 + i * (w + 4)
            if any(d.text(s, x + 24, 540 - 26 + 5, title, fg) for fg in (FOAM, MUTED, PAPER, INK)):
                return x + w // 2, 540 - 26 + 7
    return None


def run(Vm, PROMPT, args, image, firmware, outdir, helpers):
    results = []
    kit = helpers['stick_folder'](image)
    results.append(('wsettings: the stick folder is beside the image (make usb-image writes it)', kit is not None))
    if not kit:
        return results
    passphrase = helpers['smoke'].WLANSIM_PASSPHRASE.encode()
    files = helpers['written_by_stick_tool'](kit, skip=('surfos-dev/surfos.bin', 'surfos-dev/initrd.tar'))
    cfg = files.get('boot/grub/grub.cfg', b'')
    results.append(('wsettings: GRUB\'s SurfOS entry is there to be given the simulated Wi-Fi', cfg.count(PLAIN) == 1))
    if cfg.count(PLAIN) != 1:
        return results
    files['boot/grub/grub.cfg'] = cfg.replace(PLAIN, WLANSIM)
    tmpd = tempfile.mkdtemp(prefix='surfos-wsettings-', dir='/tmp' if os.path.isdir('/tmp') else None)
    stick = os.path.join(tmpd, 'wsettings.img')
    helpers['mkimage'].make_fat32(stick, files, size_mb=1100, label='DATASTICK', cluster_bytes=16384,
                                  serial=0x5E77C0FA)
    with open(stick, 'rb') as f:
        before = f.read().count(passphrase)
    extra = ['-global', 'VGA.xres=1920', '-global', 'VGA.yres=1080', '-netdev', 'user,id=n0', '-device',
             helpers['usbnet']]
    vm = Vm('wsettings', args, stick, firmware, mem='512', serial=True, outdir=outdir, extra=extra, copy_image=False)
    d = gas.Driver(vm, base._font(), results, outdir)
    grid = term.Grid()

    def shot(name):
        if outdir:
            vm.screen().png(os.path.join(outdir, name))

    def band(w, text):
        return lambda s: w.text(s, gs.BAND_AT[0], gs.BAND_AT[1], text, INK, BAND)

    def links():                    # the station's "link up" lines on the serial line: one for each association
        return bytes(vm.transcript).count(b'wlan0: link up')

    def listen(seconds):            # the serial line read for that long (a pump ends when something comes)
        end = time.time() + seconds
        while time.time() < end:
            vm.pump(min(1.0, max(0.05, end - time.time())))

    def to(title, front):           # a window to the front by its taskbar button (which tucks away one in front)
        s = vm.screen()
        if front(s):
            return
        at = taskbar(d, s, title)
        d.require(f'wsettings: {title} has its taskbar button', at is not None)
        d.click(*at)
        d.move(*gs.PARK)
        d.require(f'wsettings: {title} in front', vm.wait_screen(front, 10, interval=0.2) is not None)

    def in_terminal(command, test, name, timeout=30):
        to('Terminal', d.title('Terminal'))
        d.keys(command + '\n')
        d.require(name, term.wait_rows(d, grid, test, timeout) is not None)

    try:
        d.require('wsettings: boots to the shell', vm.expect(PROMPT, args.boot_timeout))
        vm.send('telnetd on\n')         # M2's server without a password, from the machine's own console: the Terminal's
        d.require('wsettings: telnetd runs, started on the serial console', vm.expect('telnetd: listening', 30) and
                  vm.expect(PROMPT, 30))       # pseudo-terminal is no place for one (telnetd refuses there)
        vm.send('gui\n')
        d.require('wsettings: the desktop', vm.wait_screen(lambda s: (s.w, s.h) == (1920, 1080) and
                                                           d.pixel(s, 0, 0) == PAPER, 30, interval=0.2) is not None)
        d.move(*gs.PARK)
        d.key('ctrl-t')
        d.require('wsettings: the Terminal', vm.wait_screen(d.title('Terminal'), 15, interval=0.1) is not None)
        d.require('wsettings: its prompt', term.wait_rows(d, grid, term.prompted, 30) is not None)
        d.keys('wsettings &\n')
        s = d.visible('wsettings: Settings opens on its Wi-Fi page', lambda s: gs.find(d, s, 'Settings - Wi-Fi') is not None,
                      timeout=20)
        w = gs.Win(d, gs.find(d, s, 'Settings - Wi-Fi'))
        d.click(w.at[0] + 300, w.at[1] + 14)
        d.visible('wsettings: it has the focus', w.titled('Settings - Wi-Fi'), timeout=30)
        d.move(*gs.PARK)

        def listed(s):
            return w.text(s, ROW0[0] - 34, ROW0[1] - 8, 'SurfSim', INK) and \
                w.text(s, ROW0[0] - 34, ROW0[1] - 8 + 18, 'SurfSimWPA', INK)

        def station(text):
            return lambda s: w.text(s, 130, 40, text, INK, PAPER)

        joined = lambda s: station('Joined SurfSimWPA (WPA2-Personal): address')(s) and \
            w.text(s, 130, 56, '10.0.2.', INK, PAPER)
        s = d.visible('wsettings: the card off (nothing saved: the network manager leaves it so), or networks listed',
                      lambda s: station('The Wi-Fi card is off')(s) or listed(s), timeout=40)
        if station('The Wi-Fi card is off')(s):
            w.click(*CARD)
            d.move(*gs.PARK)
        d.visible('wsettings: the networks in range, from the scan Settings makes by itself: SurfSim, SurfSimWPA',
                  listed, 'wsettings-scan.png', timeout=40)
        # a WPA2 join, its password typed into the field (dots), saved by ticking Save it
        w.click(ROW0[0], ROW0[1] + 18)
        w.click(*JOIN)
        d.move(*gs.PARK)
        d.visible('wsettings: Join asks SurfSimWPA\'s password in the panel, Save it unticked',
                  lambda s: w.text(s, 16, 48, 'Password for SurfSimWPA', INK, PAPER))
        d.check('wsettings (M3): Save it, unticked, says that telnetd starts on a network saved so (telnetd runs)',
                w.text(vm.screen(), 36, 219, 'Save it: joined by itself at every start, and telnetd starts on it', INK))
        d.keys(passphrase.decode())                     # into the password field, which has the focus
        d.visible('wsettings: the field shows a dot for each character', lambda s: w.text(s, 148, 189, '\x07' * 8, INK),
                  'wsettings-password.png')
        w.click(*SAVE_BOX)                              # then Save it ticked, and Join
        w.click(*ASK_YES)
        d.move(*gs.PARK)
        s = d.visible('wsettings: joined, with an address', band(w, 'Joined SurfSimWPA: address 10.0.2.'),
                      'wsettings-joined.png', timeout=90)
        d.check('wsettings: the station\'s line says the network and its address', joined(s))
        nets = helpers['read_fat32'](stick, 'surfos-config/networks.conf') or b''
        body = b'\n'.join(line for line in nets.splitlines() if not line.startswith(b'#'))
        key = hashlib.pbkdf2_hmac('sha1', passphrase, b'SurfSimWPA', 4096, 32).hex().encode()
        d.check('wsettings: networks.conf on the stick holds SurfSimWPA with its key (PBKDF2 of the passphrase)',
                b'network = "SurfSimWPA"' in body and key in body and b'hidden' not in body)
        d.check('wsettings: networks.conf never holds the passphrase', passphrase not in nets)
        # M1: the card off and on (it joins the saved network, as wifi connect), Disconnect at once: nothing comes back
        w.click(*CARD)
        d.visible('wsettings: the card off', band(w, 'The Wi-Fi card is off'), timeout=30)
        w.click(*CARD)
        d.move(*gs.PARK)
        d.visible('wsettings: the card on joins the saved network', joined, timeout=60)
        w.click(*LEAVE)
        d.move(*gs.PARK)
        d.visible('wsettings: Disconnect right after the card\'s start: left', band(w, 'Left the network'), timeout=30)
        n = links()                     # the windows below are the guest's own time (sleep, in ms): a slow host
        in_terminal(f'sleep {QUIET_MS}', lambda rows: term.then_prompt(rows, f'slept {QUIET_MS} ms'),     # stretches it
                    'wsettings: the guest\'s own 45 s go by, in the Terminal', timeout=QUIET_MS // 1000 + 90)
        to('Settings', w.titled('Settings - Wi-Fi'))
        d.check('wsettings (M1): nothing joined again by itself in the 45 s after Disconnect (the keeper looks again '
                '30 s after a connect)', links() == n and
                vm.wait_screen(station('Not joined to a network'), 10, interval=0.5) is not None)
        shot('wsettings-m1.png')
        n = links()
        in_terminal(f'wifi connect; wifi leave; sleep {QUIET_MS}', lambda rows: term.then_prompt(rows, f'slept {QUIET_MS} '
                    'ms') and any('wlan0: not associated' in r for r in rows),
                    'wsettings: wifi connect, then wifi leave at once, then 45 s, in the Terminal',
                    timeout=QUIET_MS // 1000 + 120)
        d.check('wsettings (M1): after wifi connect; wifi leave, nothing joins again by itself in 45 s (one association: '
                'the connect\'s own)', links() == n + 1)
        # M1, the other order: a stop, then a connect that finds the saved network gone: the keeper looks again
        n = links()
        in_terminal(f'wlansim off SurfSimWPA; wifi connect; wlansim on SurfSimWPA; sleep {AGAIN_MS}',
                    lambda rows: term.then_prompt(rows, f'slept {AGAIN_MS} ms') and
                    any('is not in range' in r for r in rows),
                    'wsettings: SurfSimWPA off, wifi connect finds it gone, SurfSimWPA on again, 75 s',
                    timeout=AGAIN_MS // 1000 + 120)
        if not d.check('wsettings (M1): a stop before the connect is no reason to stay away: the keeper joins '
                       'SurfSimWPA again by itself', links() == n + 1):
            d.keys('dmesg | grep NET | tail -n 12\n')   # what the network manager did, for the evidence
            listen(3)
            print('the Terminal shows', *grid.text(vm.screen()), sep='\n    ', flush=True)
        in_terminal('wifi leave', lambda rows: term.then_prompt(rows, 'wlan0: not associated'), 'wsettings: wifi leave')
        # M2: telnetd runs (started before the desktop): an open network's Join only asks, joining on Join
        to('Settings', w.titled('Settings - Wi-Fi'))
        w.click(*SCAN)                                  # what was heard a minute ago has gone from the list: again
        d.move(*gs.PARK)
        d.visible('wsettings: Scan: the networks in range again', listed, timeout=40)
        n = links()
        w.click(*ROW0)
        w.click(*JOIN)
        d.move(*gs.PARK)
        d.visible('wsettings (M2): an open network\'s Join while telnetd runs: the warning first',
                  lambda s: w.text(s, 16, 48, 'A telnet server runs or is to start', INK, PAPER),
                  'wsettings-m2-asked.png', timeout=20)
        w.click(*ASK_NO)
        d.move(*gs.PARK)
        d.visible('wsettings (M2): Cancel: the page again', lambda s: listed(s))
        listen(3)
        d.check('wsettings (M2): nothing joined while the person had not answered', links() == n and
                station('Not joined to a network')(vm.screen()))
        w.click(*ROW0)
        w.click(*JOIN)
        d.move(*gs.PARK)
        d.visible('wsettings (M2): asked again', lambda s: w.text(s, 16, 48, 'A telnet server runs', INK, PAPER))
        w.click(*ASK_YES)
        d.move(*gs.PARK)
        d.visible('wsettings (M2): Join, the person\'s word: joined, and the band says the server reaches it',
                  lambda s: band(w, 'Joined SurfSim')(s) and links() == n + 1, 'wsettings-m2-joined.png', timeout=60)
        w.click(*LEAVE)
        d.move(*gs.PARK)
        d.visible('wsettings: Disconnect', band(w, 'Left the network'), timeout=30)
        d.key('ctrl-w')
        d.visible('wsettings: Ctrl+W closes Settings', lambda s: gs.find(d, s) is None, timeout=15)

        def counted(rows):
            for i in range(len(rows) - 1):
                if 'grep -c "+NET: wsettings"' in rows[i] and rows[i + 1].strip().isdigit():
                    return int(rows[i + 1].strip()) >= 8
            return False
        in_terminal('dmesg | grep -c "+NET: wsettings"', counted, 'wsettings: a line in the kernel log for each job')
        d.check('wsettings: the serial line never has the passphrase', passphrase not in bytes(vm.transcript))
    except AssertionError as e:
        shot('wsettings-failed.png')
        if not results or results[-1][1]:               # stopped with no failed check (a move that gave up): one now
            results.append((f'wsettings: its checks stopped early ({e})', False))
    finally:
        vm.close()
    try:
        with open(stick, 'rb') as f:
            after = f.read().count(passphrase)
        results.append((f'wsettings: the passphrase nowhere new on the stick ({before} before, in the kernel image; '
                        f'{after} after)', after == before))
        if results and all(ok for _, ok in results):
            for what, ok, text in helpers['fat_checks'](stick, 'wsettings stick'):
                results.append((what, ok))
                if not ok:
                    print(text, flush=True)
    finally:
        shutil.rmtree(tmpd, ignore_errors=True)
    return results
