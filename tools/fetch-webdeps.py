#!/usr/bin/env python3
"""Fetch and check what the web browser Undertow is built from: Mbed TLS, for HTTPS.

    tools/fetch-webdeps.py                  make third_party/mbedtls/ what third_party/undertow-lock.json pins
    tools/fetch-webdeps.py --archive PATH   the same from a copy of the release archive (checked the same way)
    tools/fetch-webdeps.py --offline        never download: use the cache (third_party/cache/) or --archive
    tools/fetch-webdeps.py --check          only check, change nothing: the extracted files against the SHA-256
                                            values recorded when they were extracted, and the CA bundle
                                            rootfs/etc/ssl/cert.pem against the lock; exit 1 when anything is wrong

The lock pins the official release archive by URL, size and SHA-256. The archive is downloaded into the cache
(third_party/cache/, ignored by git), checked, and only then extracted: just what the build needs (the lock's
"extract" list: include/, library/, LICENSE), never a member that is a link, a device or a FIFO, has an absolute
path or a ".." component, or lies outside the archive's top directory; any such member refuses the whole archive.
The files are extracted into a new directory next to third_party/mbedtls/ and put in its place only when all of
them are there, so a bad download or archive never replaces a good tree. third_party/mbedtls/UNDERTOW-LOCK
records what was extracted (the lock's entry and every file's SHA-256); while it matches the lock nothing is
done, so running this before every build is cheap (devenv/build.sh does, before the tree is copied into Docker,
and the Makefile does when the stamp is missing or older than the lock).

A cached archive that does not match is deleted and fetched again; a download that does not match is deleted and
reported. Without a network, a verified archive in the cache (or --archive) is enough; with neither the message
says what to do. Messages start with "fetch-webdeps:"; the exit status is 0 or 1.
"""
import argparse
import contextlib
import hashlib
import json
import os
import posixpath
import shutil
import sys
import tarfile
import tempfile
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOCK = os.path.join(ROOT, 'third_party', 'undertow-lock.json')
DEST = os.path.join(ROOT, 'third_party', 'mbedtls')
CACHE = os.path.join(ROOT, 'third_party', 'cache')
STAMP = 'UNDERTOW-LOCK'
# what the build reads first: a tree without these is not the library, whatever the archive said
SENTINELS = ('LICENSE', 'include/mbedtls/build_info.h', 'include/psa/crypto.h', 'library/ssl_tls.c')
# the lock entry's fields that decide whether an extracted tree is the one pinned
PINNED = ('version', 'url', 'sha256', 'size', 'root', 'extract')


class Failure(Exception):
    """A failure with a message for the user."""


def say(msg):
    print('fetch-webdeps: ' + msg, file=sys.stderr)


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 16), b''):
            h.update(chunk)
    return h.hexdigest()


def safe_rel(name):
    """name as a relative POSIX path of plain components, or None ('..', absolute, empty, '.' parts)."""
    if not name or name.startswith('/') or '\\' in name or '\0' in name:
        return None
    parts = [p for p in name.split('/') if p != '']
    if not parts or any(p in ('.', '..') for p in parts):
        return None
    return '/'.join(parts)


def load_lock(path):
    try:
        with open(path, encoding='utf-8') as f:
            lock = json.load(f)
        entry = lock['mbedtls']
        for key in PINNED:
            if key not in entry:
                raise KeyError(key)
    except (OSError, ValueError, KeyError, TypeError) as e:
        raise Failure(f'{path}: not a usable lock file ({e})')
    sha = entry['sha256']
    if not (isinstance(sha, str) and len(sha) == 64 and all(c in '0123456789abcdef' for c in sha)):
        raise Failure(f'{path}: mbedtls.sha256 must be 64 lowercase hex digits')
    if not isinstance(entry['size'], int) or entry['size'] <= 0:
        raise Failure(f'{path}: mbedtls.size must be the archive size in bytes')
    if safe_rel(entry['root']) is None or '/' in safe_rel(entry['root']):
        raise Failure(f'{path}: mbedtls.root must be one directory name')
    for item in entry['extract']:
        if safe_rel(item.rstrip('/')) is None:
            raise Failure(f'{path}: mbedtls.extract has an unsafe entry {item!r}')
    if not entry['url'].startswith(('https://', 'http://')):
        raise Failure(f'{path}: mbedtls.url must be an http(s) address')
    return lock


def pinned(entry):
    return {key: entry[key] for key in PINNED}


def read_stamp(dest):
    try:
        with open(os.path.join(dest, STAMP), encoding='utf-8') as f:
            stamp = json.load(f)
        if not isinstance(stamp.get('lock'), dict) or not isinstance(stamp.get('files'), dict):
            return None
        return stamp
    except (OSError, ValueError):
        return None


