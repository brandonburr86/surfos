# Wi-Fi for SurfOS on the IdeaPad 3 15IIL05: what is needed before building it

Status: **tabled** by the owner on 2026-10-06, to be picked up later. Nothing is built
yet. This note keeps what was worked out: what the owner needs to supply, the decisions
to make, what the result would look like, and how big the job is.

The goal: SurfOS reaches the network through the laptop's internal wireless card. A
command lists the networks in range (their SSIDs), joins one with its password, and then
gets an address by DHCP on its own.

## Which card is it?

Everything depends on the exact wireless chip, because each chip needs a completely
different driver. Lenovo's specification sheet (PSREF, IdeaPad 3 15IIL05, August 2022)
lists only two kinds of card, both on an M.2 module with Bluetooth 5.0:

* 802.11ac 2x2 Wi-Fi + Bluetooth 5.0
* Wi-Fi 6, 802.11ax 2x2 Wi-Fi + Bluetooth 5.0

It does not name the chip. On this Intel 10th-generation (Ice Lake) platform that is in
practice either an Intel card (Wireless-AC 9560 or Wi-Fi 6 AX201, both "CNVi" parts whose
radio sits on the M.2 module and whose controller is in the chipset) or a Realtek card
(for example the RTL8822CE). Intel and Realtek cards share nothing at the driver level.

## What the owner needs to supply

1. **The card's exact hardware IDs** (the single most important item). Any one of:
   * **SurfOS itself** (since 2026-10-07, the easiest): boot it on the laptop and run
     `wifiinfo`. It prints the chipset, every wireless PCI device in full (vendor, device,
     subsystem, revision, power state, interrupts, PCI Express link, and all 256 bytes of
     its configuration space), with what the ids probably are, and the USB devices, among
     them the Bluetooth half of the Wi-Fi card, whose id names the card (`8087:0026` an
     AX201, `8087:0aaa` a 9560). Send the whole output: copy it from a telnet session on the
     Mac (`telnetd on`, `docs/USB-BOOT.md` section 11), or save it on a data stick with
     `wifiinfo > /data/wifi.txt`. `lspci -v` shows every PCI device the same way, and
     `lspci 00:14.3` one in full.
   * **Windows**, if it is still on the laptop: *Device Manager* → *Network adapters* →
     the Wi-Fi adapter's name; then *Properties* → *Details* → *Hardware Ids*, the first
     line, which looks like `PCI\VEN_xxxx&DEV_xxxx&SUBSYS_xxxxxxxx&REV_xx`.
   * **An Ubuntu live USB** booted on the laptop: the output of `lspci -nnk`, and of
     `sudo dmesg | grep -iE 'iwlwifi|rtw|ath'`. The second names the exact firmware file
     and the chip revision the card uses.
2. **A recording of Linux joining the network**, if an Ubuntu live USB can be booted:
   commands to be supplied when the work starts. It shows the sequence of commands that
   works with this exact card and firmware, the biggest single way to lower the risk.
3. **The network's settings**, from the router's admin page, or the Mac (hold *Option* and
   click the Wi-Fi icon in the menu bar: *Security*, *Channel* with its band and width, *PHY
   Mode*), or `iw dev wlan0 scan` in Linux, `netsh wlan show networks mode=bssid` in
   Windows. SurfOS cannot read them before it has the driver:
   * Security: **WPA2-Personal** is the target; WPA2/WPA3 mixed ("transition") mode is
     fine (SurfOS would use WPA2); WPA3-only is much harder; Enterprise (802.1X) logins
     are out of scope.
   * Whether Protected Management Frames (802.11w) are required.
   * The band (2.4 or 5 GHz) and channel; whether the network name is hidden; the
     country (it decides which channels may be used).
   * A phone hotspot set to WPA2 on 2.4 GHz is an ideal first test network.
   * **Not the password.** It is typed into SurfOS when joining, and must never be put in
     files, logs or chats.

## Decisions to make

* **Firmware.** These cards do not work without the manufacturer's binary firmware file
  (1-2 MB, distributed in Linux's `linux-firmware` collection under a license that allows
  redistribution unchanged). It would ship on the boot stick. Is that acceptable?
* **Testing on the laptop.** QEMU cannot emulate any Wi-Fi card, so every step of the
  driver is tested on the real laptop: write the stick, boot, try, report. Expect many
  rounds. Ways to make each round cheaper:
  * keep the USB Ethernet adapter plugged in during development, so SurfOS can send its
    debug log over the wired network to the Mac, where it can be read directly;
  * if the laptop's firmware can boot over that adapter, load the kernel over the network
    so a new build only needs a reboot, not a new stick.
* **Scope of a first version:** scan, join with WPA2 and a password, then DHCP. WPA3,
  roaming and power saving later.
* **A fallback if the internal card resists:** a cheap USB Wi-Fi dongle with open-source
  firmware (for example one with the Atheros AR9271 chip). It would reuse everything except
  the chip driver.

## What the result would look like

```
wifi scan                # network names with signal, channel and security
wifi join "My Network"   # asks for the password (hidden as you type), joins, then DHCP
wifi status              # connection, signal, IP address
wifi leave
```

The card would appear as `wlan0`; SurfOS's existing DHCP, DNS and TCP would run on it
unchanged.

## How big the job is

Bigger than anything SurfOS has had so far. Three layers:

1. **The chip driver**: loading the firmware, the card's command interface, sending and
   receiving packets through its rings. Only testable on the laptop.
2. **The 802.11 layer**: scanning (beacons and probe responses), authentication and
   association, converting between 802.11 and Ethernet frames.
3. **WPA2 security**: turning the password into a key (PBKDF2 with the SSID), the 4-way
   handshake with the router, AES-CCMP encryption (partly done by the card itself).

Layers 2 and 3 can be built and tested on the Mac first, against the published test
vectors; layer 1 cannot. Realistically several milestones.

**The first step, when this is picked up again:** the hardware IDs (item 1 above): run
`wifiinfo` on the laptop and send its output.
