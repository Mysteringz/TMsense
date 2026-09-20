#include "tm_ota.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <Update.h>
#include <WiFi.h>
#include <esp_ota_ops.h>
#include <mbedtls/sha256.h>
#include <string.h>

#include "tm_settings.h"

static const char* NS = "tmota";

static TmOtaReporter s_report = NULL;
static bool s_busy = false;
static uint32_t s_image = 0;          // first four bytes of the SHA-256
static TmOtaRequest s_req;
static uint8_t s_gateway[4];

// Probation of a freshly flashed image.
static bool s_probation = false;
static uint32_t s_probation_start = 0;
static bool s_saw_uplink = false;

static void report(uint8_t state, uint8_t percent, uint8_t error) {
    if (!s_report) return;
    TmOtaStatus st;
    st.state = state;
    st.percent = percent;
    st.error = error;
    st.image = s_image;
    s_report(&st);
}

static uint32_t image_id(const uint8_t sha[32]) {
    return (uint32_t) sha[0] | ((uint32_t) sha[1] << 8) | ((uint32_t) sha[2] << 16) | ((uint32_t) sha[3] << 24);
}

/** Which app partition is running, as a small number: ota_0 -> 0, ota_1 -> 1. */
static uint8_t running_slot() {
    const esp_partition_t* p = esp_ota_get_running_partition();
    return p ? (uint8_t) (p->subtype - ESP_PARTITION_SUBTYPE_APP_OTA_0) : 0xff;
}

void tm_ota_init(TmOtaReporter reporter) {
    s_report = reporter;
    Preferences prefs;
    prefs.begin(NS, false);
    const bool pending = prefs.getUChar("pend", 0) != 0;
    const uint8_t target = prefs.getUChar("slot", 0xff);
    s_image = prefs.getUInt("img", 0);
    if (!pending) {
        prefs.end();
        return;
    }
    if (running_slot() == target) {
        // The new image is the one running. It has not earned its place yet.
        s_probation = true;
        s_probation_start = millis();
        s_saw_uplink = false;
        Serial.printf("[ota] new image on probation for %lu s\n", (unsigned long) (TM_OTA_PROVE_MS / 1000));
    } else {
        // We are back on the old image: the new one never got this far.
        prefs.putUChar("pend", 0);
        Serial.println("[ota] rolled back to the previous image");
        report(TM_OTA_REVERTED, 0, TM_OTA_ERR_FLASH);
    }
    prefs.end();
}

bool tm_ota_busy() { return s_busy; }

void tm_ota_health(bool wifi_ok, bool sensor_ok, bool uplink_ok) {
    if (!s_probation) return;
    if (uplink_ok) s_saw_uplink = true;
    const uint32_t elapsed = millis() - s_probation_start;
    if (wifi_ok && sensor_ok && s_saw_uplink) {
        // Proven: keep it, and stop the bootloader from ever reverting it.
        s_probation = false;
        esp_ota_mark_app_valid_cancel_rollback();
        Preferences prefs;
        prefs.begin(NS, false);
        prefs.putUChar("pend", 0);
        prefs.end();
        Serial.println("[ota] new image confirmed");
        report(TM_OTA_CONFIRMED, 100, TM_OTA_ERR_NONE);
        return;
    }
    if (elapsed < TM_OTA_PROVE_MS) return;
    // Out of time. Go back to the image that was working and reboot; the next
    // boot reports the revert.
    const esp_partition_t* other = esp_ota_get_next_update_partition(NULL);
    Serial.printf("[ota] new image failed to prove itself (wifi=%d sensor=%d uplink=%d): reverting\n",
                  (int) wifi_ok, (int) sensor_ok, (int) s_saw_uplink);
    report(TM_OTA_FAILED, 0, wifi_ok ? TM_OTA_ERR_FLASH : TM_OTA_ERR_NO_WIFI);
    if (other && esp_ota_set_boot_partition(other) == ESP_OK) {
        delay(200);
        ESP.restart();
    }
    s_probation = false;   // cannot revert: stay up rather than loop
}

void tm_ota_begin(const TmOtaRequest* req, const uint8_t gateway[4]) {
    if (s_busy || s_probation) {
        s_image = image_id(req->sha256);
        report(TM_OTA_FAILED, 0, TM_OTA_ERR_BUSY);
        return;
    }
    s_req = *req;
    memcpy(s_gateway, gateway, 4);
    s_image = image_id(req->sha256);
    if (WiFi.status() != WL_CONNECTED) {
        report(TM_OTA_FAILED, 0, TM_OTA_ERR_NO_WIFI);
        return;
    }
    s_busy = true;
    report(TM_OTA_DOWNLOADING, 0, TM_OTA_ERR_NONE);
}

