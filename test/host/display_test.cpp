// Host tests for the start-up display's text and its "may go dark" rule:
// src/tm_status_text.cpp compiled unchanged. Built by build_cloud_host.sh.
#include <stdio.h>
#include <string.h>
#include "tm_status_text.h"

static int g_checks = 0, g_failed = 0;
#define CHECK(cond, what)                                                          \
    do {                                                                           \
        ++g_checks;                                                                \
        if (!(cond)) {                                                             \
            ++g_failed;                                                            \
            fprintf(stderr, "FAIL %s:%d: %s  [%s]\n", __FILE__, __LINE__, what, #cond); \
        }                                                                          \
    } while (0)

static TmBootStatus good(bool cloud) {
    TmBootStatus st;
    memset(&st, 0, sizeof(st));
    strcpy(st.fw, "tmsense-1.5");
    st.node_id = 3;
    const uint8_t uid[6] = {0x30, 0xed, 0xa0, 0xcb, 0xf5, 0xf8};
    memcpy(st.uid, uid, 6);
    st.sensor_ok = st.have_scene = true;
    st.scene_min = 23.4f;
    st.scene_max = 29.3f;
    st.ssid_set = st.wifi_joined = st.key_set = true;
    strcpy(st.ssid, "EsanHouse");
    st.rssi = -64;
    st.cloud = cloud;
    st.time_ok = cloud;
    strcpy(st.uplink, cloud ? "ready" : "192.168.0.43");
    st.ack_age_s = cloud ? 2 : -1;
    return st;
}

static bool has(const char lines[TM_STATUS_LINES][TM_STATUS_COLS + 1], const char* s) {
    for (int i = 0; i < TM_STATUS_LINES; ++i) if (strstr(lines[i], s)) return true;
    return false;
}

int main() {
    char l[TM_STATUS_LINES][TM_STATUS_COLS + 1];

    // When the node counts as running (and the screen may go dark).
    TmBootStatus st = good(false);
    CHECK(tm_status_healthy(&st), "udp: sensor, Wi-Fi and key are all a udp node can prove");
    st.key_set = false;
    CHECK(!tm_status_healthy(&st), "no key: the edge would refuse everything, so not running");
    st = good(false);
    st.sensor_ok = false;
    CHECK(!tm_status_healthy(&st), "no sensor: not running");
    st = good(false);
    st.wifi_joined = false;
    CHECK(!tm_status_healthy(&st), "no Wi-Fi: not running");
    st = good(true);
    CHECK(tm_status_healthy(&st), "wss: a fresh ACK");
    st.ack_age_s = -1;
    CHECK(!tm_status_healthy(&st), "wss: connected is not enough; it needs an ACK");
    st.ack_age_s = 11;
    CHECK(!tm_status_healthy(&st), "wss: an old ACK is not a working uplink");
    st = good(true);
    st.time_ok = false;
    CHECK(!tm_status_healthy(&st), "wss: no clock, no verified TLS");

    // What it says.
    st = good(true);
    tm_status_render(&st, -1, l);
    for (int i = 0; i < TM_STATUS_LINES; ++i) CHECK(strlen(l[i]) <= TM_STATUS_COLS, "every line fits the 21-column screen");
    CHECK(!strcmp(l[0], "tmsense-1.5        #3"), "firmware left, node label right");
    CHECK(!strcmp(l[1], "30:ed:a0:cb:f5:f8"), "the uid");
    CHECK(!strcmp(l[2], "Sensor  OK 23-29C"), "sensor with its scene range");
    CHECK(!strcmp(l[3], "WiFi    EsanHouse -64"), "Wi-Fi with signal");
    CHECK(!strcmp(l[7], "Edge    ACK 2s ago"), "the edge's acknowledgement");
    tm_status_render(&st, 7, l);
    CHECK(!strcmp(l[7], "ALL OK - off in 7s"), "the countdown before the screen sleeps");

    st.sensor_ok = false;
    st.key_set = false;
    st.wifi_joined = false;
    strcpy(st.uplink, "tls: certificate refused");
    st.ack_age_s = -1;
    tm_status_render(&st, -1, l);
    CHECK(has(l, "NOT FOUND") && has(l, "MISSING") && !strcmp(l[3], "WiFi    no EsanHouse") && has(l, "no ACK yet"),
          "every failure is spelt out");
    CHECK(!strcmp(l[6], "Cloud   tls: certific"), "the cloud error, cut to the width");

    st = good(false);
    strcpy(st.ssid, "A-very-long-campus-network-name");
    tm_status_render(&st, -1, l);
    CHECK(!strcmp(l[3], "WiFi    A-very-lo -64"), "a long SSID is cut, the signal kept");
    CHECK(!strcmp(l[6], "Send to 192.168.0.43") && !strcmp(l[7], "Edge    (udp: no ACK)"), "udp lines");
    st.ssid_set = false;
    st.node_id = 0;
    tm_status_render(&st, -1, l);
    CHECK(has(l, "NO SSID SET") && !strcmp(l[0], "tmsense-1.5"), "unprovisioned node");

    printf("display_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
