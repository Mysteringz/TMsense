# TMsense

TMsense is firmware for ceiling-mounted thermal occupancy nodes built from a Heltec WiFi LoRa 32 V3 board and an MLX90640 sensor. It detects warm blobs in a 32×24 temperature grid and sends compact reports to TMedge.

The firmware supports local Wi-Fi UDP and direct secure WebSocket transport. Optional RAW packets support diagnostics and calibration. LoRa settings are present, but the LoRa uplink is not implemented.

## Interesting techniques

- **Small-blob detection.** The [detector](src/tm_detector.cpp) learns a per-pixel background and noise model, thresholds temperature contrast, and splits connected regions at heat peaks. Pixels assigned to accepted people are protected from background adaptation.
- **Complete-frame acquisition.** The [sensor reader](src/tm_sensor.cpp) waits for both MLX90640 subpages before emitting a frame. It repairs broken or implausible pixels using valid neighbors.
- **Compact binary telemetry.** The [wire contract](include/tm_protocol.h) uses explicit little-endian fields and quantized measurements. Reports hold up to 24 detections while remaining within a 222-byte transport budget.
- **Persistent replay protection.** [Packet authentication](src/tm_packet.cpp) uses truncated HMAC-SHA256 tags. Boot and command counters survive restarts through [persistent settings](src/tm_settings.cpp).
- **Separate sensing and network work.** The [cloud transport](src/tm_cloud.cpp) runs networking in a dedicated FreeRTOS task with bounded queues, keeping TLS work away from the sensor loop.
- **Explicit certificate-date checks.** Direct-cloud connections verify the certificate chain, hostname, and validity dates. A cloud report acknowledgement proves delivery to the edge.
- **One firmware image for every node.** The release configuration in [platformio.ini](platformio.ini) excludes bench credentials. Identity and site settings are provisioned through the serial console and stored in flash.
- **Checked updates with rollback.** [OTA handling](src/tm_ota.cpp) verifies the requested SHA-256 before booting an image. Startup probation checks Wi-Fi, sensor operation, and uplink health; direct-cloud operation requires fresh accepted reports.
- **Runtime sampling controls.** [Settings](src/tm_settings.cpp) expose sensor refresh, RAW cadence, and detector parameters without rebuilding. Detector timing and RAW cadence are measured in frames, so their intervals in seconds change with the sampling rate.

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

Host tests exercise the detector, packet serializer, cloud session, WebSocket framing, and display logic. The companion TMedge cross-check parses bytes emitted by the actual firmware serializer.
