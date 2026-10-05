# Docker development environment

The `ai-dev` Makefile needs modern GCC, binutils, NASM and Python 3. The older
Debian etch/GCC 3.3 restoration environment fails on `-fno-pie` and is incompatible
with this branch. Keep the modern flags: they disable PIE, stack protection and
other host features that cannot be used in this freestanding kernel.

This image uses Debian bookworm with GCC multilib on `linux/amd64`, producing ELF32
x86 kernels. Docker emulates amd64 on Apple Silicon. The host blibc tests contain
x86 inline assembly too, so they must run in the x86 container rather than on an
ARM host. The image includes Python 3, mtools for the disk images, QEMU for the test
suite, and GRUB's EFI files and OVMF for the USB image. No host compiler installation
or Documents bind mount is required. The source revision recorded in the USB image's
manifest is passed in from the host's git (the container's copy has no `.git`).

```sh
./devenv/build.sh             # kernel, user programs, test disk and initrd
./devenv/run.sh               # VGA window, serial console on this terminal
./devenv/run.sh -display none # headless; use the serial shell
./devenv/boottest.sh 15 help  # PS/2 keyboard + VGA screenshot
./devenv/build.sh O=2         # optimized kernel and user programs
O=2 ./devenv/run.sh           # run the optimized kernel
./devenv/build.sh unittest   # native host blibc tests inside Docker
./devenv/build.sh test-all   # complete QEMU suite at O0 and O2 inside Docker
./devenv/build.sh clean      # successful even though no kernel remains
```

The USB image for UEFI PCs (`docs/USB-BOOT.md`):

```sh
./devenv/build-usb.sh         # build/usb/surfos-ideapad-15iil05.img (+ .sha256, build-manifest.json), O=2
O=0 ./devenv/build-usb.sh     # the same with the debug build
./devenv/test-usb.sh          # boot that file under host QEMU + OVMF from an emulated USB stick: 8 scenarios
./devenv/test-usb.sh --screen # just boot it without a serial port and print the screen
./devenv/run-usb.sh           # boot a copy of it in a QEMU window to try it by hand
```

`build-usb.sh` runs `make usb-image` in the container (GRUB 2.06's x86_64-efi files,
grub-mkstandalone and mtools are in the image) and copies the result to `build/usb/` with
its checksum verified. The test and run scripts need `qemu-system-x86_64` and its EDK2
firmware on the host (Homebrew's `qemu` ships both). None of them writes to a disk device.

Docker Desktop must be running, and the invoking process must have access to its
socket. Host `run.sh` and `boottest.sh` require `qemu-system-i386` on PATH.

The image `surfos-modern-toolchain` and volume `surfos-modern-build` are separate
from the original restoration environment. Source edits and deletions are synced
into the volume; generated `build/` files stay there for incremental builds.
Docker rebuilds the image using its layer cache each invocation so Dockerfile
changes take effect. A failed build returns a nonzero status and keeps the last
successful export. The build log records the make output.

From a standalone clone, successful exports go to `build/docker/`, with the
default kernel at `build/docker/build/O0/surfos.bin` and images at
`build/docker/build/images/`. The log is `build/docker-build.log`.
`SURFOS_OUT` and `SURFOS_LOG` override these locations. The parent restoration
project's `devenv/` wrappers set these to `build/surfos/` and `build/build.log`.

Script regression tests (no Docker needed):

```sh
python3 -m unittest discover -s tests -p test_devenv.py
```

These check wrapper behavior and failure handling; they do not compile or boot
the kernel. Actual verification requires `build.sh test-all` and a host QEMU boot.
