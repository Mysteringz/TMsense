/**
 * TMnode: thermal occupancy node.
 *
 * Reads the MLX90640, finds people as warm blobs, and sends a small signed
 * REPORT per frame to TMedge (plus a RAW frame while commissioning, and a
 * STATUS every 10 s). Takes signed commands from TMedge. Everything a person
 * configures is in flash, set over USB serial -- see `help` on the console.
 */
#include <Arduino.h>
#include <esp_system.h>
#include "tm_config.h"
#include "tm_detector.h"
#include "tm_packet.h"
#include "tm_protocol.h"
#include "tm_sensor.h"
#include "tm_ota.h"
#include "tm_settings.h"
#include "tm_transport.h"

static TmDetector s_detector;
static TmPacketContext s_ctx;
static float s_frame[TM_GRID_SIZE];
static uint8_t s_packet[TM_PACKET_MAX_SIZE];
static uint8_t s_rx[TM_HEADER_SIZE + TM_OTA_SIZE + TM_TAG_SIZE];

static bool s_sensor_ok = false;
static uint32_t s_sensor_retry_ms = 0;
static uint8_t s_consecutive_errors = 0;
static uint16_t s_sensor_errors = 0;
static uint32_t s_frames = 0;
static uint32_t s_last_frame_ms = 0;
static float s_fps = 0.0f;
static uint32_t s_last_status_ms = 0;
static uint32_t s_identify_until_ms = 0;
static uint8_t s_last_logged_count = 255;
/** Set whenever a packet reached an edge; read once a second by the OTA probation check. */
static bool s_uplink_ok = false;
static uint32_t s_last_second_ms = 0;

/** True once per second, for work that does not belong in the frame loop. */
static bool now_ms_second_tick() {
    const uint32_t now = millis();
    if (now - s_last_second_ms < 1000) return false;
    s_last_second_ms = now;
    return true;
}
static uint32_t s_last_log_ms = 0;
static uint32_t s_last_poll_ms = 0;

static void apply_params() {
    TmDetectorParams p;
    tm_settings_to_detector(&p);
    // Thresholds take effect at once; the learned background is kept, so
    // tuning from the console does not blind the node for 20 s every time.
    s_detector.params = p;
}

static void load_key() {
    s_ctx.key_len = strlen(g_settings.key);
    memcpy(s_ctx.key, g_settings.key, s_ctx.key_len);
}

static void start_sensor() {
    s_sensor_ok = tm_sensor_begin(TM_PIN_SDA, TM_PIN_SCL, TM_I2C_HZ,
                                  (uint8_t) g_settings.params[TM_PARAM_REFRESH]);
    s_consecutive_errors = 0;
    Serial.printf("[sensor] MLX90640 %s\n", s_sensor_ok ? "ready" : "NOT FOUND (check SDA 41 / SCL 42)");
}

/** OTA progress goes out on the same signed uplink as everything else. */
static void ota_report(const TmOtaStatus* st) {
    const size_t n = tm_build_ota_status(s_packet, &s_ctx, millis(), st);
    if (tm_transport_send(s_packet, n, true) > 0) s_uplink_ok = true;
}

static void handle_ota(const TmOtaRequest& req) {
    // The same replay counter as commands: an old update request, replayed
    // even after a reboot, does nothing.
    if (req.seq <= g_settings.last_cmd) {
        Serial.printf("[ota] stale request %lu ignored (last %lu)\n",
                      (unsigned long) req.seq, (unsigned long) g_settings.last_cmd);
        return;
    }
    g_settings.last_cmd = req.seq;
    tm_settings_persist_last_cmd();
    uint8_t gw[4];
    tm_transport_remote(gw);
    Serial.printf("[ota] update requested from %u.%u.%u.%u%s\n", gw[0], gw[1], gw[2], gw[3], req.path);
    tm_ota_begin(&req, gw);
}

static void send_status() {
    TmStatus st;
    memset(&st, 0, sizeof(st));
    strncpy(st.fw_version, TM_FW_VERSION, sizeof(st.fw_version));
    tm_transport_ip(st.ip);
    st.rssi = tm_transport_rssi();
    st.channel = tm_transport_channel();
    st.free_heap = ESP.getFreeHeap();
    st.min_heap = ESP.getMinFreeHeap();
    st.stack_free = (uint16_t) uxTaskGetStackHighWaterMark(NULL);
    st.wifi_drops = tm_transport_drops();
    st.sensor_errors = s_sensor_errors;
    st.frames = s_frames;
    st.fps_x100 = (uint16_t) lroundf(s_fps * 100.0f);
    st.vdd_x100 = (uint16_t) lroundf((isfinite(tm_sensor_vdd()) ? tm_sensor_vdd() : 0.0f) * 100.0f);
    st.ta = isfinite(tm_sensor_ta()) ? tm_sensor_ta() : 0.0f;
    st.last_cmd = g_settings.last_cmd;
    st.flags = (s_sensor_ok ? TM_STATUS_SENSOR_OK : 0) |
               (s_detector.background_ready ? TM_STATUS_BACKGROUND_READY : 0) |
               (s_ctx.key_len > 0 ? TM_STATUS_SIGNED : 0);
    const size_t n = tm_build_status(s_packet, &s_ctx, millis(), &st, g_settings.params, TM_PARAM_COUNT);
    if (tm_transport_send(s_packet, n, true) > 0) s_uplink_ok = true;
    s_last_status_ms = millis();
}

