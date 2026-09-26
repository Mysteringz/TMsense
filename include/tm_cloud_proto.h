#ifndef TM_CLOUD_PROTO_H
#define TM_CLOUD_PROTO_H

/**
 * tmnode.v1, the node's side of the direct-to-cloud transport -- the pieces
 * with no Arduino in them, so host tests and TMedge's crosscheck run them.
 * The contract is TMedge/docs/DIRECT_NODE_PROTOCOL.md; the edge's end is
 * TMedge/src/edge/nodelink.ts.
 *
 * This is a transport, not a packet format: every REPORT, RAW, STATUS,
 * COMMAND and OTA keeps its tm_protocol.h bytes and HMAC and travels as one
 * binary WebSocket message. What is new is only the session around them:
 * a challenge answered with the site key, acknowledgements of *accepted*
 * reports, and a download grant for OTA.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "tm_packet.h"

#define TM_CLOUD_SUBPROTOCOL "tmnode.v1"
#define TM_CLOUD_VERSION 1
/** Longest cloud_url, in ASCII bytes. TMflash enforces the same limit. */
#define TM_CLOUD_URL_MAX 128
#define TM_CLOUD_HOST_MAX 100
#define TM_CLOUD_PATH_MAX 64
/** Largest control (text) message either way. */
#define TM_CLOUD_CONTROL_MAX 512

// --- endpoint ----------------------------------------------------------------

typedef struct {
    bool tls;                          // wss://
    char host[TM_CLOUD_HOST_MAX + 1];
    uint16_t port;
    char path[TM_CLOUD_PATH_MAX + 1];
} TmCloudUrl;

#define TM_URL_OK 0
#define TM_URL_LENGTH -1       // empty, or over TM_CLOUD_URL_MAX
#define TM_URL_CHARS -2        // control characters, spaces, non-ASCII, %, \ ...
#define TM_URL_SCHEME -3       // not wss:// (ws:// only in a test build)
#define TM_URL_USERINFO -4     // user:password@ -- credentials never go in a URL
#define TM_URL_HOST -5
#define TM_URL_PORT -6         // production is 443 only
#define TM_URL_PATH -7
#define TM_URL_QUERY -8        // ? or #

/**
 * Validate and split a cloud endpoint URL. Production accepts only
 * wss://<host>[:443]/<path>; `test_build` additionally allows ws:// and any
 * port, for a local fixture, and nothing else.
 */
int tm_cloud_parse_url(const char* url, bool test_build, TmCloudUrl* out);
const char* tm_cloud_url_error(int code);

// --- control messages ----------------------------------------------------------

typedef enum {
    TM_CTL_UNKNOWN = 0,
    TM_CTL_CHALLENGE,
    TM_CTL_READY,
    TM_CTL_ACK,
    TM_CTL_OTA_GRANT,
} TmCloudControlType;

typedef struct {
    TmCloudControlType type;
    char nonce[65];          // challenge
    char session[33];        // ready, ack
    char uid[18];            // ready
    uint32_t heartbeat_ms;   // ready
    uint16_t boot;           // ack
    uint32_t seq;            // ack, ota_grant
    char build[17];          // ota_grant
    char token[65];          // ota_grant
    uint32_t expires_in_ms;  // ota_grant
} TmCloudControl;

#define TM_CTL_OK 0
#define TM_CTL_MALFORMED -1    // not a flat JSON object of strings and integers
#define TM_CTL_VERSION -2
#define TM_CTL_FIELDS -3       // a required field missing, the wrong shape, or too long

/**
 * Parse one control message. Strict: a flat object, string values without
 * escapes, non-negative integer values, v == 1, every required field present
 * and exactly the expected shape. Unknown keys are ignored and an unknown
 * type is TM_CTL_UNKNOWN, so the edge can add to v1 without breaking nodes.
 */
int tm_cloud_parse_control(const char* text, size_t len, TmCloudControl* out);

/** "aa:bb:cc:dd:ee:ff" */
void tm_cloud_uid_string(const uint8_t uid[6], char out[18]);

