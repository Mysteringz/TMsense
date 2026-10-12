#!/usr/bin/env python3
"""Bench receiver for TMnode packets: verify, decode, summarise.

    TM_KEY=... python3 tools/listen.py [--iface en0] [--seconds 60] [--json out.jsonl]
    TM_KEY=... python3 tools/listen.py --command <node-ip> reset-bg|identify|set <param-id> <value>

Independent of TMedge on purpose, so a node can be checked with nothing but
Python. It mirrors include/tm_protocol.h; if that changes, change this too.
It hears only `transport udp` nodes: a `transport wss` node sends the same
bytes, but inside a TLS WebSocket to TMedge (tmnode.v1), not to this port.
On a Mac whose VPN blocks the LAN, pass --iface en0 (see bind_to_interface).
"""
import argparse, hashlib, hmac, json, os, socket, struct, sys, time
from pathlib import Path

HEADER = struct.Struct('<2sBB6sHIIH')     # magic, version, type, uid, boot, seq, uptime, len
TAG = 8
SECURE_HEADER = struct.Struct("<2sBB6sIIIHH16s")
TYPES = {1: 'REPORT', 2: 'RAW', 3: 'STATUS', 4: 'OTA_STATUS', 16: 'COMMAND', 17: 'OTA', 18: 'ACK'}
PARAMS = ['min_contrast', 'min_peak', 'noise_k', 'min_area', 'max_area', 'bg_tau', 'bg_frames',
          'raw_every', 'refresh', 'split_sep']


def bind_to_interface(sock, iface):
    if sys.platform == 'darwin':
        sock.setsockopt(socket.IPPROTO_IP, 25, socket.if_nametoindex(iface))   # IP_BOUND_IF
    else:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_BINDTODEVICE, iface.encode())


def parse(dg, key, devices=None):
    if len(dg) < HEADER.size + TAG or len(dg) > 834:
        raise ValueError('short or oversized')
    if dg[:2] != b'TM':
        raise ValueError('bad magic/version')
    secure_id = None
    if dg[2] == 2:
        if len(dg) < SECURE_HEADER.size + 16:
            raise ValueError('short encrypted header')
        _, ver, typ, uid, boot, seq, uptime, n, secure_id, epoch = SECURE_HEADER.unpack_from(dg)
        if len(dg) != SECURE_HEADER.size + n + 16 or not secure_id:
            raise ValueError('encrypted length/key id')
        policy = (devices or {}).get('nodes', {}).get(uid.hex(':'), {})
        keys = [] if policy.get('revoked') else [policy.get('current', {}), policy.get('previous', {})]
        device = next((k for k in keys if k.get('id') == secure_id and
                       ('expiresAt' not in k or k['expiresAt'] > time.time()*1000)), None)
        if not device:
            raise ValueError('encrypted device key unavailable; use --device-keys')
        try:
            from cryptography.hazmat.primitives.ciphers.aead import AESGCM
            master = bytes.fromhex(device['secret'])
            if len(master) != 32:
                raise ValueError('invalid key')
            prk = hmac.new(epoch, master, hashlib.sha256).digest()
            info = b'tmsense2/uplink\0' + uid + struct.pack('<H', secure_id) + b'\x01'
            derived = hmac.new(prk, info, hashlib.sha256).digest()
            body = AESGCM(derived).decrypt(struct.pack('<III', 0, boot, seq), dg[SECURE_HEADER.size:], dg[:SECURE_HEADER.size])
        except ImportError as exc:
            raise ValueError('encrypted receiver needs Python cryptography') from exc
        except Exception as exc:
            raise ValueError('encrypted authentication failed') from exc
        signed = True
    elif dg[2] == 1:
        magic, ver, typ, uid, boot, seq, uptime, n = HEADER.unpack_from(dg)
        if devices is not None and uid.hex(':') not in devices.get('legacy', []):
            raise ValueError('plaintext device traffic refused')
        if len(dg) != HEADER.size + n + TAG:
            raise ValueError('length mismatch')
        body, tag = dg[HEADER.size:HEADER.size + n], dg[HEADER.size + n:]
        signed = tag != b'\0' * TAG
        if key:
            good = hmac.new(key, dg[:HEADER.size + n], hashlib.sha256).digest()[:TAG]
            if not hmac.compare_digest(good, tag):
                raise ValueError('BAD SIGNATURE')
    else:
        raise ValueError('bad magic/version')
    pkt = {'type': TYPES.get(typ, typ), 'uid': uid.hex(':'), 'boot': boot, 'seq': seq, 'uptime': uptime,
           'signed': signed, 'encrypted': secure_id is not None}
    if secure_id is not None:
        pkt['key_id'] = secure_id
    if typ == 1:
        if n < 14 or body[13] > 24 or n != 14 + 7 * body[13]:
            raise ValueError('bad REPORT payload length/count')
        frame, ta, lo, hi, bg, flags, count = struct.unpack_from('<IhhhhBB', body)
        dets = []
        for i in range(count):
            x8, y8, area, con, peak, heat = struct.unpack_from('<BBBBBH', body, 14 + 7 * i)
            dets.append({'x': x8 / 8, 'y': y8 / 8, 'area': area, 'contrast': con * 0.05,
                         'peak': peak * 0.25, 'heat': heat / 10})
        pkt.update(frame=frame, ta=ta / 100, scene=[lo / 100, hi / 100], bg_mean=bg / 100, flags=flags, dets=dets)
    elif typ == 2:
        if n != 8 + 768:
            raise ValueError('bad RAW payload length')
        frame, tmin, step = struct.unpack_from('<IhH', body)
        if step == 0:
            raise ValueError('bad RAW quantization step')
        px = body[8:8 + 768]
        pkt.update(frame=frame, t_min=tmin / 100, step=step / 10000,
                   t_max=tmin / 100 + max(px) * step / 10000, pixels=len(px))
    elif typ == 3:
        if n < 48 or body[47] > len(PARAMS) or n != 48 + 4 * body[47]:
            raise ValueError('bad STATUS payload length/count')
        fw = body[:12].rstrip(b'\0').decode('ascii')
        ip = '.'.join(map(str, body[12:16]))
        rssi, ch, heap, minheap, stack, drops, serr, frames, fps, vdd, ta, lastcmd, flags, pc = \
            struct.unpack_from('<bBIIHHHIHHhIBB', body, 16)
        params = dict(zip(PARAMS, struct.unpack_from(f'<{pc}i', body, 48)))
        pkt.update(fw=fw, ip=ip, rssi=rssi, channel=ch, heap=heap, min_heap=minheap, stack_free=stack,
                   wifi_drops=drops, sensor_errors=serr, frames=frames, fps=fps / 100, vdd=vdd / 100,
                   ta=ta / 100, last_cmd=lastcmd, flags=flags, params=params)
    elif typ == 4:
        if n != 8:
            raise ValueError('bad OTA_STATUS payload length')
        state, percent, error, _, image = struct.unpack('<BBBBI', body)
        pkt.update(state=state, percent=percent, error=error, image=image)
    return pkt


