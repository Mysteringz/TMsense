#ifndef TM_STATUS_TEXT_H
#define TM_STATUS_TEXT_H

/**
 * What the OLED says during start-up, as eight 21-character lines: the
 * services a node needs before it counts anyone, and whether each works.
 * Plain C with no Arduino, so the host tests check the wording and, more
 * importantly, the rule for when the node counts as running and the screen
 * may go dark.
 */

#include <stdbool.h>
#include <stdint.h>

#define TM_STATUS_LINES 8
#define TM_STATUS_COLS 21

typedef struct {
    char fw[13];
    uint16_t node_id;          // 0 = unset
    uint8_t uid[6];
    bool sensor_ok;
    bool have_scene;
    float scene_min, scene_max;
    bool ssid_set;
    bool wifi_joined;
    char ssid[33];
    int8_t rssi;
    bool key_set;
    bool cloud;                // transport wss
    bool time_ok;              // wss only: a clock good enough to check certificates
    char uplink[48];           // wss: session state or its last error (as long as last_error); udp: first edge
    int32_t ack_age_s;         // wss: seconds since the edge acknowledged a REPORT; -1 never
} TmBootStatus;

/**
 * Healthy = every critical service works: the sensor reads, Wi-Fi is
 * joined, packets are signed, and -- over wss, where the edge can say so --
 * a REPORT was acknowledged in the last 10 s. UDP has no acknowledgement,
 * so for a udp node Wi-Fi plus the sensor is the most that can be known.
 */
bool tm_status_healthy(const TmBootStatus* st);

/**
 * Render the lines. `off_in_s` >= 0 puts "ALL OK - off in Ns" on the last
 * line; -1 shows the edge line instead.
 */
void tm_status_render(const TmBootStatus* st, int32_t off_in_s, char lines[TM_STATUS_LINES][TM_STATUS_COLS + 1]);

#endif // TM_STATUS_TEXT_H
