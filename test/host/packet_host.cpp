// Emits packets built by src/tm_packet.cpp (unchanged) for TMedge's
// cross-check, and parses commands TMedge builds.
//
//   packet_host emit              one JSON line per packet: name, hex, inputs
//   packet_host parse <hex>       JSON result of tm_parse_command
//
// Key "crosscheck-key", uid 01:02:03:04:05:06, boot 7, first seq 100.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tm_packet.h"

static void hex_line(const char* name, const uint8_t* p, size_t n, const char* inputs) {
    printf("{\"name\":\"%s\",\"inputs\":%s,\"hex\":\"", name, inputs);
    for (size_t i = 0; i < n; ++i) printf("%02x", p[i]);
    printf("\"}\n");
}

static void context(TmPacketContext* ctx) {
    memset(ctx, 0, sizeof(*ctx));
    const uint8_t uid[6] = {1, 2, 3, 4, 5, 6};
    memcpy(ctx->uid, uid, 6);
    ctx->boot = 7;
    ctx->seq = 100;
    ctx->key_len = strlen("crosscheck-key");
    memcpy(ctx->key, "crosscheck-key", ctx->key_len);
}

int main(int argc, char** argv) {
    static uint8_t buf[TM_PACKET_MAX_SIZE];
    TmPacketContext ctx;
    context(&ctx);

    if (argc >= 3 && !strcmp(argv[1], "parse")) {
        const char* hex = argv[2];
        size_t n = strlen(hex) / 2;
        for (size_t i = 0; i < n; ++i) sscanf(hex + 2 * i, "%2hhx", &buf[i]);
        TmCommand cmd;
        memset(&cmd, 0, sizeof(cmd));
        const int r = tm_parse_command(buf, n, &ctx, &cmd);
        printf("{\"result\":%d,\"seq\":%u,\"opcode\":%u,\"arg0\":%u,\"value\":%d}\n", r, (unsigned) cmd.seq,
               (unsigned) cmd.opcode, (unsigned) cmd.arg0, (int) cmd.value);
        return 0;
    }

    TmReportInfo info = {4242, 35.81f, 21.04f, 33.87f, 23.5f, TM_REPORT_BACKGROUND_READY};
    TmDetection dets[30];
    for (int i = 0; i < 30; ++i) {
        dets[i].x = 1.5f + i;          // 1.5, 2.5, ...
        dets[i].y = 3.25f + (i % 20);
        dets[i].area = (uint16_t) (4 + i);
        dets[i].contrast = 2.35f + 0.1f * i;
        dets[i].peak = 30.5f + 0.25f * (i % 8);
        dets[i].heat = 12.3f + i;
    }
    size_t n = tm_build_report(buf, &ctx, 123456, &info, dets, 3);
    hex_line("report3", buf, n, "{\"seq\":100,\"uptime\":123456,\"frame\":4242,\"count\":3}");

    n = tm_build_report(buf, &ctx, 123457, &info, dets, 30);
    hex_line("report_truncated", buf, n, "{\"seq\":101,\"count\":24,\"truncated\":true}");

    info.flags = 0;
    n = tm_build_report(buf, &ctx, 123458, &info, dets, 0);
    hex_line("report_empty", buf, n, "{\"seq\":102,\"count\":0}");

    float temps[TM_GRID_SIZE];
    for (int i = 0; i < TM_GRID_SIZE; ++i) temps[i] = 20.0f + (i % 32) * 0.3f + (i / 32) * 0.05f;
    n = tm_build_raw(buf, &ctx, 123459, 4242, temps);
    hex_line("raw", buf, n, "{\"seq\":103,\"frame\":4242,\"t0\":20.0,\"t_last\":30.45}");

    TmStatus st;
    memset(&st, 0, sizeof(st));
    strncpy(st.fw_version, "tmnode-9.9.9", sizeof(st.fw_version));
    st.ip[0] = 192; st.ip[1] = 168; st.ip[2] = 0; st.ip[3] = 9;
    st.rssi = -57; st.channel = 10; st.free_heap = 238676; st.min_heap = 200000; st.stack_free = 5728;
    st.wifi_drops = 2; st.sensor_errors = 1; st.frames = 99999; st.fps_x100 = 98; st.vdd_x100 = 332;
    st.ta = 35.81f; st.last_cmd = 1789754860u; st.flags = TM_STATUS_SENSOR_OK | TM_STATUS_SIGNED;
    const int32_t params[TM_PARAM_COUNT] = {60, 120, 40, 1, 60, 90, 20, 1, 2, 19};
    n = tm_build_status(buf, &ctx, 123460, &st, params, TM_PARAM_COUNT);
    hex_line("status", buf, n, "{\"seq\":104}");

    ctx.key_len = 0;
    n = tm_build_report(buf, &ctx, 1, &info, dets, 1);
    hex_line("report_unsigned", buf, n, "{\"seq\":105}");
    return 0;
}
