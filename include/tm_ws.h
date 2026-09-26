#ifndef TM_WS_H
#define TM_WS_H

/**
 * The client half of RFC 6455 WebSocket, as much of it as tmnode.v1 needs.
 *
 * Written here rather than taken from a library because it is small, because
 * it has to be bounded (one static reassembly buffer, a hard message limit,
 * no compression, no allocation), and because it is plain C with no Arduino
 * dependency, so the host tests and TMedge's crosscheck run this exact code.
 * TLS underneath is the ESP32 core's own WiFiClientSecure (tm_cloud.cpp).
 *
 * TCP reads are not messages: tm_ws_feed takes whatever bytes arrived and
 * yields at most one event per call, reassembling continuation frames.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/** Largest message reassembled: above the 806-byte RAW and the 512-byte control limit. */
#define TM_WS_MAX_MESSAGE 1024
#define TM_WS_MAX_CONTROL 125

#define TM_WS_OP_CONT 0x0
#define TM_WS_OP_TEXT 0x1
#define TM_WS_OP_BINARY 0x2
#define TM_WS_OP_CLOSE 0x8
#define TM_WS_OP_PING 0x9
#define TM_WS_OP_PONG 0xA

typedef enum {
    TM_WS_EV_NONE = 0,
    TM_WS_EV_TEXT,
    TM_WS_EV_BINARY,
    TM_WS_EV_PING,
    TM_WS_EV_PONG,
    TM_WS_EV_CLOSE,
    TM_WS_EV_ERROR,
} TmWsEventType;

typedef struct {
    TmWsEventType type;
    const uint8_t* data;   // into the parser; valid until the next feed
    size_t len;
    uint16_t close_code;   // TM_WS_EV_CLOSE
    int error;             // TM_WS_EV_ERROR: TM_WS_ERR_*
} TmWsEvent;

#define TM_WS_ERR_MASKED -1        // a server must not mask its frames
#define TM_WS_ERR_RSV -2           // extension bits: nothing was negotiated
#define TM_WS_ERR_OPCODE -3
#define TM_WS_ERR_CONTROL -4       // fragmented or oversized control frame
#define TM_WS_ERR_SEQUENCE -5      // continuation without a start, or a new message mid-fragment
#define TM_WS_ERR_TOO_BIG -6

typedef struct {
    uint8_t hdr[14];
    uint8_t hdr_len;
    uint8_t hdr_need;
    uint64_t payload_len;
    uint64_t payload_got;
    uint8_t opcode;
    bool fin;
    bool in_frame;
    uint8_t msg_opcode;            // TEXT or BINARY while a message is being assembled; 0 if none
    size_t msg_len;
    uint8_t msg[TM_WS_MAX_MESSAGE];
    uint8_t ctl[TM_WS_MAX_CONTROL];
    size_t ctl_len;
    bool failed;
} TmWsParser;

void tm_ws_parser_reset(TmWsParser* p);

/**
 * Consume bytes; returns how many were consumed. When a whole message or
 * control frame is complete it stops there and fills `ev`. Call again with
 * the rest. After an error the parser consumes nothing more.
 */
size_t tm_ws_feed(TmWsParser* p, const uint8_t* data, size_t len, TmWsEvent* ev);

/** A single, final, masked client frame. Returns its size, or 0 if `max` is too small. */
size_t tm_ws_frame(uint8_t* out, size_t max, uint8_t opcode, const uint8_t* payload, size_t len, const uint8_t mask[4]);

/** Standard base64 of `len` bytes; returns chars written (NUL added), 0 if `max` is too small. */
size_t tm_base64(char* out, size_t max, const uint8_t* in, size_t len);

/** The opening HTTP request. `key_b64` is 16 random bytes in base64. */
size_t tm_ws_request(char* out, size_t max, const char* host, uint16_t port, const char* path,
                     const char* key_b64, const char* subprotocol);

#define TM_WS_HS_OK 0
#define TM_WS_HS_STATUS -1       // not "HTTP/1.1 101"
#define TM_WS_HS_UPGRADE -2
#define TM_WS_HS_ACCEPT -3       // Sec-WebSocket-Accept does not match our key
#define TM_WS_HS_PROTOCOL -4     // not the subprotocol we asked for
#define TM_WS_HS_EXTENSION -5    // the server negotiated an extension we did not offer

/** Check the server's response head (everything before the blank line). */
int tm_ws_check_response(const char* head, size_t len, const char* key_b64, const char* subprotocol);

#endif // TM_WS_H
