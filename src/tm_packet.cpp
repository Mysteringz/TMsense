#include "tm_packet.h"

#include <math.h>
#include <string.h>
#include "mbedtls/md.h"

static void put16(uint8_t* p, uint16_t v) { p[0] = (uint8_t) v; p[1] = (uint8_t) (v >> 8); }
static void put32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t) v; p[1] = (uint8_t) (v >> 8); p[2] = (uint8_t) (v >> 16); p[3] = (uint8_t) (v >> 24);
}
static uint16_t get16(const uint8_t* p) { return (uint16_t) (p[0] | (p[1] << 8)); }
static uint32_t get32(const uint8_t* p) {
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

static int16_t centi(float c) {
    const float v = roundf(c * 100.0f);
    return (int16_t) (v > 32767.0f ? 32767.0f : (v < -32768.0f ? -32768.0f : v));
}

static uint8_t clamp_u8(float v) {
    const float r = roundf(v);
    return (uint8_t) (r < 0.0f ? 0.0f : (r > 255.0f ? 255.0f : r));
}

void tm_auth_tag(const uint8_t* key, size_t key_len, const uint8_t* data, size_t len,
                 uint8_t out[TM_TAG_SIZE]) {
    if (key_len == 0) {
        memset(out, 0, TM_TAG_SIZE);
        return;
    }
    uint8_t full[32];
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), key, key_len, data, len, full);
    memcpy(out, full, TM_TAG_SIZE);
}

static void write_header(uint8_t* out, uint8_t type, const uint8_t uid[TM_UID_SIZE],
                         uint16_t boot, uint32_t seq, uint32_t uptime_ms) {
    out[0] = TM_MAGIC_0;
    out[1] = TM_MAGIC_1;
    out[2] = TM_PROTOCOL_VERSION;
    out[3] = type;
    memcpy(out + 4, uid, TM_UID_SIZE);
    put16(out + 10, boot);
    put32(out + 12, seq);
    put32(out + 16, uptime_ms);
}

// Fill in the length, sign, and return the datagram size.
static size_t seal(uint8_t* out, size_t payload_len, const uint8_t* key, size_t key_len) {
    put16(out + 20, (uint16_t) payload_len);
    tm_auth_tag(key, key_len, out, TM_HEADER_SIZE + payload_len, out + TM_HEADER_SIZE + payload_len);
    return TM_HEADER_SIZE + payload_len + TM_TAG_SIZE;
}

size_t tm_build_report(uint8_t* out, TmPacketContext* ctx, uint32_t uptime_ms,
                       const TmReportInfo* info, const TmDetection* dets, uint8_t count) {
    uint8_t flags = info->flags;
    if (count > TM_MAX_DETECTIONS) {
        count = TM_MAX_DETECTIONS;
        flags |= TM_REPORT_TRUNCATED;
    }
    write_header(out, TM_TYPE_REPORT, ctx->uid, ctx->boot, ctx->seq++, uptime_ms);
    uint8_t* p = out + TM_HEADER_SIZE;
    put32(p + 0, info->frame);
    put16(p + 4, (uint16_t) centi(info->ta));
    put16(p + 6, (uint16_t) centi(info->scene_min));
    put16(p + 8, (uint16_t) centi(info->scene_max));
    put16(p + 10, (uint16_t) centi(info->bg_mean));
    p[12] = flags;
    p[13] = count;
    p += TM_REPORT_FIXED_SIZE;
    for (uint8_t i = 0; i < count; ++i) {
        const TmDetection* d = &dets[i];
        p[0] = clamp_u8(d->x * 8.0f);
        p[1] = clamp_u8(d->y * 8.0f);
        p[2] = (uint8_t) (d->area > 255 ? 255 : d->area);
        p[3] = clamp_u8(d->contrast / 0.05f);
        p[4] = clamp_u8(d->peak / 0.25f);
        const float heat = roundf(d->heat * 10.0f);
        put16(p + 5, (uint16_t) (heat > 65535.0f ? 65535.0f : (heat < 0.0f ? 0.0f : heat)));
        p += TM_DETECTION_SIZE;
    }
    return seal(out, TM_REPORT_FIXED_SIZE + (size_t) count * TM_DETECTION_SIZE, ctx->key, ctx->key_len);
}

