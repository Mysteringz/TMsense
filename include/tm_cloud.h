#ifndef TM_CLOUD_H
#define TM_CLOUD_H

/**
 * Direct-to-cloud transport (`transport wss`): one outbound WebSocket over
 * TLS to TMedge, through the Cloudflare Tunnel. See tm_cloud_session.h for the
 * protocol and TMedge/docs/DIRECT_NODE_PROTOCOL.md for the contract.
 *
 * DNS, SNTP, the TLS handshake and every socket call run in one FreeRTOS task
 * of their own, so a slow network never stalls the sensor loop. The loop and
 * the task share only copies, through a small locked queue each way.
 *
 * Arduino-only. The logic underneath (tm_ws, tm_cloud_proto,
 * tm_cloud_session) is host-tested.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "tm_cloud_session.h"

class WiFiClientSecure;

/** 2026-01-01: a clock before this has not been set, and cannot judge a certificate. */
#define TM_MIN_PLAUSIBLE_EPOCH 1767225600L

typedef struct {
    TmCloudState state;
    uint32_t reports_acked;       // this boot
    uint32_t last_report_ack_ms;  // millis(); 0 = never
    uint32_t connects;
    uint32_t queue_dropped;
    uint32_t task_stack_free;     // bytes, high-water mark
    char last_error[48];
} TmCloudInfo;

/** Start (or reconfigure) the transport. Copies everything it needs. */
void tm_cloud_begin(const TmCloudUrl* url, const uint8_t uid[6], const uint8_t* key, size_t key_len, uint16_t boot);
/** Close the session and idle: transport switched back to udp. */
void tm_cloud_stop();
bool tm_cloud_active();

/** Queue a copy of one packet. False if it was dropped. */
bool tm_cloud_send(const uint8_t* data, size_t len);
/** A downlink packet (COMMAND / OTA), copied into buf; 0 if none. */
size_t tm_cloud_receive(uint8_t* buf, size_t max);

void tm_cloud_info(TmCloudInfo* out);
/** True once after each new session: send a fresh STATUS. */
bool tm_cloud_take_status_request();

/** The download grant for this OTA sequence and build, if one arrived and is still valid. */
bool tm_cloud_grant(uint32_t seq, const char* build, char token[65]);

/**
 * Close the session (and keep it closed) or resume. Waits up to `wait_ms`
 * for the task to act; returns whether it did.
 */
bool tm_cloud_pause(bool paused, uint32_t wait_ms);

/** SNTP has given a plausible time. */
bool tm_cloud_time_ok();

/**
 * Open a TLS connection with the node's trust roots, hostname check and a
 * certificate validity check against the current time. The OTA download
 * uses it too. 0 = ok, -1 unreachable, -2 certificate refused, -3 a
 * certificate in the chain is not valid now (or the time is unknown).
 */
int tm_cloud_tls_connect(WiFiClientSecure* c, const char* host, uint16_t port);

#endif // TM_CLOUD_H
