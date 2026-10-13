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
                         uint32_t boot, uint32_t seq, uint32_t uptime_ms) {
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
static size_t seal(uint8_t* out, size_t payload_len, const TmPacketContext* ctx) {
    if (ctx->secure_id) {
        const uint8_t type = out[3];
        const bool command = type == TM_TYPE_COMMAND || type == TM_TYPE_OTA;
        const char* role = type == TM_TYPE_ACK ? "ack" : command ? "downlink" : "uplink";
        uint8_t key[32];
        if (!tm_secure_derive(ctx->master, ctx->uid, ctx->secure_id, role, ctx->epoch, 16, key)) return 0;
        const uint32_t seq = command ? get32(out + TM_HEADER_SIZE) : get32(out + 12);
        const size_t n = tm_secure_seal(out, TM_PACKET_MAX_SIZE, type, ctx->uid, command ? 0 : ctx->boot,
            seq, get32(out + 16), ctx->secure_id, ctx->epoch, key, out + TM_HEADER_SIZE, payload_len);
        memset(key, 0, sizeof(key));
        return n;
    }
    put16(out + 20, (uint16_t)payload_len);
    tm_auth_tag(ctx->key, ctx->key_len, out, TM_HEADER_SIZE + payload_len, out + TM_HEADER_SIZE + payload_len);
    return TM_HEADER_SIZE + payload_len + TM_TAG_SIZE;
}

static int payload(const uint8_t* in, size_t len, const TmPacketContext* ctx, uint8_t type, uint8_t* out, size_t size) {
    if (len < TM_HEADER_SIZE + TM_TAG_SIZE) return TM_PARSE_SHORT;
    if (in[0] != TM_MAGIC_0 || in[1] != TM_MAGIC_1) return TM_PARSE_MAGIC;
    if (in[3] != type) return TM_PARSE_TYPE;
    if (memcmp(in + 4, ctx->uid, TM_UID_SIZE)) return TM_PARSE_NOT_FOR_US;
    if (ctx->secure_id) {
        if (len < TM_SECURE_HEADER + TM_SECURE_TAG || in[2] != 2) return TM_PARSE_VERSION;
        if (get16(in + 24) != ctx->secure_id) return TM_PARSE_BAD_TAG;
        uint8_t key[32];
        if (!tm_secure_derive(ctx->master, ctx->uid, ctx->secure_id, type == TM_TYPE_ACK ? "ack" : "downlink", in + 26, 16, key)) return TM_PARSE_BAD_TAG;
        const int n = tm_secure_open(in, len, key, out, size);
        memset(key, 0, sizeof(key));
        if (n != (int)size) return TM_PARSE_BAD_TAG;
        if (type != TM_TYPE_ACK && (tm_wire_boot(in) != 0 || tm_wire_seq(in) != get32(out))) return TM_PARSE_BAD_TAG;
        return TM_PARSE_OK;
    }
    if (in[2] != TM_PROTOCOL_VERSION) return TM_PARSE_VERSION;
    if (get16(in + 20) != size || len != TM_HEADER_SIZE + size + TM_TAG_SIZE) return TM_PARSE_LENGTH;
    if (!ctx->key_len) return TM_PARSE_BAD_TAG;
    uint8_t tag[TM_TAG_SIZE]; tm_auth_tag(ctx->key, ctx->key_len, in, TM_HEADER_SIZE + size, tag);
    uint8_t diff = 0; for (int i = 0; i < TM_TAG_SIZE; ++i) diff |= tag[i] ^ in[TM_HEADER_SIZE + size + i];
    if (diff) return TM_PARSE_BAD_TAG;
    memcpy(out, in + TM_HEADER_SIZE, size);
    return TM_PARSE_OK;
}

size_t tm_build_report(uint8_t* out, TmPacketContext* ctx, uint32_t uptime_ms,
                       const TmReportInfo* info, const TmDetection* dets, uint8_t count) {
    if (ctx->seq == UINT32_MAX) return 0;
    uint8_t flags = info->flags;
    const uint8_t maximum = ctx->secure_id ? TM_SECURE_REPORT_DETECTIONS : TM_MAX_DETECTIONS;
    if (count > maximum) {
        count = maximum;
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
    return seal(out, TM_REPORT_FIXED_SIZE + (size_t) count * TM_DETECTION_SIZE, ctx);
}

size_t tm_build_raw(uint8_t* out, TmPacketContext* ctx, uint32_t uptime_ms,
                    uint32_t frame, const float* temps) {
    if (ctx->seq == UINT32_MAX) return 0;
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
    return seal(out, TM_RAW_SIZE, ctx);
}

size_t tm_build_status(uint8_t* out, TmPacketContext* ctx, uint32_t uptime_ms,
                       const TmStatus* s, const int32_t* params, uint8_t param_count) {
    if (ctx->seq == UINT32_MAX || param_count > TM_PARAM_COUNT) return 0;
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
    return seal(out, TM_STATUS_FIXED_SIZE + 4u * param_count, ctx);
}

size_t tm_build_command(uint8_t* out, const TmPacketContext* ctx, const TmCommand* cmd) {
    write_header(out, TM_TYPE_COMMAND, ctx->uid, 0, 0, 0);
    uint8_t* p = out + TM_HEADER_SIZE;
    put32(p, cmd->seq);
    p[4] = cmd->opcode;
    p[5] = cmd->arg0;
    put32(p + 6, (uint32_t) cmd->value);
    return seal(out, TM_COMMAND_SIZE, ctx);
}

size_t tm_build_ota_status(uint8_t* out, TmPacketContext* ctx, uint32_t uptime_ms, const TmOtaStatus* st) {
    if (ctx->seq == UINT32_MAX) return 0;
    write_header(out, TM_TYPE_OTA_STATUS, ctx->uid, ctx->boot, ctx->seq++, uptime_ms);
    uint8_t* p = out + TM_HEADER_SIZE;
    p[0] = st->state;
    p[1] = st->percent;
    p[2] = st->error;
    p[3] = 0;
    put32(p + 4, st->image);
    return seal(out, TM_OTA_STATUS_SIZE, ctx);
}

int tm_parse_ota(const uint8_t* in, size_t len, const TmPacketContext* ctx, TmOtaRequest* out) {
    uint8_t p[TM_OTA_SIZE];
    const int r = payload(in, len, ctx, TM_TYPE_OTA, p, sizeof(p));
    if (r) return r;
    out->seq = get32(p);
    out->port = get16(p + 4);
    out->size = get32(p + 6);
    memcpy(out->sha256, p + 10, 32);
    memcpy(out->path, p + 42, TM_OTA_PATH_LEN);
    out->path[TM_OTA_PATH_LEN - 1] = 0;
    return TM_PARSE_OK;
}

int tm_parse_command(const uint8_t* in, size_t len, const TmPacketContext* ctx, TmCommand* out) {
    uint8_t p[TM_COMMAND_SIZE];
    const int r = payload(in, len, ctx, TM_TYPE_COMMAND, p, sizeof(p));
    if (r) return r;
    out->seq = get32(p);
    out->opcode = p[4];
    out->arg0 = p[5];
    out->value = (int32_t) get32(p + 6);
    return TM_PARSE_OK;
}

size_t tm_build_ack(uint8_t* out, const TmPacketContext* ctx, uint32_t boot, uint32_t seq, uint8_t type) {
    TmPacketContext target = *ctx; target.boot = boot;
    write_header(out, TM_TYPE_ACK, ctx->uid, boot, seq, 0);
    out[TM_HEADER_SIZE] = type;
    return seal(out, 1, &target);
}
int tm_parse_ack(const uint8_t* in, size_t len, const TmPacketContext* ctx, uint8_t* type) {
    const int r = payload(in, len, ctx, TM_TYPE_ACK, type, 1);
    if (r) return r;
    return tm_wire_boot(in) == ctx->boot && (*type == TM_TYPE_REPORT || *type == TM_TYPE_STATUS) ? TM_PARSE_OK : TM_PARSE_BAD_TAG;
}
