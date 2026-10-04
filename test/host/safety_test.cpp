// Compile the real settings, OTA and sensor-driver code against deterministic
// Arduino/NVS shims. Inject flash failures and simulate rebooted app slots;
// none of these tests connects to hardware or a live endpoint.
#include <Arduino.h>
#include <Preferences.h>
#include <Wire.h>
#include "tm_settings.h"
#include "tm_cloud.h"
#include "MLX90640_I2C_Driver.h"
#include "MLX90640_API.h"
#include "../../src/tm_ota.cpp"

static int checks = 0, failed = 0;
#define CHECK(c, message) do { ++checks; if (!(c)) { ++failed; fprintf(stderr, "FAIL: %s\n", message); } } while (0)
static std::vector<TmOtaStatus> reports;
static void capture(const TmOtaStatus* report) { reports.push_back(*report); }

void tm_transport_uid(uint8_t out[6]) { memset(out, 1, 6); }
bool tm_cloud_grant(uint32_t, const char*, char[65]) { return false; }
bool tm_cloud_pause(bool, uint32_t) { return true; }
void tm_cloud_info(TmCloudInfo* info) { memset(info, 0, sizeof(*info)); info->state = TM_CLOUD_READY; }
int tm_cloud_tls_connect(WiFiClientSecure*, const char*, uint16_t) { return -1; }

bool HostUpdate::end(bool) {
    host_update_end_called = true;
    const auto i = host_nvs.find("tmota/pend");
    host_update_end_saw_probation = i != host_nvs.end() && i->second == std::vector<uint8_t>{1};
    host_boot_slot = 1 - host_running_slot;
    return true;
}

static uint8_t console(const std::string& text) {
    Serial.input = text + "\n"; Serial.at = 0; Serial.output.clear(); return tm_console_poll();
}