def send_command(ip, key, uid_hex, opcode, arg0=0, value=0):
    body = struct.pack('<IBBi', int(time.time()), opcode, arg0, value)
    head = HEADER.pack(b'TM', 1, 16, bytes.fromhex(uid_hex.replace(':', '')), 0, 0, 0, len(body))
    tag = hmac.new(key, head + body, hashlib.sha256).digest()[:TAG]
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    if os.environ.get('UDP_IFACE'):
        bind_to_interface(s, os.environ['UDP_IFACE'])
    s.sendto(head + body + tag, (ip, 5201))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--iface', default=os.environ.get('UDP_IFACE', ''))
    ap.add_argument('--port', type=int, default=5200)
    ap.add_argument('--seconds', type=float, default=0)
    ap.add_argument('--json', default='')
    ap.add_argument('--quiet', action='store_true')
    ap.add_argument('--device-keys', default=os.environ.get('DEVICE_KEYS_FILE', ''))
    ap.add_argument('--command', nargs='+', metavar=('IP UID OP', 'ARGS'))
    a = ap.parse_args()
    key = os.environ.get('TM_KEY', '').encode()
    devices = None
    if a.device_keys:
        path = Path(a.device_keys)
        if path.stat().st_mode & 0o077 or path.stat().st_size > 2*1024*1024:
            raise SystemExit('device keys require a bounded mode-0600 file')
        devices = json.loads(path.read_text())
        if devices.get('version') != 2 or not isinstance(devices.get('nodes'), dict):
            raise SystemExit('invalid device key file')

    if a.command:
        if devices is not None:
            raise SystemExit('Use the TMedge console for encrypted commands; bench commands are legacy-only.')
        ip, uid, op, *rest = a.command
        if a.iface:
            os.environ['UDP_IFACE'] = a.iface
        ops = {'set': 1, 'reset-bg': 2, 'identify': 3, 'reboot': 4, 'save': 5}
        arg0 = PARAMS.index(rest[0]) if op == 'set' else 0
        value = int(rest[1]) if op == 'set' else (int(rest[0]) if rest else 5)
        send_command(ip, key, uid, ops[op], arg0, value)
        print('sent')
        return

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    if a.iface:
        bind_to_interface(sock, a.iface)
    sock.bind(('0.0.0.0', a.port))
    sock.settimeout(1.0)
    out = open(a.json, 'w') if a.json else None
    end = time.time() + a.seconds if a.seconds else None
    counts, errors, last_seq = {}, {}, {}
    gaps = 0
    print(f'listening udp/{a.port}' + (f' via {a.iface}' if a.iface else '') + (' (verifying)' if key else ' (NOT verifying: no TM_KEY)'))
    while end is None or time.time() < end:
        try:
            dg, src = sock.recvfrom(2048)
        except socket.timeout:
            continue
        try:
            p = parse(dg, key, devices)
        except ValueError as e:
            errors[str(e)] = errors.get(str(e), 0) + 1
            print(f'!! {src[0]}: {e}')
            continue
        p['from'] = src[0]
        p['t'] = time.time()
        counts[p['type']] = counts.get(p['type'], 0) + 1
        k = (p['uid'], p['boot'])
        if k in last_seq and p['seq'] != last_seq[k] + 1:
            gaps += p['seq'] - last_seq[k] - 1
        last_seq[k] = p['seq']
        if out:
            out.write(json.dumps(p) + '\n')
        if not a.quiet:
            if p['type'] == 'REPORT':
                print(f"{p['uid']} REPORT f{p['frame']} people={len(p['dets'])} scene {p['scene'][0]:.1f}..{p['scene'][1]:.1f} "
                      f"flags={p['flags']} " + ' '.join(f"({d['x']:.1f},{d['y']:.1f} +{d['contrast']:.1f}C a{d['area']})" for d in p['dets']))
            elif p['type'] == 'STATUS':
                print(f"{p['uid']} STATUS {p['fw']} ip={p['ip']} rssi={p['rssi']} fps={p['fps']} heap={p['heap']} "
                      f"stack_free={p['stack_free']} Ta={p['ta']} Vdd={p['vdd']} last_cmd={p['last_cmd']} {p['params']}")
    print(f'\npackets {counts}  errors {errors}  sequence gaps {gaps}')


if __name__ == '__main__':
    main()
