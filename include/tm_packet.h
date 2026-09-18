#ifndef TM_PACKET_H
#define TM_PACKET_H

/**
 * Builds and parses tm_protocol.h packets. No Arduino dependencies, so the
 * host cross-check (TMedge `npm run crosscheck`) compiles this exact file.
 */

#include <stddef.h>
#include <stdint.h>
#include "tm_protocol.h"
#include "tm_detector.h"

#define TM_PACKET_MAX_SIZE (TM_HEADER_SIZE + TM_RAW_SIZE + TM_TAG_SIZE)
#define TM_KEY_MAX_LEN 64

typedef struct {
    uint8_t uid[TM_UID_SIZE];
    uint16_t boot;
    uint32_t seq;            // next sequence number; the builders advance it
    uint8_t key[TM_KEY_MAX_LEN];
    size_t key_len;          // 0 = unsigned (edge rejects unless ALLOW_UNSIGNED)
} TmPacketContext;

typedef struct {
    char fw_version[TM_FW_VERSION_LEN];
    uint8_t ip[4];
    int8_t rssi;
    uint8_t channel;
    uint32_t free_heap;
    uint32_t min_heap;
    uint16_t stack_free;
    uint16_t wifi_drops;
    uint16_t sensor_errors;
    uint32_t frames;
    uint16_t fps_x100;
    uint16_t vdd_x100;
    float ta;
    uint32_t last_cmd;
    uint8_t flags;
} TmStatus;

typedef struct {
    uint32_t seq;
    uint8_t opcode;
    uint8_t arg0;
    int32_t value;
} TmCommand;

typedef struct {
    uint32_t frame;
    float ta;
    float scene_min;
    float scene_max;
    float bg_mean;
    uint8_t flags;
} TmReportInfo;

size_t tm_build_report(uint8_t* out, TmPacketContext* ctx, uint32_t uptime_ms,
                       const TmReportInfo* info, const TmDetection* dets, uint8_t count);

/** Quantises `temps` (C) to 8 bits over the frame's own range. */
size_t tm_build_raw(uint8_t* out, TmPacketContext* ctx, uint32_t uptime_ms,
                    uint32_t frame, const float* temps);

size_t tm_build_status(uint8_t* out, TmPacketContext* ctx, uint32_t uptime_ms,
                       const TmStatus* status, const int32_t* params, uint8_t param_count);

/** Build a COMMAND (used by host tests; the edge has its own builder). */
size_t tm_build_command(uint8_t* out, const TmPacketContext* ctx, const TmCommand* cmd);

#define TM_PARSE_OK 0
#define TM_PARSE_SHORT -1
#define TM_PARSE_MAGIC -2
#define TM_PARSE_VERSION -3
#define TM_PARSE_TYPE -4
#define TM_PARSE_NOT_FOR_US -5
#define TM_PARSE_LENGTH -6
#define TM_PARSE_BAD_TAG -7

/** Verify and decode a COMMAND addressed to ctx->uid. */
int tm_parse_command(const uint8_t* in, size_t len, const TmPacketContext* ctx, TmCommand* out);

/** HMAC-SHA256(key, data) truncated to TM_TAG_SIZE. Zeros when key_len == 0. */
void tm_auth_tag(const uint8_t* key, size_t key_len, const uint8_t* data, size_t len,
                 uint8_t out[TM_TAG_SIZE]);

#endif // TM_PACKET_H
