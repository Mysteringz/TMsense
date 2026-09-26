#ifndef TM_TRANSPORT_H
#define TM_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/**
 * The uplink, whatever carries it. Callers hand it signed packets and get
 * commands back; which transport is underneath is a setting:
 *
 *   udp  Wi-Fi UDP to the local gateway or edge in `edges` (5200 up, 5201
 *        down). Deliberately light on the network it shares: one datagram per
 *        packet, no broadcast or multicast (both are expensive on campus
 *        Wi-Fi), no retries -- a lost REPORT is superseded a second later.
 *   wss  one TLS WebSocket straight to TMedge at `cloud_url` (tm_cloud.h),
 *        for a site with no gateway machine. The same packets, byte for byte.
 *
 * Wi-Fi association is managed here for both. "Wi-Fi joined" and "the edge
 * accepted a report" are different facts, and only the second proves the
 * uplink works (tm_transport_state).
 */

void tm_transport_begin();
/** Keep the link up; call every loop. Safe while disconnected. */
void tm_transport_update();
/** Re-read settings (SSID, edges) and reconnect. */
void tm_transport_restart();

/** Wi-Fi associated (says nothing about the edge). */
bool tm_transport_connected();
/** transport wss is in use. */
bool tm_transport_is_cloud();
/**
 * The uplink is working: udp -- Wi-Fi is up (UDP has no acknowledgement, so
 * this is the most it can say); wss -- the cloud session is authenticated.
 */
bool tm_transport_uplink_ready();
/** One line for `show`: the uplink's state and, for wss, why it last failed. */
void tm_transport_describe(char* out, size_t max);
/** Seconds since the edge last acknowledged a REPORT: -1 never, -2 not applicable (udp has no ACK). */
int32_t tm_transport_report_ack_age_s();
/** REPORTs the edge has acknowledged this boot (wss only; 0 for udp). */
uint32_t tm_transport_reports_acked();
/** Send to the first edge, or to every configured edge. Returns edges reached. */
int tm_transport_send(const uint8_t* data, size_t len, bool all_edges);
/** A pending downlink datagram, copied into buf; 0 if none. */
size_t tm_transport_receive(uint8_t* buf, size_t max);

/** Who sent the last datagram tm_transport_receive returned: the node's
    gateway, and the only machine an OTA image may be fetched from. */
void tm_transport_remote(uint8_t out[4]);

void tm_transport_uid(uint8_t out[6]);
int8_t tm_transport_rssi();
uint8_t tm_transport_channel();
void tm_transport_ip(uint8_t out[4]);
uint16_t tm_transport_drops();

#endif // TM_TRANSPORT_H
