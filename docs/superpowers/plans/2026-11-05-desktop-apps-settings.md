# Account: the settings track of the desktop applications

Branch `apps/settings`. The brief is `2026-11-05-desktop-apps.md`. Nothing here has run on the laptop or on a real
network: only on the host, and in QEMU, against wlansim's simulated access points with the test passphrase of
`tools/qemu-run.py`. How the gates went is in the track's report, not here, so that this file is part of the tree that
was tested.

The work came in the three phases the integrator set: the kernel interface and the model in new files only, the
window on kit's toolkit (after merging kit 2ba18949, which carries desk's amended ABI), then the wiring into existing
files and the gates (after merging desk's implementation, 2c5afd30).

## What was built

| Files | What |
|---|---|
| `include/surfos/netctl.h` | `SYS_netctl`'s interface: the operations, the structures (no pointers, fixed sizes), the rules |
| `user/include/surfos/netctl.h` | the same for programs, as `surfos/win.h` reaches them |
| `kernel/netctl.c` | the call: six reads, a step put in force, seven jobs, the job's thread |
| `include/surfos/syscall.h`, `kernel/syscall.c`, `include/surfos/sysimpl.h` | call 55 in the table |
| `kernel/netman.c`, `include/net/netman.h` | `netman_wifi_connect_quiet()` beside `netman_wifi_connect()`, which is unchanged; `netman_apply()` beside `netman_set()` |
| `user/wsettings/settings.c`, `.h` | Settings' model: the pages, the controls, the words; no system call, host-tested |
| `user/wsettings/window.c`, `.h` | its window on the toolkit (gk.h): the widgets, and each event to the model |
| `user/wsettings/sys.c`, `.h`, `main.c` | the call by `int 0x80`, About's figures (sysinfo, the build id, CPUID), the loop |
| `tests/host/test_netctl.c` | `kernel/netctl.c` as it is, against stand-ins for the station, the network manager, DHCP and memory |
| `tests/host/test_wsettings.c` | the model against a simulated kernel (`tests/host/wsettings_fake.h`) |
| `tests/host/test_wsettings_win.c` | the window on kit's fake desktop, with the model and the simulated kernel |
| `tools/gui_apps_settings.py` | the screen checks (guiapps) |
| `rootfs/apps/wsettings.app`, `rootfs/man/wsettings.txt` | the registry's entry (an icon drawn here: three sliders), the manual |
| `Makefile`, `tools/gui_apps_scenario.py`, `docs/GUI.md` | insertions only (`git diff` shows no line removed) |

`shell/wifi.c` and `net/netcfg.c` are not changed: desk's keys (`desktop.theme`, `pointer.speed`) are in the table
as desk left them. Settings puts a step of theirs in force through `netman_apply()` and writes the last one through
`netman_set()`, the path the desktop's own Pointer & desktop takes (below), so the desktop, which reads them each
second, follows within one.

## Decisions a reviewer would ask about

### Why a system call at all, and why jobs

A program with a window cannot use the `wifi` and `settings` commands through `SYS_kcmd`: `wifi join` asks its password
with `shell_ask_secret()` on a terminal, and a windowed program has none (the desktop starts it on `/dev/null`); the
commands answer in sentences for people, which a program would have to parse; and they wait (a scan up to 10 s, a join,
DHCP up to 24 s, a write of the stick), so the window could neither draw nor answer its close button meanwhile. So the
brief's "small structured interface" is one call, `SYS_netctl` (55), with an operation, a structure and its size:

- **Reads** answer at once and are never busy: the card and the station (`NETCTL_WIFI`), the networks heard
  (`NETCTL_NETWORKS`), the saved networks (`NETCTL_SAVED`), the network manager's table of settings
  (`NETCTL_SETTINGS`), the interfaces with their leases (`NETCTL_IFACES`), the last job (`NETCTL_JOB`). Each fills a
  zeroed structure of the kernel's and copies it out whole: the caller's memory is checked for exactly that size,
  writable, first and again just before the copy.
- **Jobs** are what waits: the card on or off, a scan, a join, a leave, the joined network saved, a saved network
  forgotten, a setting changed. The call checks the request, copies it and returns a job number at once; a kernel
  thread, `netctl`, does the work through the functions the commands use (`wlan_*`, `netman_*`), and `NETCTL_JOB`
  says its stage while it runs and its result, flags and reason when it is done, and the job's end is one `+NET:`
  line in the kernel log.

**One job at a time, for every program.** A second start is `-EBUSY`, whichever program makes it, so two Settings
windows or Settings and a test program cannot interleave half-done changes, and the record needs no per-program
bookkeeping. The station has its own one-request-per-device rule, so a job that meets the network manager's keeper or
a `wifi` command at work gets `-EBUSY` from the station, and says so.

**The thread is a kernel thread** made per job (`kthread_create_on(..., &conArray[0], ..., TF_DETACHED)`, as the
network manager's are). No user's kill reaches it (`task_user_may_kill()` refuses kernel threads), so a job always
ends, and wipes its key, also when its program was killed or closed meanwhile. Every wait inside it is bounded by the
station's, the stick's and DHCP's own timeouts.

**Sizes are exact.** Every structure has a fixed size, checked at compile time in `kernel/netctl.c` (and its numbers
against the station's, the network manager's and `syscall.h`'s), and the call refuses any other size: the size is the
interface's version. No structure holds a pointer, so the 32-bit kernel, the 64-bit host tests and any later program
agree on the layout.

### The theme and the pointer's speed: a step at once, one write

desk's review asked that a setting a person steps through (an arrow held down in a radio group repeats) not write the
stick at every step, as its own Pointer & desktop no longer does. The desktop reads `desktop.theme` and `pointer.speed`
from the network manager each second, so a step must reach the network manager at once without being written:

- `NETCTL_APPLY` (no job: it never waits) puts a step in force with `netman_apply()`, which `netcfg_set()`s the value
  into the settings in RAM under `nm_lock` as `netman_set()` does, and writes nothing. Only those two keys: they act at
  once and nothing else hangs on them (a step of `wifi` or `telnetd` would mean something else); any other is -EINVAL.
  Under `settings=off` nothing is put in force (1), as `netman_set()` changes nothing then.
- Settings' model keeps the last step of each key and, once that key's steps have stopped for 1.5 s, writes it with one
  `NETCTL_SET` job (the network manager's usual write, which also says whether it was saved). Two keys stepped are
  written one job after the other. Closing the window writes what is waiting first; when a job of its own runs then,
  the window closes once that job has ended and the write has started (the job ends in the kernel either way). A
  second press of the close button closes at once. A kernel without `NETCTL_APPLY` gets the write at once, as before.
- The radio groups are never greyed by a job, since a step needs none: a person stepping while the last step is
  being written loses no click.

### The password and the key

The brief: the passphrase comes in a buffer, becomes the PMK at once, the buffer is wiped, and it is never echoed,
logged or written.

1. The join's structure is checked to be the caller's to **write**, then copied to the kernel's stack, and the
   caller's passphrase field is wiped there and then, **whatever the call answers**: a busy kernel, a wrong length or a
   refused network all leave the program's memory without it. A structure that is not the caller's is refused
   (`-EFAULT`) before anything is read or written.
2. The cheap checks come before the expensive part: the name's length, the flags, a NUL within 64 bytes, a card that
   is on, no job running. Then the security (below), then `wpa_passphrase_to_pmk()` (PBKDF2-HMAC-SHA1, 4096 rounds, the
   SSID as the salt), and the copy is wiped.
3. The PMK goes into the job's record, to the station (`wlan_join_unless()`, which keeps its own copy for its
   reconnects, as for `wifi join`), and, when the request says so, to `netman_save_network()` (networks.conf's `psk`,
   as `wifi save` keeps it); it is wiped right after that save, or by the job's end on every other path (a failed
   join, a thread that could not start).
4. No read copies a key: `struct netctl_job` has no field for one, `NETCTL_WIFI` takes the station's snapshot without
   it, the saved networks come from `netman_networks()`, which never gives keys. The job's log line names the
   operation, the network and the outcome only.
5. In the program: the toolkit's password field (`GK_PASSWORD`) shows a dot a character and copies nothing; the
   window takes its text into a buffer on the stack and calls `gk_field_wipe()` before anything else, the model copies
   it into the request and wipes the window's buffer before the call, the kernel wipes the request's field, and the
   model and the window wipe what they held afterwards. The field is wiped too when the question goes and when the
   window closes. A password the model refuses (too short, too long, not printable ASCII) is wiped all the same, and
   asked again in an empty field.

**Saving with the join's key.** A join that asks to be saved saves the network the station is joined to with this
job's key only while the station's association is the one this job's request made (`join_requests`, as the network
manager tells its own associations from others): otherwise a `wifi join` typed in a Terminal between the join and the
save would have its network saved with this job's key. The test makes that happen and gets "not saved".

### What a join decides

What `wifi join` decides (`shell/wifi.c`'s `security_of()`, copied rather than shared, since the first phase edited no
existing file and the copy is short): among the networks of that name heard in the last minute, WPA2 (or WPA2/WPA3)
if any has it, since an open network of the same name is no reason to send the password in the clear nor to go without
it; else open if one is; else the strongest one's, refused when SurfOS does not join it (WEP, WPA3 only, enterprise,
other: `-ENOSYS`, and the window says why in the command's words). A password for an open network is refused
(`-EINVAL`): sent nowhere, but a sign the person meant another network. A WPA2 network without one is `-EACCES`.

**A name not heard in the last minute is looked for first.** The station's list keeps what it heard for a minute
(`SCAN_AGE`), and a person easily looks at a list, or types a password, for longer: the run against wlansim (below)
showed the rows going a minute after the scan. The call cannot wait for a scan, so it makes the PMK when a password
came, as always, and marks the request; the job then scans first, as `wifi join` does ("not heard yet: scan"), and
decides on what it heard as the call would have, with the refusals above in the job's `why`, which the window says. A
name still not heard is a hidden network, joined by a probe for its name, WPA2 when a password came and open otherwise,
and saved as hidden. Before this, such a join was saved as hidden although the network was not.

After a join the job asks for wlan0's address as the network manager asks for an association's (three tries 2 s apart,
quiet), and stops when the station is no longer on that association. It is a join like one typed in a Terminal: the
keeper leaves it alone and looks for saved networks again only once the station gives it up.

### The card

`NETCTL_CARD` on starts the card and, when the setting `wifi` is on and networks are saved, does what `wifi connect`
does: the first saved network in range joined, and the card handed back to the keeper. Without that, a card switched
off and on from Settings would stay unjoined, since the keeper treats a stop as the owner's (as after `wifi down`).
`netman_wifi_connect()` prints on its caller's console, which for the job's thread would be F1 behind the desktop, so
`netman_wifi_connect_quiet()` beside it says the same things as the keeper does, as `+NET:` lines in the kernel log;
the existing function is unchanged.

### The model

`user/wsettings/settings.c` is the program less its window: it holds the kernel's last answers, groups the access points
into a row per name (the security a join would use, the strongest signal), keeps the lists' selections by name so that
they follow a network when the list moves, starts the jobs, polls the running one on each tick and says its stage and
its outcome in words, and decides what to ask before acting (an open network while a telnet or FTP server without a
password runs, with the `wifi` command's warning; a WPA2 network's password, with "Save it"; a network by its name;
forgetting; `telnetd` at startup, with the `settings` command's warning before the change; closing with a host name
typed and not set). A network joined by its name with no password is an open one, and is asked about as one from the
list while a server without a password runs. After a change it says when the change acts, in the `settings` command's words (`when()` in
`shell/settings.c`), and what came of a change not saved, or made under `settings=off`. It reads the kernel's answers
every 2 s and when a job ends, and asks for a drawing only when what its page shows changed (the ages of what was
heard change every second; the rows do not). The first look at the Wi-Fi page with the card on starts a scan by
itself, quietly (a busy card is no news), and so does the card's start when nothing is heard yet, and a look at the
page again when nothing was heard in the last minute. What a scan heard leaves the kernel's list after a minute
unheard: the rows go, the band's count of them with them (other words in the band stay), and the note under the list
says that Scan looks again. Settings does not scan every minute by itself: a scan while joined takes the card off its
channel for a moment, and every job is a line in the kernel log. Error numbers are the
kernel's, written out in the model (`E_*`), because the host's differ (macOS's `ETIMEDOUT` is 60); `sys.c` checks they
are libsurf's at compile time.

### The window

On kit's toolkit, the pattern of `user/wkit/`: a window of 600 x 400 (at least 560 x 360, resizable) with five tabs
(F5 to F9) and a band at the bottom, two lines, for what the last change did. Decisions:

- **The questions are a panel in the tabs' place**, not the toolkit's modal boxes: those have two lines of text, and
  the `settings` command's warning about telnetd is six lines at this width. The panel's words are wrapped to the
  window (`ws_wrap()`), so no warning is ever cut; the tests check every line is drawn. A question's safe answer
  (Cancel) has the focus; Esc is No; Enter in the password field joins.
- **Text the window wraps** (the band, the station's line, notes, the panel) is drawn by the program in areas, at the
  area's width, the last line cut with the desktop's dot when there is more.
- **While a job of this program's runs**, what would start another is greyed, and a switch shows the value asked for
  (the card off while it stops, telnetd on while it is written), not the kernel's still-old one.
- **Widgets change only when what they show changes** (`show_model()` compares first), so a tick that changed nothing
  presents nothing; the test checks it.
- **Menus:** File > Close (Ctrl+W), View (the pages, F5 to F9), Wi-Fi (Scan, Ctrl+R; Join a network by its name,
  Ctrl+J; Disconnect), Help > About Settings.
- **The registry:** `rootfs/apps/wsettings.app`, in Apps > Programs and with a tile; no file types (Settings opens no
  files).

## What was left out, and why

- **A port for telnetd.** The System page says `telnetd.port` but does not change it: the brief's page is "telnetd at
  startup" and history. `settings telnetd.port N` in a Terminal does it.
- **Joining a particular access point** (`wifi join <ssid> <bssid>`): the window joins by name, the strongest access
  point, as the network manager does.
- **`wifi connect` as a button.** The card's start does it when it should (above).
- **A libsurf wrapper for the call.** `user/libsurf/syscalls.c` is shared and no track's; the program makes the call
  itself (`sys.c`) and `netctl.h` carries the number, which `kernel/netctl.c` checks against `syscall.h`'s.
- **A smoke-test block and a wlansim scenario for the screen.** The guiapps boot has no network and no simulated Wi-Fi,
  and `tools/qemu-run.py` and `tools/qemu-usb.py` were not among the files this track could change. The kernel side's
  joins are host-tested against stand-ins, and were run once by hand against the real kernel with wlansim (below); a
  permanent scenario is for the integrator to decide.

## Tests

| Test | Checks | What |
|---|---|---|
| `test_netctl` | 369 | the kernel side: every op's size and pointer refused first, nothing written past a structure; the reads; jobs one at a time, their stages and numbers (wrapping); the join's security, and a name not heard looked for first by the job (heard then: decided so, its refusals in words; still not: hidden); the password wiped whatever the answer; the PMK against IEEE 802.11i-2004 annex H.4.1's vector, handed to the station and to the save, wiped after; DHCP while the association lasts; a save refused after another join; the card with the quiet connect; leave, save, forget, set; a step put in force with no job and no write, only for the desktop's two keys; out of memory; a thread that cannot start; nothing allocated and no lock held after each |
| `test_wsettings` | 321 | the model: words (SSIDs with controls, bad UTF-8, C1, surrogates, overlong forms; wrapping), rows per name, selections that follow, joins of every kind and their refusals (the kernel's words for what its scan found), the password never in the model, what was heard going after a minute (the count with it, a scan at the next look), busy and wait, the card, leave, save, forget, settings with their warnings and `when()` texts, steps in force at once and written once when they stop (two keys one after the other, another program's job waited for, before closing, a kernel without the step), the host name's rule, the desktop's choices, interfaces and leases, About, a kernel without the call, closing, drawing only on change; every call's size as it should be |
| `test_wsettings_win` | 140 | the window on the fake desktop: opening, the scan by itself and the greying meanwhile, the networks in their columns, the rows gone after a minute unheard and the note, a WPA2 join (dots, a password too short refused and wiped, Enter, the field wiped at once, nowhere in the window's memory), the open network's warning in full, by name, the card, forget, telnetd's warning in full, history, the host name, closing with one typed, the desktop's choices stepped by a click and by arrows held down (each in force, the stick written once), About, menus and shortcuts, nothing presented when nothing changed, the theme |
| `tools/gui_apps_settings.py` | 23 (guiapps) | on the desktop of a USB boot: the window, its pages, telnetd's warning on the screen, every line, history and the theme through the kernel to the network manager (the desktop turning dusk at once and back, each written once the steps stop), About, Ctrl+W (surfsh collects the program, ps finds none), and Apps > Programs > Settings |

All three host tests also run under clang's AddressSanitizer and UndefinedBehaviorSanitizer on the Mac (by hand:
sanitizers stay out of `make unittest`), and with the Docker image's `gcc -m32`. Mutations checked that they fail: the
caller's password not wiped, the PMK not wiped at a job's end, the save's association not checked, a second job not
refused, an address kept not taken, the model's copy of a password not wiped, a name not heard joined without the
job's scan, one still not heard not taken as hidden, a scan's count kept in the band after its rows went, no scan at
a look with nothing heard.

**By hand, against the real kernel with wlansim** (a script in the session's scratch, not in the tree): the USB
build's stick folder made into a one-stick image as `qemu-usb.py`'s settings scenario makes it, GRUB's entry given
`wlansim`, a USB Ethernet adapter for wlansim's bridge, the O2 build; Settings on the desktop, driven through the
screen: the card on (nothing saved, so the network manager had left it off) and the scan after it, the networks in
their columns; SurfSimWPA joined with the test passphrase, dots in the field, an address, saved (networks.conf read on
the host: the key PBKDF2 makes, never the passphrase); Disconnect; Forget, asked first, and gone from networks.conf; a
minute later the rows gone and the note saying so; By name (Ctrl+J) for SurfSimWPA, looked for first ("Looking for
SurfSimWPA..."), joined and saved as a network that is not hidden; the card off, and on again joining the saved
network by itself; the passphrase on neither the serial line nor anywhere new on the stick (the kernel image holds
wlansim's own copy). The first run found the minute's rows going, which no host test had shown: the fixes above (the
join's scan, the rows' note) come from it.

## Where the code comes from

Written for this tree, from its own code and these documents; no other program's source was used or recalled.

- **IEEE 802.11-2016** (and 802.11i-2004 annex H.4.1 for the PBKDF2 test vector): the passphrase of 8 to 63 printable
  characters and the PMK; the kernel's own `wpa_passphrase_to_pmk()` (`net/wlan/wpa.c`) does the work.
- **RFC 3629** (UTF-8): which byte sequences the model shows as text; the rest is `\xHH`, as the kernel's
  `ieee80211_ssid_text()` decides for the terminal.
- **Intel's Software Developer's Manual, volume 2A, CPUID**: the vendor and brand string leaves, and the EFLAGS.ID
  test for CPUID's presence (About's processor).
- **This tree:** the decisions and the words of `shell/wifi.c` and `shell/settings.c` (the join's security, the
  refusals, the warnings, when a change acts), the network manager's way of getting an association's address
  (`kernel/netman.c`), `net/netcfg.c`'s rule for a host name, `kernel/win.c`'s and `kernel/syscall.c`'s way of
  checking a program's pointers and sizes, `tests/host/test_win.c`'s way of testing kernel code on the host, kit's
  `user/wkit/` for a program on the toolkit, and the desktop's colours and look.
- **Word wrapping** at spaces, a longer word cut, is the common way of filling lines; the function is this file's.
- **Behaviour followed:** a settings program's usual shape (pages, a list of networks with signal bars, a password
  box) is common to many systems; no product's look, names or artwork were copied, the icon was drawn here, and the
  words are SurfOS's own commands'.
