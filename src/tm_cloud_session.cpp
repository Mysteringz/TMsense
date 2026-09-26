#include "tm_cloud_session.h"

#include <stdio.h>
#include <string.h>
#include "tm_protocol.h"

const char* tm_cloud_state_name(TmCloudState s) {
    switch (s) {
        case TM_CLOUD_OFF: return "off";
        case TM_CLOUD_WAIT_TIME: return "waiting-for-time";
        case TM_CLOUD_BACKOFF: return "backoff";
        case TM_CLOUD_CONNECTING: return "connecting";
        case TM_CLOUD_UPGRADING: return "upgrading";
        case TM_CLOUD_AUTHENTICATING: return "authenticating";
        case TM_CLOUD_READY: return "ready";
        case TM_CLOUD_PAUSED: return "paused";
    }
    return "?";
}

void tm_cloud_session_init(TmCloudSession* s, const TmCloudUrl* url, const char uid[18],
                           const uint8_t* key, size_t key_len, uint16_t boot) {
    // Callbacks survive a re-init; everything else starts over.
    size_t (*next)(void*, uint8_t*, size_t, uint8_t*, uint32_t) = s->next_uplink;
    void (*discard)(void*) = s->discard_uplink;
    void (*down)(void*, const uint8_t*, size_t) = s->on_downlink;
    void* ctx = s->cb_ctx;
    memset(s, 0, sizeof(*s));
    s->next_uplink = next;
    s->discard_uplink = discard;
    s->on_downlink = down;
    s->cb_ctx = ctx;
    s->url = *url;
    memcpy(s->uid, uid, 18);
    s->key = key;
    s->key_len = key_len;
    s->boot = boot;
    s->state = TM_CLOUD_OFF;
    tm_ws_parser_reset(&s->parser);
}

static void set_state(TmCloudSession* s, TmCloudState st, uint32_t now) {
    s->state = st;
    s->state_since_ms = now;
}

static void note(TmCloudSession* s, const char* why) {
    snprintf(s->last_error, sizeof(s->last_error), "%s", why);
}

/** Forget the connection and everything tied to it; try again after a backoff. */
static void drop(TmCloudSession* s, const TmCloudIo* io, uint32_t now, const char* why) {
    io->close(io->ctx);
    tm_ws_parser_reset(&s->parser);
    tm_ack_reset(&s->acks);
    s->session[0] = 0;
    s->challenge_answered = false;
    s->report_acked_this_session = false;
    s->want_status = false;
    s->http_len = 0;
    // What was queued for the dead session would reach the edge late, and a
    // late count is a wrong one: start the next session from fresh frames.
    if (s->discard_uplink) s->discard_uplink(s->cb_ctx);
    if (why) note(s, why);
    s->next_attempt_ms = now + tm_backoff_ms(s->attempt, io->random(io->ctx));
    if (s->attempt < 16) ++s->attempt;
    set_state(s, TM_CLOUD_BACKOFF, now);
}

static bool send_frame(TmCloudSession* s, const TmCloudIo* io, uint8_t opcode, const uint8_t* data, size_t len) {
    uint8_t mask[4];
    const uint32_t r = io->random(io->ctx);
    memcpy(mask, &r, 4);
    const size_t n = tm_ws_frame(s->tx, sizeof(s->tx), opcode, data, len, mask);
    return n > 0 && io->write(io->ctx, s->tx, n) == (int) n;
}

