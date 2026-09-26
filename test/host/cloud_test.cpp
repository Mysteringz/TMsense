// Host tests for the direct-to-cloud transport: the firmware's own tm_ws.cpp,
// tm_cloud_proto.cpp and tm_cloud_session.cpp, compiled unchanged. Each check
// is a claim about behaviour a node on a ceiling depends on.
//
//   test/host/build_cloud_host.sh && build/cloud_test [test/host/fixtures/tmnode_auth_vectors.json]
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include "tm_cloud_proto.h"
#include "tm_cloud_session.h"
#include "tm_packet.h"
#include "tm_ws.h"
#include "mbedtls/sha1.h"

static int g_checks = 0, g_failed = 0;
#define CHECK(cond, what)                                                            \
    do {                                                                             \
        ++g_checks;                                                                  \
        if (!(cond)) {                                                               \
            ++g_failed;                                                              \
            fprintf(stderr, "FAIL %s:%d: %s  [%s]\n", __FILE__, __LINE__, what, #cond); \
        }                                                                            \
    } while (0)

// --- a server's frames, built by hand (servers do not mask) ----------------------

static std::vector<uint8_t> server_frame(uint8_t opcode, const std::string& payload, bool fin = true) {
    std::vector<uint8_t> f;
    f.push_back((uint8_t) ((fin ? 0x80 : 0) | opcode));
    const size_t n = payload.size();
    if (n < 126) {
        f.push_back((uint8_t) n);
    } else {
        f.push_back(126);
        f.push_back((uint8_t) (n >> 8));
        f.push_back((uint8_t) n);
    }
    f.insert(f.end(), payload.begin(), payload.end());
    return f;
}

/** Unmask one client frame; returns opcode, payload in `out`. */
static int client_frame(const uint8_t* d, size_t n, std::string* out, size_t* used) {
    if (n < 6 || !(d[1] & 0x80)) return -1;
    size_t len = d[1] & 0x7F, o = 2;
    if (len == 126) { len = (size_t) (d[2] << 8 | d[3]); o = 4; }
    const uint8_t* mask = d + o;
    o += 4;
    if (n < o + len) return -1;
    out->resize(len);
    for (size_t i = 0; i < len; ++i) (*out)[i] = (char) (d[o + i] ^ mask[i & 3]);
    *used = o + len;
    return d[0] & 0x0F;
}

static void test_ws() {
    TmWsParser p;
    TmWsEvent ev;
    tm_ws_parser_reset(&p);

    // A text message split into three frames, delivered one byte at a time,
    // with a ping in the middle of it: reassembled, and the ping still seen.
    std::vector<uint8_t> bytes;
    for (auto part : {server_frame(TM_WS_OP_TEXT, "{\"type\":", false), server_frame(TM_WS_OP_PING, "hi"),
                      server_frame(TM_WS_OP_CONT, "\"ack\",", false), server_frame(TM_WS_OP_CONT, "\"v\":1}")}) {
        bytes.insert(bytes.end(), part.begin(), part.end());
    }
    std::vector<std::string> seen;
    for (size_t i = 0; i < bytes.size(); ++i) {
        size_t off = 0;
        while (off < 1) {
            off += tm_ws_feed(&p, &bytes[i] + off, 1 - off, &ev);
            if (ev.type == TM_WS_EV_TEXT) seen.push_back(std::string((const char*) ev.data, ev.len));
            if (ev.type == TM_WS_EV_PING) seen.push_back("PING:" + std::string((const char*) ev.data, ev.len));
            if (ev.type == TM_WS_EV_NONE) break;
        }
    }
    CHECK(seen.size() == 2 && seen[0] == "PING:hi" && seen[1] == "{\"type\":\"ack\",\"v\":1}",
          "fragmented message reassembles across byte-sized reads, with a control frame between fragments");

    auto first_event = [](const std::vector<uint8_t>& b) {
        TmWsParser q;
        tm_ws_parser_reset(&q);
        TmWsEvent e;
        size_t off = 0;
        do {
            off += tm_ws_feed(&q, b.data() + off, b.size() - off, &e);
        } while (e.type == TM_WS_EV_NONE && off < b.size());
        return e;
    };
    std::vector<uint8_t> masked = server_frame(TM_WS_OP_TEXT, "x");
    masked[1] |= 0x80;
    masked.insert(masked.begin() + 2, {1, 2, 3, 4});
    CHECK(first_event(masked).error == TM_WS_ERR_MASKED, "a masked server frame is a protocol error");
    std::vector<uint8_t> rsv = server_frame(TM_WS_OP_BINARY, "x");
    rsv[0] |= 0x40;
    CHECK(first_event(rsv).error == TM_WS_ERR_RSV, "compression bits set without negotiation are refused");
    CHECK(first_event(server_frame(TM_WS_OP_CONT, "x")).error == TM_WS_ERR_SEQUENCE, "continuation without a start");
    CHECK(first_event(server_frame(TM_WS_OP_PING, "x", false)).error == TM_WS_ERR_CONTROL, "fragmented ping");
    CHECK(first_event(server_frame(TM_WS_OP_BINARY, std::string(1025, 'a'))).error == TM_WS_ERR_TOO_BIG,
          "a message over the reassembly buffer is refused from its header");
    std::vector<uint8_t> two = server_frame(TM_WS_OP_BINARY, std::string(600, 'a'), false);
    auto more = server_frame(TM_WS_OP_CONT, std::string(600, 'b'));
    two.insert(two.end(), more.begin(), more.end());
    CHECK(first_event(two).error == TM_WS_ERR_TOO_BIG, "fragments that add up to too much are refused");
    TmWsEvent big = first_event(server_frame(TM_WS_OP_BINARY, std::string(806, 'r')));
    CHECK(big.type == TM_WS_EV_BINARY && big.len == 806, "an 806-byte RAW (16-bit length) passes");
    TmWsEvent cl = first_event(server_frame(TM_WS_OP_CLOSE, std::string("\x0f\xa9", 2)));
    CHECK(cl.type == TM_WS_EV_CLOSE && cl.close_code == 4009, "close code read");

    uint8_t out[64];
    const uint8_t mask[4] = {9, 8, 7, 6};
    const size_t n = tm_ws_frame(out, sizeof(out), TM_WS_OP_BINARY, (const uint8_t*) "abc", 3, mask);
    std::string back;
    size_t used = 0;
    CHECK(n == 9 && client_frame(out, n, &back, &used) == TM_WS_OP_BINARY && back == "abc", "client frames are masked, final");
    CHECK(tm_ws_frame(out, 8, TM_WS_OP_BINARY, (const uint8_t*) "abc", 3, mask) == 0, "frame refuses to overflow");

    // RFC 6455's own example key and accept.
    const char* key = "dGhlIHNhbXBsZSBub25jZQ==";
    std::string ok = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                     "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\nSec-WebSocket-Protocol: tmnode.v1\r\n\r\n";
    CHECK(tm_ws_check_response(ok.c_str(), ok.size(), key, "tmnode.v1") == TM_WS_HS_OK, "RFC 6455 accept example");
    std::string bad_accept = ok;
    bad_accept.replace(bad_accept.find("s3pP"), 4, "AAAA");
    CHECK(tm_ws_check_response(bad_accept.c_str(), bad_accept.size(), key, "tmnode.v1") == TM_WS_HS_ACCEPT, "wrong accept");
    std::string no_proto = ok;
    no_proto.replace(no_proto.find("tmnode.v1"), 9, "tmnode.v2");
    CHECK(tm_ws_check_response(no_proto.c_str(), no_proto.size(), key, "tmnode.v1") == TM_WS_HS_PROTOCOL, "wrong subprotocol");
    std::string ext = ok.substr(0, ok.size() - 2) + "Sec-WebSocket-Extensions: permessage-deflate\r\n\r\n";
    CHECK(tm_ws_check_response(ext.c_str(), ext.size(), key, "tmnode.v1") == TM_WS_HS_EXTENSION, "unasked-for compression");
    std::string login = "HTTP/1.1 302 Found\r\nLocation: https://x.cloudflareaccess.com/\r\n\r\n";
    CHECK(tm_ws_check_response(login.c_str(), login.size(), key, "tmnode.v1") == TM_WS_HS_STATUS,
          "an Access login redirect is not an upgrade");
}

static void test_url() {
    TmCloudUrl u;
    CHECK(tm_cloud_parse_url("wss://sense.hkumyseat.com/tmnode", false, &u) == TM_URL_OK && u.port == 443 &&
          !strcmp(u.host, "sense.hkumyseat.com") && !strcmp(u.path, "/tmnode") && u.tls, "the production endpoint");
    CHECK(tm_cloud_parse_url("wss://sense.hkumyseat.com:443/tmnode", false, &u) == TM_URL_OK, "explicit :443");
    struct { const char* url; int want; const char* why; } bad[] = {
        {"", TM_URL_LENGTH, "empty"},
        {"ws://sense.hkumyseat.com/tmnode", TM_URL_SCHEME, "plain ws in production"},
        {"https://sense.hkumyseat.com/tmnode", TM_URL_SCHEME, "not a websocket"},
        {"wss://sense.hkumyseat.com:8443/tmnode", TM_URL_PORT, "non-443 in production"},
        {"wss://user:pw@sense.hkumyseat.com/tmnode", TM_URL_USERINFO, "credentials"},
        {"wss://sense.hkumyseat.com/tmnode?key=x", TM_URL_QUERY, "query"},
        {"wss://sense.hkumyseat.com/tmnode#x", TM_URL_QUERY, "fragment"},
        {"wss://Sense.HKUMySeat.com/tmnode", TM_URL_HOST, "uppercase host (one canonical form)"},
        {"wss://13.251.45.51/tmnode", TM_URL_HOST, "an IP has no certificate name"},
        {"wss://-bad.example.com/tmnode", TM_URL_HOST, "label starts with -"},
        {"wss://bad..example.com/tmnode", TM_URL_HOST, "empty label"},
        {"wss://sense.hkumyseat.com", TM_URL_PATH, "no path"},
        {"wss://sense.hkumyseat.com/", TM_URL_PATH, "bare slash"},
        {"wss://sense.hkumyseat.com/a/../b", TM_URL_PATH, "dot segments"},
        {"wss://sense.hkumyseat.com//tmnode", TM_URL_PATH, "empty segment"},
        {"wss://sense.hkumyseat.com/tm node", TM_URL_CHARS, "space"},
        {"wss://sense.hkumyseat.com/tm%20node", TM_URL_CHARS, "percent-encoding"},
        {"wss://sense.hkumyseat.com/tm\x01node", TM_URL_CHARS, "control character"},
        {"wss://sense.hkumyseat.com:0443/tmnode", TM_URL_PORT, "leading zero port"},
    };
    for (auto& b : bad) CHECK(tm_cloud_parse_url(b.url, false, &u) == b.want, b.why);
    std::string longest = "wss://" + std::string(60, 'a') + ".example.com/" + std::string(128 - 6 - 60 - 13, 'p');
    CHECK(longest.size() == 128 && tm_cloud_parse_url(longest.c_str(), false, &u) == TM_URL_OK,
          "a 128-byte URL is accepted whole");
    std::string too_long = longest + "p";
    CHECK(tm_cloud_parse_url(too_long.c_str(), false, &u) == TM_URL_LENGTH, "129 bytes is refused, not truncated");
    CHECK(tm_cloud_parse_url("ws://tmedge.local:5211/tmnode", true, &u) == TM_URL_OK && !u.tls && u.port == 5211,
          "a test build may use a local fixture");
    CHECK(tm_cloud_parse_url("wss://192.168.0.179:8443/tmnode", true, &u) == TM_URL_OK && u.tls,
          "a test build may name a LAN fixture by address");
}

static void test_control() {
    TmCloudControl c;
    std::string ch = "{\"type\":\"challenge\",\"v\":1,\"nonce\":\"" + std::string(64, 'a') + "\"}";
    CHECK(tm_cloud_parse_control(ch.c_str(), ch.size(), &c) == TM_CTL_OK && c.type == TM_CTL_CHALLENGE, "challenge");
    std::string up = "{\"type\":\"challenge\",\"v\":1,\"nonce\":\"" + std::string(64, 'A') + "\"}";
    CHECK(tm_cloud_parse_control(up.c_str(), up.size(), &c) == TM_CTL_FIELDS, "nonce must be lowercase hex");
    std::string ack = "{\"type\":\"ack\",\"v\":1,\"session\":\"abc\",\"boot\":21,\"seq\":4294967295}";
    CHECK(tm_cloud_parse_control(ack.c_str(), ack.size(), &c) == TM_CTL_OK && c.boot == 21 && c.seq == 4294967295u, "ack");
    std::string v2 = "{\"type\":\"ack\",\"v\":2,\"session\":\"abc\",\"boot\":21,\"seq\":1}";
    CHECK(tm_cloud_parse_control(v2.c_str(), v2.size(), &c) == TM_CTL_VERSION, "another version");
    std::string extra = "{\"type\":\"ready\",\"v\":1,\"uid\":\"01:02:03:04:05:06\",\"session\":\"s\",\"heartbeatMs\":15000,\"new\":\"x\"}";
    CHECK(tm_cloud_parse_control(extra.c_str(), extra.size(), &c) == TM_CTL_OK && c.type == TM_CTL_READY, "unknown keys ignored");
    std::string news = "{\"type\":\"future\",\"v\":1}";
    CHECK(tm_cloud_parse_control(news.c_str(), news.size(), &c) == TM_CTL_OK && c.type == TM_CTL_UNKNOWN, "unknown type tolerated");
    const char* malformed[] = {"", "[]", "{\"type\":\"ack\"", "{\"type\":\"a\\\"ck\",\"v\":1}", "{\"type\":{},\"v\":1}",
                               "{\"type\":\"ack\",\"v\":1.0}", "{\"type\":\"ack\",\"v\":-1}", "{\"type\":\"ack\",\"v\":1} x",
                               "{\"type\":true,\"v\":1}"};
    for (auto m : malformed) CHECK(tm_cloud_parse_control(m, strlen(m), &c) == TM_CTL_MALFORMED, m);
    std::string dup = "{\"type\":\"ack\",\"v\":1,\"session\":\"a\",\"boot\":1,\"seq\":1,\"seq\":2}";
    CHECK(tm_cloud_parse_control(dup.c_str(), dup.size(), &c) == TM_CTL_FIELDS, "a duplicate key is not guessed at");
    std::string huge = "{\"type\":\"ack\",\"v\":1,\"session\":\"" + std::string(600, 'a') + "\"}";
    CHECK(tm_cloud_parse_control(huge.c_str(), huge.size(), &c) == TM_CTL_MALFORMED, "over 512 bytes");
    std::string grant = "{\"type\":\"ota_grant\",\"v\":1,\"seq\":1789755000,\"build\":\"0123456789abcdef\",\"token\":\"" +
                        std::string(64, 'e') + "\",\"expiresInMs\":600000}";
    CHECK(tm_cloud_parse_control(grant.c_str(), grant.size(), &c) == TM_CTL_OK && c.type == TM_CTL_OTA_GRANT &&
          c.expires_in_ms == 600000 && !strcmp(c.build, "0123456789abcdef"), "ota_grant");
}

static std::string json_field(const std::string& s, size_t from, const char* key, size_t* at) {
    const std::string k = std::string("\"") + key + "\": \"";
    const size_t p = s.find(k, from);
    if (p == std::string::npos) return "";
    const size_t e = s.find('"', p + k.size());
    *at = e;
    return s.substr(p + k.size(), e - p - k.size());
}

static void test_auth(const char* vectors_path) {
    FILE* f = fopen(vectors_path, "r");
    CHECK(f != NULL, "auth vector file");
    if (!f) return;
    std::string s;
    char buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
    fclose(f);
    size_t at = 0;
    int count = 0;
    for (;;) {
        const std::string key = json_field(s, at, "key", &at);
        if (key.empty()) break;
        const std::string uid = json_field(s, at, "uid", &at);
        const std::string nonce = json_field(s, at, "nonce", &at);
        const std::string mac = json_field(s, at, "mac", &at);
        char out[TM_CLOUD_CONTROL_MAX];
        tm_cloud_auth_json(out, sizeof(out), uid.c_str(), nonce.c_str(), (const uint8_t*) key.data(), key.size());
        const std::string want = "{\"type\":\"auth\",\"v\":1,\"uid\":\"" + uid + "\",\"nonce\":\"" + nonce + "\",\"mac\":\"" + mac + "\"}";
        CHECK(want == out, "auth proof matches the shared vector");
        ++count;
    }
    CHECK(count == 3, "all three vectors read");
}

static void test_acks_and_queue() {
    TmAckTracker t;
    tm_ack_reset(&t);
    tm_ack_sent(&t, TM_TYPE_REPORT, 5, 10, 1000);
    tm_ack_sent(&t, TM_TYPE_STATUS, 5, 11, 1000);
    tm_ack_sent(&t, TM_TYPE_REPORT, 5, 12, 2000);
    CHECK(tm_ack_match(&t, 5, 99) == 0, "an ACK for a packet never sent is ignored");
    CHECK(tm_ack_match(&t, 4, 10) == 0, "an ACK from another boot is ignored");
    CHECK(tm_ack_oldest_report_age(&t, 7000) == 6000, "oldest unacknowledged report age");
    CHECK(tm_ack_match(&t, 5, 11) == TM_TYPE_STATUS, "status ack");
    CHECK(tm_ack_oldest_report_age(&t, 7000) == 5000, "the report before an acknowledged packet is written off");
    CHECK(tm_ack_match(&t, 5, 12) == TM_TYPE_REPORT && t.n == 0, "report ack");
    CHECK(tm_ack_match(&t, 5, 12) == 0, "the same ACK twice counts once");

    static TmPacketQueue q;
    tm_queue_clear(&q);
    uint8_t pkt[TM_PACKET_MAX_SIZE];
    auto make = [&](uint8_t type, uint8_t tagbyte) {
        memset(pkt, 0, sizeof(pkt));
        pkt[3] = type;
        pkt[4] = tagbyte;
        return (size_t) (type == TM_TYPE_RAW ? 806 : 44);
    };
    tm_queue_push(&q, pkt, make(TM_TYPE_REPORT, 1), 0);
    tm_queue_push(&q, pkt, make(TM_TYPE_RAW, 2), 0);
    tm_queue_push(&q, pkt, make(TM_TYPE_REPORT, 3), 0);
    tm_queue_push(&q, pkt, make(TM_TYPE_STATUS, 4), 0);
    CHECK(tm_queue_push(&q, pkt, make(TM_TYPE_REPORT, 5), 0) && q.count == 4, "full: the RAW makes room");
    CHECK(!tm_queue_push(&q, pkt, make(TM_TYPE_RAW, 6), 0), "full of reports: a RAW is the one dropped");
    std::vector<int> order;
    uint8_t type;
    while (tm_queue_pop(&q, pkt, sizeof(pkt), &type, 0)) order.push_back(pkt[4]);
    CHECK((order == std::vector<int>{1, 3, 4, 5}), "sent in the order built: no newer STATUS overtakes an older REPORT");
    tm_queue_push(&q, pkt, make(TM_TYPE_REPORT, 7), 1000);
    tm_queue_push(&q, pkt, make(TM_TYPE_REPORT, 8), 2900);
    CHECK(tm_queue_pop(&q, pkt, sizeof(pkt), &type, 3100) && pkt[4] == 8, "an observation older than 2 s is dropped, not sent late");

    uint32_t lo = 0xFFFFFFFF, hi = 0;
    for (uint32_t r = 0; r < 1000; ++r) {
        const uint32_t d = tm_backoff_ms(0, r * 2654435761u);
        lo = d < lo ? d : lo;
        hi = d > hi ? d : hi;
    }
    CHECK(lo >= 500 && hi <= 1000, "first retry about a second, jittered");
    CHECK(tm_backoff_ms(20, 0xFFFFFFFF) <= 60000 && tm_backoff_ms(20, 0) >= 30000, "capped at 60 s");

    TmHttpHead h;
    std::string ok = "HTTP/1.1 200 OK\r\nContent-Length: 926000\r\nCache-Control: no-store\r\n\r\n";
    CHECK(tm_http_parse_head(ok.c_str(), ok.size(), &h) && h.status == 200 && h.content_length == 926000 && !h.chunked, "200 head");
    std::string redir = "HTTP/1.1 302 Found\r\nLocation: http://evil/\r\n\r\n";
    CHECK(tm_http_parse_head(redir.c_str(), redir.size(), &h) && h.status == 302 && h.has_location, "redirect seen");
    std::string chunked = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n";
    CHECK(tm_http_parse_head(chunked.c_str(), chunked.size(), &h) && h.chunked && h.content_length == -1, "chunked seen");
    std::string neg = "HTTP/1.1 200 OK\r\nContent-Length: -1\r\n\r\n";
    CHECK(!tm_http_parse_head(neg.c_str(), neg.size(), &h), "a nonsense length is refused");
}

// --- the session state machine, against a scripted edge -----------------------------

struct FakeEdge {
    std::vector<uint8_t> inbox;        // bytes the node will read
    std::vector<uint8_t> written;      // bytes the node wrote
    bool connected = false;
    int connect_result = 0;
    int connects = 0;
    uint32_t rnd = 12345;
    std::vector<std::vector<uint8_t>> queue;
    int discards = 0;
    std::vector<std::vector<uint8_t>> downlinks;
};

static int fe_connect(void* c, const char*, uint16_t) {
    FakeEdge* e = (FakeEdge*) c;
    ++e->connects;
    e->connected = e->connect_result == 0;
    e->written.clear();
    return e->connect_result;
}
static int fe_write(void* c, const uint8_t* d, size_t n) {
    FakeEdge* e = (FakeEdge*) c;
    if (!e->connected) return -1;
    e->written.insert(e->written.end(), d, d + n);
    return (int) n;
}
static int fe_read(void* c, uint8_t* b, size_t max) {
    FakeEdge* e = (FakeEdge*) c;
    if (!e->connected) return -1;
    const size_t n = e->inbox.size() < max ? e->inbox.size() : max;
    memcpy(b, e->inbox.data(), n);
    e->inbox.erase(e->inbox.begin(), e->inbox.begin() + (long) n);
    return (int) n;
}
static void fe_close(void* c) { ((FakeEdge*) c)->connected = false; }
static uint32_t fe_random(void* c) {
    FakeEdge* e = (FakeEdge*) c;
    e->rnd = e->rnd * 1103515245u + 12345u;
    return e->rnd;
}
static size_t fe_next(void* c, uint8_t* buf, size_t max, uint8_t* type, uint32_t) {
    FakeEdge* e = (FakeEdge*) c;
    if (e->queue.empty() || e->queue.front().size() > max) return 0;
    const size_t n = e->queue.front().size();
    memcpy(buf, e->queue.front().data(), n);
    *type = buf[3];
    e->queue.erase(e->queue.begin());
    return n;
}
static void fe_discard(void* c) {
    ((FakeEdge*) c)->queue.clear();
    ++((FakeEdge*) c)->discards;
}
static void fe_down(void* c, const uint8_t* d, size_t n) { ((FakeEdge*) c)->downlinks.push_back(std::vector<uint8_t>(d, d + n)); }

static void push(FakeEdge& e, const std::vector<uint8_t>& b) { e.inbox.insert(e.inbox.end(), b.begin(), b.end()); }

static std::string accept_for(const std::string& request) {
    const std::string k = "Sec-WebSocket-Key: ";
    const size_t p = request.find(k);
    const std::string key = request.substr(p + k.size(), 24);
    const std::string material = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    uint8_t d[20];
    mbedtls_sha1_ret((const unsigned char*) material.data(), material.size(), d);
    char b64[32];
    tm_base64(b64, sizeof(b64), d, 20);
    return b64;
}

/** Drive a fresh session through connect, upgrade, challenge and ready. */
static void handshake(TmCloudSession* s, const TmCloudIo* io, FakeEdge& e, uint32_t now, const char* uid = "01:02:03:04:05:06") {
    tm_cloud_session_step(s, io, now, true, true);
    std::string req(e.written.begin(), e.written.end());
    push(e, std::vector<uint8_t>());
    std::string resp = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                       "Sec-WebSocket-Accept: " + accept_for(req) + "\r\nSec-WebSocket-Protocol: tmnode.v1\r\n\r\n";
    e.inbox.insert(e.inbox.end(), resp.begin(), resp.end());
    push(e, server_frame(TM_WS_OP_TEXT, "{\"type\":\"challenge\",\"v\":1,\"nonce\":\"" + std::string(64, '0') + "\"}"));
    e.written.clear();
    tm_cloud_session_step(s, io, now + 10, true, true);
    tm_cloud_session_step(s, io, now + 20, true, true);
    push(e, server_frame(TM_WS_OP_TEXT, std::string("{\"type\":\"ready\",\"v\":1,\"uid\":\"") + uid + "\",\"session\":\"S1\",\"heartbeatMs\":15000}"));
    tm_cloud_session_step(s, io, now + 30, true, true);
}

static std::vector<uint8_t> report_bytes(uint16_t boot, uint32_t seq) {
    TmPacketContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    const uint8_t uid[6] = {1, 2, 3, 4, 5, 6};
    memcpy(ctx.uid, uid, 6);
    ctx.boot = boot;
    ctx.seq = seq;
    ctx.key_len = 14;
    memcpy(ctx.key, "crosscheck-key", 14);
    TmReportInfo info = {1, 30.0f, 20.0f, 30.0f, 21.0f, TM_REPORT_BACKGROUND_READY};
    uint8_t b[TM_PACKET_MAX_SIZE];
    const size_t n = tm_build_report(b, &ctx, 1000, &info, NULL, 0);
    return std::vector<uint8_t>(b, b + n);
}

static void test_session() {
    static TmCloudSession s;
    FakeEdge e;
    TmCloudIo io = {fe_connect, fe_write, fe_read, fe_close, fe_random, &e};
    TmCloudUrl url;
    tm_cloud_parse_url("wss://sense.hkumyseat.com/tmnode", false, &url);
    const uint8_t key[] = "crosscheck-key";
    memset(&s, 0, sizeof(s));
    s.next_uplink = fe_next;
    s.discard_uplink = fe_discard;
    s.on_downlink = fe_down;
    s.cb_ctx = &e;
    tm_cloud_session_init(&s, &url, "01:02:03:04:05:06", key, 14, 5);

    tm_cloud_session_step(&s, &io, 100, true, false);
    CHECK(s.state == TM_CLOUD_WAIT_TIME && e.connects == 0, "no clock: it waits, and never connects unverified");
    tm_cloud_session_step(&s, &io, 100, false, true);
    CHECK(s.state == TM_CLOUD_OFF && e.connects == 0, "no Wi-Fi: off");

    handshake(&s, &io, e, 1000);
    // The auth frame the node wrote: check it is the proof, and complete.
    std::string auth;
    size_t used = 0;
    CHECK(client_frame(e.written.data(), e.written.size(), &auth, &used) == TM_WS_OP_TEXT &&
          auth.find("\"type\":\"auth\"") != std::string::npos && auth.find("\"mac\":\"") != std::string::npos,
          "the challenge is answered with a masked auth message");
    CHECK(s.state == TM_CLOUD_READY && s.want_status && e.discards >= 1, "ready: fresh STATUS wanted, backlog discarded");

    // A report goes up; its ACK is the evidence.
    e.written.clear();
    e.queue.push_back(report_bytes(5, 40));
    tm_cloud_session_step(&s, &io, 2000, true, true);
    std::string up;
    CHECK(client_frame(e.written.data(), e.written.size(), &up, &used) == TM_WS_OP_BINARY && up.size() == 44,
          "the packet travels as one binary message, unchanged");
    push(e, server_frame(TM_WS_OP_TEXT, "{\"type\":\"ack\",\"v\":1,\"session\":\"OTHER\",\"boot\":5,\"seq\":40}"));
    push(e, server_frame(TM_WS_OP_TEXT, "{\"type\":\"ack\",\"v\":1,\"session\":\"S1\",\"boot\":5,\"seq\":41}"));
    tm_cloud_session_step(&s, &io, 2100, true, true);
    CHECK(s.reports_acked == 0 && s.ignored_acks == 2, "ACKs for another session or an unsent packet prove nothing");
    push(e, server_frame(TM_WS_OP_TEXT, "{\"type\":\"ack\",\"v\":1,\"session\":\"S1\",\"boot\":5,\"seq\":40}"));
    tm_cloud_session_step(&s, &io, 2200, true, true);
    CHECK(s.reports_acked == 1 && s.last_report_ack_ms == 2200 && s.attempt == 0, "the report's own ACK counts");

    // Pings are answered with the same payload; commands come down.
    e.written.clear();
    push(e, server_frame(TM_WS_OP_PING, "beat"));
    push(e, server_frame(TM_WS_OP_BINARY, std::string(42, 'c')));
    tm_cloud_session_step(&s, &io, 2300, true, true);
    std::string pong;
    CHECK(client_frame(e.written.data(), e.written.size(), &pong, &used) == TM_WS_OP_PONG && pong == "beat", "pong");
    CHECK(e.downlinks.size() == 1 && e.downlinks[0].size() == 42, "a binary message goes to the command parser");

    // A grant arrives; it can be taken for its own sequence and build only.
    push(e, server_frame(TM_WS_OP_TEXT, "{\"type\":\"ota_grant\",\"v\":1,\"seq\":77,\"build\":\"0123456789abcdef\",\"token\":\"" +
                                          std::string(64, 'e') + "\",\"expiresInMs\":600000}"));
    tm_cloud_session_step(&s, &io, 2400, true, true);
    char token[65];
    CHECK(!tm_cloud_session_grant(&s, 78, "0123456789abcdef", 2500, token), "another sequence");
    CHECK(!tm_cloud_session_grant(&s, 77, "fedcba9876543210", 2500, token), "another build");
    CHECK(tm_cloud_session_grant(&s, 77, "0123456789abcdef", 2500, token) && token[0] == 'e', "the matching grant");

    // A report that is never acknowledged: the path is stalled, however open the socket looks.
    e.queue.push_back(report_bytes(5, 41));
    tm_cloud_session_step(&s, &io, 3000, true, true);
    for (uint32_t t = 3000; t <= 8200; t += 400) {
        push(e, server_frame(TM_WS_OP_PING, ""));   // the edge still answers pings
        tm_cloud_session_step(&s, &io, t, true, true);
    }
    CHECK(s.state == TM_CLOUD_BACKOFF && !strcmp(s.last_error, "reports not acknowledged"), "ACK deadline closes a stalled path");
    CHECK(s.session[0] == 0 && s.acks.n == 0, "session evidence is reset");
    CHECK(tm_cloud_session_grant(&s, 77, "0123456789abcdef", 9000, token), "the grant outlives the socket");
    CHECK(!tm_cloud_session_grant(&s, 77, "0123456789abcdef", 2400 + 600001, token), "until it expires");

    // Reconnect after the backoff, then silence.
    e.inbox.clear();
    handshake(&s, &io, e, s.next_attempt_ms + 1);
    CHECK(s.state == TM_CLOUD_READY && s.connects == 2, "reconnected");
    const uint32_t t0 = s.last_rx_ms;
    tm_cloud_session_step(&s, &io, t0 + TM_CLOUD_SILENCE_MS + 1, true, true);
    CHECK(s.state == TM_CLOUD_BACKOFF && !strcmp(s.last_error, "edge went silent"), "45 s of silence closes it");

    // The edge refuses the session, and a replaced session.
    e.inbox.clear();
    handshake(&s, &io, e, s.next_attempt_ms + 1);
    push(e, server_frame(TM_WS_OP_CLOSE, std::string("\x0f\xa9replaced", 10)));
    tm_cloud_session_step(&s, &io, s.state_since_ms + 100, true, true);
    CHECK(s.state == TM_CLOUD_BACKOFF && !strcmp(s.last_error, "edge closed (4009)"), "close code kept for `show`");

    // A ready for someone else is not ours.
    e.inbox.clear();
    handshake(&s, &io, e, s.next_attempt_ms + 1, "0a:0b:0c:0d:0e:0f");
    CHECK(s.state == TM_CLOUD_BACKOFF && !strcmp(s.last_error, "unexpected ready"), "ready for another uid");

    // A certificate failure is reported, and backoff grows.
    e.connect_result = -2;
    const uint8_t before = s.attempt;
    tm_cloud_session_step(&s, &io, s.next_attempt_ms + 1, true, true);
    CHECK(s.state == TM_CLOUD_BACKOFF && !strcmp(s.last_error, "tls: certificate refused") && s.attempt == before + 1,
          "a refused certificate is a visible failure with backoff");

    // Pausing for an OTA download closes the socket and stays closed.
    e.connect_result = 0;
    e.inbox.clear();
    handshake(&s, &io, e, s.next_attempt_ms + 1);
    tm_cloud_session_pause(&s, &io, true, 100000);
    const int connects = e.connects;
    tm_cloud_session_step(&s, &io, 200000, true, true);
    CHECK(s.state == TM_CLOUD_PAUSED && !e.connected && e.connects == connects, "paused means closed");
    tm_cloud_session_pause(&s, &io, false, 200001);
    tm_cloud_session_step(&s, &io, 200002, true, true);
    CHECK(e.connects == connects + 1, "resumed");
}

int main(int argc, char** argv) {
    test_ws();
    test_url();
    test_control();
    test_auth(argc > 1 ? argv[1] : "test/host/fixtures/tmnode_auth_vectors.json");
    test_acks_and_queue();
    test_session();
    printf("cloud_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
