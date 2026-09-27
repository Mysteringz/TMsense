#ifndef TM_CLOUD_SESSION_H
#define TM_CLOUD_SESSION_H

/**
 * The tmnode.v1 session: connect, upgrade, answer the challenge, then carry
 * packets up and commands down, acknowledgements and grants beside them.
 *
 * Plain C over a small I/O table, so the same state machine runs on the node
 * (tm_cloud.cpp: WiFiClientSecure, one FreeRTOS task) and on a desktop
 * (test/host/cloud_host.cpp: a POSIX socket, driven against the real TMedge
 * listener by `npm run crosscheck`).
 *
 * The session owns its socket. It exchanges packets with the rest of the
 * firmware only through copies: the uplink queue it pops from and the
 * downlink callback it calls, both supplied by the caller, which does the
 * locking.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "tm_cloud_proto.h"
#include "tm_ws.h"

typedef struct {
    /** Open the transport (TLS, with certificate chain, hostname and validity checks). 0 = ok. */
    int (*connect)(void* ctx, const char* host, uint16_t port);
    /** Write all of it; returns len or < 0. */
    int (*write)(void* ctx, const uint8_t* data, size_t len);
    /** Whatever is available now: > 0 bytes, 0 nothing yet, < 0 closed. Never blocks for long. */
    int (*read)(void* ctx, uint8_t* buf, size_t max);
    void (*close)(void* ctx);
    uint32_t (*random)(void* ctx);
    void* ctx;
} TmCloudIo;

typedef enum {
    TM_CLOUD_OFF = 0,        // transport udp, or no Wi-Fi yet
    TM_CLOUD_WAIT_TIME,      // no plausible clock: certificates cannot be checked yet
    TM_CLOUD_BACKOFF,
    TM_CLOUD_CONNECTING,     // DNS + TCP + TLS
    TM_CLOUD_UPGRADING,      // HTTP -> WebSocket
    TM_CLOUD_AUTHENTICATING, // challenge answered, waiting for ready
    TM_CLOUD_READY,          // authenticated: packets flow
    TM_CLOUD_PAUSED,         // closed on purpose, e.g. for an OTA download
} TmCloudState;

const char* tm_cloud_state_name(TmCloudState s);

#define TM_CLOUD_HANDSHAKE_MS 10000
/** Nothing at all heard from the edge for this long: the connection is dead. */
#define TM_CLOUD_SILENCE_MS 45000
/** A REPORT not acknowledged within this: the data path is stalled even if the socket is up. */
#define TM_CLOUD_ACK_DEADLINE_MS 5000

typedef struct {
    // Configuration, set before the first step.
    TmCloudUrl url;
    char uid[18];
    const uint8_t* key;
    size_t key_len;
    uint16_t boot;
    /** Pop the next uplink packet (a copy); 0 if none. */
    size_t (*next_uplink)(void* ctx, uint8_t* buf, size_t max, uint8_t* type, uint32_t now_ms);
    /** Drop everything queued: a new session starts from fresh observations only. */
    void (*discard_uplink)(void* ctx);
    /** One verified-shape binary message from the edge (a COMMAND or OTA packet); the caller parses it. */
    void (*on_downlink)(void* ctx, const uint8_t* data, size_t len);
    void* cb_ctx;

    // State, owned by the session.
    TmCloudState state;
    uint32_t state_since_ms;
    uint32_t next_attempt_ms;
    uint8_t attempt;
    uint32_t last_rx_ms;
    char ws_key[25];
    // Cloudflare adds a dozen headers (cf-ray, nel, report-to, ...) to every
    // response, the 101 included: 512 bytes was measured too small on a node.
    char http[2048];
    size_t http_len;
    TmWsParser parser;
    char session[33];
    bool challenge_answered;
    TmAckTracker acks;
    bool report_acked_this_session;
    uint8_t rx[512];
    uint8_t pkt[TM_PACKET_MAX_SIZE];
    uint8_t tx[TM_PACKET_MAX_SIZE + 14];

    // Evidence and diagnostics, read by the firmware.
    uint32_t reports_acked;        // this boot, all sessions
    uint32_t last_report_ack_ms;   // 0 = never
    uint32_t connects;             // sessions that reached READY
    uint32_t sent;
    uint32_t ignored_acks;
    bool want_status;              // a fresh STATUS is due (just became READY)
    char last_error[48];

    // An OTA download grant: kept across reconnects, lost on reboot.
    bool grant_valid;
    uint32_t grant_seq;
    char grant_build[17];
    char grant_token[65];
    uint32_t grant_expires_ms;
} TmCloudSession;

/** Prepare a session; `url` must already have passed tm_cloud_parse_url. */
void tm_cloud_session_init(TmCloudSession* s, const TmCloudUrl* url, const char uid[18],
                           const uint8_t* key, size_t key_len, uint16_t boot);

/**
 * Advance the session. `link_up`: Wi-Fi associated. `time_ok`: the clock is
 * plausible enough to check certificate validity -- without it the session
 * waits, visibly, and never connects unverified.
 */
void tm_cloud_session_step(TmCloudSession* s, const TmCloudIo* io, uint32_t now_ms, bool link_up, bool time_ok);

/** Close on purpose (OTA download needs the heap) or resume. */
void tm_cloud_session_pause(TmCloudSession* s, const TmCloudIo* io, bool paused, uint32_t now_ms);

/** Take the grant for this OTA sequence and build, if one arrived and has not expired. */
bool tm_cloud_session_grant(TmCloudSession* s, uint32_t seq, const char* build, uint32_t now_ms, char token[65]);

#endif // TM_CLOUD_SESSION_H
