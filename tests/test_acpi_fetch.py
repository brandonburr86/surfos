import importlib.util
from pathlib import Path
import struct
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('acpi_fetch', ROOT/'tools'/'acpi-fetch.py')
acpi = importlib.util.module_from_spec(spec)
spec.loader.exec_module(acpi)


def table(signature=b'DSDT', payload=b'\x10\x00test'):
    data = bytearray(36 + len(payload))
    data[:4] = signature
    struct.pack_into('<I', data, 4, len(data))
    data[8] = 2
    data[10:16] = b'SURFOS'
    data[36:] = payload
    data[9] = (-sum(data)) & 255
    return bytes(data)


def dump(data):
    lines = [f'ACPI {data[:4].decode()} {len(data)} bytes']
    lines += [f'{i:08x}: ' + data[i:i+16].hex(' ') for i in range(0, len(data), 16)]
    return '\n'.join(lines)


class DecodeTests(unittest.TestCase):
    def test_complete_table(self):
        data = table()
        self.assertEqual(acpi.decode_dump(dump(data), 'DSDT'), data)

    def test_each_aml_kind(self):
        for sig in (b'DSDT', b'SSDT'):
            data = table(sig, bytes(range(256)))
            self.assertEqual(acpi.decode_dump(dump(data), sig.decode()), data)

    def test_large_table_crosses_mapping_window(self):
        data = table(payload=bytes(range(256)) * 1024)
        self.assertEqual(acpi.decode_dump(dump(data), 'DSDT'), data)

    def test_reject_corrupt_checksum(self):
        data = bytearray(table()); data[-1] ^= 1
        with self.assertRaises(ValueError): acpi.decode_dump(dump(data), 'DSDT')

    def test_reject_wrong_signature(self):
        with self.assertRaises(ValueError): acpi.decode_dump(dump(table(b'SSDT')), 'DSDT')

    def test_reject_truncated_and_duplicate_rows(self):
        lines = dump(table()).splitlines()
        for output in ('\n'.join(lines[:-1]), '\n'.join(lines+[lines[-1]])):
            with self.assertRaises(ValueError): acpi.decode_dump(output, 'DSDT')

    def test_reject_offset_gap(self):
        output = dump(table()).replace('00000010:', '00000020:')
        with self.assertRaises(ValueError): acpi.decode_dump(output, 'DSDT')

    def test_reject_unbounded_length(self):
        for size in (0, 35, 1024 * 1024 + 1, 0xffffffff):
            with self.assertRaises(ValueError):
                acpi.decode_dump(f'ACPI DSDT {size} bytes', 'DSDT')

    def test_reject_header_length_mismatch_even_with_valid_checksum(self):
        data = bytearray(table()); data[4] += 1; data[9] -= 1
        with self.assertRaises(ValueError): acpi.decode_dump(dump(data), 'DSDT')

    def test_reject_trailing_nonhex_output(self):
        with self.assertRaises(ValueError):
            acpi.decode_dump(dump(table())+'\ninterrupted', 'DSDT')

    def test_only_count_inventory_rows(self):
        inventory = '    SSDT 0x1000 40 bytes #0\n    SSDT 0x2000 50 bytes #1\ntext SSDT\n    FACP 0x3000 268 bytes\n'
        self.assertEqual(acpi.ssdt_count(inventory), 2)


if __name__ == '__main__': unittest.main()