/**
 * The `auth` reply: HMAC-SHA256(key, "tmnode1|<uid>|<nonce>") as 64 lowercase
 * hex. Returns bytes written, 0 on overflow. The proof is never logged.
 */
size_t tm_cloud_auth_json(char* out, size_t max, const char* uid, const char* nonce,
                          const uint8_t* key, size_t key_len);

// --- acknowledgements ----------------------------------------------------------

/**
 * The REPORT and STATUS packets sent in this session and not yet
 * acknowledged. An ACK counts only if it names one of them: an ACK for
 * anything else -- another boot, another session, a packet never sent -- is
 * ignored, because it is the node's only evidence the edge took its reports.
 */
#define TM_ACK_SLOTS 16

typedef struct {
    struct {
        uint8_t type;
        uint16_t boot;
        uint32_t seq;
        uint32_t sent_ms;
    } e[TM_ACK_SLOTS];
    uint8_t n;
} TmAckTracker;

void tm_ack_reset(TmAckTracker* t);
void tm_ack_sent(TmAckTracker* t, uint8_t type, uint16_t boot, uint32_t seq, uint32_t now_ms);
/**
 * Match an ACK. Returns the acknowledged packet's type (TM_TYPE_REPORT or
 * TM_TYPE_STATUS), or 0 if it names nothing outstanding. Entries sent before
 * the acknowledged one are dropped too: the edge acknowledges in order, so
 * they were refused (late, or lost) and will never be acknowledged.
 */
uint8_t tm_ack_match(TmAckTracker* t, uint16_t boot, uint32_t seq);
/** Age of the oldest unacknowledged REPORT, 0 if there is none. */
uint32_t tm_ack_oldest_report_age(const TmAckTracker* t, uint32_t now_ms);

// --- outgoing queue ------------------------------------------------------------

/**
 * Packets waiting for the socket. Four slots and no history: a node that
 * cannot send keeps nothing, and an observation older than two seconds is
 * dropped rather than delivered late, because the edge would (rightly)
 * refuse it and a count that old is not what the room looks like now.
 * Under pressure RAW goes first. Order is always the order the packets were
 * built in, so a newer STATUS can never overtake an older REPORT and turn it
 * into a replay.
 */
#define TM_QUEUE_SLOTS 4
#define TM_QUEUE_MAX_AGE_MS 2000

typedef struct {
    struct {
        uint16_t len;
        uint8_t type;
        uint32_t at_ms;
        uint8_t data[TM_PACKET_MAX_SIZE];
    } slot[TM_QUEUE_SLOTS];
    uint8_t head;
    uint8_t count;
    uint32_t dropped;
} TmPacketQueue;

void tm_queue_clear(TmPacketQueue* q);
/** Copy a packet in. False if it was dropped (queue full of non-RAW, or too big). */
bool tm_queue_push(TmPacketQueue* q, const uint8_t* data, size_t len, uint32_t now_ms);
/** Oldest packet still young enough; stale ones are discarded on the way. 0 if none. */
size_t tm_queue_pop(TmPacketQueue* q, uint8_t* out, size_t max, uint8_t* type, uint32_t now_ms);

// --- reconnection ----------------------------------------------------------------

#define TM_BACKOFF_MIN_MS 1000
#define TM_BACKOFF_MAX_MS 60000

/**
 * Delay before reconnect attempt `attempt` (0 = first retry): exponential
 * from 1 s, capped at 60 s, with half of it random so a site's nodes do not
 * all reconnect in the same second after an outage.
 */
uint32_t tm_backoff_ms(uint8_t attempt, uint32_t random);

// --- HTTP response head (OTA download) ---------------------------------------------

typedef struct {
    int status;
    int32_t content_length;   // -1 if absent
    bool chunked;
    bool has_location;
} TmHttpHead;

/** Parse a response head; false if it is not HTTP/1.x. */
bool tm_http_parse_head(const char* head, size_t len, TmHttpHead* out);

#endif // TM_CLOUD_PROTO_H