static void handle_command(const TmCommand& cmd) {
    if (cmd.seq <= g_settings.last_cmd) {
        Serial.printf("[cmd] stale command %lu ignored (last %lu)\n",
                      (unsigned long) cmd.seq, (unsigned long) g_settings.last_cmd);
        return;
    }
    g_settings.last_cmd = cmd.seq;
    tm_settings_persist_last_cmd();
    switch (cmd.opcode) {
        case TM_CMD_SET_PARAM: {
            const bool ok = tm_settings_set_param(cmd.arg0, cmd.value);
            Serial.printf("[cmd] %s = %ld %s\n", tm_param_name(cmd.arg0), (long) cmd.value, ok ? "" : "(REJECTED)");
            if (ok) {
                apply_params();
                if (cmd.arg0 == TM_PARAM_REFRESH) tm_sensor_set_refresh((uint8_t) cmd.value);
            }
            break;
        }
        case TM_CMD_RESET_BACKGROUND:
            Serial.println("[cmd] relearning background");
            tm_detector_reset_background(&s_detector);
            break;
        case TM_CMD_IDENTIFY:
            s_identify_until_ms = millis() + (uint32_t) constrain(cmd.value, 1, 120) * 1000u;
            Serial.println("[cmd] identify");
            break;
        case TM_CMD_SAVE_PARAMS:
            Serial.printf("[cmd] save %s\n", tm_settings_save() ? "ok" : "FAILED");
            break;
        case TM_CMD_REBOOT:
            Serial.println("[cmd] reboot");
            send_status();
            delay(100);
            ESP.restart();
            break;
        default:
            Serial.printf("[cmd] unknown opcode %u\n", (unsigned) cmd.opcode);
    }
    send_status();   // acknowledges: carries last_cmd and the params now in force
}

static void process_frame() {
    const uint32_t now = millis();
    if (s_last_frame_ms != 0) {
        const float inst = 1000.0f / (float) (now - s_last_frame_ms > 0 ? now - s_last_frame_ms : 1);
        s_fps = s_fps == 0.0f ? inst : 0.9f * s_fps + 0.1f * inst;
    }
    s_last_frame_ms = now;
    const uint32_t frame_no = s_frames++;

    const uint8_t count = tm_detector_step(&s_detector, s_frame);

    TmReportInfo info;
    info.frame = frame_no;
    info.ta = isfinite(tm_sensor_ta()) ? tm_sensor_ta() : 0.0f;
    info.scene_min = 1e9f;
    info.scene_max = -1e9f;
    for (int i = 0; i < TM_GRID_SIZE; ++i) {
        if (s_frame[i] < info.scene_min) info.scene_min = s_frame[i];
        if (s_frame[i] > info.scene_max) info.scene_max = s_frame[i];
    }
    info.bg_mean = s_detector.background_ready ? tm_detector_background_mean(&s_detector) : 0.0f;
    info.flags = (s_detector.background_ready ? TM_REPORT_BACKGROUND_READY : 0) |
                 (s_detector.global_shift ? TM_REPORT_GLOBAL_SHIFT : 0) |
                 (s_detector.truncated ? TM_REPORT_TRUNCATED : 0);

    size_t n = tm_build_report(s_packet, &s_ctx, now, &info, s_detector.detections, count);
    tm_transport_send(s_packet, n, true);

    const int32_t raw_every = g_settings.params[TM_PARAM_RAW_EVERY];
    if (raw_every > 0 && frame_no % (uint32_t) raw_every == 0) {
        n = tm_build_raw(s_packet, &s_ctx, now, frame_no, s_frame);
        tm_transport_send(s_packet, n, false);
    }

    // A line when the count changes, and a heartbeat every 30 s: enough to
    // watch a node on the bench without a line per second burying everything.
    if (count != s_last_logged_count || now - s_last_log_ms > 30000) {
        s_last_logged_count = count;
        s_last_log_ms = now;
        if (!s_detector.background_ready) {
            Serial.printf("[frame %lu] learning background %u/%ld  scene %.1f..%.1f C\n", (unsigned long) frame_no,
                          (unsigned) s_detector.frames_learned, (long) g_settings.params[TM_PARAM_BG_FRAMES],
                          (double) info.scene_min, (double) info.scene_max);
        } else {
            Serial.printf("[frame %lu] people=%u scene %.1f..%.1f C fps=%.2f%s\n", (unsigned long) frame_no,
                          (unsigned) count, (double) info.scene_min, (double) info.scene_max, (double) s_fps,
                          s_detector.global_shift ? " GLOBAL-SHIFT" : "");
            for (uint8_t i = 0; i < count; ++i) {
                const TmDetection* d = &s_detector.detections[i];
                Serial.printf("   at (%.1f, %.1f) area=%u +%.1f C heat=%.1f\n", (double) d->x, (double) d->y,
                              (unsigned) d->area, (double) d->contrast, (double) d->heat);
            }
        }
    }
}

