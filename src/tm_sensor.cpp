#include "tm_sensor.h"

#include <Arduino.h>
#include <Wire.h>
#include <math.h>
#include "MLX90640_API.h"
#include "MLX90640_I2C_Driver.h"

#define TM_MLX_ADDRESS 0x33
// Reflected temperature offset for open air, per the Melexis application note.
#define TM_TA_SHIFT 8.0f
#define TM_EMISSIVITY 0.95f

// ~9 kB of calibration: static, never on the loop task's 8 kB stack.
static paramsMLX90640 s_params;
static uint16_t s_raw[834];
static float s_frame[TM_GRID_SIZE];
static bool s_have[2];
static float s_ta = NAN;
static float s_vdd = NAN;

static bool present() {
    Wire.beginTransmission(TM_MLX_ADDRESS);
    return Wire.endTransmission() == 0;
}

bool tm_sensor_begin(int sda, int scl, uint32_t i2c_hz, uint8_t refresh_code) {
    Wire.begin(sda, scl);
    // Calibration EEPROM is read at 400 kHz; frames at the faster rate.
    Wire.setClock(400000);

    // The sensor acknowledges a moment after power-on, and a cold boot gets
    // here first. Measured on a Heltec V3: a single probe failed every time.
    bool found = false;
    for (int attempt = 0; attempt < 10 && !found; ++attempt) {
        if (attempt > 0) delay(20);
        found = present();
    }
    if (!found) return false;

    static uint16_t ee[832];
    if (MLX90640_DumpEE(TM_MLX_ADDRESS, ee) != 0) return false;
    if (MLX90640_ExtractParameters(ee, &s_params) != 0) return false;
    if (!tm_sensor_set_refresh(refresh_code)) return false;

    Wire.setClock(i2c_hz);
    s_have[0] = s_have[1] = false;
    return true;
}

bool tm_sensor_set_refresh(uint8_t refresh_code) {
    if (refresh_code < 1 || refresh_code > 5) return false;
    return MLX90640_SetRefreshRate(TM_MLX_ADDRESS, refresh_code) == 0;
}

// Pixels the EEPROM marks broken or outlying, and any reading that is not a
// temperature a room can have, take the mean of their valid 4-neighbours. A
// single stuck pixel otherwise reads as a permanent tiny person.
static bool plausible(float t) { return isfinite(t) && t > -40.0f && t < 200.0f; }

static void repair(int index) {
    const int x = index % TM_GRID_W, y = index / TM_GRID_W;
    float sum = 0.0f;
    int n = 0;
    const int nx[4] = {x - 1, x + 1, x, x};
    const int ny[4] = {y, y, y - 1, y + 1};
    for (int k = 0; k < 4; ++k) {
        if (nx[k] < 0 || nx[k] >= TM_GRID_W || ny[k] < 0 || ny[k] >= TM_GRID_H) continue;
        const float v = s_frame[ny[k] * TM_GRID_W + nx[k]];
        if (plausible(v)) { sum += v; ++n; }
    }
    s_frame[index] = n > 0 ? sum / n : (plausible(s_ta) ? s_ta : 20.0f);
}

static void repair_frame() {
    for (int k = 0; k < 5; ++k) {
        if (s_params.brokenPixels[k] < TM_GRID_SIZE) repair(s_params.brokenPixels[k]);
        if (s_params.outlierPixels[k] < TM_GRID_SIZE) repair(s_params.outlierPixels[k]);
    }
    for (int i = 0; i < TM_GRID_SIZE; ++i) {
        if (!plausible(s_frame[i])) repair(i);
    }
}

int tm_sensor_poll(float* frame) {
    uint16_t status = 0;
    if (MLX90640_I2CRead(TM_MLX_ADDRESS, 0x8000, 1, &status) != 0) return -1;
    if ((status & 0x0008) == 0) return 0;   // no new subpage yet

    const int subpage = MLX90640_GetFrameData(TM_MLX_ADDRESS, s_raw);
    if (subpage < 0) return subpage;

    s_vdd = MLX90640_GetVdd(s_raw, &s_params);
    s_ta = MLX90640_GetTa(s_raw, &s_params);
    MLX90640_CalculateTo(s_raw, &s_params, TM_EMISSIVITY, s_ta - TM_TA_SHIFT, s_frame);
    s_have[subpage & 1] = true;

    // A full frame needs both subpages; the chess pattern puts half the
    // pixels in each. Emitting after one would mix this second's pixels with
    // last second's.
    if (!(s_have[0] && s_have[1])) return 0;
    s_have[0] = s_have[1] = false;
    repair_frame();
    memcpy(frame, s_frame, sizeof(s_frame));
    return 1;
}

float tm_sensor_ta() { return s_ta; }
float tm_sensor_vdd() { return s_vdd; }