static void on_control(TmCloudSession* s, const TmCloudIo* io, const uint8_t* data, size_t len, uint32_t now) {
    TmCloudControl c;
    const int r = tm_cloud_parse_control((const char*) data, len, &c);
    if (r != TM_CTL_OK) {
        drop(s, io, now, r == TM_CTL_VERSION ? "edge speaks another version" : "bad control message");
        return;
    }
    switch (c.type) {
        case TM_CTL_CHALLENGE: {
            if (s->state != TM_CLOUD_AUTHENTICATING || s->challenge_answered) {
                drop(s, io, now, "unexpected challenge");
                return;
            }
            char auth[TM_CLOUD_CONTROL_MAX];
            const size_t n = tm_cloud_auth_json(auth, sizeof(auth), s->uid, c.nonce, s->key, s->key_len);
            const bool ok = n > 0 && send_frame(s, io, TM_WS_OP_TEXT, (const uint8_t*) auth, n);
            memset(auth, 0, sizeof(auth));
            if (!ok) {
                drop(s, io, now, "could not send auth");
                return;
            }
            s->challenge_answered = true;
            return;
        }
        case TM_CTL_READY:
            if (s->state != TM_CLOUD_AUTHENTICATING || !s->challenge_answered || strcmp(c.uid, s->uid) != 0) {
                drop(s, io, now, "unexpected ready");
                return;
            }
            memcpy(s->session, c.session, sizeof(s->session));
            tm_ack_reset(&s->acks);
            if (s->discard_uplink) s->discard_uplink(s->cb_ctx);
            s->want_status = true;
            ++s->connects;
            s->last_error[0] = 0;
            set_state(s, TM_CLOUD_READY, now);
            return;
        case TM_CTL_ACK: {
            if (s->state != TM_CLOUD_READY || strcmp(c.session, s->session) != 0 || c.boot != s->boot) {
                ++s->ignored_acks;
                return;
            }
            const uint8_t type = tm_ack_match(&s->acks, c.boot, c.seq);
            if (type == TM_TYPE_REPORT) {
                ++s->reports_acked;
                s->last_report_ack_ms = now ? now : 1;
                s->report_acked_this_session = true;
                // Only an accepted report proves the path works end to end:
                // that, not a TCP connect, is what resets the backoff.
                s->attempt = 0;
            } else if (type == 0) {
                ++s->ignored_acks;
            }
            return;
        }
        case TM_CTL_OTA_GRANT:
            if (s->state != TM_CLOUD_READY) return;
            s->grant_valid = true;
            s->grant_seq = c.seq;
            memcpy(s->grant_build, c.build, sizeof(s->grant_build));
            memcpy(s->grant_token, c.token, sizeof(s->grant_token));
            s->grant_expires_ms = now + c.expires_in_ms;
            memset(&c, 0, sizeof(c));
            return;
        case TM_CTL_UNKNOWN:
            return;   // a newer edge's addition to v1
    }
}

/** Feed received bytes through the WebSocket parser and act on each event. */
static void on_bytes(TmCloudSession* s, const TmCloudIo* io, const uint8_t* data, size_t len, uint32_t now) {
    size_t off = 0;
    while (off <= len && s->state >= TM_CLOUD_UPGRADING) {
        TmWsEvent ev;
        const size_t used = tm_ws_feed(&s->parser, data + off, len - off, &ev);
        off += used;
        switch (ev.type) {
            case TM_WS_EV_NONE:
                return;
            case TM_WS_EV_ERROR:
                drop(s, io, now, ev.error == TM_WS_ERR_TOO_BIG ? "message too big" : "websocket framing error");
                return;
            case TM_WS_EV_PING:
                if (!send_frame(s, io, TM_WS_OP_PONG, ev.data, ev.len)) drop(s, io, now, "write failed");
                break;
            case TM_WS_EV_PONG:
                break;
            case TM_WS_EV_CLOSE: {
                char why[40];
                snprintf(why, sizeof(why), "edge closed (%u)", (unsigned) ev.close_code);
                const uint8_t code[2] = {(uint8_t) (ev.close_code >> 8), (uint8_t) ev.close_code};
                send_frame(s, io, TM_WS_OP_CLOSE, code, ev.close_code == 1005 ? 0 : 2);
                drop(s, io, now, why);
                return;
            }
            case TM_WS_EV_TEXT:
                on_control(s, io, ev.data, ev.len, now);
                break;
            case TM_WS_EV_BINARY:
                if (s->state != TM_CLOUD_READY) {
                    drop(s, io, now, "packet before ready");
                    return;
                }
                if (s->on_downlink) s->on_downlink(s->cb_ctx, ev.data, ev.len);
                break;
        }
        if (used == 0 && ev.type == TM_WS_EV_NONE) return;
    }
}

