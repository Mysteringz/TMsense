#!/usr/bin/env python3
"""Malformed UDP payloads must become parse errors, never terminate listen.py."""
import importlib.util
from pathlib import Path
import struct
import subprocess
import unittest

spec = importlib.util.spec_from_file_location('listen', Path(__file__).resolve().parents[2] / 'tools/listen.py')
listen = importlib.util.module_from_spec(spec)
spec.loader.exec_module(listen)


def packet(kind, body):
    return listen.HEADER.pack(b'TM', 1, kind, b'\x01' * 6, 1, 1, 1, len(body)) + body + b'\0' * listen.TAG


class PayloadTests(unittest.TestCase):
    def test_malformed_payloads_are_recoverable(self):
        for kind, body in [(1, b''), (1, b'\0' * 13 + b'\xff'), (1, b'\0' * 15),
                           (2, b'\0' * 8), (2, b'\0' * 776), (3, b''),
                           (3, b'\0' * 47 + b'\xff'), (3, b'\0' * 49), (4, b'')]:
            with self.subTest(kind=kind, length=len(body)), self.assertRaises(ValueError):
                listen.parse(packet(kind, body), b'')

    def test_report_without_detections(self):
        self.assertEqual(listen.parse(packet(1, b'\0' * 14), b'')['dets'], [])

    def test_raw_and_status(self):
        raw = struct.pack('<IhH', 1, 2000, 1) + bytes(768)
        self.assertEqual(listen.parse(packet(2, raw), b'')['pixels'], 768)
        self.assertEqual(listen.parse(packet(3, b'\0' * 48), b'')['params'], {})

    def test_encrypted_firmware_vectors_and_downgrade(self):
        root = Path(__file__).resolve().parents[2]
        binary = subprocess.check_output([str(root / 'test/host/build_packet_host.sh')], text=True).strip()
        import json
        records = [json.loads(line) for line in subprocess.check_output([binary, 'emit-secure'], text=True).splitlines()]
        devices = {'version': 2, 'nodes': {'01:02:03:04:05:06': {'current': {'id': 7, 'secret': '11'*32}}}}
        for row in records:
            dg = bytes.fromhex(row['hex'])
            result = listen.parse(dg, b'', devices)
            self.assertTrue(result['encrypted'])
            self.assertEqual(result['boot'], 70000)
            for i in range(len(dg)):
                changed = bytearray(dg); changed[i] ^= 1
                with self.assertRaises(ValueError): listen.parse(changed, b'', devices)
        with self.assertRaises(ValueError): listen.parse(packet(1, b'\0'*14), b'', devices)

    def test_ota_status(self):
        p = listen.parse(packet(4, struct.pack('<BBBBI', 5, 100, 0, 0, 123)), b'')
        self.assertEqual((p['type'], p['percent'], p['image']), ('OTA_STATUS', 100, 123))


if __name__ == '__main__':
    unittest.main()
