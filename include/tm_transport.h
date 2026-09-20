#ifndef TM_TRANSPORT_H
#define TM_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/**
 * Wi-Fi UDP transport. The packet format is transport-independent (a REPORT
 * fits a LoRa frame); this is the first transport, not the only one.
 *
 * It is deliberately light on the network it shares: one datagram per packet,
 * no broadcast or multicast (both are expensive on campus Wi-Fi, which sends
 * them at the lowest basic rate to every client), no TCP, no retries -- a lost
 * REPORT is superseded by the next one a second later.
 */

void tm_transport_begin();
/** Keep the link up; call every loop. Safe while disconnected. */
void tm_transport_update();
/** Re-read settings (SSID, edges) and reconnect. */
void tm_transport_restart();

bool tm_transport_connected();
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
