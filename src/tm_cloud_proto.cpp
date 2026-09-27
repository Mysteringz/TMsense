#include "tm_cloud_proto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "mbedtls/md.h"

// --- endpoint ----------------------------------------------------------------

const char* tm_cloud_url_error(int code) {
    switch (code) {
        case TM_URL_OK: return "ok";
        case TM_URL_LENGTH: return "cloud_url must be 1..128 characters";
        case TM_URL_CHARS: return "cloud_url may contain only printable ASCII, no spaces, % or \\";
        case TM_URL_SCHEME: return "cloud_url must start with wss://";
        case TM_URL_USERINFO: return "cloud_url must not contain a user name or password";
        case TM_URL_HOST: return "cloud_url host must be a lowercase DNS name";
        case TM_URL_PORT: return "cloud_url port must be 443";
        case TM_URL_PATH: return "cloud_url path must be like /tmnode";
        case TM_URL_QUERY: return "cloud_url must not have a query or fragment";
        default: return "cloud_url is invalid";
    }
}

static bool host_ok(const char* h, size_t n, bool allow_ip) {
    if (n == 0 || n > TM_CLOUD_HOST_MAX || h[0] == '.' || h[n - 1] == '.') return false;
    size_t label = 0;
    bool alpha = false;
    for (size_t i = 0; i < n; ++i) {
        const char c = h[i];
        if (c == '.') {
            if (label == 0 || h[i - 1] == '-') return false;
            label = 0;
            continue;
        }
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
        if (!ok) return false;   // uppercase included: one canonical spelling, compared byte for byte
        if (c == '-' && label == 0) return false;
        if (c >= 'a' && c <= 'z') alpha = true;
        if (++label > 63) return false;
    }
    if (h[n - 1] == '-') return false;
    // A bare IPv4 address has no name for the certificate to vouch for; only
    // a test build may use one, for a fixture on the LAN.
    return alpha || allow_ip;
}

