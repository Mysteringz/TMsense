#include "tm_ws.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "mbedtls/sha1.h"

void tm_ws_parser_reset(TmWsParser* p) {
    p->hdr_len = 0;
    p->hdr_need = 2;
    p->payload_len = 0;
    p->payload_got = 0;
    p->opcode = 0;
    p->fin = false;
    p->in_frame = false;
    p->msg_opcode = 0;
    p->msg_len = 0;
    p->ctl_len = 0;
    p->failed = false;
}

static size_t fail(TmWsParser* p, TmWsEvent* ev, int err, size_t consumed) {
    p->failed = true;
    ev->type = TM_WS_EV_ERROR;
    ev->error = err;
    return consumed;
}

/** The header is complete: validate it before a single payload byte is accepted. */
static int start_frame(TmWsParser* p) {
    const uint8_t b0 = p->hdr[0], b1 = p->hdr[1];
    if (b0 & 0x70) return TM_WS_ERR_RSV;
    if (b1 & 0x80) return TM_WS_ERR_MASKED;
    p->fin = (b0 & 0x80) != 0;
    p->opcode = b0 & 0x0F;
    const uint8_t len7 = b1 & 0x7F;
    if (len7 < 126) p->payload_len = len7;
    else if (len7 == 126) p->payload_len = ((uint64_t) p->hdr[2] << 8) | p->hdr[3];
    else {
        p->payload_len = 0;
        for (int i = 0; i < 8; ++i) p->payload_len = (p->payload_len << 8) | p->hdr[2 + i];
    }
    p->payload_got = 0;
    switch (p->opcode) {
        case TM_WS_OP_CLOSE:
        case TM_WS_OP_PING:
        case TM_WS_OP_PONG:
            if (!p->fin || p->payload_len > TM_WS_MAX_CONTROL) return TM_WS_ERR_CONTROL;
            p->ctl_len = 0;
            return 0;
        case TM_WS_OP_TEXT:
        case TM_WS_OP_BINARY:
            if (p->msg_opcode) return TM_WS_ERR_SEQUENCE;
            p->msg_opcode = p->opcode;
            p->msg_len = 0;
            break;
        case TM_WS_OP_CONT:
            if (!p->msg_opcode) return TM_WS_ERR_SEQUENCE;
            break;
        default:
            return TM_WS_ERR_OPCODE;
    }
    // Refused from the length alone, before buffering any of it.
    if (p->payload_len > TM_WS_MAX_MESSAGE - p->msg_len) return TM_WS_ERR_TOO_BIG;
    return 0;
}

/** The frame's payload is all in: emit an event if it finished something. */
static bool end_frame(TmWsParser* p, TmWsEvent* ev) {
    p->in_frame = false;
    p->hdr_len = 0;
    p->hdr_need = 2;
    switch (p->opcode) {
        case TM_WS_OP_PING:
        case TM_WS_OP_PONG:
            ev->type = p->opcode == TM_WS_OP_PING ? TM_WS_EV_PING : TM_WS_EV_PONG;
            ev->data = p->ctl;
            ev->len = p->ctl_len;
            return true;
        case TM_WS_OP_CLOSE:
            ev->type = TM_WS_EV_CLOSE;
            ev->close_code = p->ctl_len >= 2 ? (uint16_t) ((p->ctl[0] << 8) | p->ctl[1]) : 1005;
            ev->data = p->ctl;
            ev->len = p->ctl_len;
            return true;
        default:
            if (!p->fin) return false;
            ev->type = p->msg_opcode == TM_WS_OP_TEXT ? TM_WS_EV_TEXT : TM_WS_EV_BINARY;
            ev->data = p->msg;
            ev->len = p->msg_len;
            p->msg_opcode = 0;
            return true;
    }
}

size_t tm_ws_feed(TmWsParser* p, const uint8_t* data, size_t len, TmWsEvent* ev) {
    ev->type = TM_WS_EV_NONE;
    ev->data = NULL;
    ev->len = 0;
    if (p->failed) return 0;
    size_t i = 0;
    while (i < len) {
        if (!p->in_frame) {
            p->hdr[p->hdr_len++] = data[i++];
            if (p->hdr_len == 2) {
                const uint8_t len7 = p->hdr[1] & 0x7F;
                p->hdr_need = (uint8_t) (2 + (len7 == 126 ? 2 : len7 == 127 ? 8 : 0));
            }
            if (p->hdr_len < p->hdr_need) continue;
            const int err = start_frame(p);
            if (err) return fail(p, ev, err, i);
            p->in_frame = true;
            if (p->payload_len == 0 && end_frame(p, ev)) return i;
            continue;
        }
        size_t take = (size_t) (p->payload_len - p->payload_got);
        if (take > len - i) take = len - i;
        const bool control = p->opcode >= TM_WS_OP_CLOSE;
        if (control) {
            memcpy(p->ctl + p->ctl_len, data + i, take);
            p->ctl_len += take;
        } else {
            memcpy(p->msg + p->msg_len, data + i, take);
            p->msg_len += take;
        }
        p->payload_got += take;
        i += take;
        if (p->payload_got == p->payload_len && end_frame(p, ev)) return i;
    }
    return i;
}

