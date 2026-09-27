#include "tm_cloud.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_random.h>
#include <string.h>
#include <time.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include "tm_ca_roots.h"

#if defined(TM_CLOUD_TEST_BUILD) && __has_include("tm_test_ca.h")
// Test build only: a local fixture's CA, beside the real roots. Git-ignored;
// a release image (env:tmflash) never has it.
#include "tm_test_ca.h"
#endif

// Everything here is static: the session alone holds ~4 kB of buffers, far
// too much for the 8 kB loop stack, and none of it should be allocated at
// run time on a device that must stay up for months.
static TmCloudSession s_session;
static TmPacketQueue s_up;
static struct {
    uint16_t len;
    uint8_t data[TM_HEADER_SIZE + TM_OTA_SIZE + TM_TAG_SIZE];
} s_down[2];
static uint8_t s_down_count = 0;
static uint8_t s_key[TM_KEY_MAX_LEN];
static size_t s_key_len = 0;

static WiFiClientSecure s_tls;
static WiFiClient s_plain;   // test build only: ws:// to a local fixture
static bool s_use_tls = true;

static SemaphoreHandle_t s_lock = NULL;
static StaticSemaphore_t s_lock_buf;
static TaskHandle_t s_task = NULL;
static volatile bool s_enabled = false;
static volatile bool s_reconfigure = false;
static volatile bool s_pause_wanted = false;
static volatile bool s_paused = false;
static volatile bool s_status_request = false;
static bool s_sntp_started = false;
static TmCloudInfo s_info;
// The session's grant, copied out under the lock: the session itself is the
// task's alone, and the OTA code on the loop must never read it mid-write.
static struct {
    bool valid;
    uint32_t seq;
    char build[17];
    char token[65];
    uint32_t expires_ms;
} s_grant;
static TmCloudUrl s_pending_url;
static uint8_t s_pending_uid[6];
static uint16_t s_pending_boot = 0;

#define LOCK() xSemaphoreTake(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_lock)

// --- TLS --------------------------------------------------------------------------

/** a < b for an mbedTLS certificate time against a broken-down UTC time. */
static int cmp_time(const mbedtls_x509_time* x, const struct tm* t) {
    const int a[6] = {x->year, x->mon, x->day, x->hour, x->min, x->sec};
    const int b[6] = {t->tm_year + 1900, t->tm_mon + 1, t->tm_mday, t->tm_hour, t->tm_min, t->tm_sec};
    for (int i = 0; i < 6; ++i) {
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    }
    return 0;
}

bool tm_cloud_time_ok() { return time(NULL) > TM_MIN_PLAUSIBLE_EPOCH; }

/**
 * The ESP32 core's mbedTLS is built without MBEDTLS_HAVE_TIME_DATE, so it
 * checks the chain and the host name but not whether a certificate has
 * expired or is not yet valid. This does that part, against SNTP time, for
 * every certificate the server presented.
 */
static bool chain_valid_now(WiFiClientSecure* c) {
    if (!tm_cloud_time_ok()) return false;
    const mbedtls_x509_crt* crt = c->getPeerCertificate();
    if (!crt) return false;
    const time_t now = time(NULL);
    struct tm utc;
    gmtime_r(&now, &utc);
    for (; crt && crt->raw.len > 0; crt = crt->next) {
        if (cmp_time(&crt->valid_from, &utc) > 0 || cmp_time(&crt->valid_to, &utc) < 0) return false;
    }
    return true;
}

static const char* trust_roots() {
#if defined(TM_CLOUD_TEST_BUILD) && defined(TM_TEST_CA)
    static char both[sizeof(TM_CA_ROOTS) + sizeof(TM_TEST_CA)];
    if (!both[0]) {
        strcpy(both, TM_CA_ROOTS);
        strcat(both, TM_TEST_CA);
    }
    return both;
#else
    return TM_CA_ROOTS;
#endif
}

int tm_cloud_tls_connect(WiFiClientSecure* c, const char* host, uint16_t port) {
    c->stop();
    if (!tm_cloud_time_ok()) return -3;
    // Never setInsecure(): a root CA means MBEDTLS_SSL_VERIFY_REQUIRED, and
    // the core sets the SNI/verification host name from `host`.
    c->setCACert(trust_roots());
    c->setHandshakeTimeout(15);
    if (!c->connect(host, port, 10000)) {
        char err[64];
        const int e = c->lastError(err, sizeof(err));
        c->stop();
        // -0x2700 is MBEDTLS_ERR_X509_CERT_VERIFY_FAILED: chain or name.
        return e == -0x2700 ? -2 : -1;
    }
    if (!chain_valid_now(c)) {
        c->stop();
        return -3;
    }
    c->setTimeout(10);   // socket read/write timeouts; the socket only exists now
    return 0;
}