static void test_settings() {
    host_nvs.clear(); host_nvs_fail.clear();
    tm_settings_begin();
    strcpy(g_settings.ssid, "test network");
    CHECK(tm_settings_save(), "empty optional strings and nonempty SSID save successfully");
    host_nvs_fail = "tmnode/ssid";
    strcpy(g_settings.ssid, "new network");
    CHECK(!tm_settings_save(), "failed SSID persistence cannot report saved");
    host_nvs_fail = "tmnode/last_cmd";
    CHECK(!tm_settings_accept_command(123) && g_settings.last_cmd == 0, "flash failure refuses command before replay counter advances");
    host_nvs_fail.clear();
    CHECK(tm_settings_accept_command(123) && g_settings.last_cmd == 123, "command accepted only after replay counter persists");
    CHECK(!tm_settings_accept_command(123), "replayed command refused");
    const auto prior = g_settings.params[TM_PARAM_MIN_AREA];
    CHECK(console("param min_area 2junk") == 0 && g_settings.params[TM_PARAM_MIN_AREA] == prior,
          "numeric command rejects trailing junk");
    CHECK(console("param min_area 2 extra") == 0 && g_settings.params[TM_PARAM_MIN_AREA] == prior,
          "numeric command rejects extra arguments");
    CHECK(console("param min_area 2") == TM_CONSOLE_PARAMS_CHANGED && g_settings.params[TM_PARAM_MIN_AREA] == 2,
          "valid numeric command remains supported");
    CHECK(console("set pass " + std::string(65, 'x')) == 0 && !g_settings.password[0], "overlong password is refused whole");
    CHECK(console("set edges 1.2.3.4,5.6.7.8,9.10.11.12") == 0, "third edge is refused instead of silently dropped");
    console("set key test-secret"); console("show");
    CHECK(Serial.output.find("test-secret") == std::string::npos, "show never prints signing key");
    const auto boot = g_settings.boot;
    CHECK(console("factory") & TM_CONSOLE_NETWORK_CHANGED, "factory reset refreshes live transport and signing state");
    CHECK(g_settings.boot == boot, "factory reset preserves boot counter");
    CHECK(g_settings.last_cmd == 123, "factory reset preserves command replay counter when the same key is reprovisioned");
    host_nvs_fail = "tmnode/boot";
    CHECK(tm_settings_factory_reset() && g_settings.boot == boot && host_nvs.count("tmnode/boot"),
          "factory reset never erases or rewrites boot key even when boot-key writes fail");
    host_nvs_fail.clear();
    Preferences prefs; prefs.begin("tmnode", false);
    prefs.putUChar("transport", TM_TRANSPORT_WSS); prefs.putString("cloud_url", "corrupt"); prefs.end();
    tm_settings_begin();
    CHECK(g_settings.transport == TM_TRANSPORT_WSS, "corrupt stored cloud URL does not silently fall back to UDP");
    prefs.begin("tmnode", false); prefs.putUShort("boot", UINT16_MAX); prefs.end();
    tm_settings_begin();
    CHECK(g_settings.boot == UINT16_MAX && !g_settings.replay_ready,
          "boot counter exhaustion fails closed instead of wrapping into replayable telemetry");
    prefs.begin("tmnode", false); prefs.putUShort("boot", 5); prefs.end();
    host_nvs_fail = "tmnode/boot"; tm_settings_begin();
    CHECK(!g_settings.replay_ready, "boot counter persistence failure disables replayable telemetry");
    host_nvs_fail.clear();
    host_nvs.clear(); tm_settings_begin();
    CHECK(console("set secure 7:" + std::string(64, '1')) == TM_CONSOLE_REBOOT, "valid encryption enrollment persists and reboots");
    tm_settings_begin();
    CHECK(g_settings.secure_id == 7 && g_settings.replay_ready, "encrypted enrollment loads with durable boot counter");
    CHECK(tm_settings_factory_reset() && g_settings.secure_id == 7, "factory reset preserves encryption policy and key");
    prefs.begin("tmnode", false); prefs.putUInt("boot32", 70000); prefs.end(); tm_settings_begin();
    CHECK(g_settings.boot == 70001 && g_settings.replay_ready, "encrypted boot counter continues beyond legacy 16-bit lifetime");
    host_nvs_fail = "tmnode/boot32"; tm_settings_begin();
    CHECK(!g_settings.replay_ready, "wide boot-counter write failure disables transport"); host_nvs_fail.clear();
    uint8_t zero_key[34] = {7};
    prefs.begin("tmnode", false); prefs.putBytes("secure", zero_key, sizeof(zero_key)); prefs.end(); tm_settings_begin();
    CHECK(!g_settings.replay_ready && !g_settings.secure_id, "corrupted all-zero stored encryption key disables transport");
    prefs.begin("tmnode", false); prefs.remove("secure"); prefs.end(); tm_settings_begin();
    CHECK(!g_settings.replay_ready && !g_settings.secure_id, "missing enrolled key fails closed instead of falling back to plaintext");
    host_nvs.clear(); tm_settings_begin(); host_nvs_fail = "tmnode/secure";
    console("set secure 7:" + std::string(64, '1')); host_nvs_fail.clear(); tm_settings_begin();
    CHECK(!g_settings.replay_ready, "interrupted enrollment durably requires encryption");
    CHECK(Serial.output.find(std::string(64, '1')) == std::string::npos, "encryption secrets never appear in serial output");
}

static void reset_ota() {
    s_probation = false; s_busy = false; s_image = 0; s_saw_uplink = false;
    s_cloud = false; host_running_slot = 0; host_boot_slot = 0; host_now = 100;
    host_update_end_called = false; host_update_end_saw_probation = false;
    host_stream_read_delay = 0; host_stream_read_cap = SIZE_MAX;
    host_nvs.clear(); host_nvs_fail.clear(); host_valid_calls = 0; host_valid_fail = false; reports.clear();
    tm_ota_init(capture);
}

static bool download() {
    Client stream;
    stream.bytes = {0xe9, 1, 2, 3, 4, 5};
    s_req = {};
    s_req.size = stream.bytes.size();
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha); mbedtls_sha256_starts(&sha, 0);
    mbedtls_sha256_update(&sha, stream.bytes.data(), stream.bytes.size());
    mbedtls_sha256_finish(&sha, s_req.sha256); mbedtls_sha256_free(&sha);
    s_image = image_id(s_req.sha256);
    return write_image(&stream, &stream);
}