void setup() {
    Serial.begin(TM_SERIAL_BAUD);
    delay(200);
    pinMode(TM_PIN_LED, OUTPUT);
    digitalWrite(TM_PIN_LED, LOW);

    tm_settings_begin();
    memset(&s_ctx, 0, sizeof(s_ctx));
    tm_transport_uid(s_ctx.uid);
    s_ctx.boot = g_settings.boot;
    load_key();

    TmDetectorParams params;
    tm_settings_to_detector(&params);
    tm_detector_init(&s_detector, &params);

    // The banner answers what a person at a misbehaving node asks first:
    // which node is this, is it signing, where is it sending. Never the key.
    Serial.printf("\n=== TMsense %s ===\n", TM_FW_VERSION);
    Serial.printf("[boot] uid     %02x:%02x:%02x:%02x:%02x:%02x   boot #%u\n", s_ctx.uid[0], s_ctx.uid[1],
                  s_ctx.uid[2], s_ctx.uid[3], s_ctx.uid[4], s_ctx.uid[5], (unsigned) s_ctx.boot);
    if (g_settings.node_id) Serial.printf("[boot] node id %u\n", (unsigned) g_settings.node_id);
    Serial.printf("[boot] signing %s\n", s_ctx.key_len ? "on" : "OFF (no key; `set key ...`)");
    Serial.printf("[boot] edges   %s %s -> udp/%d\n", g_settings.edges[0][0] ? g_settings.edges[0] : "(none)",
                  g_settings.edges[1], TM_UPLINK_PORT);
    Serial.println("[boot] type `help` for the console");

    start_sensor();
    tm_transport_begin();
    // After a flash this decides whether the new image keeps its place.
    tm_ota_init(ota_report);
}

void loop() {
    const uint8_t actions = tm_console_poll();
    if (actions & TM_CONSOLE_REBOOT) ESP.restart();
    if (actions & TM_CONSOLE_PARAMS_CHANGED) {
        apply_params();
        tm_sensor_set_refresh((uint8_t) g_settings.params[TM_PARAM_REFRESH]);
    }
    if (actions & TM_CONSOLE_RESET_BG) tm_detector_reset_background(&s_detector);
    if (actions & TM_CONSOLE_NETWORK_CHANGED) {
        load_key();
        tm_transport_restart();
    }

    tm_transport_update();

    const size_t rx = tm_transport_receive(s_rx, sizeof(s_rx));
    if (rx > 0) {
        if (rx > 3 && s_rx[3] == TM_TYPE_OTA) {
            TmOtaRequest req;
            const int r = tm_parse_ota(s_rx, rx, &s_ctx, &req);
            if (r == TM_PARSE_OK) handle_ota(req);
            else Serial.printf("[ota] rejected datagram (%d)\n", r);
        } else {
            TmCommand cmd;
            const int r = tm_parse_command(s_rx, rx, &s_ctx, &cmd);
            if (r == TM_PARSE_OK) handle_command(cmd);
            else Serial.printf("[cmd] rejected datagram (%d)\n", r);
        }
    }

    // Fetching an image blocks for a few seconds; the sensor waits.
    if (tm_ota_busy()) {
        tm_ota_update();
        return;
    }
    if (now_ms_second_tick()) {
        tm_ota_health(tm_transport_connected(), s_sensor_ok, s_uplink_ok);
        s_uplink_ok = false;
    }

    const uint32_t now = millis();
    if (!s_sensor_ok) {
        if (now - s_sensor_retry_ms > 5000) {
            s_sensor_retry_ms = now;
            start_sensor();
        }
    } else if (now - s_last_poll_ms >= 20) {
        // A subpage takes 250-500 ms; asking every 20 ms costs ~50 I2C reads a
        // second instead of ~1000, and adds at most 20 ms of latency.
        s_last_poll_ms = now;
        const int r = tm_sensor_poll(s_frame);
        if (r == 1) {
            s_consecutive_errors = 0;
            process_frame();
        } else if (r < 0) {
            ++s_sensor_errors;
            // Several failures in a row: the sensor went away. Re-init it, so
            // its calibration is re-read when it comes back.
            if (++s_consecutive_errors >= 5) {
                Serial.printf("[sensor] %u read errors in a row, re-initialising\n", (unsigned) s_consecutive_errors);
                s_sensor_ok = false;
                s_sensor_retry_ms = now;
            }
        }
    }

    if (now - s_last_status_ms > TM_STATUS_INTERVAL_MS) send_status();

    if (s_identify_until_ms) {
        if ((int32_t) (now - s_identify_until_ms) >= 0) {
            s_identify_until_ms = 0;
            digitalWrite(TM_PIN_LED, LOW);
        } else {
            digitalWrite(TM_PIN_LED, (now / 150) % 2 ? HIGH : LOW);
        }
    }

    delay(1);   // yield: the sensor poll is non-blocking, don't spin the core
}
