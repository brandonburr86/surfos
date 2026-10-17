#!/usr/bin/env python3
"""One stick for everything (docs/USB-BOOT.md section 14): SurfOS's boot files and its builds onto a FAT32 USB stick
that keeps your own files, from this computer, with no image to write (10/17/26).

    tools/stick.py check   /Volumes/NAME             what the stick holds: the boot files, both builds, free space
    tools/stick.py install /Volumes/NAME [--eject]    makes the stick start SurfOS: the boot loader (EFI/BOOT), the
                                                     menu (boot/grub), boot/ONE-STICK.txt, the development build
                                                     (surfos-dev/) and, when the stick has none yet, the fallback
                                                     build (boot/); later again, to bring the loader and the menu up
                                                     to date
                           [--fallback]              ... and make this build the fallback too
    tools/stick.py update  /Volumes/NAME [--eject]    a new development build into surfos-dev/, nothing else
                           [--kernel F --initrd F]   ... from these two files instead of the stick folder
    tools/stick.py kit                               only checks the stick folder against its manifest

The files come from the stick folder devenv/build-usb.sh makes next to the image (build/usb/stick/, or --kit), each
checked against the build's manifest (build-manifest.json beside the folder) before anything is written. Every file
is written as NAME.surfos-new beside its place and read back through the file system, and only once all of them are
there are they renamed over the old ones, in order: in surfos-dev/ the initrd before the kernel (as tools/devloop.py
does), and when installing the boot loader last, so a stick that is pulled out halfway does not start a half-written
system. Nothing else on the stick is touched: it is never formatted or partitioned, nothing is deleted but this
tool's own temporary files and the folders it made for them, and a boot loader or GRUB menu of another system at
SurfOS's paths stops it before it writes anything.

The path must be where a FAT volume of an external (USB) disk is mounted: diskutil says so on macOS, and
/proc/self/mounts and sysfs on Linux. The computer's own disks, its EFI system partition included, are refused, and
so is the flashed SurfOS image (label SURFOSUSB): SurfOS only reads that stick, and it stays the rescue stick.

Exit status 0; 1 with the reason.
"""
import argparse
import hashlib
import importlib.util
import io
import json
import os
import plistlib
import shutil
import subprocess
import sys
import tarfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KIT = os.path.join(REPO, 'build', 'usb', 'stick')
MANIFEST = 'build-manifest.json'                # beside the stick folder (tools/mkusb.py writes both)

LOADER = 'EFI/BOOT/BOOTX64.EFI'
MENU = 'boot/grub/grub.cfg'
MARKER = 'boot/ONE-STICK.txt'                   # without it GRUB's menu does not look in surfos-dev on its own stick
INFO = 'boot/BUILD-INFO.txt'                    # describes the build in boot/
DEV = ['surfos-dev/initrd.tar', 'surfos-dev/surfos.bin']        # renamed in this order: the kernel last
FALLBACK = ['boot/surfos.bin', 'boot/initrd.tar']
# install: everything the loader starts is in place before the loader is
INSTALL = FALLBACK + [INFO, 'boot/grub/x86_64-efi/fs.lst', MARKER, MENU] + DEV + [LOADER]
NEW = '.surfos-new'                             # a file being written
SLACK = 1 << 20                                 # bytes left free on the stick after the new files

# what SurfOS's own files hold, so that another system's boot loader or menu at the same path is never replaced
OURS = {LOADER: lambda d: b'SurfOS USB bootstrap' in d,         # the configuration grub-mkstandalone embeds in it
        MENU: lambda d: d.startswith(b'# SurfOS USB image'),
        INFO: lambda d: d.startswith(b'SurfOS USB boot image'),
        'boot/grub/x86_64-efi/fs.lst': lambda d: not d.strip()}  # SurfOS's is empty; another GRUB lists its modules


class StickError(Exception):
    pass


def say(text):
    print(f'stick: {text}', flush=True)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def mkusb():
    spec = importlib.util.spec_from_file_location('mkusb', os.path.join(REPO, 'tools', 'mkusb.py'))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def mib(n):
    return f'{n / (1 << 20):.0f} MB' if n < 10 << 30 else f'{n / (1 << 30):.1f} GB'