int tm_cloud_parse_url(const char* url, bool test_build, TmCloudUrl* out) {
    memset(out, 0, sizeof(*out));
    const size_t n = strnlen(url, TM_CLOUD_URL_MAX + 1);
    if (n == 0 || n > TM_CLOUD_URL_MAX) return TM_URL_LENGTH;
    for (size_t i = 0; i < n; ++i) {
        const unsigned char c = (unsigned char) url[i];
        if (c <= 0x20 || c >= 0x7f || c == '%' || c == '\\') return TM_URL_CHARS;
    }
    if (strchr(url, '?') || strchr(url, '#')) return TM_URL_QUERY;
    const char* p;
    if (!strncmp(url, "wss://", 6)) {
        out->tls = true;
        p = url + 6;
    } else if (test_build && !strncmp(url, "ws://", 5)) {
        out->tls = false;
        p = url + 5;
    } else {
        return TM_URL_SCHEME;
    }
    const char* slash = strchr(p, '/');
    if (!slash) return TM_URL_PATH;
    const char* at = (const char*) memchr(p, '@', (size_t) (slash - p));
    if (at) return TM_URL_USERINFO;
    const char* colon = (const char*) memchr(p, ':', (size_t) (slash - p));
    const char* host_end = colon ? colon : slash;
    if (!host_ok(p, (size_t) (host_end - p), test_build)) return TM_URL_HOST;
    memcpy(out->host, p, (size_t) (host_end - p));
    out->port = out->tls ? 443 : 80;
    if (colon) {
        if (slash - colon < 2 || slash - colon > 6) return TM_URL_PORT;
        long port = 0;
        for (const char* d = colon + 1; d < slash; ++d) {
            if (*d < '0' || *d > '9') return TM_URL_PORT;
            port = port * 10 + (*d - '0');
        }
        if (port < 1 || port > 65535 || (colon[1] == '0')) return TM_URL_PORT;
        out->port = (uint16_t) port;
    }
    if (!test_build && out->port != 443) return TM_URL_PORT;
    const size_t plen = strlen(slash);
    if (plen < 2 || plen > TM_CLOUD_PATH_MAX) return TM_URL_PATH;
    for (size_t i = 0; i < plen; ++i) {
        const char c = slash[i];
        const bool ok = isalnum((unsigned char) c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~';
        if (!ok) return TM_URL_PATH;
        if (c == '/' && (slash[i + 1] == '/' || (slash[i + 1] == '.' && (slash[i + 2] == '/' || slash[i + 2] == 0 ||
                         (slash[i + 2] == '.' && (slash[i + 3] == '/' || slash[i + 3] == 0)))))) {
            return TM_URL_PATH;
        }
    }
    memcpy(out->path, slash, plen);
    return TM_URL_OK;
}

// --- control messages ----------------------------------------------------------

typedef struct {
    const char* key;
    size_t key_len;
    bool is_string;
    const char* str;
    size_t str_len;
    uint64_t num;
} JsonField;

#define JSON_MAX_FIELDS 12

static void skip_ws(const char* s, size_t len, size_t* i) {
    while (*i < len && (s[*i] == ' ' || s[*i] == '\t' || s[*i] == '\n' || s[*i] == '\r')) ++*i;
}

/** Flat object of "key": "string" | integer. No escapes, no nesting, no floats: nothing v1 needs. */
static int parse_flat(const char* s, size_t len, JsonField* f, int* count) {
    size_t i = 0;
    *count = 0;
    skip_ws(s, len, &i);
    if (i >= len || s[i++] != '{') return TM_CTL_MALFORMED;
    skip_ws(s, len, &i);
    if (i < len && s[i] == '}') {
        ++i;
    } else {
        for (;;) {
            if (*count >= JSON_MAX_FIELDS) return TM_CTL_MALFORMED;
            JsonField* x = &f[(*count)++];
            skip_ws(s, len, &i);
            if (i >= len || s[i++] != '"') return TM_CTL_MALFORMED;
            x->key = s + i;
            while (i < len && s[i] != '"') {
                if (s[i] == '\\' || (unsigned char) s[i] < 0x20) return TM_CTL_MALFORMED;
                ++i;
            }
            if (i >= len) return TM_CTL_MALFORMED;
            x->key_len = (size_t) (s + i - x->key);
            ++i;
            skip_ws(s, len, &i);
            if (i >= len || s[i++] != ':') return TM_CTL_MALFORMED;
            skip_ws(s, len, &i);
            if (i >= len) return TM_CTL_MALFORMED;
            if (s[i] == '"') {
                ++i;
                x->is_string = true;
                x->str = s + i;
                while (i < len && s[i] != '"') {
                    if (s[i] == '\\' || (unsigned char) s[i] < 0x20) return TM_CTL_MALFORMED;
                    ++i;
                }
                if (i >= len) return TM_CTL_MALFORMED;
                x->str_len = (size_t) (s + i - x->str);
                ++i;
            } else if (s[i] >= '0' && s[i] <= '9') {
                x->is_string = false;
                x->num = 0;
                size_t digits = 0;
                while (i < len && s[i] >= '0' && s[i] <= '9') {
                    if (++digits > 15) return TM_CTL_MALFORMED;
                    x->num = x->num * 10 + (uint64_t) (s[i] - '0');
                    ++i;
                }
                if (i < len && (s[i] == '.' || s[i] == 'e' || s[i] == 'E')) return TM_CTL_MALFORMED;
            } else {
                return TM_CTL_MALFORMED;
            }
            skip_ws(s, len, &i);
            if (i >= len) return TM_CTL_MALFORMED;
            if (s[i] == ',') { ++i; continue; }
            if (s[i] == '}') { ++i; break; }
            return TM_CTL_MALFORMED;
        }
    }
    skip_ws(s, len, &i);
    return i == len ? TM_CTL_OK : TM_CTL_MALFORMED;
}

static const JsonField* field(const JsonField* f, int n, const char* key) {
    const size_t k = strlen(key);
    const JsonField* found = NULL;
    for (int i = 0; i < n; ++i) {
        if (f[i].key_len == k && !memcmp(f[i].key, key, k)) {
            if (found) return NULL;   // a duplicate key is ambiguous: treat as absent
            found = &f[i];
        }
    }
    return found;
}

static bool get_str(const JsonField* f, int n, const char* key, char* out, size_t max, size_t exact) {
    const JsonField* x = field(f, n, key);
    if (!x || !x->is_string || x->str_len >= max || (exact && x->str_len != exact) || x->str_len == 0) return false;
    memcpy(out, x->str, x->str_len);
    out[x->str_len] = 0;
    return true;
}

static bool get_u32(const JsonField* f, int n, const char* key, uint32_t* out, uint32_t max) {
    const JsonField* x = field(f, n, key);
    if (!x || x->is_string || x->num > max) return false;
    *out = (uint32_t) x->num;
    return true;
}

static bool lower_hex(const char* s) {
    for (; *s; ++s) if (!((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f'))) return false;
    return true;
}

int tm_cloud_parse_control(const char* text, size_t len, TmCloudControl* out) {
    memset(out, 0, sizeof(*out));
    if (len == 0 || len > TM_CLOUD_CONTROL_MAX) return TM_CTL_MALFORMED;
    JsonField f[JSON_MAX_FIELDS];
    int n = 0;
    const int r = parse_flat(text, len, f, &n);
    if (r != TM_CTL_OK) return r;
    char type[16];
    if (!get_str(f, n, "type", type, sizeof(type), 0)) return TM_CTL_FIELDS;
    uint32_t v = 0;
    if (!get_u32(f, n, "v", &v, 0xFFFF)) return TM_CTL_FIELDS;
    if (v != TM_CLOUD_VERSION) return TM_CTL_VERSION;
    if (!strcmp(type, "challenge")) {
        if (!get_str(f, n, "nonce", out->nonce, sizeof(out->nonce), 64) || !lower_hex(out->nonce)) return TM_CTL_FIELDS;
        out->type = TM_CTL_CHALLENGE;
    } else if (!strcmp(type, "ready")) {
        if (!get_str(f, n, "session", out->session, sizeof(out->session), 0)) return TM_CTL_FIELDS;
        if (!get_str(f, n, "uid", out->uid, sizeof(out->uid), 17)) return TM_CTL_FIELDS;
        if (!get_u32(f, n, "heartbeatMs", &out->heartbeat_ms, 3600000)) return TM_CTL_FIELDS;
        out->type = TM_CTL_READY;
    } else if (!strcmp(type, "ack")) {
        uint32_t boot = 0;
        if (!get_str(f, n, "session", out->session, sizeof(out->session), 0)) return TM_CTL_FIELDS;
        if (!get_u32(f, n, "boot", &boot, 0xFFFF) || !get_u32(f, n, "seq", &out->seq, 0xFFFFFFFFu)) return TM_CTL_FIELDS;
        out->boot = (uint16_t) boot;
        out->type = TM_CTL_ACK;
    } else if (!strcmp(type, "ota_grant")) {
        if (!get_u32(f, n, "seq", &out->seq, 0xFFFFFFFFu)) return TM_CTL_FIELDS;
        if (!get_str(f, n, "build", out->build, sizeof(out->build), 16) || !lower_hex(out->build)) return TM_CTL_FIELDS;
        if (!get_str(f, n, "token", out->token, sizeof(out->token), 64) || !lower_hex(out->token)) return TM_CTL_FIELDS;
        if (!get_u32(f, n, "expiresInMs", &out->expires_in_ms, 3600000)) return TM_CTL_FIELDS;
        out->type = TM_CTL_OTA_GRANT;
    } else {
        out->type = TM_CTL_UNKNOWN;
    }
    return TM_CTL_OK;
}

void tm_cloud_uid_string(const uint8_t uid[6], char out[18]) {
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x", uid[0], uid[1], uid[2], uid[3], uid[4], uid[5]);
}

size_t tm_cloud_auth_json(char* out, size_t max, const char* uid, const char* nonce,
                          const uint8_t* key, size_t key_len) {
    char material[8 + 18 + 1 + 64 + 1];
    const int m = snprintf(material, sizeof(material), "tmnode1|%s|%s", uid, nonce);
    if (m <= 0 || (size_t) m >= sizeof(material)) return 0;
    uint8_t mac[32];
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), key, key_len,
                    (const unsigned char*) material, (size_t) m, mac);
    char hex[65];
    for (int i = 0; i < 32; ++i) snprintf(hex + 2 * i, 3, "%02x", mac[i]);
    const int n = snprintf(out, max, "{\"type\":\"auth\",\"v\":%d,\"uid\":\"%s\",\"nonce\":\"%s\",\"mac\":\"%s\"}",
                           TM_CLOUD_VERSION, uid, nonce, hex);
    memset(mac, 0, sizeof(mac));
    memset(hex, 0, sizeof(hex));
    return n > 0 && (size_t) n < max ? (size_t) n : 0;
}

// --- acknowledgements ----------------------------------------------------------

void tm_ack_reset(TmAckTracker* t) { t->n = 0; }

void tm_ack_sent(TmAckTracker* t, uint8_t type, uint16_t boot, uint32_t seq, uint32_t now_ms) {
    if (t->n == TM_ACK_SLOTS) {
        // Full: the oldest has waited through 15 newer packets; forget it.
        memmove(&t->e[0], &t->e[1], sizeof(t->e[0]) * (TM_ACK_SLOTS - 1));
        --t->n;
    }
    t->e[t->n].type = type;
    t->e[t->n].boot = boot;
    t->e[t->n].seq = seq;
    t->e[t->n].sent_ms = now_ms;
    ++t->n;
}

uint8_t tm_ack_match(TmAckTracker* t, uint16_t boot, uint32_t seq) {
    for (uint8_t i = 0; i < t->n; ++i) {
        if (t->e[i].boot != boot || t->e[i].seq != seq) continue;
        const uint8_t type = t->e[i].type;
        const uint8_t drop = (uint8_t) (i + 1);
        memmove(&t->e[0], &t->e[drop], sizeof(t->e[0]) * (t->n - drop));
        t->n = (uint8_t) (t->n - drop);
        return type;
    }
    return 0;
}

uint32_t tm_ack_oldest_report_age(const TmAckTracker* t, uint32_t now_ms) {
    for (uint8_t i = 0; i < t->n; ++i) {
        if (t->e[i].type == TM_TYPE_REPORT) return now_ms - t->e[i].sent_ms;
    }
    return 0;
}

// --- outgoing queue ------------------------------------------------------------

void tm_queue_clear(TmPacketQueue* q) {
    q->head = 0;
    q->count = 0;
}

static void queue_remove(TmPacketQueue* q, uint8_t k) {
    // Shift everything after logical index k one place towards the head.
    for (uint8_t j = k; j + 1 < q->count; ++j) {
        const uint8_t a = (uint8_t) ((q->head + j) % TM_QUEUE_SLOTS);
        const uint8_t b = (uint8_t) ((q->head + j + 1) % TM_QUEUE_SLOTS);
        q->slot[a].len = q->slot[b].len;
        q->slot[a].type = q->slot[b].type;
        q->slot[a].at_ms = q->slot[b].at_ms;
        memcpy(q->slot[a].data, q->slot[b].data, q->slot[b].len);
    }
    --q->count;
}

bool tm_queue_push(TmPacketQueue* q, const uint8_t* data, size_t len, uint32_t now_ms) {
    if (len < 4 || len > TM_PACKET_MAX_SIZE) {
        ++q->dropped;
        return false;
    }
    const uint8_t type = data[3];
    if (q->count == TM_QUEUE_SLOTS) {
        uint8_t victim = 0xFF;
        for (uint8_t k = 0; k < q->count; ++k) {
            if (q->slot[(q->head + k) % TM_QUEUE_SLOTS].type == TM_TYPE_RAW) { victim = k; break; }
        }
        if (victim == 0xFF) {
            if (type == TM_TYPE_RAW) {
                ++q->dropped;
                return false;
            }
            victim = 0;   // all REPORT/STATUS: the oldest is the least current
        }
        queue_remove(q, victim);
        ++q->dropped;
    }
    const uint8_t at = (uint8_t) ((q->head + q->count) % TM_QUEUE_SLOTS);
    q->slot[at].len = (uint16_t) len;
    q->slot[at].type = type;
    q->slot[at].at_ms = now_ms;
    memcpy(q->slot[at].data, data, len);
    ++q->count;
    return true;
}

size_t tm_queue_pop(TmPacketQueue* q, uint8_t* out, size_t max, uint8_t* type, uint32_t now_ms) {
    while (q->count > 0) {
        const uint8_t h = q->head;
        q->head = (uint8_t) ((q->head + 1) % TM_QUEUE_SLOTS);
        --q->count;
        if (now_ms - q->slot[h].at_ms > TM_QUEUE_MAX_AGE_MS || q->slot[h].len > max) {
            ++q->dropped;
            continue;
        }
        memcpy(out, q->slot[h].data, q->slot[h].len);
        *type = q->slot[h].type;
        return q->slot[h].len;
    }
    return 0;
}

// --- reconnection ----------------------------------------------------------------

uint32_t tm_backoff_ms(uint8_t attempt, uint32_t random) {
    uint32_t base = TM_BACKOFF_MIN_MS;
    for (uint8_t i = 0; i < attempt && base < TM_BACKOFF_MAX_MS; ++i) base *= 2;
    if (base > TM_BACKOFF_MAX_MS) base = TM_BACKOFF_MAX_MS;
    return base / 2 + random % (base / 2 + 1);
}

// --- HTTP response head ---------------------------------------------------------------

bool tm_http_parse_head(const char* head, size_t len, TmHttpHead* out) {
    out->status = 0;
    out->content_length = -1;
    out->chunked = false;
    out->has_location = false;
    if (len < 12 || strncmp(head, "HTTP/1.", 7) != 0 || head[8] != ' ') return false;
    if (!isdigit((unsigned char) head[9]) || !isdigit((unsigned char) head[10]) || !isdigit((unsigned char) head[11])) return false;
    out->status = (head[9] - '0') * 100 + (head[10] - '0') * 10 + (head[11] - '0');
    const char* p = head;
    const char* end = head + len;
    while (p < end && *p != '\n') ++p;
    while (p < end) {
        ++p;
        if (p >= end) break;
        const char* line = p;
        while (p < end && *p != '\n') ++p;
        size_t n = (size_t) (p - line);
        if (n && line[n - 1] == '\r') --n;
        char name[24];
        const char* colon = (const char*) memchr(line, ':', n);
        if (!colon || (size_t) (colon - line) >= sizeof(name)) continue;
        size_t k = (size_t) (colon - line);
        for (size_t i = 0; i < k; ++i) name[i] = (char) tolower((unsigned char) line[i]);
        name[k] = 0;
        const char* v = colon + 1;
        const char* ve = line + n;
        while (v < ve && *v == ' ') ++v;
        if (!strcmp(name, "content-length")) {
            int64_t cl = 0;
            if (v == ve) return false;
            for (const char* d = v; d < ve; ++d) {
                if (*d < '0' || *d > '9' || cl > 100000000) return false;
                cl = cl * 10 + (*d - '0');
            }
            out->content_length = (int32_t) cl;
        } else if (!strcmp(name, "transfer-encoding")) {
            out->chunked = true;   // anything but identity is refused by the caller
        } else if (!strcmp(name, "location")) {
            out->has_location = true;
        }
    }
    return true;
}
