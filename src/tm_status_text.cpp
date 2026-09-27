#include "tm_status_text.h"

#include <stdio.h>
#include <string.h>

bool tm_status_healthy(const TmBootStatus* st) {
    if (!st->sensor_ok || !st->wifi_joined || !st->key_set) return false;
    if (st->cloud) return st->time_ok && st->ack_age_s >= 0 && st->ack_age_s <= 10;
    return true;
}

/** Copy, cut to the screen width. Cutting is the point, so it is done by hand, not by snprintf. */
static void put(char out[TM_STATUS_COLS + 1], const char* s) {
    size_t n = strlen(s);
    if (n > TM_STATUS_COLS) n = TM_STATUS_COLS;
    memcpy(out, s, n);
    out[n] = 0;
}

/** "label   value": 8 columns of label, then the value, cut to the screen width. */
static void line(char out[TM_STATUS_COLS + 1], const char* label, const char* value) {
    char full[64];
    snprintf(full, sizeof(full), "%-8s%.50s", label, value);
    put(out, full);
}

void tm_status_render(const TmBootStatus* st, int32_t off_in_s, char lines[TM_STATUS_LINES][TM_STATUS_COLS + 1]) {
    char v[64];
    for (int i = 0; i < TM_STATUS_LINES; ++i) lines[i][0] = 0;

    // Firmware on the left, the node's label (as on its enclosure) on the right.
    char id[8] = "";
    if (st->node_id) snprintf(id, sizeof(id), "#%u", (unsigned) st->node_id);
    if (id[0]) snprintf(v, sizeof(v), "%-*.12s%s", (int) (TM_STATUS_COLS - strlen(id)), st->fw, id);
    else snprintf(v, sizeof(v), "%.12s", st->fw);
    put(lines[0], v);
    snprintf(v, sizeof(v), "%02x:%02x:%02x:%02x:%02x:%02x", st->uid[0], st->uid[1], st->uid[2], st->uid[3], st->uid[4],
             st->uid[5]);
    put(lines[1], v);

    if (!st->sensor_ok) snprintf(v, sizeof(v), "NOT FOUND");
    else if (st->have_scene) snprintf(v, sizeof(v), "OK %.0f-%.0fC", (double) st->scene_min, (double) st->scene_max);
    else snprintf(v, sizeof(v), "OK");
    line(lines[2], "Sensor", v);

    if (!st->ssid_set) snprintf(v, sizeof(v), "NO SSID SET");
    // Not joined yet: which network it is trying is the useful half (13 columns).
    else if (!st->wifi_joined) snprintf(v, sizeof(v), "no %.40s", st->ssid);
    else {
        // The RSSI is what an installer moves the node for: keep it, cut the SSID.
        char rssi[8];
        snprintf(rssi, sizeof(rssi), " %d", (int) st->rssi);
        const size_t room = TM_STATUS_COLS - 8 - strlen(rssi);
        size_t n = strlen(st->ssid);
        if (n > room) n = room;
        memcpy(v, st->ssid, n);
        memcpy(v + n, rssi, strlen(rssi) + 1);
    }
    line(lines[3], "WiFi", v);

    line(lines[4], "Key", st->key_set ? "set" : "MISSING");

    if (st->cloud) {
        line(lines[5], "Time", st->time_ok ? "synced" : "waiting");
        line(lines[6], "Cloud", st->uplink);
    } else {
        line(lines[5], "Uplink", "udp");
        line(lines[6], "Send to", st->uplink[0] ? st->uplink : "NOT SET");
    }

    if (off_in_s >= 0) {
        snprintf(v, sizeof(v), "ALL OK - off in %lds", (long) off_in_s);
        put(lines[7], v);
    } else if (st->cloud) {
        if (st->ack_age_s < 0) snprintf(v, sizeof(v), "no ACK yet");
        else snprintf(v, sizeof(v), "ACK %lds ago", (long) st->ack_age_s);
        line(lines[7], "Edge", v);
    } else {
        line(lines[7], "Edge", "(udp: no ACK)");
    }
}