def wanted(rel, extract):
    """Is rel (relative to the archive's top directory) on the extract list (a file, or a directory/)?"""
    for item in extract:
        if item.endswith('/'):
            if rel == item[:-1] or rel.startswith(item):
                return True
        elif rel == item:
            return True
    return False


# ---- the archive ------------------------------------------------------------------------------------

def download(url, path, size):
    """url into path (via path.part), at most size + 1 bytes; raises OSError on any network failure."""
    tmp = path + '.part'
    try:
        with urllib.request.urlopen(url, timeout=60) as r, open(tmp, 'wb') as f:
            got = 0
            while True:
                chunk = r.read(1 << 16)
                if not chunk:
                    break
                got += len(chunk)
                if got > size:                  # more than the pinned archive: not it, and not worth keeping
                    break
                f.write(chunk)
        os.replace(tmp, path)
    finally:
        if os.path.exists(tmp):
            os.unlink(tmp)


def obtain(entry, cache, archive, offline):
    """The path of an archive whose SHA-256 is the pinned one."""
    want = entry['sha256']
    if archive:
        if not os.path.isfile(archive):
            raise Failure(f'{archive}: no such file')
        got = sha256_file(archive)
        if got != want:
            raise Failure(f'{archive} is not the pinned Mbed TLS {entry["version"]} archive: its SHA-256 is {got}, '
                          f'the lock says {want}; nothing was changed')
        return archive
    name = posixpath.basename(entry['url'])
    cached = os.path.join(cache, name)
    if os.path.isfile(cached):
        got = sha256_file(cached)
        if got == want:
            return cached
        say(f'{cached} does not match the lock (SHA-256 {got}): deleted, fetching it again')
        os.unlink(cached)
    if offline:
        raise Failure(f'no verified copy of {name} in {cache} and --offline: copy the archive there (SHA-256 {want}, '
                      f'from {entry["url"]}) or pass --archive PATH')
    os.makedirs(cache, exist_ok=True)
    say(f'downloading {entry["url"]}')
    try:
        download(entry['url'], cached, entry['size'])
    except (OSError, ValueError) as e:
        raise Failure(f'cannot download {entry["url"]} ({e}) and {cache} has no verified copy. With a network, run '
                      f'this again; without one, copy {name} (SHA-256 {want}) into {cache}, or pass --archive PATH')
    got = sha256_file(cached)
    if got != want:
        os.unlink(cached)
        raise Failure(f'the download from {entry["url"]} does not match the lock: SHA-256 {got}, the lock says '
                      f'{want}. It was deleted and nothing was changed')
    return cached


def check_members(members, root):
    """Refuse the whole archive for any member that is not a plain file or directory under root/."""
    for m in members:
        rel = safe_rel(m.name)
        if rel is None:
            raise Failure(f'the archive has an unsafe member name {m.name!r} (absolute, "..", or empty): refused')
        if not (m.isreg() or m.isdir()):
            kind = 'a link' if (m.issym() or m.islnk()) else 'a device or FIFO' if (m.isdev()) else 'not a file'
            raise Failure(f'the archive member {m.name!r} is {kind}: refused')
        if rel != root and not rel.startswith(root + '/'):
            raise Failure(f'the archive member {m.name!r} is outside {root}/: refused')


def extract(archive, entry, tmp):
    """The members on the extract list into tmp; the files' SHA-256 values by relative path."""
    root, files = entry['root'], {}
    try:
        with tarfile.open(archive, 'r:*') as tar:
            members = tar.getmembers()
            check_members(members, root)
            for m in members:
                rel = safe_rel(m.name)
                if rel == root:
                    continue
                rel = rel[len(root) + 1:]
                if not wanted(rel, entry['extract']):
                    continue
                path = os.path.join(tmp, *rel.split('/'))
                if m.isdir():
                    os.makedirs(path, mode=0o755, exist_ok=True)
                    continue
                os.makedirs(os.path.dirname(path), mode=0o755, exist_ok=True)
                h = hashlib.sha256()
                src = tar.extractfile(m)
                with open(path, 'xb') as out:   # 'x': a name twice in the archive is an error, not an overwrite
                    for chunk in iter(lambda: src.read(1 << 16), b''):
                        h.update(chunk)
                        out.write(chunk)
                os.chmod(path, 0o644)
                os.utime(path, (m.mtime, m.mtime))
                files[rel] = h.hexdigest()
    except (tarfile.TarError, EOFError, OSError) as e:
        raise Failure(f'{archive}: cannot be read as the release archive ({e}); nothing was changed')
    missing = [s for s in SENTINELS if s not in files]
    if missing:
        raise Failure(f'{archive} lacks {", ".join(missing)}: not the Mbed TLS release; nothing was changed')
    return files


