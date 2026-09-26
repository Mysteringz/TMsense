// A node on a desktop: the firmware's own tm_cloud_session.cpp (and tm_ws,
// tm_cloud_proto, tm_packet) over a plain TCP socket, driven line by line on
// stdin, reporting JSON lines on stdout. TMedge's `npm run crosscheck` runs it
// against the real edge listener, so the firmware and the edge are checked
// against each other rather than each against its own idea of the protocol.
//
//   cloud_host ws://127.0.0.1:<port>/tmnode
//   stdin:  status | report <n-detections> | raw | replay | quit
//   stdout: {"event":"state",...} {"event":"ack",...} {"event":"downlink",...} {"event":"sent",...}
//
// Identity as packet_host: uid 01:02:03:04:05:06, key "crosscheck-key", boot 7.
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include "tm_cloud_session.h"
#include "tm_packet.h"

static int g_fd = -1;
static TmPacketQueue g_queue;
static TmPacketContext g_ctx;
static uint8_t g_last[TM_PACKET_MAX_SIZE];
static size_t g_last_len = 0;
static TmCloudSession g_s;

static uint32_t now_ms() {
    static struct timeval t0;
    struct timeval t;
    gettimeofday(&t, NULL);
    if (!t0.tv_sec) t0 = t;
    return (uint32_t) ((t.tv_sec - t0.tv_sec) * 1000 + (t.tv_usec - t0.tv_usec) / 1000);
}

static void hex(const uint8_t* d, size_t n) {
    for (size_t i = 0; i < n; ++i) printf("%02x", d[i]);
}