/** Fetch, hash and write the image. Blocking: an update is not a background job. */
static void run_download() {
    s_busy = false;   // whatever happens below, we are done after this pass

    char url[96];
    snprintf(url, sizeof(url), "http://%u.%u.%u.%u:%u%s", s_gateway[0], s_gateway[1], s_gateway[2], s_gateway[3],
             (unsigned) s_req.port, s_req.path);
    Serial.printf("[ota] fetching %s (%lu bytes)\n", url, (unsigned long) s_req.size);

    HTTPClient http;
    http.setTimeout(15000);
    http.setConnectTimeout(8000);
    if (!http.begin(url)) {
        report(TM_OTA_FAILED, 0, TM_OTA_ERR_HTTP);
        return;
    }
    const int code = http.GET();
    if (code != HTTP_CODE_OK) {
        Serial.printf("[ota] gateway answered %d\n", code);
        http.end();
        report(TM_OTA_FAILED, 0, TM_OTA_ERR_HTTP);
        return;
    }
    const int len = http.getSize();
    if (len > 0 && (uint32_t) len != s_req.size) {
        Serial.printf("[ota] gateway offers %d bytes, expected %lu\n", len, (unsigned long) s_req.size);
        http.end();
        report(TM_OTA_FAILED, 0, TM_OTA_ERR_SIZE);
        return;
    }
    if (!Update.begin(s_req.size)) {
        Serial.printf("[ota] no room for the image: %s\n", Update.errorString());
        http.end();
        report(TM_OTA_FAILED, 0, TM_OTA_ERR_FLASH);
        return;
    }

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);

    // 1 kB at a time, off the loop task's stack.
    static uint8_t chunk[1024];
    WiFiClient* stream = http.getStreamPtr();
    uint32_t got = 0;
    uint8_t last_percent = 0;
    uint32_t last_report = millis();
    uint32_t idle_since = millis();
    while (got < s_req.size) {
        const size_t avail = stream->available();
        if (avail == 0) {
            if (!http.connected() || millis() - idle_since > 15000) break;
            delay(5);
            continue;
        }
        idle_since = millis();
        const int n = stream->readBytes(chunk, avail > sizeof(chunk) ? sizeof(chunk) : avail);
        if (n <= 0) continue;
        if (Update.write(chunk, (size_t) n) != (size_t) n) {
            Serial.printf("[ota] write failed: %s\n", Update.errorString());
            Update.abort();
            mbedtls_sha256_free(&sha);
            http.end();
            report(TM_OTA_FAILED, (uint8_t) (100ULL * got / s_req.size), TM_OTA_ERR_FLASH);
            return;
        }
        mbedtls_sha256_update(&sha, chunk, (size_t) n);
        got += (uint32_t) n;
        const uint8_t percent = (uint8_t) (100ULL * got / s_req.size);
        if (percent != last_percent && millis() - last_report > 2000) {
            last_percent = percent;
            last_report = millis();
            report(TM_OTA_DOWNLOADING, percent, TM_OTA_ERR_NONE);
        }
    }
    http.end();

    uint8_t digest[32];
    mbedtls_sha256_finish(&sha, digest);
    mbedtls_sha256_free(&sha);

    if (got != s_req.size) {
        Serial.printf("[ota] got %lu of %lu bytes\n", (unsigned long) got, (unsigned long) s_req.size);
        Update.abort();
        report(TM_OTA_FAILED, (uint8_t) (100ULL * got / s_req.size), TM_OTA_ERR_SIZE);
        return;
    }
    report(TM_OTA_VERIFYING, 100, TM_OTA_ERR_NONE);
    if (memcmp(digest, s_req.sha256, 32) != 0) {
        // The bytes are not the ones the edge signed for. Nothing is booted.
        Serial.println("[ota] image hash does not match the signed request");
        Update.abort();
        report(TM_OTA_FAILED, 100, TM_OTA_ERR_SHA);
        return;
    }

    report(TM_OTA_APPLYING, 100, TM_OTA_ERR_NONE);
    if (!Update.end(true)) {
        Serial.printf("[ota] could not finish: %s\n", Update.errorString());
        report(TM_OTA_FAILED, 100, TM_OTA_ERR_FLASH);
        return;
    }

    // Remember what we flashed and where, so the next boot can tell whether
    // it is running the new image or has fallen back to the old one.
    const esp_partition_t* target = esp_ota_get_boot_partition();
    Preferences prefs;
    prefs.begin(NS, false);
    prefs.putUChar("pend", 1);
    prefs.putUChar("slot", target ? (uint8_t) (target->subtype - ESP_PARTITION_SUBTYPE_APP_OTA_0) : 0xff);
    prefs.putUInt("img", s_image);
    prefs.end();

    Serial.println("[ota] image written; rebooting into it");
    report(TM_OTA_REBOOTING, 100, TM_OTA_ERR_NONE);
    delay(300);   // let the packet leave before the radio goes
    ESP.restart();
}

void tm_ota_update() {
    if (s_busy) run_download();
}
