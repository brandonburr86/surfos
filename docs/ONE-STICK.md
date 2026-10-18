# One stick for everything: the quick steps

*Written 2026-10-17.* Your 30 GB FAT32 data stick can be your only USB stick: SurfOS boots
from it, keeps your files on it, and a new kernel is two files copied onto it, with no
image to flash. The full guide, with how it works and every caveat, is
[USB-BOOT.md section 14](USB-BOOT.md#14-one-stick-for-everything).

Run the commands below from the `06-restoration` folder in Terminal. Where it says
`<stick>`, use the stick's name as Finder shows it under *Locations* (for example
`/Volumes/DATASTICK`).

## Set up the stick (once)

1. Make sure there is a build: `devenv/build-usb.sh` writes `build/usb/stick/` (the
   2026-10-17 release is already there).
2. Plug the data stick into the Mac, then:

   ```sh
   tools/stick.py check   /Volumes/<stick>            # what the stick holds now
   tools/stick.py install /Volumes/<stick> --eject    # make it start SurfOS, then eject it
   ```

   `install` copies the boot loader (`EFI/BOOT`), the menu (`boot/grub`), a fallback
   build (`boot/`) and the development build (`surfos-dev/`) onto the stick as ordinary
   files. Your other files stay as they are; an old build in `surfos-dev` is replaced. It
   never formats or partitions anything, and it refuses the flashed SURFOSUSB stick.

## Start SurfOS from it

1. Plug that stick into the laptop **on its own** and power on. With USB first in the
   boot order it starts by itself; if it does not, tap **F12** at power-on and choose
   the stick (its entry names its maker, such as *USB Disk 2.0*).
2. The menu:
   * **SurfOS (development build from this stick)** starts after 5 seconds, with the
     network and the telnet server (no password: on a network you do not trust, choose
     **SurfOS (without the settings on the stick)**, further down, instead).
   * **SurfOS** starts the fallback build in `boot/`: choose it with the arrow keys when
     a development build misbehaves. A build from 10/17/26 on there reads the settings, so
     it starts the telnet server too when they say `telnetd = on` (an older one reads none:
     `tools/stick.py install /Volumes/DATASTICK --fallback` puts the new build there too).
   * **SurfOS (without the settings on the stick: no Wi-Fi joined, no telnetd)** starts
     the fallback build without the settings: no network joined, no telnet server.
3. Check that SurfOS knows its stick: `lsblk` should show the stick's partition with
   `boot drive: writable`. `fdisk` and `mkfs` refuse that stick; its files may be changed.
4. To use your files: `mount usb0p1 /data` (use the name `lsblk` shows), and `eject usb0`
   before you pull the stick out.

## Each new build

A build is two files that belong together, `surfos.bin` (the kernel) and `initrd.tar`
(the programs). Either way, they go into the stick's `surfos-dev` folder:

* **With the stick left in the laptop** (nothing to plug in or out):

  ```sh
  devenv/build-usb.sh && tools/devloop.py deploy
  ```

  `deploy` writes the build onto the stick SurfOS started from, ejects it and restarts
  the laptop into it. With the USB Ethernet adapter the laptop comes back by itself, and
  over Wi-Fi too once the network is saved on the stick (`wifi save`) and `telnetd = on`
  is set ([USB-BOOT.md section 15](USB-BOOT.md#15-wi-fi-saved-networks-and-the-settings-on-the-stick)).

* **On the Mac:**

  ```sh
  devenv/build-usb.sh
  tools/stick.py update /Volumes/<stick> --eject      # only surfos-dev changes
  ```

To make a development build that has proved itself the fallback too:
`tools/stick.py install /Volumes/<stick> --fallback --eject`.

## Keep in mind

* **Keep the flashed SURFOSUSB stick as a rescue stick.** SurfOS only reads it, and
  `tools/stick.py` never writes it. If the one stick ever stops starting (a bug in a
  development build, or a crash while it was being written), `tools/stick.py install`
  on the Mac puts the boot files back in a minute, and the rescue stick starts SurfOS
  meanwhile.
* Do not name your stick SURFOSUSB, and never copy the stick folder onto the flashed
  stick.
* Eject the stick before pulling it out, on the Mac (Finder, or `--eject`) and in
  SurfOS (`eject usb0`).
* `tools/stick.py check /Volumes/<stick>` tells you what is on a stick, which builds
  they are, and whether SurfOS starts from it.
* *Added 10/17/26:* the stick also keeps your settings and saved Wi-Fi networks, in its
  `surfos-config` folder: `wifi save` while joined, `settings telnetd on` and the rest
  ([USB-BOOT.md section 15](USB-BOOT.md#15-wi-fi-saved-networks-and-the-settings-on-the-stick)).
  The saved network's key is in that folder: whoever has the stick can join your Wi-Fi.

## What to tell Claude after the first try on the laptop

Whether the laptop started the stick by itself or needed F12, and what `lsblk` printed.
It goes in [the hardware record](hardware/ideapad-3-15iil05.md).