static bool receive(TmCloudSession* s, const TmCloudIo* io, uint32_t now) {
    for (int rounds = 0; rounds < 8; ++rounds) {
        const int n = io->read(io->ctx, s->rx, sizeof(s->rx));
        if (n < 0) {
            drop(s, io, now, "connection closed");
            return false;
        }
        if (n == 0) return true;
        s->last_rx_ms = now;
        on_bytes(s, io, s->rx, (size_t) n, now);
        if (s->state < TM_CLOUD_UPGRADING) return false;
    }
    return true;
}

static void upgrade_step(TmCloudSession* s, const TmCloudIo* io, uint32_t now) {
    while (s->http_len < sizeof(s->http) - 1) {
        // One byte at a time until the blank line, so nothing after it is
        // swallowed: the challenge may arrive in the same TCP segment.
        uint8_t b;
        const int n = io->read(io->ctx, &b, 1);
        if (n < 0) {
            drop(s, io, now, "closed during upgrade");
            return;
        }
        if (n == 0) break;
        s->http[s->http_len++] = (char) b;
        s->http[s->http_len] = 0;
        if (s->http_len >= 4 && !memcmp(s->http + s->http_len - 4, "\r\n\r\n", 4)) {
            const int r = tm_ws_check_response(s->http, s->http_len, s->ws_key, TM_CLOUD_SUBPROTOCOL);
            if (r != TM_WS_HS_OK) {
                char why[40];
                snprintf(why, sizeof(why), "upgrade refused (%.3s)", s->http_len > 12 ? s->http + 9 : "???");
                drop(s, io, now, r == TM_WS_HS_STATUS ? why : "bad upgrade response");
                return;
            }
            s->last_rx_ms = now;
            set_state(s, TM_CLOUD_AUTHENTICATING, now);
            return;
        }
    }
    if (s->http_len >= sizeof(s->http) - 1) drop(s, io, now, "upgrade response too long");
}

static void transmit(TmCloudSession* s, const TmCloudIo* io, uint32_t now) {
    if (!s->next_uplink) return;
    for (int k = 0; k < 4; ++k) {
        uint8_t type = 0;
        const size_t n = s->next_uplink(s->cb_ctx, s->pkt, sizeof(s->pkt), &type, now);
        if (n == 0) return;
        if (!send_frame(s, io, TM_WS_OP_BINARY, s->pkt, n)) {
            drop(s, io, now, "write failed");
            return;
        }
        ++s->sent;
        if ((type == TM_TYPE_REPORT || type == TM_TYPE_STATUS) && n >= TM_HEADER_SIZE) {
            const uint16_t boot = (uint16_t) (s->pkt[10] | (s->pkt[11] << 8));
            const uint32_t seq = (uint32_t) s->pkt[12] | ((uint32_t) s->pkt[13] << 8) | ((uint32_t) s->pkt[14] << 16) |
                                 ((uint32_t) s->pkt[15] << 24);
            tm_ack_sent(&s->acks, type, boot, seq, now);
        }
    }
}