static void test_ota() {
    reset_ota();
    Client drip; drip.bytes = std::vector<uint8_t>(32, 1);
    s_req = {}; s_req.size = drip.bytes.size();
    host_stream_read_delay = 14000; host_stream_read_cap = 1;
    CHECK(!write_image(&drip, &drip) && !host_update_end_called && host_now < TM_OTA_DOWNLOAD_MS + 15000,
          "continuous tiny reads cannot hold the sensor loop indefinitely during OTA");
    reset_ota(); host_nvs_fail = "tmota/slot";
    CHECK(!download() && !host_update_end_called && host_boot_slot == 0,
          "NVS failure refuses activation before Update.end changes boot slot");
    reset_ota();
    CHECK(download() && host_update_end_saw_probation && host_boot_slot == 1,
          "probation record is persistent before image activation");
    host_running_slot = 1;
    tm_ota_init(capture);
    CHECK(s_probation && host_valid_calls == 0, "new image starts unconfirmed");
    tm_ota_health(true, false, true);
    CHECK(s_probation && host_valid_calls == 0, "uplink alone cannot prove sensor health");
    TmOtaRequest refused = {}; memset(refused.sha256, 0xbb, sizeof(refused.sha256));
    const uint32_t active_image = s_image;
    uint8_t gw[4] = {1, 2, 3, 4};
    tm_ota_begin(&refused, gw);
    CHECK(s_image == active_image && reports.back().error == TM_OTA_ERR_BUSY, "busy rejection preserves active probation image ID");
    host_nvs_fail = "tmota/pend";
    tm_ota_health(true, true, true);
    CHECK(s_probation && host_valid_calls == 0, "failed confirmation persistence cannot announce confirmed");
    host_nvs_fail.clear();
    tm_ota_health(true, true, true);
    CHECK(!s_probation && host_valid_calls == 1 && reports.back().state == TM_OTA_CONFIRMED && reports.back().image == active_image,
          "healthy image confirms and keeps its own identity");

    reset_ota(); download(); host_running_slot = 1; tm_ota_init(capture);
    host_valid_fail = true; tm_ota_health(true, true, true);
    CHECK(s_probation && reports.back().state != TM_OTA_CONFIRMED && host_nvs["tmota/pend"][0] == 1,
          "bootloader confirmation failure keeps durable probation and cannot announce success");
    CHECK(host_nvs["tmota/tried"][0] == 1, "failed bootloader confirmation cannot restart probation after a reset");
    host_valid_fail = false; tm_ota_health(true, true, true);
    CHECK(!s_probation && reports.back().state == TM_OTA_CONFIRMED, "bootloader confirmation can retry after storage recovers");

    reset_ota(); download(); host_running_slot = 1; tm_ota_init(capture);
    s_probation = false;  // simulate a reset before it ever confirms
    bool restarted = false;
    try { tm_ota_init(capture); } catch (const HostRestart&) { restarted = true; }
    CHECK(restarted && host_boot_slot == 0 && host_valid_calls == 0,
          "second boot of unconfirmed image rolls back instead of restarting probation");

    reset_ota(); download(); host_running_slot = 1; tm_ota_init(capture);
    host_nvs_fail = "tmota/pend"; host_now += TM_OTA_PROVE_MS + 1; restarted = false;
    try { tm_ota_health(true, true, true); } catch (const HostRestart&) { restarted = true; }
    CHECK(restarted && host_boot_slot == 0, "confirmation flash failure still reaches rollback deadline");
}

extern void ExtractVDDParameters(uint16_t*, paramsMLX90640*);
extern void ExtractAlphaParameters(uint16_t*, paramsMLX90640*);
extern void ExtractOffsetParameters(uint16_t*, paramsMLX90640*);
static void test_driver() {
    Wire.transmissions = 0; Wire.fail_verify = true;
    CHECK(MLX90640_I2CWrite(0x33, 0x8000, 0) == -1,
          "failed register verification read is an I2C error, never uninitialized-data success");
    static uint16_t ee[832] = {};
    static paramsMLX90640 params;
    ee[51] = 0x1020; ExtractVDDParameters(ee, &params);
    CHECK(params.vdd25 == -15360 && params.kVdd == 512, "negative VDD calibration computes without undefined left shift");
    ee[32] = 0x0110; ee[34] = 15; ee[40] = 15; ee[33] = 100;
    ExtractAlphaParameters(ee, &params);
    CHECK(params.alpha[0] > 0 && params.alpha[0] < params.alpha[1], "negative row and column alpha calibration is signed arithmetic");
    ee[16] = 0x0110; ee[18] = 15; ee[24] = 15; ee[17] = 100;
    ExtractOffsetParameters(ee, &params);
    CHECK(params.offset[0] == 96 && params.offset[1] == 98, "negative offset calibration is signed arithmetic");
}

int main() {
    test_settings(); test_ota(); test_driver();
    printf("safety_test: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