size_t tm_build_raw(uint8_t* out, TmPacketContext* ctx, uint32_t uptime_ms,
                    uint32_t frame, const float* temps) {
    float lo = 1e9f, hi = -1e9f;
    for (int i = 0; i < TM_GRID_SIZE; ++i) {
        if (temps[i] < lo) lo = temps[i];
        if (temps[i] > hi) hi = temps[i];
    }
    // Round the origin down to the centi-degree the receiver will see, so
    // level 0 means the same temperature on both ends.
    const int16_t lo_c = centi(floorf(lo * 100.0f) / 100.0f);
    const float origin = lo_c / 100.0f;
    // Step in 1/10000 C, at least 1, and large enough that `hi` fits in 255.
    uint32_t step_q = (uint32_t) ceilf((hi - origin) / 255.0f * 10000.0f);
    if (step_q < 1) step_q = 1;
    if (step_q > 65535) step_q = 65535;
    const float step = step_q / 10000.0f;

    write_header(out, TM_TYPE_RAW, ctx->uid, ctx->boot, ctx->seq++, uptime_ms);
    uint8_t* p = out + TM_HEADER_SIZE;
    put32(p + 0, frame);
    put16(p + 4, (uint16_t) lo_c);
    put16(p + 6, (uint16_t) step_q);
    for (int i = 0; i < TM_GRID_SIZE; ++i) p[8 + i] = clamp_u8((temps[i] - origin) / step);
    return seal(out, TM_RAW_SIZE, ctx->key, ctx->key_len);
}

size_t tm_build_status(uint8_t* out, TmPacketContext* ctx, uint32_t uptime_ms,
                       const TmStatus* s, const int32_t* params, uint8_t param_count) {
    write_header(out, TM_TYPE_STATUS, ctx->uid, ctx->boot, ctx->seq++, uptime_ms);
    uint8_t* p = out + TM_HEADER_SIZE;
    memset(p, 0, TM_FW_VERSION_LEN);
    strncpy((char*) p, s->fw_version, TM_FW_VERSION_LEN);
    memcpy(p + 12, s->ip, 4);
    p[16] = (uint8_t) s->rssi;
    p[17] = s->channel;
    put32(p + 18, s->free_heap);
    put32(p + 22, s->min_heap);
    put16(p + 26, s->stack_free);
    put16(p + 28, s->wifi_drops);
    put16(p + 30, s->sensor_errors);
    put32(p + 32, s->frames);
    put16(p + 36, s->fps_x100);
    put16(p + 38, s->vdd_x100);
    put16(p + 40, (uint16_t) centi(s->ta));
    put32(p + 42, s->last_cmd);
    p[46] = s->flags;
    p[47] = param_count;
    for (uint8_t i = 0; i < param_count; ++i) put32(p + TM_STATUS_FIXED_SIZE + 4 * i, (uint32_t) params[i]);
    return seal(out, TM_STATUS_FIXED_SIZE + 4u * param_count, ctx->key, ctx->key_len);
}

size_t tm_build_command(uint8_t* out, const TmPacketContext* ctx, const TmCommand* cmd) {
    write_header(out, TM_TYPE_COMMAND, ctx->uid, 0, 0, 0);
    uint8_t* p = out + TM_HEADER_SIZE;
    put32(p, cmd->seq);
    p[4] = cmd->opcode;
    p[5] = cmd->arg0;
    put32(p + 6, (uint32_t) cmd->value);
    return seal(out, TM_COMMAND_SIZE, ctx->key, ctx->key_len);
}

int tm_parse_command(const uint8_t* in, size_t len, const TmPacketContext* ctx, TmCommand* out) {
    if (len < TM_HEADER_SIZE + TM_TAG_SIZE) return TM_PARSE_SHORT;
    if (in[0] != TM_MAGIC_0 || in[1] != TM_MAGIC_1) return TM_PARSE_MAGIC;
    if (in[2] != TM_PROTOCOL_VERSION) return TM_PARSE_VERSION;
    if (in[3] != TM_TYPE_COMMAND) return TM_PARSE_TYPE;
    if (memcmp(in + 4, ctx->uid, TM_UID_SIZE) != 0) return TM_PARSE_NOT_FOR_US;
    const uint16_t payload = get16(in + 20);
    if (payload != TM_COMMAND_SIZE || len != (size_t) TM_HEADER_SIZE + payload + TM_TAG_SIZE) return TM_PARSE_LENGTH;
    // An unsigned node still demands a signature on commands: accepting
    // unsigned commands would let anyone on the network reconfigure it.
    if (ctx->key_len == 0) return TM_PARSE_BAD_TAG;
    uint8_t tag[TM_TAG_SIZE];
    tm_auth_tag(ctx->key, ctx->key_len, in, TM_HEADER_SIZE + payload, tag);
    uint8_t diff = 0;
    for (int i = 0; i < TM_TAG_SIZE; ++i) diff |= (uint8_t) (tag[i] ^ in[TM_HEADER_SIZE + payload + i]);
    if (diff != 0) return TM_PARSE_BAD_TAG;
    const uint8_t* p = in + TM_HEADER_SIZE;
    out->seq = get32(p);
    out->opcode = p[4];
    out->arg0 = p[5];
    out->value = (int32_t) get32(p + 6);
    return TM_PARSE_OK;
}
