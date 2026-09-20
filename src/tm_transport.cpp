#include "tm_transport.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <string.h>
#include <esp_mac.h>
#include "tm_config.h"
#include "tm_settings.h"

static WiFiUDP s_up;
static WiFiUDP s_down;
static IPAddress s_edges[TM_MAX_EDGES];
static int s_edge_count = 0;
static bool s_was_connected = false;
static bool s_down_open = false;
static uint32_t s_last_attempt_ms = 0;
static uint16_t s_drops = 0;

static void load_edges() {
    s_edge_count = 0;
    for (int i = 0; i < TM_MAX_EDGES; ++i) {
        if (!g_settings.edges[i][0]) continue;
        IPAddress ip;
        if (ip.fromString(g_settings.edges[i])) {
            s_edges[s_edge_count++] = ip;
        } else {
            // A typo here otherwise sends every packet nowhere, silently.
            Serial.printf("[wifi] edge \"%s\" is not an IPv4 address, ignored\n", g_settings.edges[i]);
        }
    }
}

void tm_transport_uid(uint8_t out[6]) { esp_read_mac(out, ESP_MAC_WIFI_STA); }

static bool lora_mode() { return g_settings.mode == TM_MODE_LORA; }

void tm_transport_begin() {
    load_edges();
    if (lora_mode()) {
        // The LoRa uplink (to TMLAccess) is not written yet. Say so plainly
        // rather than quietly falling back to Wi-Fi: a node set to LoRa is
        // presumably somewhere Wi-Fi is not wanted.
        WiFi.mode(WIFI_OFF);
        Serial.printf("[lora] mode is lora (TMLAccess %s), but this firmware has no LoRa uplink yet: "
                      "nothing will be sent. `set mode wifi` to use Wi-Fi.\n",
                      g_settings.lora_gw[0] ? g_settings.lora_gw : "(none)");
        return;
    }
    WiFi.mode(WIFI_STA);
    // Modem sleep saves little on a USB-powered node and makes downlink
    // commands arrive late or not at all.
    WiFi.setSleep(false);
    WiFi.setAutoReconnect(true);
    if (!g_settings.ssid[0]) {
        Serial.println("[wifi] no SSID set - `set ssid <name>`, `set pass <pw>`, `save`");
        return;
    }
    WiFi.begin(g_settings.ssid, g_settings.password);
    s_last_attempt_ms = millis();
    Serial.printf("[wifi] connecting to \"%s\"\n", g_settings.ssid);
    if (s_edge_count == 0) Serial.println("[wifi] no edge set - `set edges <ip>`");
}

void tm_transport_restart() {
    s_down.stop();
    s_down_open = false;
    s_was_connected = false;
    WiFi.disconnect(true);
    delay(50);
    tm_transport_begin();
}

void tm_transport_update() {
    if (lora_mode() || !g_settings.ssid[0]) return;
    if (WiFi.status() == WL_CONNECTED) {
        if (!s_was_connected) {
            s_was_connected = true;
            s_down_open = s_down.begin(TM_DOWNLINK_PORT) == 1;
            Serial.printf("[wifi] connected ip=%s rssi=%d dBm ch=%d, commands on udp/%d%s\n",
                          WiFi.localIP().toString().c_str(), (int) WiFi.RSSI(), (int) WiFi.channel(),
                          TM_DOWNLINK_PORT, s_down_open ? "" : " (FAILED to open)");
        }
        return;
    }
    if (s_was_connected) {
        s_was_connected = false;
        s_down.stop();
        s_down_open = false;
        ++s_drops;
        Serial.println("[wifi] connection lost");
    }
    const uint32_t now = millis();
    if (now - s_last_attempt_ms < TM_WIFI_RETRY_MS) return;
    s_last_attempt_ms = now;
    WiFi.disconnect();
    WiFi.begin(g_settings.ssid, g_settings.password);
}

bool tm_transport_connected() { return !lora_mode() && WiFi.status() == WL_CONNECTED; }

int tm_transport_send(const uint8_t* data, size_t len, bool all_edges) {
    if (!tm_transport_connected() || s_edge_count == 0) return 0;
    int reached = 0;
    for (int i = 0; i < (all_edges ? s_edge_count : 1); ++i) {
        if (!s_up.beginPacket(s_edges[i], TM_UPLINK_PORT)) continue;
        s_up.write(data, len);
        if (s_up.endPacket() == 1) ++reached;
    }
    return reached;
}

static uint8_t s_remote[4] = {0, 0, 0, 0};

size_t tm_transport_receive(uint8_t* buf, size_t max) {
    if (!s_down_open) return 0;
    const int n = s_down.parsePacket();
    if (n <= 0) return 0;
    if ((size_t) n > max) {
        s_down.flush();
        return 0;
    }
    const IPAddress from = s_down.remoteIP();
    for (int i = 0; i < 4; ++i) s_remote[i] = from[i];
    return (size_t) s_down.read(buf, max);
}

void tm_transport_remote(uint8_t out[4]) { memcpy(out, s_remote, 4); }

int8_t tm_transport_rssi() { return tm_transport_connected() ? (int8_t) WiFi.RSSI() : 0; }
uint8_t tm_transport_channel() { return tm_transport_connected() ? (uint8_t) WiFi.channel() : 0; }
uint16_t tm_transport_drops() { return s_drops; }

void tm_transport_ip(uint8_t out[4]) {
    const IPAddress ip = tm_transport_connected() ? WiFi.localIP() : IPAddress(0, 0, 0, 0);
    for (int i = 0; i < 4; ++i) out[i] = ip[i];
}
