# TMsense

TMsense is firmware for ceiling-mounted thermal occupancy nodes built from a Heltec WiFi LoRa 32 V3 board and an MLX90640 sensor. It detects warm blobs in a 32×24 temperature grid and sends compact reports to TMedge.

The firmware supports local Wi-Fi UDP and direct secure WebSocket transport. Optional RAW packets support diagnostics and calibration. LoRa settings are present, but the LoRa uplink is not implemented.

## Interesting techniques

- **Small-blob detection.** The [detector](src/tm_detector.cpp) learns a per-pixel background and noise model, thresholds temperature contrast, and splits connected regions at heat peaks. Pixels assigned to accepted people are protected from background adaptation.
- **Complete-frame acquisition.** The [sensor reader](src/tm_sensor.cpp) waits for both MLX90640 subpages before emitting a frame. It repairs broken or implausible pixels using valid neighbors.
- **Compact binary telemetry.** The [wire contract](include/tm_protocol.h) uses explicit little-endian fields and quantized measurements. Reports hold up to 24 detections while remaining within a 222-byte transport budget.
- **Authenticated telemetry and persistent replay protection.** Legacy [packet authentication](src/tm_packet.cpp) uses truncated HMAC-SHA256 tags. Enrolled nodes use [AES-256-GCM encryption](src/tm_secure.cpp) with per-device keys on UDP and WSS. Boot and command counters survive restarts and factory resets through [persistent settings](src/tm_settings.cpp). See the [encryption and provisioning runbook](https://github.com/Mysteringz/TMedge/blob/main/docs/ENCRYPTED_NODE_PROTOCOL.md) for enrollment and recovery requirements.
- **Separate sensing and network work.** The [cloud transport](src/tm_cloud.cpp) runs networking in a dedicated FreeRTOS task with bounded queues, keeping TLS work away from the sensor loop.
- **Explicit certificate-date checks.** Direct-cloud connections verify the certificate chain, hostname, and validity dates. A cloud report acknowledgement proves delivery to the edge.
- **One firmware image for every node.** The release configuration in [platformio.ini](platformio.ini) excludes bench credentials. Identity and site settings are provisioned through the serial console and stored in flash.
- **Checked updates with rollback.** [OTA handling](src/tm_ota.cpp) verifies the requested SHA-256 before booting an image. Startup probation checks Wi-Fi, a recent complete sensor frame, and uplink health; direct-cloud and enrolled UDP operation require fresh accepted REPORT acknowledgements. Restarting an unconfirmed image rolls back on its next boot.
- **Runtime sampling controls.** [Settings](src/tm_settings.cpp) expose `set fps 1`, `set fps 2` and `set fps 4` for complete thermal frames per second (default 1), plus RAW cadence and detector parameters, without rebuilding. The rate applies immediately; `save` keeps it across reboots. Detector timing and RAW cadence are measured in frames, so their intervals in seconds shorten and telemetry traffic increases at higher rates.

TMsense 1.8 reports the configured rate as `fps` before `boot` in `show` and advertises capability `fps1` for TMflash. The 1, 2 and 4 fps settings use MLX90640 refresh codes 2, 3 and 4 because each complete frame needs two subpages. The existing `param refresh` control and stored settings remain compatible.

## Technologies and libraries

- [Arduino ESP32](https://docs.espressif.com/projects/arduino-esp32/en/latest/) and [PlatformIO](https://docs.platformio.org/en/latest/) provide the firmware framework and build configuration.
- The [Melexis MLX90640 library](https://github.com/melexis/mlx90640-library) handles calibration and temperature conversion.
- [Mbed TLS](https://www.trustedfirmware.org/projects/mbed-tls/) supplies authentication and cryptographic primitives.
- [ESP32 FreeRTOS](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/system/freertos_idf.html) handles task scheduling. [Preferences](https://docs.espressif.com/projects/arduino-esp32/en/latest/api/preferences.html) stores settings in NVS.
- [NumPy](https://numpy.org/doc/) supports simulated detector scenes. [Pillow](https://pillow.readthedocs.io/en/stable/) generates OLED assets.

## Display assets and fonts

OLED status text uses the repository's own [5×7 bitmap font](tools/font5x7.py). The splash wordmark is generated from [DIN Alternate Bold](https://support.apple.com/en-us/103197) by the [asset generator](tools/make_display_assets.py). Generated bitmaps are committed in the [display asset header](include/tm_display_assets.h), so firmware builds do not need the font or Pillow.

## Project structure

```text
TMsense/
├── .github/workflows/
├── include/
├── src/
├── test/
│   └── host/
│       ├── fixtures/
│       └── mbedtls/
├── tools/
├── README.md
└── platformio.ini
```

- [include/](include/) contains the wire contract, module interfaces, trust roots, and generated display assets.
- [src/](src/) contains acquisition, detection, transport, serial settings, OTA, and OLED code.
- [test/host/](test/host/) runs firmware components on a desktop, including simulated thermal scenes, protocol fixtures, and crypto compatibility shims.
- [tools/](tools/) contains build helpers, a telemetry listener, certificate updates, and display-asset generation.

Host tests exercise the detector, packet serializer, encryption, cloud session, WebSocket framing, display logic, and OTA/settings/sensor fault handling. The companion TMedge cross-check parses bytes emitted by the actual firmware serializer.
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
set fps <1|2|4>           complete thermal frames per second (default 1)
save
reboot
```

`show` prints the settings; the password and key are never printed. `help`
lists everything else (detector parameters, `reset-bg`, `factory`).

TMsense 1.8 adds `set fps 1`, `set fps 2` and `set fps 4`. The rate takes
effect immediately; `save` keeps it across reboots. `show` reports the configured
rate as `fps` before `boot`, and advertises capability `fps1` for TMflash.
These rates use MLX refresh codes 2, 3 and 4: the sensor needs two subpages
for each complete frame. The existing `param refresh` control and stored
settings remain compatible. RAW cadence and detector timing parameters stay
in frames, so their intervals in seconds shorten at higher rates. Traffic
scales with the selected rate; the figures above are for 1 fps.

`pio run -e tmflash` builds the release image TMflash uses: identical, but
built with `TM_NO_NODE_CONFIG` so no bench defaults are compiled in.

For bench work you can also bake in defaults: copy
`include/node_config.example.h` to `include/node_config.h` (git-ignored).
Settings saved in flash override these defaults.

## Updating over the air

From `tmsense-1.2` the node takes firmware updates from TMedge's console: it
receives a signed request naming a port, a path and a SHA-256, downloads the
image from the gateway that delivered the request, checks the hash, writes it
to the spare app partition and reboots.

A new image is then on probation for three minutes. It has to join Wi-Fi, read
the sensor and get a packet accepted by the edge; if it cannot, the node sets
the boot partition back to the image that was working and reboots. `src/tm_ota.cpp`.

Progress is reported as OTA_STATUS packets, so a rollout can be watched from
the console rather than guessed at.

From `tmsense-1.7`, probation is recorded before activation, and restarting an
unconfirmed image rolls back on its next boot. Confirmation needs a recent
complete sensor frame; a successful probe alone is insufficient. Flash writes
must persist before commands or OTA are applied. Downloads stop after three
minutes even if tiny reads continue to arrive.

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
test/host/build_cloud_host.sh && build/cloud_test && build/display_test
bash test/host/build_safety_host.sh && build/safety_test  # OTA/NVS/I2C fault injection, UBSan
python3 test/host/listen_test.py         # malformed UDP payload handling
bash test/host/build_secure_host.sh      # actual mbedTLS AES-GCM vectors/tamper/downgrade (Linux libmbedtls-dev)
TM_KEY=... python3 tools/listen.py --iface en0   # watch a live node
```

The detector scenarios render physically based scenes: f-theta optics,
people as warm discs at head height, sub-pixel coverage, and sensor noise
and drift. The C detector code runs on them unchanged.

## Security

TMsense 1.7 supports individually enrolled AES-256-GCM payload encryption on
UDP and WSS. See [the paired protocol and provisioning runbook](https://github.com/Mysteringz/TMedge/blob/main/docs/ENCRYPTED_NODE_PROTOCOL.md).
Encryption is opt-in through trusted local `set secure <id>:<64hex>` provisioning
and a private matching edge key policy. Provision compatible firmware in both
OTA slots before enrollment; older recovery firmware cannot preserve the policy.

- Enrolled telemetry, frames, commands and OTA requests use wire v2 with a
  full 16-byte GCM tag, per-device direction keys and fresh key epochs. Direct
  socket controls/grants are also encrypted and bound to their session challenge.
- Legacy packets carry a truncated HMAC-SHA256 tag (8 bytes) without encryption.
- `(boot, seq)` only ever increases, because the boot counter is kept in
  flash, so replays are rejected across reboots too.
- Commands from the edge must be signed, addressed to this node's MAC, and
  newer than the last command applied. That check survives reboots.
- Factory reset preserves boot32, command high-water and encryption enrollment.
  Interrupted enrollment and persistent counter failure disable networking.
  Legacy nodes stop at 65,535 boots; enrolled v2 nodes use a 32-bit boot counter.
- Enrolled UDP probation needs an authenticated accepted REPORT ACK. Legacy UDP
  can only prove a send and needs migration for this assurance.
- The legacy signing key is shared by a site; enrolled masters must be unique.
  Never place production keys in firmware source or host-test fixtures. The
  listener uses a private `--device-keys` file and Python `cryptography`; CI pins
  its dependency. Host cloud tests need OpenSSL (macOS: `brew install openssl@3`).

## Continuous integration

CI runs the packet harness, detector scenarios and a PlatformIO `tmflash`
compile without bench secrets, then packages that build the way a release
would be. The firmware artifact is retained for 30 days.
Flashing remains an explicit pilot-first operation.

## Releases

TMedge's web flasher (algo console, module 07) writes the **latest release of
this repository** to boards, and nothing else. Publishing a release is
therefore what puts firmware on newly flashed nodes.

To publish: raise `TM_FW_VERSION` in `include/tm_config.h`, merge to `main`,
then run the **Release** workflow from `main` (Actions, "Run workflow", or
`gh workflow run release.yml --ref main`). It refuses a commit CI has not
passed, builds `env:tmflash`, and publishes four files:

```
bootloader.bin  partitions.bin  firmware.bin    what a blank board needs
tmflash-manifest.json                           version, commit, size and SHA-256 of each
```

The tag is derived from the version (`tmsense-1.5` becomes `v1.5`), never
typed, and a version that is already released is refused: a node reports that
string, and it must name one image. The workflow runs in the
`firmware-release` environment; add required reviewers to it in the
repository settings to make a release wait for a second person.

`tools/release_manifest.py` writes the manifest and TMedge reads it
(`src/shared/flasher/release.ts` there). Change the two together.

Default-branch changes go through a pull request with required checks.
GitHub Actions dependencies are pinned and updated through Dependabot PRs.