# ---- the stick ---------------------------------------------------------------------------------------------------

def volume_info(path):
    """(what the volume is, in words; its FAT label or '') for the volume mounted at path; StickError unless it is a
    FAT volume of an external disk that may be written"""
    real = os.path.realpath(path)
    if not os.path.isdir(real) or not os.path.ismount(real):
        raise StickError(f'{path} is not where a volume is mounted (on macOS: /Volumes/<the stick\'s name>)')
    if sys.platform == 'darwin':
        r = subprocess.run(['diskutil', 'info', '-plist', real], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if r.returncode:
            raise StickError(f'diskutil does not know {real}: {r.stderr.decode(errors="replace").strip()}')
        return check_mac(real, plistlib.loads(r.stdout))
    if sys.platform.startswith('linux'):
        with open('/proc/self/mounts') as f:
            return check_linux(real, f.read())
    raise StickError('this tool knows macOS and Linux only: copy the files of the stick folder by hand '
                     '(docs/USB-BOOT.md section 14)')


def check_mac(real, info):
    """the decision on diskutil's answer, info (a dict from diskutil info -plist)"""
    fs = info.get('FilesystemType')
    where = f'{real} ({info.get("DeviceIdentifier", "?")}, {info.get("FilesystemName", fs)}, {info.get("BusProtocol", "?")})'
    if info.get('MountPoint') != real:
        raise StickError(f'{real} is not the mount point diskutil gives ({info.get("MountPoint")})')
    if info.get('Internal', True) or not info.get('RemovableMediaOrExternalDevice', False):
        raise StickError(f'{where} is on one of this computer\'s own disks: only a USB stick is written')
    if fs != 'msdos':
        raise StickError(f'{where} is not FAT. The stick has to be FAT32: Disk Utility, View > Show All Devices, the '
                         f'stick, Erase, MS-DOS (FAT), Master Boot Record (this erases it)')
    if not info.get('WritableVolume', False):
        raise StickError(f'{where} is mounted read-only')
    return where, info.get('VolumeName') or ''


def check_linux(real, mounts, sys_root='/sys', dev_root='/dev'):
    """the same on Linux: mounts is /proc/self/mounts; the disk must be removable or on USB (sysfs)"""
    entry = None
    for line in mounts.splitlines():
        f = line.split()
        if len(f) >= 4 and f[1].replace('\\040', ' ') == real:
            entry = f                                   # the last mount on a path is the one seen there
    if not entry:
        raise StickError(f'{real} is not in /proc/self/mounts')
    dev, fstype, opts = entry[0], entry[2], entry[3].split(',')
    where = f'{real} ({dev}, {fstype})'
    if real == '/' or real == '/efi' or real == '/boot' or real.startswith('/boot/'):
        raise StickError(f'{where} is this computer\'s own system or boot partition')
    if fstype not in ('vfat', 'msdos'):
        raise StickError(f'{where} is not FAT: the stick has to be FAT32')
    if 'ro' in opts:
        raise StickError(f'{where} is mounted read-only')
    name = os.path.basename(os.path.realpath(dev))
    node = os.path.realpath(os.path.join(sys_root, 'class', 'block', name))
    disk = os.path.dirname(node) if os.path.exists(os.path.join(node, 'partition')) else node
    try:
        with open(os.path.join(disk, 'removable')) as f:
            removable = f.read().strip() == '1'
    except OSError:
        removable = False
    if not (removable or '/usb' in disk):
        raise StickError(f'{where} is not on a removable or USB disk: only a USB stick is written')
    label = ''
    try:
        if os.path.realpath(os.path.join(dev_root, 'disk', 'by-label', 'SURFOSUSB')) == os.path.realpath(dev):
            label = 'SURFOSUSB'
    except OSError:
        pass
    return where, label


def eject(volume):
    if sys.platform == 'darwin':
        r = subprocess.run(['diskutil', 'eject', volume], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if r.returncode:
            raise StickError(f'diskutil eject {volume}: {r.stdout.strip()}')
        say(f'{volume} is ejected: the stick can be pulled out')
    else:
        say(f'unmount {volume} before the stick is pulled out (umount {volume})')


# ---- what goes onto it -------------------------------------------------------------------------------------------

def build_id(initrd):
    """etc/build-id of an initrd (its bytes)"""
    try:
        with tarfile.open(fileobj=io.BytesIO(initrd)) as t:
            for name in ('etc/build-id', './etc/build-id'):
                try:
                    return t.extractfile(name).read().decode(errors='replace').strip()
                except KeyError:
                    continue
    except tarfile.TarError as e:
        raise StickError(f'the initrd is no tar archive ({e})')
    raise StickError('the initrd has no etc/build-id: it was not built by this tree')


def load_kit(kit, manifest_path=None):
    """{path on the stick: bytes} of every file of the stick folder, each one checked against the manifest"""
    manifest_path = manifest_path or os.path.join(os.path.dirname(os.path.abspath(kit)), MANIFEST)
    try:
        with open(manifest_path) as f:
            manifest = json.load(f)
    except (OSError, ValueError) as e:
        raise StickError(f'no build manifest at {manifest_path} ({e}): devenv/build-usb.sh makes the stick folder')
    listed = (manifest.get('stick') or {}).get('files') or {}
    out = {}
    for rel in INSTALL:
        want = listed.get(rel)
        if not want:
            raise StickError(f'{manifest_path} lists no {rel}: build again with this tree (devenv/build-usb.sh)')
        try:
            with open(os.path.join(kit, *rel.split('/')), 'rb') as f:
                data = f.read()
        except OSError as e:
            raise StickError(f'the stick folder {kit} has no {rel} ({e.strerror}): devenv/build-usb.sh makes it')
        if len(data) != want.get('bytes') or sha256(data) != want.get('sha256'):
            raise StickError(f'{os.path.join(kit, rel)} is not what the build made (the manifest has another '
                             f'SHA-256): build again')
        out[rel] = data
    build_id(out['surfos-dev/initrd.tar'])
    return out


def load_build(kernel_path, initrd_path):
    """the development build from a kernel and its initrd, checked as tools/mkusb.py checks them"""
    mk = mkusb()
    try:
        mk.check_input(kernel_path, 'kernel')
        mk.check_kernel(kernel_path)
        mk.check_input(initrd_path, 'initrd')
    except mk.BuildError as e:
        raise StickError(str(e))
    with open(kernel_path, 'rb') as f:
        kernel = f.read()
    with open(initrd_path, 'rb') as f:
        initrd = f.read()
    build_id(initrd)
    return {'surfos-dev/initrd.tar': initrd, 'surfos-dev/surfos.bin': kernel}


# ---- writing -----------------------------------------------------------------------------------------------------

def place(volume, rel):
    return os.path.join(volume, *rel.split('/'))


def read_back(path):
    with open(path, 'rb') as f:
        return f.read()


def actual(volume, rel):
    """the file at rel on the volume, its names matched regardless of case as FAT matches them; None when not there"""
    path = volume
    for part in rel.split('/'):
        try:
            names = os.listdir(path)
        except OSError:
            return None
        match = part if part in names else next((n for n in names if n.lower() == part.lower()), None)
        if match is None:
            return None
        path = os.path.join(path, match)
    return path if os.path.isfile(path) else None


def foreign(volume):
    """the files at SurfOS's paths on the stick that another system put there (a boot loader, a GRUB menu)"""
    return [rel for rel, ours in OURS.items() if actual(volume, rel) and not ours(read_back(actual(volume, rel)))]


def write(volume, files, order):
    """files ({path: bytes}) onto the volume: each as .surfos-new, read back; then all renamed into place in order.
    On a failure the new files go, and so do the folders made for them while still empty"""
    need = sum(len(files[rel]) for rel in order)
    free = shutil.disk_usage(volume).free
    if need + SLACK > free:
        raise StickError(f'{volume} has {mib(free)} free; the files need {mib(need + SLACK)}')
    staged, made, done = [], [], []
    try:
        for rel in order:
            path = place(volume, rel)
            missing, d = [], os.path.dirname(path)
            while not os.path.isdir(d):
                missing.append(d)
                d = os.path.dirname(d)
            for d in reversed(missing):
                os.mkdir(d)
                made.append(d)
            tmp = path + NEW
            staged.append(tmp)
            with open(tmp, 'wb') as f:
                f.write(files[rel])
                f.flush()
                os.fsync(f.fileno())
            if read_back(tmp) != files[rel]:
                raise StickError(f'{rel}: what was written to {tmp} does not read back the same; nothing was renamed')
        for rel, tmp in zip(order, list(staged)):
            os.replace(tmp, place(volume, rel))
            staged.remove(tmp)
            done.append(rel)
            say(f'{rel}: {len(files[rel])} bytes, sha256 {sha256(files[rel])[:16]}...')
        made = []
        os.sync()
    except OSError as e:
        raise StickError(f'writing {volume} failed ({e.strerror or e}): ' +
                         (f'in place: {", ".join(done)}; ' if done else 'nothing was renamed into place; ') +
                         'run the same command again once the stick is back')
    finally:
        for tmp in staged:                              # written, never renamed
            try:
                os.remove(tmp)
            except OSError:
                pass
        for d in reversed(made):                        # made for them, and empty again
            try:
                os.rmdir(d)
            except OSError:
                pass


# ---- check -------------------------------------------------------------------------------------------------------

def state(volume, kit_files=None):
    """what the stick holds: [(what, path, status)], whether it starts SurfOS, whether the development build is
    started"""
    other = foreign(volume)

    def have(rel):
        return os.path.isfile(place(volume, rel))

    def same(rel):
        if not have(rel):
            return 'missing'
        if rel in other:
            return 'another system\'s, not SurfOS\'s'
        if kit_files is None or rel not in kit_files:
            return 'there'
        return 'there, as in the stick folder' if read_back(place(volume, rel)) == kit_files[rel] else \
               'there, not the stick folder\'s'

    def build(paths):
        if not all(have(p) for p in paths):
            return 'missing' if not any(have(p) for p in paths) else 'incomplete: ' + ', '.join(
                p for p in paths if not have(p)) + ' missing'
        try:
            bid = build_id(read_back(place(volume, next(p for p in paths if p.endswith('initrd.tar')))))
        except StickError as e:
            return f'there, but {e}'
        newer = ''
        if kit_files is not None and all(p in kit_files for p in paths):
            newer = ', as in the stick folder' if all(read_back(place(volume, p)) == kit_files[p] for p in paths) \
                    else ', not the stick folder\'s build'
        return f'build {bid}{newer}'
    rows = [('boot loader', LOADER, same(LOADER)), ('menu', MENU, same(MENU)), ('marker', MARKER, same(MARKER)),
            ('fallback build', 'boot/', build(FALLBACK)), ('development build', 'surfos-dev/', build(DEV))]
    starts = have(LOADER) and have(MENU) and all(have(p) for p in FALLBACK) and not other
    dev = starts and have(MARKER) and all(have(p) for p in DEV)
    return rows, starts, dev


def check(volume, where, kit, manifest_path):
    try:
        kit_files = load_kit(kit, manifest_path)
    except StickError:
        kit_files = None
    usage = shutil.disk_usage(volume)
    print(f'{where}: {mib(usage.total)}, {mib(usage.free)} free')
    rows, starts, dev = state(volume, kit_files)
    for what, path, status in rows:
        print(f'  {what:18} {path:22} {status}')
    if foreign(volume):
        print('  Another system\'s boot files are at SurfOS\'s paths: tools/stick.py install will not replace them')
    elif not starts:
        print(f'  SurfOS does not start from this stick yet: tools/stick.py install {volume}')
    elif dev:
        print('  SurfOS starts from this stick, the development build first')
    else:
        print('  SurfOS starts from this stick, the build in boot/ (no development build in surfos-dev/'
              + (', or no ' + MARKER if not os.path.isfile(place(volume, MARKER)) else '') + ')')
    if kit_files is None:
        print('  (no stick folder to compare with: devenv/build-usb.sh makes it)')
    return 0


# ---- the commands ------------------------------------------------------------------------------------------------

def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='command', required=True)
    for name, text in (('check', 'what the stick holds'), ('install', 'make the stick start SurfOS, or bring its boot '
                       'loader and menu up to date'), ('update', 'a new development build into surfos-dev/'),
                       ('kit', 'check the stick folder against its manifest')):
        p = sub.add_parser(name, help=text)
        if name != 'kit':
            p.add_argument('volume', help='where the stick is mounted (macOS: /Volumes/<its name>)')
        p.add_argument('--kit', default=KIT, help='the stick folder (default: build/usb/stick)')
        p.add_argument('--manifest', help='its build manifest (default: build-manifest.json beside the folder)')
        if name in ('install', 'update'):
            p.add_argument('--eject', action='store_true', help='eject the stick afterwards (macOS: diskutil eject)')
        if name == 'install':
            p.add_argument('--fallback', action='store_true', help='make this build the fallback in boot/ too (an '
                           'install keeps the stick\'s fallback otherwise)')
        if name == 'update':
            p.add_argument('--kernel', help='surfos.bin to put there (with --initrd) instead of the stick folder\'s')
            p.add_argument('--initrd', help='its initrd.tar')
    a = ap.parse_args(argv)
    try:
        if a.command == 'kit':
            files = load_kit(a.kit, a.manifest)
            say(f'{a.kit}: {len(files)} files as the manifest lists them; build '
                f'{build_id(files["surfos-dev/initrd.tar"])} in boot/ and surfos-dev/')
            return 0
        where, label = volume_info(a.volume)
        volume = os.path.realpath(a.volume)
        if a.command == 'check':
            return check(volume, where, a.kit, a.manifest)
        if label.strip().upper() == 'SURFOSUSB':
            raise StickError(f'{where} is the flashed SurfOS image (SURFOSUSB): SurfOS only reads it, so a build here '
                             f'would never be updated over the network, and it is the rescue stick. Nothing was '
                             f'written: use another FAT32 stick (or write a new image to this one, step 4)')
        kept = False
        if a.command == 'install':
            files = load_kit(a.kit, a.manifest)
            other = foreign(volume)
            if other:
                raise StickError(f'{where} already holds another system\'s boot files where SurfOS\'s go: '
                                 f'{", ".join(other)}. Nothing was written; move them off the stick first if it '
                                 f'should start SurfOS instead')
            kept = all(os.path.isfile(place(volume, p)) for p in FALLBACK) and not a.fallback
            order = [p for p in INSTALL if not (kept and p in FALLBACK + [INFO])]
        else:
            if bool(a.kernel) != bool(a.initrd):
                raise StickError('--kernel and --initrd go together (a kernel and its own initrd)')
            files = load_build(a.kernel, a.initrd) if a.kernel else \
                {rel: data for rel, data in load_kit(a.kit, a.manifest).items() if rel in DEV}
            order = DEV
        say(f'{where}: {len(order)} files')
        write(volume, files, order)
        bid = build_id(files['surfos-dev/initrd.tar'])
        _, starts, dev = state(volume)
        if a.command == 'install':
            say(f'surfos-dev/ holds build {bid}, the menu\'s first entry; ' +
                ('boot/ keeps its build, the menu\'s "SurfOS" entry (install --fallback replaces it too)' if kept else
                 f'boot/ holds the same build, the menu\'s "SurfOS" entry'))
        else:
            say(f'surfos-dev/ holds build {bid}: the menu\'s first entry starts it')
        if not dev:
            missing = [p for p in [LOADER, MENU, MARKER] + FALLBACK if not os.path.isfile(place(volume, p))]
            if missing:
                say(f'note: the stick does not start this build yet ({", ".join(missing)} missing): '
                    f'tools/stick.py install does that')
            else:
                say('note: another system\'s boot loader or menu is on this stick, so it does not start SurfOS')
        if a.eject:
            eject(volume)
        else:
            say('eject the stick before pulling it out (Finder, or tools/stick.py ... --eject)')
        return 0
    except StickError as e:
        print(f'stick: {e}', file=sys.stderr)
        return 1
    except OSError as e:                                # the stick gone while it was read, say
        where = e.filename or getattr(a, 'volume', None) or a.kit
        print(f'stick: {where}: {e.strerror or e}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
