#include "tm_display.h"

#include <Arduino.h>
#include <Wire.h>
#include <string.h>
#include "tm_config.h"
#include "tm_display_assets.h"

#define OLED_ADDR 0x3C

// 1 kB: static, like every buffer this size (never on the 8 kB loop stack).
static uint8_t s_fb[1024];
static uint32_t s_shown_hash = 0;
static bool s_present = false;
static bool s_on = false;

static bool cmd(const uint8_t* c, size_t n) {
    Wire1.beginTransmission(OLED_ADDR);
    Wire1.write(0x00);   // control byte: commands follow
    Wire1.write(c, n);
    return Wire1.endTransmission() == 0;
}

/** Push the whole framebuffer: ~1 kB at 400 kHz, about 25 ms. */
static void flush() {
    static const uint8_t window[] = {0x21, 0, 127, 0x22, 0, 7};   // columns 0..127, pages 0..7
    cmd(window, sizeof(window));
    for (size_t i = 0; i < sizeof(s_fb); i += 16) {
        Wire1.beginTransmission(OLED_ADDR);
        Wire1.write(0x40);   // control byte: data follows
        Wire1.write(s_fb + i, 16);
        Wire1.endTransmission();
    }
}

static uint32_t hash_fb() {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < sizeof(s_fb); ++i) h = (h ^ s_fb[i]) * 16777619u;
    return h;
}

bool tm_display_begin() {
    // Vext (active low) powers the panel. It is only ever switched on here,
    // never off: on some builds the sensor is wired to the same rail.
    pinMode(TM_PIN_VEXT, OUTPUT);
    digitalWrite(TM_PIN_VEXT, LOW);
    delay(20);
    pinMode(TM_PIN_OLED_RST, OUTPUT);
    digitalWrite(TM_PIN_OLED_RST, LOW);
    delay(10);
    digitalWrite(TM_PIN_OLED_RST, HIGH);
    delay(10);
    Wire1.begin(TM_PIN_OLED_SDA, TM_PIN_OLED_SCL, 400000);
    Wire1.beginTransmission(OLED_ADDR);
    s_present = Wire1.endTransmission() == 0;
    if (!s_present) return false;
    static const uint8_t init[] = {
        0xAE,        // off while configuring
        0xD5, 0x80,  // clock
        0xA8, 0x3F,  // 64 rows
        0xD3, 0x00,  // no offset
        0x40,        // start line 0
        0x8D, 0x14,  // charge pump on
        0x20, 0x00,  // horizontal addressing
        0xA1, 0xC8,  // upright on the Heltec board
        0xDA, 0x12,  // COM pins
        0x81, 0xCF,  // contrast
        0xD9, 0xF1,  // precharge
        0xDB, 0x40,  // VCOMH
        0xA4, 0xA6,  // show RAM, not inverted
    };
    cmd(init, sizeof(init));
    memset(s_fb, 0, sizeof(s_fb));
    flush();
    tm_display_power(true);
    return true;
}

bool tm_display_present() { return s_present; }
bool tm_display_is_on() { return s_on; }

void tm_display_power(bool on) {
    if (!s_present || on == s_on) return;
    // Asleep the SSD1306 draws microamps; with the charge pump off, less still.
    static const uint8_t wake[] = {0x8D, 0x14, 0xAF};
    static const uint8_t sleep[] = {0xAE, 0x8D, 0x10};
    if (on) cmd(wake, sizeof(wake));
    else cmd(sleep, sizeof(sleep));
    s_on = on;
}

void tm_display_logo() {
    if (!s_present) return;
    tm_display_power(true);
    // A wipe from the left, eight columns at a time.
    memset(s_fb, 0, sizeof(s_fb));
    for (int x = 8; x <= 128; x += 8) {
        for (int page = 0; page < 8; ++page) memcpy(s_fb + page * 128, TM_LOGO + page * 128, (size_t) x);
        flush();
    }
    s_shown_hash = hash_fb();
}

static void draw_char(int col, int row, char c) {
    if (c < 32 || c > 126) c = '?';
    const uint8_t* g = TM_FONT5X7 + (c - 32) * 5;
    uint8_t* p = s_fb + row * 128 + col * 6;
    for (int i = 0; i < 5; ++i) p[i] = g[i];
    p[5] = 0;
}

void tm_display_status(const char lines[TM_STATUS_LINES][TM_STATUS_COLS + 1]) {
    if (!s_present || !s_on) return;
    memset(s_fb, 0, sizeof(s_fb));
    for (int r = 0; r < TM_STATUS_LINES; ++r) {
        for (int c = 0; c < TM_STATUS_COLS && lines[r][c]; ++c) draw_char(c, r, lines[r][c]);
    }
    const uint32_t h = hash_fb();
    if (h == s_shown_hash) return;   // unchanged: no 25 ms on the bus
    s_shown_hash = h;
    flush();
}