def publish(tmp, dest):
    """Put tmp where dest is: the old tree moves aside first and is removed only after."""
    old = None
    if os.path.lexists(dest):
        old = tempfile.mkdtemp(prefix='.mbedtls.old-', dir=os.path.dirname(dest))
        os.rmdir(old)
        os.rename(dest, old)
    try:
        os.rename(tmp, dest)
    except OSError:
        if old:
            os.rename(old, dest)
        raise
    if old:
        shutil.rmtree(old, ignore_errors=True)


@contextlib.contextmanager
def exclusive(parent):
    """One fetch at a time in this tree (two makes at once would otherwise extract twice)."""
    os.makedirs(parent, exist_ok=True)
    with open(os.path.join(parent, '.fetch-webdeps.lock'), 'w') as f:
        try:
            import fcntl
            fcntl.flock(f, fcntl.LOCK_EX)
        except ImportError:
            pass
        yield


def fetch(lock_path, dest, cache, archive, offline):
    lock = load_lock(lock_path)
    entry = lock['mbedtls']
    with exclusive(os.path.dirname(dest)):
        stamp = read_stamp(dest)
        if stamp and stamp['lock'] == pinned(entry) and \
                all(os.path.isfile(os.path.join(dest, *rel.split('/'))) for rel in stamp['files']):
            # current: make sees a stamp newer than the lock, and never a newer one for nothing (that would
            # rebuild the library at every build)
            if os.path.getmtime(os.path.join(dest, STAMP)) < os.path.getmtime(lock_path):
                os.utime(os.path.join(dest, STAMP))
            say(f'Mbed TLS {entry["version"]} in {dest}: ok')
            return
        path = obtain(entry, cache, archive, offline)
        tmp = tempfile.mkdtemp(prefix='.mbedtls.new-', dir=os.path.dirname(dest))
        try:
            files = extract(path, entry, tmp)
            with open(os.path.join(tmp, STAMP), 'w', encoding='utf-8') as f:
                json.dump({'lock': pinned(entry), 'files': files}, f, indent=1, sort_keys=True)
                f.write('\n')
            os.chmod(tmp, 0o755)
            publish(tmp, dest)
        finally:
            if os.path.exists(tmp):
                shutil.rmtree(tmp, ignore_errors=True)
        say(f'Mbed TLS {entry["version"]}: {len(files)} files extracted into {dest}')


def check(lock_path, dest):
    """Everything as pinned? Prints what is wrong; returns the number of problems."""
    lock = load_lock(lock_path)
    entry, bad = lock['mbedtls'], 0
    stamp = read_stamp(dest)
    if not stamp:
        say(f'{dest}: missing (run tools/fetch-webdeps.py)')
        bad += 1
    elif stamp['lock'] != pinned(entry):
        say(f'{dest}: extracted from another archive than the lock pins (run tools/fetch-webdeps.py)')
        bad += 1
    else:
        found = set()
        for top, dirs, names in os.walk(dest):
            for name in names:
                rel = os.path.relpath(os.path.join(top, name), dest).replace(os.sep, '/')
                if rel != STAMP:
                    found.add(rel)
        for rel in sorted(found - set(stamp['files'])):
            say(f'{dest}/{rel}: not from the archive')
            bad += 1
        for rel, want in sorted(stamp['files'].items()):
            path = os.path.join(dest, *rel.split('/'))
            if not os.path.isfile(path):
                say(f'{dest}/{rel}: missing')
                bad += 1
            elif sha256_file(path) != want:
                say(f'{dest}/{rel}: changed since it was extracted')
                bad += 1
        if not bad:
            say(f'Mbed TLS {entry["version"]}: {len(stamp["files"])} files as extracted')
    ca = lock.get('ca_bundle')
    if ca:
        path = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(lock_path))), *ca['file'].split('/'))
        if not os.path.isfile(path):
            say(f'{path}: missing')
            bad += 1
        elif sha256_file(path) != ca['sha256']:
            say(f'{path}: its SHA-256 is not the one in the lock')
            bad += 1
        else:
            say(f'{ca["file"]}: ok')
    return bad


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--archive', help='use this copy of the release archive (checked against the lock)')
    ap.add_argument('--offline', action='store_true', help='never download')
    ap.add_argument('--check', action='store_true', help='only check what is there')
    ap.add_argument('--lock', default=LOCK, help=argparse.SUPPRESS)
    ap.add_argument('--dest', default=DEST, help=argparse.SUPPRESS)
    ap.add_argument('--cache', default=CACHE, help=argparse.SUPPRESS)
    a = ap.parse_args(argv)
    try:
        if a.check:
            return 1 if check(a.lock, a.dest) else 0
        fetch(a.lock, a.dest, a.cache, a.archive, a.offline)
        return 0
    except Failure as e:
        say(str(e))
        return 1


if __name__ == '__main__':
    sys.exit(main())
