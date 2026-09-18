# TMnode — notes for AI coding sessions

Firmware for the thermal occupancy node. Pairs with `../TMedge`.

## Map

```
include/tm_protocol.h      wire format — SOURCE OF TRUTH (mirrored by TMedge/src/shared/protocol.ts)
src/tm_packet.cpp          builds/parses it, HMAC tag (mbedTLS; host shim in test/host/mbedtls)
src/tm_detector.cpp        person detector (plain C, host-testable)
src/tm_sensor.cpp          MLX90640, non-blocking subpage acquisition
src/tm_settings.cpp        settings in flash + serial console
src/tm_transport.cpp       Wi-Fi UDP up (5200) / down (5201)
src/main.cpp               loop
src/MLX90640_*             Melexis driver (vendor code, with fixes; keep changes minimal)
```

## Commands

```bash
tools/build.sh [upload [port]]           # arduino-cli build / flash
python3 test/host/detector_test.py       # must pass after any detector change
TM_KEY=... python3 tools/listen.py --iface en0
```

## Invariants

- **Wire format changes touch both repos in one go**: `tm_protocol.h`,
  `tm_packet.cpp`, `tools/listen.py`, and TMedge's `protocol.ts`. Then run
  `npm run crosscheck` in TMedge, which parses bytes from `packet_host`.
- **A REPORT must fit a LoRa frame** (≤ 222 B). Anything bigger belongs in RAW
  or a new packet type.
- **One image, no per-node compile.** Identity = factory MAC; settings in NVS.
  Never add a setting that only a recompile can change.
- **Never print the key or Wi-Fi password.** `show` reports only whether they
  are set.
- **The boot counter is never reset** (not even by `factory`). Resetting it
  makes the node's packets look like replays to every edge.
- **The detector never absorbs an accepted person into the background.** A
  student sitting still for two hours is still there.
- **Buffers over ~1 kB are static**, not on the 8 kB loop-task stack.
- Detector changes need a scenario in `detector_test.py` that fails without
  them.

## Hardware notes

Heltec V3, MLX90640 at 0x33 on SDA 41 / SCL 42, 1 MHz I²C. Refresh code 2
(2 Hz subpages) gives 1 full frame/s. The MLX needs a moment after power-on,
which is why the probe retries.
On the dev Mac, NordVPN blocks LAN traffic: bind receivers to `en0`.