static int io_connect(void*, const char* host, uint16_t port) {
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char p[8];
    snprintf(p, sizeof(p), "%u", (unsigned) port);
    if (getaddrinfo(host, p, &hints, &res) != 0) return -1;
    g_fd = socket(AF_INET, SOCK_STREAM, 0);
    const int ok = connect(g_fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (ok != 0) {
        close(g_fd);
        g_fd = -1;
        return -1;
    }
    int one = 1;
    setsockopt(g_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    fcntl(g_fd, F_SETFL, fcntl(g_fd, F_GETFL) | O_NONBLOCK);
    return 0;
}

static int io_write(void*, const uint8_t* d, size_t n) {
    size_t done = 0;
    while (done < n) {
        const ssize_t w = send(g_fd, d + done, n - done, 0);
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            usleep(1000);
            continue;
        }
        if (w <= 0) return -1;
        done += (size_t) w;
    }
    return (int) n;
}

static int io_read(void*, uint8_t* b, size_t max) {
    if (g_fd < 0) return -1;
    const ssize_t r = recv(g_fd, b, max, 0);
    if (r > 0) return (int) r;
    if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
    return -1;
}

static void io_close(void*) {
    if (g_fd >= 0) close(g_fd);
    g_fd = -1;
}

static uint32_t io_random(void*) { return (uint32_t) random() ^ ((uint32_t) random() << 16); }

static size_t cb_next(void*, uint8_t* buf, size_t max, uint8_t* type, uint32_t now) {
    return tm_queue_pop(&g_queue, buf, max, type, now);
}
static void cb_discard(void*) { tm_queue_clear(&g_queue); }

/** Exactly what main.cpp does with a downlink: parse it as the firmware would. */
static void cb_down(void*, const uint8_t* d, size_t n) {
    printf("{\"event\":\"downlink\",\"hex\":\"");
    hex(d, n);
    printf("\"");
    if (n > 3 && d[3] == TM_TYPE_OTA) {
        TmOtaRequest req;
        memset(&req, 0, sizeof(req));
        const int r = tm_parse_ota(d, n, &g_ctx, &req);
        char build[17] = {0};
        if (strlen(req.path) == 24) memcpy(build, req.path + 4, 16);
        char token[65];
        const bool granted = r == TM_PARSE_OK && tm_cloud_session_grant(&g_s, req.seq, build, now_ms(), token);
        printf(",\"kind\":\"ota\",\"result\":%d,\"seq\":%u,\"port\":%u,\"path\":\"%s\",\"granted\":%s", r,
               (unsigned) req.seq, (unsigned) req.port, req.path, granted ? "true" : "false");
        if (granted) printf(",\"tokenLength\":%zu", strlen(token));
    } else {
        TmCommand cmd;
        memset(&cmd, 0, sizeof(cmd));
        const int r = tm_parse_command(d, n, &g_ctx, &cmd);
        printf(",\"kind\":\"command\",\"result\":%d,\"seq\":%u,\"opcode\":%u,\"arg0\":%u,\"value\":%d", r, (unsigned) cmd.seq,
               (unsigned) cmd.opcode, (unsigned) cmd.arg0, (int) cmd.value);
    }
    printf("}\n");
    fflush(stdout);
}

static void enqueue(const uint8_t* p, size_t n, const char* name) {
    memcpy(g_last, p, n);
    g_last_len = n;
    const bool ok = tm_queue_push(&g_queue, p, n, now_ms());
    printf("{\"event\":\"sent\",\"name\":\"%s\",\"queued\":%s,\"hex\":\"", name, ok ? "true" : "false");
    hex(p, n);
    printf("\"}\n");
    fflush(stdout);
}

static void command(char* line) {
    static uint8_t pkt[TM_PACKET_MAX_SIZE];
    char* cmd = strtok(line, " \n");
    if (!cmd) return;
    if (!strcmp(cmd, "quit")) exit(0);
    if (!strcmp(cmd, "status")) {
        TmStatus st;
        memset(&st, 0, sizeof(st));
        strncpy(st.fw_version, "tmsense-host", sizeof(st.fw_version));
        st.frames = 1;
        st.fps_x100 = 100;
        st.flags = TM_STATUS_SENSOR_OK | TM_STATUS_SIGNED;
        int32_t params[TM_PARAM_COUNT] = {60, 120, 40, 1, 60, 90, 20, 1, 2, 19};
        enqueue(pkt, tm_build_status(pkt, &g_ctx, now_ms(), &st, params, TM_PARAM_COUNT), "status");
    } else if (!strcmp(cmd, "report")) {
        const char* a = strtok(NULL, " \n");
        const int n = a ? atoi(a) : 0;
        TmDetection dets[TM_MAX_DETECTIONS];
        for (int i = 0; i < n && i < TM_MAX_DETECTIONS; ++i) {
            dets[i].x = 3.5f + (float) i;
            dets[i].y = 7.25f;
            dets[i].area = (uint16_t) (6 + i);
            dets[i].contrast = 2.5f;
            dets[i].peak = 31.0f;
            dets[i].heat = 40.0f + (float) i;
        }
        TmReportInfo info = {42, 30.5f, 21.0f, 33.0f, 22.5f, TM_REPORT_BACKGROUND_READY};
        enqueue(pkt, tm_build_report(pkt, &g_ctx, now_ms(), &info, dets, (uint8_t) n), "report");
    } else if (!strcmp(cmd, "raw")) {
        static float temps[TM_GRID_SIZE];
        for (int i = 0; i < TM_GRID_SIZE; ++i) temps[i] = 20.0f + (float) (i % 32) * 0.3f;
        enqueue(pkt, tm_build_raw(pkt, &g_ctx, now_ms(), 42, temps), "raw");
    } else if (!strcmp(cmd, "replay")) {
        if (g_last_len) tm_queue_push(&g_queue, g_last, g_last_len, now_ms());
        printf("{\"event\":\"sent\",\"name\":\"replay\",\"queued\":true}\n");
        fflush(stdout);
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: cloud_host ws://host:port/path\n");
        return 2;
    }
    memset(&g_ctx, 0, sizeof(g_ctx));
    const uint8_t uid[6] = {1, 2, 3, 4, 5, 6};
    memcpy(g_ctx.uid, uid, 6);
    g_ctx.boot = 7;
    g_ctx.seq = 100;
    g_ctx.key_len = 14;
    memcpy(g_ctx.key, "crosscheck-key", 14);

    TmCloudUrl url;
    const int r = tm_cloud_parse_url(argv[1], true, &url);
    if (r != TM_URL_OK) {
        fprintf(stderr, "%s\n", tm_cloud_url_error(r));
        return 2;
    }
    char uid_s[18];
    tm_cloud_uid_string(uid, uid_s);
    memset(&g_s, 0, sizeof(g_s));
    g_s.next_uplink = cb_next;
    g_s.discard_uplink = cb_discard;
    g_s.on_downlink = cb_down;
    tm_cloud_session_init(&g_s, &url, uid_s, g_ctx.key, g_ctx.key_len, g_ctx.boot);
    tm_queue_clear(&g_queue);
    const TmCloudIo io = {io_connect, io_write, io_read, io_close, io_random, NULL};

    TmCloudState last = (TmCloudState) -1;
    uint32_t acked = 0;
    char line[256];
    size_t ll = 0;
    srandom((unsigned) (time(NULL) ^ getpid()));
    fcntl(0, F_SETFL, fcntl(0, F_GETFL) | O_NONBLOCK);
    for (;;) {
        tm_cloud_session_step(&g_s, &io, now_ms(), true, true);
        if (g_s.state != last) {
            last = g_s.state;
            printf("{\"event\":\"state\",\"state\":\"%s\",\"error\":\"%s\",\"session\":\"%s\"}\n", tm_cloud_state_name(g_s.state),
                   g_s.last_error, g_s.session);
            fflush(stdout);
        }
        if (g_s.reports_acked != acked) {
            acked = g_s.reports_acked;
            printf("{\"event\":\"ack\",\"reportsAcked\":%u,\"ignored\":%u}\n", (unsigned) acked, (unsigned) g_s.ignored_acks);
            fflush(stdout);
        }
        char c;
        ssize_t got;
        while ((got = read(0, &c, 1)) == 1) {
            if (c == '\n' || ll == sizeof(line) - 1) {
                line[ll] = 0;
                ll = 0;
                command(line);
            } else {
                line[ll++] = c;
            }
        }
        if (got == 0) return 0;   // the crosscheck went away: so do we
        usleep(2000);
    }
}