// --- I/O for the session, in the task ---------------------------------------------

static Client* client() { return s_use_tls ? (Client*) &s_tls : (Client*) &s_plain; }

static int io_connect(void*, const char* host, uint16_t port) {
    if (s_use_tls) return tm_cloud_tls_connect(&s_tls, host, port);
    s_plain.stop();
    return s_plain.connect(host, port, 10000) ? 0 : -1;
}

static int io_write(void*, const uint8_t* data, size_t len) {
    Client* c = client();
    size_t done = 0;
    while (done < len) {
        const size_t n = c->write(data + done, len - done);
        if (n == 0) return -1;
        done += n;
    }
    return (int) len;
}

static int io_read(void*, uint8_t* buf, size_t max) {
    Client* c = client();
    const int avail = c->available();
    if (avail > 0) {
        const int n = c->read(buf, (size_t) avail < max ? (size_t) avail : max);
        return n < 0 ? -1 : n;
    }
    return c->connected() ? 0 : -1;
}

static void io_close(void*) {
    s_tls.stop();
    s_plain.stop();
}

static uint32_t io_random(void*) { return esp_random(); }

static const TmCloudIo s_io = {io_connect, io_write, io_read, io_close, io_random, NULL};

// --- queues, shared with the loop ---------------------------------------------------

static size_t cb_next(void*, uint8_t* buf, size_t max, uint8_t* type, uint32_t now) {
    LOCK();
    const size_t n = tm_queue_pop(&s_up, buf, max, type, now);
    UNLOCK();
    return n;
}

static void cb_discard(void*) {
    LOCK();
    tm_queue_clear(&s_up);
    UNLOCK();
}

static void cb_down(void*, const uint8_t* data, size_t len) {
    if (len > sizeof(s_down[0].data)) return;   // larger than any downlink packet: not one
    LOCK();
    if (s_down_count < 2) {
        memcpy(s_down[s_down_count].data, data, len);
        s_down[s_down_count].len = (uint16_t) len;
        ++s_down_count;
    }
    UNLOCK();
}

// --- the task ----------------------------------------------------------------------

static void configure() {
    uint8_t* uid = s_pending_uid;
    char uid_s[18];
    tm_cloud_uid_string(uid, uid_s);
    s_session.next_uplink = cb_next;
    s_session.discard_uplink = cb_discard;
    s_session.on_downlink = cb_down;
    s_session.cb_ctx = NULL;
    io_close(NULL);
    tm_cloud_session_init(&s_session, &s_pending_url, uid_s, s_key, s_key_len, s_pending_boot);
    s_use_tls = s_pending_url.tls;
}

static void publish_info() {
    LOCK();
    s_info.state = s_session.state;
    s_info.reports_acked = s_session.reports_acked;
    s_info.last_report_ack_ms = s_session.last_report_ack_ms;
    s_info.connects = s_session.connects;
    s_info.queue_dropped = s_up.dropped;
    memcpy(s_info.last_error, s_session.last_error, sizeof(s_info.last_error));
    s_info.task_stack_free = uxTaskGetStackHighWaterMark(NULL);
    s_grant.valid = s_session.grant_valid;
    if (s_session.grant_valid) {
        s_grant.seq = s_session.grant_seq;
        memcpy(s_grant.build, s_session.grant_build, sizeof(s_grant.build));
        memcpy(s_grant.token, s_session.grant_token, sizeof(s_grant.token));
        s_grant.expires_ms = s_session.grant_expires_ms;
    }
    UNLOCK();
}

