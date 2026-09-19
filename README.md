# TMsense

*(formerly TMnode — the name still appears in code identifiers and the
wire format, which are unchanged.)*

Firmware for a thermal occupancy node: Heltec WiFi LoRa 32 V3 (ESP32-S3) and
an MLX90640 with the 110 x 75 deg lens, ceiling-mounted 3-5 m up. It finds
people as warm blobs and sends a small signed report per frame to **TMedge**,
which turns reports from many nodes into seat occupancy.

No image is needed downstream to count people. The node sends the full
32 x 24 frame only while it is being commissioned (`raw_every`), and only to
the edge.

## What it sends

| Packet | When | Size | Used for |
|---|---|---|---|
| REPORT | every frame (1 fps) | 44 B + 7 B per person, max 212 B | occupancy |
| RAW | every `raw_every` frames (default 1, set 0 in production) | 806 B | debug console, calibration |
| STATUS | every 10 s, and after every command | 118 B | fleet health, settings in force |

The byte layout is in [`include/tm_protocol.h`](include/tm_protocol.h), which
is the source of truth. A REPORT fits a single LoRa frame, so a LoRa transport
can be added later without changing the format.

Traffic per node on Wi-Fi: about 0.1 kB/s in production, and about 0.9 kB/s
while commissioning with RAW on every frame. It is all one-datagram UDP: no
broadcast, no TCP, no retries. The next REPORT supersedes a lost one a second
later.

## Build and flash

The easy way is **TMflash** (`../TMflash`), a Mac app that builds this
firmware, flashes one board or up to 10 at once, and provisions each one (node
ID, Wi-Fi/LoRa, SSID, password, gateway, key) in one click. By hand:

```bash
tools/build.sh upload            # arduino-cli, esp32 core 2.0.17
# or: pio run -t upload
```

Board: *Heltec WiFi LoRa 32(V3)*. Sensor on SDA 41 / SCL 42.

## Provision a node (no recompiling)

Every node runs the same image. Its identity is its factory MAC. Settings go in
over USB serial at 115200 baud and are kept in flash:

```
set id <1-65535>          node ID: a label (enclosure, manifest); identity stays the MAC
set mode wifi             or `lora` (stored; the LoRa uplink is not written yet)
set ssid <network>        2.4 GHz only
set pass <password>
set edges 10.0.0.5,10.0.0.6   where to send: the site's TMWAccess, or TMedge directly
set lora_gw <ip>          TMLAccess address, for LoRa mode
set key <shared key>      same as TMedge's TM_KEY
save
reboot
```

`show` prints the settings; the password and key are never printed. `help`
lists everything else (detector parameters, `reset-bg`, `factory`).

`pio run -e tmflash` builds the release image TMflash uses: identical, but
built with `TM_NO_NODE_CONFIG` so no bench defaults are compiled in.

For bench work you can also bake in defaults: copy
`include/node_config.example.h` to `include/node_config.h` (git-ignored).
Settings saved in flash override these defaults.

## Detector

`src/tm_detector.cpp` is written for small blobs. At 3-5 m with this lens, a
seated person covers 2-12 pixels. It thresholds in degrees C against a
per-pixel background and noise model, splits blobs at heat peaks, and never
absorbs a seated person into the background. Each blob carries its **heat**
(summed excess temperature), so the edge can tell one person from two who have
merged. Parameters can be changed live from TMedge or the serial console.

Known limit: a warm laptop looks like a small person. Telling them apart is
the edge's job (see TMedge's Phase 3 design).

## Tests

```bash
python3 test/host/detector_test.py     # 23 scenarios, 110 deg at 3 m and 5 m
test/host/build_packet_host.sh         # host build of the packet code (TMedge crosscheck uses it)
TM_KEY=... python3 tools/listen.py --iface en0   # watch a live node
```

The detector scenarios render physically based scenes: f-theta optics,
people as warm discs at head height, sub-pixel coverage, and sensor noise
and drift. The C detector code runs on them unchanged.

## Security

- Every packet carries a truncated HMAC-SHA256 tag (8 bytes).
- `(boot, seq)` only ever increases, because the boot counter is kept in
  flash, so replays are rejected across reboots too.
- Commands from the edge must be signed, addressed to this node's MAC, and
  newer than the last command applied. That check survives reboots.
- The key is shared by every node on a site. See TMedge docs for rotation.
