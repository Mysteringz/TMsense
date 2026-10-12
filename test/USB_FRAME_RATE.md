# USB frame-rate verification

Verified on 2026-10-09 with a Heltec V3 / MLX90640, node ID 3
(`30:ed:a0:cb:f5:f8`), on `/dev/cu.usbserial-0001`.
The secret-free `tmflash` image was built and uploaded as TMsense 1.6.
Saved settings and the increasing boot counter survived the upload.

TMflash's real CLI provisioned `--fps 2` with `--no-flash`, then read the
configured rate back successfully. Further checks used TMflashCore's
`SerialPort` and `NodeConsole` against the physical board.

| Selection | MLX refresh code | Measured full frames/s | Last firmware fps estimate | Survived reboot |
|---|---|---|---|---|
| 1 fps | 2 | 0.977 | 0.98 | Yes |
| 2 fps | 3 | 1.955 | 1.96 | Yes |
| 4 fps | 4 | 3.906 | 3.91 | Yes |

Each measurement collected the firmware's frame heartbeat for 70 seconds.
Measured rates use the difference between frame counters divided by elapsed
host monotonic time, independently of the firmware's smoothed fps estimate.
Reboot checks waited for a new boot counter before reading the saved rate.
Boot counters increased from 67 through 70 during this run.

`set fps 0`, `3`, `8`, `-1` and `2garbage` were all rejected, leaving the
current setting unchanged. The original refresh code 2 (1 fps) was restored
and saved afterward.

After the rate checks, the requested Wi-Fi settings were saved using the
advertised SSID spelling `EsanHouse`. On boot 72 the board joined as
`192.168.0.9`, authenticated its existing WSS uplink, and TMedge acknowledged
a report. At the user's subsequent request, the board was saved at 4 fps and
rebooted to boot 73. Readback confirmed 4 fps, the live edge acknowledged a
report from that boot, and another frame-counter measurement gave 3.904 fps.
The final saved rate is 4 fps. No credentials are recorded here.

Additional verification: all 61 final TMflash tests passed, including serial
provisioning, readback failures, old-firmware refusal and keep-current
behavior. Both firmware build environments compiled; cloud/display host
tests and all 23 detector scenarios passed. The new picker was visually
checked in native light and dark snapshots.

The broader TMedge checks have separate failures on this checkout: its
oversized-upload HTTP test fails with `fetch failed`, and the encrypted
cloud harness is refused with close code 4401. Both also fail under the
CI Node 22 runtime. Wire-format checks and the plain cloud session checks
pass; the packet layout was not changed by frame-rate selection.

## Re-verified as TMsense 1.8 (2026-10-12)

After merging main's encrypted-telemetry work, the same board was flashed with
the secret-free `tmflash` image as TMsense 1.8, using TMflash's CLI built from
its master. Saved settings survived; the boot counter rose from 87 to 92 over
the run. `show` reported `encryption: legacy (key id 0)` and capabilities
`wss1,ota-https1,aead2,udp-ack1,boot32,fps1`.

| Selection | MLX refresh code | Measured full frames/s | Last firmware fps estimate | Set by |
|---|---|---|---|---|
| 1 fps | 2 | 0.982 | 0.98 | TMflash `--no-flash --fps 1`, saved, reboot; measured after a live `set fps 1` |
| 2 fps | 3 | 1.959 | 1.96 | TMflash `--no-flash --fps 2`, saved, reboot |
| 4 fps | 4 | 3.914 | 3.93 | TMflash `--no-flash --fps 4`, saved, reboot |

Windows were 30 s for 2 and 4 fps and 61 s for 1 fps, shorter than the 70 s
used above. After the flash and after each of the three rate changes the board
rejoined Wi-Fi, authenticated its WSS uplink and TMedge acknowledged a report
from the new boot.

The five invalid `set fps` values above were rejected again. `set secure` was
checked for refusals only: a zero ID, a short key, a non-hex key and an
all-zero key were each rejected and left the node on legacy signing. The board
was not enrolled, so encrypted transport was not exercised on hardware. The
final saved rate is 4 fps, as before.