static void task(void*) {
    for (;;) {
        if (s_reconfigure) {
            s_reconfigure = false;
            configure();
        }
        if (!s_enabled) {
            if (s_session.state != TM_CLOUD_OFF) {
                tm_cloud_session_pause(&s_session, &s_io, true, millis());
                s_session.state = TM_CLOUD_OFF;
            }
            publish_info();
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        const bool link = WiFi.status() == WL_CONNECTED;
        if (link && !s_sntp_started) {
            // Certificates need the date. Several servers, and no fallback
            // to "trust anyway" if none answers.
            configTime(0, 0, "time.cloudflare.com", "pool.ntp.org", "time.google.com");
            s_sntp_started = true;
        }
        if (s_pause_wanted != s_paused) {
            tm_cloud_session_pause(&s_session, &s_io, s_pause_wanted, millis());
            s_paused = s_pause_wanted;
        }
        const TmCloudState before = s_session.state;
        tm_cloud_session_step(&s_session, &s_io, millis(), link, tm_cloud_time_ok());
        if (s_session.want_status) {
            s_session.want_status = false;
            s_status_request = true;
        }
        // TMflash waits for this line: "joined Wi-Fi" is not "the edge took
        // a report", and only the second means provisioning worked.
        static bool announced = false;
        if (s_session.report_acked_this_session && !announced) {
            announced = true;
            Serial.println("[cloud] report accepted by the edge");
        } else if (!s_session.report_acked_this_session) {
            announced = false;
        }
        if (s_session.state != before) {
            Serial.printf("[cloud] %s%s%s\n", tm_cloud_state_name(s_session.state),
                          s_session.last_error[0] && s_session.state == TM_CLOUD_BACKOFF ? ": " : "",
                          s_session.state == TM_CLOUD_BACKOFF ? s_session.last_error : "");
        }
        publish_info();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

void tm_cloud_begin(const TmCloudUrl* url, const uint8_t uid[6], const uint8_t* key, size_t key_len, uint16_t boot) {
    if (!s_lock) s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    LOCK();
    s_pending_url = *url;
    memcpy(s_pending_uid, uid, 6);
    s_pending_boot = boot;
    s_key_len = key_len > sizeof(s_key) ? sizeof(s_key) : key_len;
    memcpy(s_key, key, s_key_len);
    tm_queue_clear(&s_up);
    s_down_count = 0;
    UNLOCK();
    s_reconfigure = true;
    s_enabled = true;
    if (!s_task) {
        // Core 0, beside the Wi-Fi stack; the sensor loop keeps core 1. 12 kB:
        // an mbedTLS handshake is the deepest thing it does (see `show`).
        xTaskCreatePinnedToCore(task, "tm_cloud", 12288, NULL, 1, &s_task, 0);
    }
}

void tm_cloud_stop() { s_enabled = false; }
bool tm_cloud_active() { return s_enabled; }

bool tm_cloud_send(const uint8_t* data, size_t len) {
    if (!s_enabled || !s_lock) return false;
    LOCK();
    // Nothing is queued unless a session is ready to take it: a node that
    // cannot send keeps no history to flush later.
    const bool ok = s_info.state == TM_CLOUD_READY && tm_queue_push(&s_up, data, len, millis());
    UNLOCK();
    return ok;
}

size_t tm_cloud_receive(uint8_t* buf, size_t max) {
    if (!s_lock) return 0;
    size_t n = 0;
    LOCK();
    if (s_down_count > 0 && s_down[0].len <= max) {
        n = s_down[0].len;
        memcpy(buf, s_down[0].data, n);
        s_down[0] = s_down[1];
        --s_down_count;
    }
    UNLOCK();
    return n;
}

void tm_cloud_info(TmCloudInfo* out) {
    if (!s_lock) {
        memset(out, 0, sizeof(*out));
        return;
    }
    LOCK();
    *out = s_info;
    UNLOCK();
    if (!s_enabled) out->state = TM_CLOUD_OFF;
}

bool tm_cloud_take_status_request() {
    if (!s_status_request) return false;
    s_status_request = false;
    return true;
}

bool tm_cloud_grant(uint32_t seq, const char* build, char token[65]) {
    if (!s_lock) return false;
    LOCK();
    const bool ok = s_grant.valid && s_grant.seq == seq && !strcmp(s_grant.build, build) &&
                    (int32_t) (s_grant.expires_ms - millis()) > 0;
    if (ok) memcpy(token, s_grant.token, 65);
    UNLOCK();
    return ok;
}

bool tm_cloud_pause(bool paused, uint32_t wait_ms) {
    s_pause_wanted = paused;
    const uint32_t start = millis();
    while (s_paused != paused) {
        if (millis() - start > wait_ms) return false;
        delay(10);
    }
    return true;
}