void tm_cloud_session_step(TmCloudSession* s, const TmCloudIo* io, uint32_t now, bool link_up, bool time_ok) {
    if (s->state == TM_CLOUD_PAUSED) return;
    if (!link_up) {
        if (s->state >= TM_CLOUD_CONNECTING) drop(s, io, now, "wifi lost");
        set_state(s, TM_CLOUD_OFF, now);
        return;
    }
    switch (s->state) {
        case TM_CLOUD_OFF:
        case TM_CLOUD_WAIT_TIME:
            if (!time_ok) {
                // Never connect unverified: without a clock, a certificate's
                // validity cannot be checked, so wait -- and say so.
                if (s->state != TM_CLOUD_WAIT_TIME) set_state(s, TM_CLOUD_WAIT_TIME, now);
                return;
            }
            set_state(s, TM_CLOUD_CONNECTING, now);
            break;
        case TM_CLOUD_BACKOFF:
            if ((int32_t) (now - s->next_attempt_ms) < 0) return;
            if (!time_ok) {
                set_state(s, TM_CLOUD_WAIT_TIME, now);
                return;
            }
            set_state(s, TM_CLOUD_CONNECTING, now);
            break;
        default:
            break;
    }

    if (s->state == TM_CLOUD_CONNECTING) {
        const int r = io->connect(io->ctx, s->url.host, s->url.port);
        if (r != 0) {
            drop(s, io, now, r == -2 ? "tls: certificate refused" : r == -3 ? "tls: certificate not valid now"
                                                                          : "cannot reach cloud host");
            return;
        }
        uint8_t nonce[16];
        for (int i = 0; i < 16; i += 4) {
            const uint32_t v = io->random(io->ctx);
            memcpy(nonce + i, &v, 4);
        }
        tm_base64(s->ws_key, sizeof(s->ws_key), nonce, sizeof(nonce));
        const size_t n = tm_ws_request(s->http, sizeof(s->http), s->url.host, s->url.port, s->url.path, s->ws_key,
                                       TM_CLOUD_SUBPROTOCOL);
        if (n == 0 || io->write(io->ctx, (const uint8_t*) s->http, n) != (int) n) {
            drop(s, io, now, "write failed");
            return;
        }
        s->http_len = 0;
        tm_ws_parser_reset(&s->parser);
        set_state(s, TM_CLOUD_UPGRADING, now);
    }

    if (s->state == TM_CLOUD_UPGRADING) {
        upgrade_step(s, io, now);
        if (s->state == TM_CLOUD_UPGRADING && now - s->state_since_ms > TM_CLOUD_HANDSHAKE_MS) drop(s, io, now, "upgrade timed out");
        if (s->state != TM_CLOUD_AUTHENTICATING) return;
    }

    if (s->state == TM_CLOUD_AUTHENTICATING) {
        if (!receive(s, io, now)) return;
        if (s->state == TM_CLOUD_AUTHENTICATING && now - s->state_since_ms > TM_CLOUD_HANDSHAKE_MS) {
            drop(s, io, now, s->challenge_answered ? "authentication refused or timed out" : "no challenge");
        }
        return;
    }

    if (s->state == TM_CLOUD_READY) {
        if (!receive(s, io, now)) return;
        if (s->state != TM_CLOUD_READY) return;
        transmit(s, io, now);
        if (s->state != TM_CLOUD_READY) return;
        if (now - s->last_rx_ms > TM_CLOUD_SILENCE_MS) {
            drop(s, io, now, "edge went silent");
            return;
        }
        // The socket can look open while nothing gets through (a stalled
        // proxy, a half-dead path): the ACK is the proof, so no ACK is a
        // failure even when the edge still answers pings.
        if (tm_ack_oldest_report_age(&s->acks, now) > TM_CLOUD_ACK_DEADLINE_MS) drop(s, io, now, "reports not acknowledged");
    }
}

void tm_cloud_session_pause(TmCloudSession* s, const TmCloudIo* io, bool paused, uint32_t now) {
    if (paused) {
        if (s->state == TM_CLOUD_PAUSED) return;
        if (s->state >= TM_CLOUD_CONNECTING) {
            const uint8_t code[2] = {0x03, 0xE9};   // 1001 going away
            if (s->state >= TM_CLOUD_AUTHENTICATING) send_frame(s, io, TM_WS_OP_CLOSE, code, 2);
            drop(s, io, now, NULL);
        }
        set_state(s, TM_CLOUD_PAUSED, now);
        note(s, "paused");
        return;
    }
    if (s->state != TM_CLOUD_PAUSED) return;
    s->attempt = 0;
    set_state(s, TM_CLOUD_OFF, now);
}

bool tm_cloud_session_grant(TmCloudSession* s, uint32_t seq, const char* build, uint32_t now, char token[65]) {
    if (!s->grant_valid || s->grant_seq != seq || strcmp(s->grant_build, build) != 0) return false;
    if ((int32_t) (s->grant_expires_ms - now) <= 0) {
        s->grant_valid = false;
        memset(s->grant_token, 0, sizeof(s->grant_token));
        return false;
    }
    memcpy(token, s->grant_token, 65);
    return true;
}
