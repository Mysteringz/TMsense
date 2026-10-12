#!/usr/bin/env python3
"""Package a `pio run -e tmflash` build as a GitHub release.

    python3 tools/release_manifest.py --build .pio/build/tmflash --out dist --commit <sha>

A release is what TMedge's web flasher (algo console, module 07) writes to a
board: it reads the latest one from this repository and nothing else. So a
release has to be complete and has to say what it is:

    bootloader.bin  partitions.bin  firmware.bin   the three images a blank board needs
    tmflash-manifest.json                          version, commit, and each file's size and SHA-256

The version is TM_FW_VERSION and the tag is derived from it (tmsense-1.5 ->
v1.5), never typed: a node reports that string for the rest of its life, and
it must name exactly one image. This script refuses a build whose firmware.bin
does not carry the version the source says, which is what a stale build
directory looks like.

TMedge parses this manifest (src/shared/flasher/release.ts there). Change the
two together.
"""
import argparse, hashlib, json, os, re, shutil, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..'))
MANIFEST = 'tmflash-manifest.json'
BOARD = 'heltec_wifi_lora_32_V3'
# Largest each file can be on this board's layout: the bootloader sits below
# the partition table at 0x8000, the table is one 0xC00 block, and an OTA slot
# of the 8 MB layout is 0x330000.
FILES = (('bootloader.bin', 0x8000), ('partitions.bin', 0xC00), ('firmware.bin', 0x330000))
VERSION = re.compile(r'^tmsense-(\d+\.\d+(?:\.\d+)?)$')


class Refused(Exception):
    pass


def version_of(config_path):
    """TM_FW_VERSION as the source has it."""
    with open(config_path, encoding='utf-8') as config:
        found = re.findall(r'^\s*#define\s+TM_FW_VERSION\s+"([^"]*)"', config.read(), re.M)
    if len(found) != 1:
        raise Refused(f'{config_path} must define TM_FW_VERSION exactly once')
    # STATUS carries 12 bytes of it; a longer string would be cut on the wire.
    if not VERSION.match(found[0]) or len(found[0]) > 12:
        raise Refused(f'TM_FW_VERSION "{found[0]}" is not tmsense-<major>.<minor> in at most 12 characters')
    return found[0]


def tag_of(version):
    return 'v' + VERSION.match(version).group(1)


def package(build, out, commit, config_path):
    if not re.fullmatch(r'[0-9a-f]{40}', commit):
        raise Refused('--commit must be the full 40-character commit the build came from')
    version = version_of(config_path)
    entries = []
    os.makedirs(out, exist_ok=True)
    for name, limit in FILES:
        path = os.path.join(build, name)
        if not os.path.isfile(path):
            raise Refused(f'no {name} in {build}: run `pio run -e tmflash` first')
        with open(path, 'rb') as image:
            data = image.read(limit + 1)
        if not data or len(data) > limit:
            raise Refused(f'{name} is {"empty" if not data else "larger than this board has room for"}')
        if name == 'partitions.bin':
            if data[:2] != b'\xaa\x50':
                raise Refused(f'{name} is not an ESP-IDF partition table')
        elif data[0] != 0xE9:
            raise Refused(f'{name} is not an ESP32 image')
        if name == 'firmware.bin' and version.encode() + b'\0' not in data:
            raise Refused(f'firmware.bin does not say it is {version}: the build is older than the source')
        shutil.copyfile(path, os.path.join(out, name))
        entries.append({'name': name, 'size': len(data), 'sha256': hashlib.sha256(data).hexdigest()})
    manifest = {'format': 1, 'project': 'TMsense', 'board': BOARD, 'version': version,
                'tag': tag_of(version), 'commit': commit, 'files': entries}
    with open(os.path.join(out, MANIFEST), 'w', encoding='utf-8') as target:
        json.dump(manifest, target, indent=2)
        target.write('\n')
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--build', default=os.path.join(ROOT, '.pio', 'build', 'tmflash'))
    parser.add_argument('--out', required=True)
    parser.add_argument('--commit', required=True)
    parser.add_argument('--config', default=os.path.join(ROOT, 'include', 'tm_config.h'))
    args = parser.parse_args()
    try:
        manifest = package(args.build, args.out, args.commit, args.config)
    except Refused as refusal:
        print(f'release refused: {refusal}', file=sys.stderr)
        return 1
    print(f'{manifest["version"]} -> release {manifest["tag"]} ({sum(f["size"] for f in manifest["files"])} bytes in {args.out})')
    return 0


if __name__ == '__main__':
    sys.exit(main())