size_t tm_ws_frame(uint8_t* out, size_t max, uint8_t opcode, const uint8_t* payload, size_t len, const uint8_t mask[4]) {
    const size_t head = 2 + (len < 126 ? 0 : len <= 0xFFFF ? 2 : 8) + 4;
    if (max < head + len) return 0;
    size_t o = 0;
    out[o++] = (uint8_t) (0x80 | (opcode & 0x0F));
    if (len < 126) {
        out[o++] = (uint8_t) (0x80 | len);
    } else if (len <= 0xFFFF) {
        out[o++] = 0x80 | 126;
        out[o++] = (uint8_t) (len >> 8);
        out[o++] = (uint8_t) len;
    } else {
        out[o++] = 0x80 | 127;
        for (int s = 56; s >= 0; s -= 8) out[o++] = (uint8_t) ((uint64_t) len >> s);
    }
    memcpy(out + o, mask, 4);
    o += 4;
    for (size_t k = 0; k < len; ++k) out[o + k] = payload[k] ^ mask[k & 3];
    return o + len;
}

size_t tm_base64(char* out, size_t max, const uint8_t* in, size_t len) {
    static const char A[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const size_t need = 4 * ((len + 2) / 3);
    if (max < need + 1) return 0;
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        const uint32_t v = ((uint32_t) in[i] << 16) | (i + 1 < len ? (uint32_t) in[i + 1] << 8 : 0) |
                           (i + 2 < len ? in[i + 2] : 0);
        out[o++] = A[(v >> 18) & 63];
        out[o++] = A[(v >> 12) & 63];
        out[o++] = i + 1 < len ? A[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < len ? A[v & 63] : '=';
    }
    out[o] = 0;
    return o;
}

size_t tm_ws_request(char* out, size_t max, const char* host, uint16_t port, const char* path,
                     const char* key_b64, const char* subprotocol) {
    char hostport[140];
    if (port == 443) snprintf(hostport, sizeof(hostport), "%s", host);
    else snprintf(hostport, sizeof(hostport), "%s:%u", host, (unsigned) port);
    // No Sec-WebSocket-Extensions: compression is never offered.
    const int n = snprintf(out, max,
                           "GET %s HTTP/1.1\r\n"
                           "Host: %s\r\n"
                           "Upgrade: websocket\r\n"
                           "Connection: Upgrade\r\n"
                           "Sec-WebSocket-Key: %s\r\n"
                           "Sec-WebSocket-Version: 13\r\n"
                           "Sec-WebSocket-Protocol: %s\r\n"
                           "User-Agent: TMsense\r\n"
                           "\r\n",
                           path, hostport, key_b64, subprotocol);
    return n > 0 && (size_t) n < max ? (size_t) n : 0;
}

/** Find header `name` (case-insensitive) in a response head; copies its trimmed value. */
static bool header_value(const char* head, size_t len, const char* name, char* out, size_t max) {
    const size_t nlen = strlen(name);
    const char* p = head;
    const char* end = head + len;
    // Skip the status line.
    while (p < end && *p != '\n') ++p;
    while (p < end) {
        ++p;   // past '\n'
        if (p >= end) break;
        const char* line = p;
        while (p < end && *p != '\n') ++p;
        size_t llen = (size_t) (p - line);
        if (llen && line[llen - 1] == '\r') --llen;
        if (llen > nlen && line[nlen] == ':') {
            bool same = true;
            for (size_t k = 0; k < nlen; ++k) {
                if (tolower((unsigned char) line[k]) != tolower((unsigned char) name[k])) { same = false; break; }
            }
            if (!same) continue;
            const char* v = line + nlen + 1;
            const char* ve = line + llen;
            while (v < ve && (*v == ' ' || *v == '\t')) ++v;
            while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) --ve;
            const size_t vlen = (size_t) (ve - v);
            if (vlen >= max) return false;
            memcpy(out, v, vlen);
            out[vlen] = 0;
            return true;
        }
    }
    return false;
}

static bool contains_token_ci(const char* value, const char* token) {
    const size_t t = strlen(token);
    for (const char* p = value; *p; ++p) {
        size_t k = 0;
        while (k < t && p[k] && tolower((unsigned char) p[k]) == tolower((unsigned char) token[k])) ++k;
        if (k == t) return true;
    }
    return false;
}

int tm_ws_check_response(const char* head, size_t len, const char* key_b64, const char* subprotocol) {
    if (len < 12 || strncmp(head, "HTTP/1.1 101", 12) != 0) return TM_WS_HS_STATUS;
    char v[96];
    if (!header_value(head, len, "Upgrade", v, sizeof(v)) || !contains_token_ci(v, "websocket")) return TM_WS_HS_UPGRADE;
    if (!header_value(head, len, "Connection", v, sizeof(v)) || !contains_token_ci(v, "upgrade")) return TM_WS_HS_UPGRADE;
    char material[96];
    snprintf(material, sizeof(material), "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", key_b64);
    uint8_t digest[20];
    mbedtls_sha1_ret((const unsigned char*) material, strlen(material), digest);
    char want[32];
    tm_base64(want, sizeof(want), digest, sizeof(digest));
    if (!header_value(head, len, "Sec-WebSocket-Accept", v, sizeof(v)) || strcmp(v, want) != 0) return TM_WS_HS_ACCEPT;
    if (!header_value(head, len, "Sec-WebSocket-Protocol", v, sizeof(v)) || strcmp(v, subprotocol) != 0) return TM_WS_HS_PROTOCOL;
    if (header_value(head, len, "Sec-WebSocket-Extensions", v, sizeof(v))) return TM_WS_HS_EXTENSION;
    return TM_WS_HS_OK;
}
