#ifndef TM_OTA_H
#define TM_OTA_H

#include <stdbool.h>
#include <stdint.h>
#include "tm_packet.h"

/**
 * Over-the-air update.
 *
 * The edge sends a signed OTA request; the node fetches the image and writes
 * it to the spare app partition. Where it fetches from depends on the
 * transport the request came in on:
 *
 *   udp  plain HTTP from the gateway that delivered the request -- the one
 *        machine on its network it already talks to (address + port + path).
 *   wss  HTTPS from the node's own provisioned cloud host, on the cloud_url
 *        port (443), path /fw/<build>.bin only, with the bearer token from an
 *        `ota_grant` bound to the same sequence and build. Never a proxy's
 *        address, never another host, never a redirect or plain HTTP.
 *
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

/**
 * Start an update that arrived over the cloud session. `host`/`port` are the
 * provisioned cloud_url's; the request must name that port and a
 * /fw/<16 hex>.bin path, and a matching download grant must have arrived.
 */
void tm_ota_begin_cloud(const TmOtaRequest* req, const char* host, uint16_t port);

/** True while an image is being fetched: the caller should pause its work. */
bool tm_ota_busy();

/** Drives the download; call from the main loop. */
void tm_ota_update();

/**
 * Feed the health of the running image once a second after boot. While a
 * freshly flashed image is on probation this decides between confirming it
 * and rolling back; at all other times it does nothing.
 *
 * `uplink_ok` must be evidence the edge accepted something *this boot*: for
 * wss, a new ACK for a REPORT this image generated -- never a socket write,
 * a `ready`, a ping or an old ACK. (For udp it is still the older heuristic,
 * a datagram that left the radio; UDP has no acknowledgement.)
 */
void tm_ota_health(bool wifi_ok, bool sensor_ok, bool uplink_ok);

#endif // TM_OTA_H
