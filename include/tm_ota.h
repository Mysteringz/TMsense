#ifndef TM_OTA_H
#define TM_OTA_H

#include <stdbool.h>
#include <stdint.h>
#include "tm_packet.h"

/**
 * Over-the-air update.
 *
 * The edge sends a signed OTA request; the node fetches the image over plain
 * HTTP from the gateway that delivered the request -- the one machine on its
 * network it already talks to -- and writes it to the spare app partition.
 * Trust comes from the SHA-256 inside the signed request, not from the
 * transport: the gateway can serve whatever it likes, and a single wrong byte
 * fails the hash and the update is thrown away before it can boot.
 *
 * After flashing, the node reboots and has to prove itself: Wi-Fi up, sensor
 * reading and a packet accepted by the edge, within TM_OTA_PROVE_MS. If it
 * cannot, it sets the boot partition back to the image that was working and
 * reboots again. A node on a ceiling is expensive to reach, so the firmware
 * never assumes a fresh image is a good one.
 */

/** How long a freshly flashed image has to prove itself before it is reverted. */
#define TM_OTA_PROVE_MS 180000

/** Sends an OTA_STATUS packet; main owns the radio, so it provides this. */
typedef void (*TmOtaReporter)(const TmOtaStatus* status);

void tm_ota_init(TmOtaReporter reporter);

/**
 * Start an update. `gateway` is the address the request arrived from.
 * Refuses (and reports) if one is already running.
 */
void tm_ota_begin(const TmOtaRequest* req, const uint8_t gateway[4]);

/** True while an image is being fetched: the caller should pause its work. */
bool tm_ota_busy();

/** Drives the download; call from the main loop. */
void tm_ota_update();

/**
 * Feed the health of the running image once a second after boot. While a
 * freshly flashed image is on probation this decides between confirming it
 * and rolling back; at all other times it does nothing.
 */
void tm_ota_health(bool wifi_ok, bool sensor_ok, bool uplink_ok);

#endif // TM_OTA_H
