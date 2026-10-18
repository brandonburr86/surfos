# Brief: the network manager and the settings on the stick

Requested by the owner on 2026-10-17, for the x86 laptop (branch `netman/main`, a worktree of
`ai-dev`, apart from the other agents' branches): "a more comprehensive network manager ... so that
I can save the settings of the wireless network that I'm joining to as a configuration file. When I
load up the computer, it'll automatically join to the wireless network that I'm choosing, and then
it will join it and connect via DHCP." And a settings file, "for example, whether or not to have
Telnet on by default", saved to the USB drive in use and loaded at boot.

## What changes for the owner

* `wifi join HouseOfBurr` (the password typed at the laptop, as before), then **`wifi save`**:
  SurfOS writes the network into `surfos-config/networks.conf` on the stick it was started from.
  At every start it then brings the card up, joins the first saved network in range and asks for
  an address (DHCP), with nobody typing. `wifi saved` lists them, `wifi forget <ssid>` removes
  one, `wifi save <ssid>` saves one that is not in range (asking its password), `wifi connect`
  does now what the start does.
* **`settings`** lists the settings and `settings <key> <value>` changes one, written at once to
  `surfos-config/settings.conf` on the stick: `telnetd` (on/off: the telnet server at startup;
  off by default, it has no password), `telnetd.port`, `wifi` (on/off: join a saved network at
  startup), `ethernet` (dhcp/off: an address whenever an Ethernet link comes up) and `hostname`
  (the name DHCP requests carry, which the router lists).
* Both files are plain text the Mac can read and edit. The stick is mounted only for the moment a
  file is read or written: nothing stays mounted by itself.
* GRUB's menu gets **SurfOS (without the settings on the stick)** (`settings=off`): the way out
  if a setting ever gets in the way.

## Decisions (made here; the owner asked for the feature, not each choice)

* **Where:** the volume SurfOS was started from (`bootvol=`, the one-stick setup of 10/17/26):
  the only volume SurfOS may write that is surely the owner's own stick. A flashed SURFOSUSB stick
  is read (a folder put there on the Mac works) but never written. With no `bootvol=` (QEMU's
  `-kernel`, the ISO), or with `settings=off`, there are no settings: nothing is read or joined
  and no host name is sent. The one change in every start: DHCP leases are renewed (RFC 2131),
  and given up when they run out.
* **The Wi-Fi key, not the password.** The 2026-10-09 rule was that the password is never written
  to a file. A network that joins by itself at startup needs something stored, so `wifi save`
  stores the network's PSK (the 256-bit PMK, PBKDF2 of the password and the SSID, as
  wpa_supplicant's `psk=`), never the password; whoever has the file can join that network (said
  in the file and the guide), but the password, which may be used elsewhere, cannot be read back.
  A network may also be written on the Mac with `password = "..."`; SurfOS turns it into the key
  when it reads the file, and a later save writes the key in its place. Agents still never ask
  for, type or store the owner's password: tests use wlansim's test network.
* **telnetd at startup** is the owner's choice now (`telnetd = on`), off by default; the kernel
  word `autonet` (GRUB's development entry) still starts it whatever the file says. Either
  starts it only once there is an address on Ethernet or a WPA2 network, never because of an
  open one, and while a server runs or is to start the manager joins no open network by itself
  (first review, 2026-10-17: saved networks must not open a passwordless shell to a café).
* **Defaults keep today's behaviour:** without a settings file, or with one that sets nothing,
  a start is what it was (no telnetd, no DHCP by itself, the card left alone until a network is
  saved).
* **Order:** the saved networks are tried in the file's order; `wifi save` puts the network it
  saves first ("the one I'm choosing").
* **Manual commands win:** `wifi leave` and `wifi down` stop the automatic joining until
  `wifi connect` (which hands it back) or until a network joined by hand is lost for good; a
  network joined by hand is left alone; when the station gives up on a network (the access
  point gone), the manager looks for a saved one every 30 s, later every 2 minutes. A leave or
  down typed while the manager is at work calls off its next request (the station's `stops`).
* **What SurfOS cannot read in networks.conf is kept**, as it was, and never taken for an open
  network; a save never drops what was written by hand.

## Pieces

| Piece | Where | What |
|---|---|---|
| The files' format | `net/netcfg.c`, `include/net/netcfg.h` | pure: parse and edit `settings.conf` keeping the owner's lines and comments, parse and write `networks.conf`, SSIDs quoted with escapes, the DHCP lease timers; host-tested (`tests/host/test_netcfg.c`) |
| The manager | `kernel/netman.c`, `include/net/netman.h` | the settings in RAM, the boot volume read and written through a short mount (or the owner's own mount), the `netman` thread (settings at boot, Ethernet DHCP on link, lease renewals, telnetd at startup) and `wifiman` (the Wi-Fi keeper) |
| autonet | `kernel/autonet.c` | unchanged lines for Ethernet; every Ethernet link in its 90 s gets a lease, a Wi-Fi address is the keeper's; telnetd only for a trusted address |
| DHCP | `net/dhcp.c` | the lease per interface (server, time, obtained), renewal (`dhcp renew`), the host name (option 12); `dhcplease=` caps lease times for the tests |
| The station | `net/wlan/wlan.c`, `include/net/wlan_cmd.h` | `wlan_station()`: state, SSID, security, hidden, why it is idle, a join counter, and the key for `wifi save` |
| The boot volume | `driver/part.c`, `include/sys/bdev.h` | `bdev_boot_vol()`: the volume GRUB was started from, writable or not |
| Commands | `shell/settings.c`, `shell/wifi.c`, `shell/main.c` | `settings`, `wifi save/forget/saved/connect`, `dhcp renew` |
| The menu | `boot/grub-usb.cfg` | the entry without the settings, after the existing ones (the tests pick entries by position) |
| Tests | `tools/qemu-run.py`, `tools/qemu-usb.py` | smoke steps; boots with a settings disk that persist across reboots (wlansim: save, reboot, joined by itself, DHCP, telnetd); a USB scenario through GRUB on a one-stick |
| Docs | `docs/USB-BOOT.md` section 15, `ARCHITECTURE.md` section 19, `CLAUDE.md`, `ONE-STICK.md` | the guide, the design, the rules |

## Rules (as for every track)

* Every commit builds, passes `make unittest` and `make test-all`; anything `profile=laptop` or USB
  touches also `make test-usb` at -O0 and -O2.
* Kernel commands follow `CLAUDE.md`: status codes, `curTask->con`, no shared statics,
  `shell_ask()` / `shell_ask_secret()`, give up when interrupted.
* Nothing logs a key or a password; the settings and networks listings never show a key.
* Every write to the stick goes to a new file that is renamed over the old one, read back and
  checked, then the stick is unmounted again; `fdisk` and `mkfs` are untouched.
