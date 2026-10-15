#!/usr/bin/env python3
"""Fetch validated DSDT/SSDT firmware tables from an explicitly named SurfOS host.

Only read-only acpi/acpidump commands are sent. No network discovery, AML execution,
PCI configuration writes, or power changes occur. Decode locally with iasl -da.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct

MAX_TABLE = 1024 * 1024


def decode_dump(output, signature):
    if signature not in ('DSDT', 'SSDT'):
        raise ValueError('only DSDT and SSDT exports are supported')
    lines = [line.strip() for line in output.splitlines() if line.strip()]
    if not lines:
        raise ValueError('empty ACPI export')
    header = re.fullmatch(r'ACPI (DSDT|SSDT) ([0-9]+) bytes', lines[0])
    if not header or header[1] != signature:
        raise ValueError('missing or mismatched ACPI export header')
    size = int(header[2])
    if not 36 <= size <= MAX_TABLE:
        raise ValueError('ACPI table length is out of bounds')
    data = bytearray()
    for line in lines[1:]:
        row = re.fullmatch(r'([0-9a-fA-F]{8}):((?: [0-9a-fA-F]{2}){1,16})', line)
        if not row or int(row[1], 16) != len(data):
            raise ValueError('malformed, duplicated or missing ACPI export row')
        chunk = bytes.fromhex(row[2])
        if len(chunk) != min(16, size-len(data)):
            raise ValueError('incorrect ACPI export row length')
        data.extend(chunk)
    if len(data) != size:
        raise ValueError('truncated ACPI table')
    if data[:4] != signature.encode() or struct.unpack_from('<I', data, 4)[0] != size:
        raise ValueError('ACPI binary header does not match export')
    if sum(data) & 255:
        raise ValueError('ACPI checksum failed')
    return bytes(data)


def ssdt_count(inventory):
    count = sum(bool(re.match(r'^\s*SSDT\s', line)) for line in inventory.splitlines())
    if count > 64:
        raise ValueError('too many SSDT tables')
    return count


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', required=True, help='explicit laptop IP address')
    parser.add_argument('--port', type=int, default=23)
    parser.add_argument('--output-dir', type=Path, required=True)
    args = parser.parse_args()
    # Import here so the pure decoder can also be used without a telnet client.
    from devloop import Session, DevloopError
    session = None
    try:
        args.output_dir.mkdir(parents=True, exist_ok=True)
        if any(args.output_dir.iterdir()):
            raise ValueError('output directory must be empty to preserve previous evidence')
        session = Session(args.host, args.port)
        inventory = session.check('acpi')
        (args.output_dir/'inventory.txt').write_text(inventory+'\n')
        manifest = {'host':args.host, 'build_id':session.build_id(), 'tables':[]}
        queries = [('DSDT', 0)] + [('SSDT', i) for i in range(ssdt_count(inventory))]
        for signature, instance in queries:
            output = session.check(f'acpidump {signature} {instance}', timeout=120)
            data = decode_dump(output, signature)
            name = 'dsdt.aml' if signature == 'DSDT' else f'ssdt-{instance}.aml'
            (args.output_dir/name).write_bytes(data)
            manifest['tables'].append({'file':name, 'bytes':len(data),
                                       'sha256':hashlib.sha256(data).hexdigest()})
            print(f'{name}: {len(data)} bytes, checksum verified', flush=True)
        (args.output_dir/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
        return 0
    except (OSError, ValueError, DevloopError) as error:
        print(f'acpi-fetch: {error}')
        return 1
    finally:
        if session is not None:
            session.close()


if __name__ == '__main__':
    raise SystemExit(main())
