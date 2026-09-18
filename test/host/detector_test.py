#!/usr/bin/env python3
"""Detector scenarios for 110 deg lens at 3 m and 5 m. Runs src/tm_detector.cpp.

    python3 test/host/detector_test.py                    # exit 1 on failure
    python3 test/host/detector_test.py --param min_peak=1.5   # try a tuning

Each scenario states a claim about behaviour. Thresholds are what a student
would notice, not what the code happens to produce: a seated person must be
seen in nearly every frame, an empty room must stay empty.
"""
import argparse, json, os, subprocess, sys, tempfile, zlib
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)
from scene import Person, Blob, render, pixel_of  # noqa: E402

# A 6-person bench, 105 x 167 cm, centred at (tx, ty): three seats per long side.
def seats(tx=0.0, ty=0.0):
    return [(tx + sx * 0.83, ty + sy) for sx in (-1, 1) for sy in (-0.56, 0.0, 0.56)]


def build():
    out = os.path.join(tempfile.mkdtemp(), 'detector_host')
    subprocess.run(['g++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                    f'-I{ROOT}/include', os.path.join(HERE, 'detector_host.cpp'),
                    f'{ROOT}/src/tm_detector.cpp', '-o', out], check=True)
    return out


def run(binary, frames, args=()):
    data = np.stack(frames).astype(np.float32).tobytes()
    res = subprocess.run([binary, *args], input=data, capture_output=True, check=True)
    return [json.loads(l) for l in res.stdout.decode().splitlines()]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--param', action='append', default=[], help='detector override name=value')
    a = ap.parse_args()
    binary = build()
    results = []

    def scenario(name, mount, frames_fn, n_frames, check, learn=25, **room):
        rng = np.random.default_rng(zlib.crc32(f'{name}@{mount}'.encode()))
        frames = [render(mount, rng=rng, **room) for _ in range(learn)]
        frames += [frames_fn(i, rng) for i in range(n_frames)]
        out = run(binary, frames, a.param)[learn:]
        ok, why = check(out)
        results.append((name, ok))
        print(f"{'PASS' if ok else 'FAIL'}  [{mount} m] {name}: {why}")

    for mount in (3.0, 5.0):
        # 1. Empty room with sensor noise and a 1 C drift over 20 minutes.
        scenario('empty room, 1 C drift, 20 min: no people', mount,
                 lambda i, r: render(mount, rng=r, offset_c=1.0 * i / 1200), 1200,
                 lambda o: (sum(f['n'] for f in o) <= 0.002 * len(o),
                            f"false detections in {sum(1 for f in o if f['n'])}/{len(o)} frames"))

        # 2. One seated person at each seat of a table under the sensor, and at
        #    a table offset 2 m, where the lens is at its widest angles.
        for tx, ty, label in ((0.0, 0.0, 'table under sensor'), (2.0, 0.8, 'table 2 m off-axis')):
            for s_i, (sx, sy) in enumerate(seats(tx, ty)[:3]):
                u, v = pixel_of(sx, sy, mount)
                def one(o, u=u, v=v):
                    good = [f for f in o if f['n'] == 1 and abs(f['d'][0][0] - u) < 0.8 and abs(f['d'][0][1] - v) < 0.8]
                    return len(good) >= 0.95 * len(o), f"{len(good)}/{len(o)} frames exact; e.g. {o[-1]['d']} vs true ({u:.2f},{v:.2f})"
                scenario(f'one person, {label}, seat {s_i + 1}', mount,
                         lambda i, r, sx=sx, sy=sy: render(mount, [Person(sx, sy)], rng=r), 60, one)

        # 3. Six people around one table: counted without merging, at 3 m.
        #    At 5 m neighbours 56 cm apart are only ~1.7 px apart, close to the
        #    split separation, so the claim there is the weaker one the edge
        #    relies on: total heat scales with the people present.
        full = [Person(x, y) for x, y in seats()]
        if mount == 3.0:
            scenario('full table of 6 counts 6', mount,
                     lambda i, r: render(mount, full, rng=r), 60,
                     lambda o: (sum(f['n'] == 6 for f in o) >= 0.9 * len(o),
                                f"count histogram {np.bincount([f['n'] for f in o]).tolist()}"))
        else:
            one_heat = []
            scenario('5 m: one person heat (reference)', mount,
                     lambda i, r: render(mount, [Person(*seats()[0])], rng=r), 40,
                     lambda o: (one_heat.append(np.median([sum(d[5] for d in f['d']) for f in o])) or True,
                                f"median heat {one_heat[-1]:.1f} C*px"))
            scenario('5 m: full table heat ~6x one person', mount,
                     lambda i, r: render(mount, full, rng=r), 40,
                     lambda o: ((lambda h: 4.5 <= h / one_heat[0] <= 7.5)(np.median([sum(d[5] for d in f['d']) for f in o])),
                                f"heat ratio {np.median([sum(d[5] for d in f['d']) for f in o]) / one_heat[0]:.2f}, "
                                f"blob counts {np.bincount([f['n'] for f in o]).tolist()}"))

        # 4. A person who sits still for 30 minutes (1 fps) while the room drifts.
        scenario('seated 30 min with drift: still counted', mount,
                 lambda i, r: render(mount, [Person(0.83, 0.0)], rng=r, offset_c=0.8 * i / 1800), 1800,
                 lambda o: (sum(f['n'] == 1 for f in o[-60:]) >= 57,
                            f"counted in {sum(f['n'] == 1 for f in o[-60:])}/60 of the last minute"))

        # 5. Someone arrives, stays two minutes, leaves: gone within 10 s.
        def visit(i, r):
            return render(mount, [Person(0.83, 0.56)] if 30 <= i < 150 else [], rng=r)
        scenario('arrive, stay, leave: count follows', mount, visit, 240,
                 lambda o: (sum(f['n'] == 1 for f in o[35:150]) >= 110 and sum(f['n'] for f in o[160:]) == 0,
                            f"present {sum(f['n'] == 1 for f in o[35:150])}/115, after leaving {sum(f['n'] for f in o[160:])}"))

        # 6. Hard case: a 28 C room with the air-con off, so clothing is under
        #    2 C warmer than the floor, and a noisier sensor.
        two = [Person(0.83, 0.0, core_c=31.0, body_c=29.8), Person(-0.83, 0.56, core_c=31.0, body_c=29.8)]
        scenario('warm 28 C room, 2 people, noise 0.25 C: counts 2', mount,
                 lambda i, r: render(mount, two if i < 100 else [], floor_c=28.0, noise_c=0.25, rng=r), 400,
                 lambda o: (sum(f['n'] == 2 for f in o[:100]) >= 95 and sum(f['n'] for f in o[110:]) == 0,
                            f"2 counted in {sum(f['n'] == 2 for f in o[:100])}/100, false after leaving {sum(f['n'] for f in o[110:])}"),
                 floor_c=28.0, noise_c=0.25)

    # Recorded, not asserted: a warm laptop on the desk is a small hot blob, and
    # at this resolution it looks like a person. The edge has to handle it.
    rng = np.random.default_rng(7)
    frames = [render(3.0, rng=rng) for _ in range(25)] + \
             [render(3.0, blobs=[Blob(0.3, 0.2, 0.15, 34.0)], rng=rng) for _ in range(30)]
    out = run(binary, frames, a.param)[25:]
    print(f"NOTE  [3.0 m] warm laptop (34 C, 30 cm) detected as a blob in "
          f"{sum(f['n'] > 0 for f in out)}/{len(out)} frames -- known limit, handled at the edge")

    failed = [r for r in results if not r[1]]
    print(f"\n{len(results) - len(failed)}/{len(results)} scenarios pass")
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
