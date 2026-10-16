#!/usr/bin/env python3
"""SurfOS: checked build-time vendor patch
----------------------
File: patch-webdeps.py  Date: 10/12/26
----------------------
Make an audited build-directory copy of a pinned source; never edit the extraction.
"""
import argparse
import hashlib
import json
from pathlib import Path
import os
import tempfile


def patched_source(source, spec):
    if hashlib.sha256(source).hexdigest() != spec['source_sha256']:
        raise ValueError('vendor input SHA-256 differs from the reviewed patch input')
    result = source
    for change in spec['replacements']:
        old, new = change['old'].encode(), change['new'].encode()
        if result.count(old) != 1:
            raise ValueError('patch context must occur exactly once')
        result = result.replace(old, new)
    if hashlib.sha256(result).hexdigest() != spec['patched_sha256']:
        raise ValueError('patched output SHA-256 differs from the reviewed result')
    return result


def generate(source, specification, output):
    source, specification, output = Path(source), Path(specification), Path(output)
    if output.resolve() == source.resolve() or source.resolve().parent in output.resolve().parents:
        raise ValueError('patched source must be outside the vendor source directory')
    result = patched_source(source.read_bytes(), json.loads(specification.read_text()))
    output.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=output.name + '.', dir=output.parent)
    try:
        with os.fdopen(fd, 'wb') as f:
            f.write(result)
        os.replace(temporary, output)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', required=True)
    parser.add_argument('--patch', required=True)
    parser.add_argument('--output', required=True)
    args = parser.parse_args()
    try:
        generate(args.source, args.patch, args.output)
    except (ValueError, OSError, KeyError) as e:
        parser.exit(1, f'patch-webdeps: {e}\n')


if __name__ == '__main__':
    main()
