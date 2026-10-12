#!/usr/bin/env python3
"""What a release is allowed to be. Runs tools/release_manifest.py.

    python3 test/host/release_manifest_test.py        # exit 1 on failure

TMedge's web flasher writes the latest release to boards without a person
reading it first, so each claim here is about what can never be published:
half a build, a build older than the source, or a version that does not name
one image.
"""
import hashlib, json, os, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import release_manifest as rm  # noqa: E402

COMMIT = '0123456789abcdef0123456789abcdef01234567'
failures = []


def claim(text, ok):
    print(('ok   ' if ok else 'FAIL ') + text)
    if not ok:
        failures.append(text)


def refused(build, out, config, commit=COMMIT):
    try:
        rm.package(build, out, commit, config)
    except rm.Refused as refusal:
        return str(refusal)
    return None


def build_dir(root, version='tmsense-1.5', skip=()):
    build = os.path.join(root, 'build')
    os.makedirs(build, exist_ok=True)
    images = {
        'bootloader.bin': b'\xe9' + bytes(range(200)),
        'partitions.bin': b'\xaa\x50' + bytes(0xC00 - 2),
        'firmware.bin': b'\xe9' + bytes(64) + b'=== TMsense %s ===\0' + version.encode() + b'\0' + bytes(500),
    }
    for name, data in images.items():
        path = os.path.join(build, name)
        if name in skip:
            if os.path.exists(path):
                os.remove(path)
            continue
        with open(path, 'wb') as image:
            image.write(data)
    return build, images


def config_file(root, body):
    path = os.path.join(root, 'tm_config.h')
    with open(path, 'w', encoding='utf-8') as config:
        config.write(body)
    return path


with tempfile.TemporaryDirectory() as root:
    out = os.path.join(root, 'dist')
    config = config_file(root, '#define TM_FW_VERSION "tmsense-1.5"   // at most 12 chars\n')
    build, images = build_dir(root)

    manifest = rm.package(build, out, COMMIT, config)
    on_disk = json.load(open(os.path.join(out, rm.MANIFEST), encoding='utf-8'))
    claim('a release is the three images a blank board needs, and a manifest naming each by size and SHA-256',
          sorted(os.listdir(out)) == ['bootloader.bin', 'firmware.bin', 'partitions.bin', rm.MANIFEST]
          and on_disk == manifest
          and [(f['name'], f['size'], f['sha256']) for f in manifest['files']]
          == [(name, len(images[name]), hashlib.sha256(images[name]).hexdigest()) for name, _ in rm.FILES]
          and all(open(os.path.join(out, name), 'rb').read() == images[name] for name in images))
    claim('the tag comes from the version the firmware reports, and the commit is recorded',
          (manifest['version'], manifest['tag'], manifest['commit'], manifest['format'], manifest['board'])
          == ('tmsense-1.5', 'v1.5', COMMIT, 1, 'heltec_wifi_lora_32_V3'))

    for missing in ('bootloader.bin', 'partitions.bin', 'firmware.bin'):
        partial, _ = build_dir(root, skip=(missing,))
        claim(f'half a build is not a release: no {missing}, no release', missing in (refused(partial, out, config) or ''))
    build, _ = build_dir(root)

    newer = config_file(root, '#define TM_FW_VERSION "tmsense-1.6"\n')
    claim('a build older than the source is refused, so a tag never names an image that says something else',
          'older than the source' in (refused(build, out, newer) or ''))
    for body, why in (('#define TM_FW_VERSION "tmsense-1.5-rc1"\n', 'a version that is not a plain number'),
                      ('#define TM_FW_VERSION "tmsense-10.100"\n', 'a version STATUS would cut short'),
                      ('// no version here\n', 'a source with no version'),
                      ('#define TM_FW_VERSION "tmsense-1.5"\n#define TM_FW_VERSION "tmsense-1.6"\n', 'a source with two versions')):
        claim(f'{why} is refused', refused(build, out, config_file(root, body)) is not None)
    config = config_file(root, '#define TM_FW_VERSION "tmsense-1.5"\n')

    claim('a short commit is refused: the manifest names exactly what was built', refused(build, out, config, 'abc1234') is not None)
    with open(os.path.join(build, 'partitions.bin'), 'wb') as image:
        image.write(b'\xe9' + bytes(100))
    claim('a file that is not a partition table is not published as one', 'partition table' in (refused(build, out, config) or ''))
    build, _ = build_dir(root)
    with open(os.path.join(build, 'bootloader.bin'), 'wb') as image:
        image.write(b'\xe9' + bytes(0x8000))
    claim('a bootloader that would run into the partition table is refused', 'room' in (refused(build, out, config) or ''))

claim('the real source names a releasable version', rm.VERSION.match(rm.version_of(os.path.join(ROOT, 'include', 'tm_config.h'))) is not None)

if failures:
    print(f'\n{len(failures)} claim(s) failed')
    sys.exit(1)
print('\nall release claims hold')
