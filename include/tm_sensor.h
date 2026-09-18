#ifndef TM_SENSOR_H
#define TM_SENSOR_H

#include <stdint.h>
#include <stdbool.h>
#include "tm_protocol.h"

/**
 * MLX90640 acquisition. Non-blocking: tm_sensor_poll() asks the sensor whether
 * a subpage is ready and returns at once if not, so the loop keeps serving
 * Wi-Fi, commands and the serial console while the sensor integrates. The old
 * firmware sat in a busy-wait for up to a second per subpage.
 */

bool tm_sensor_begin(int sda, int scl, uint32_t i2c_hz, uint8_t refresh_code);

/**
 * 1 = a full frame (both subpages) was written to `frame` in C,
 * 0 = not yet, negative = I2C error (the caller counts and re-inits).
 */
int tm_sensor_poll(float* frame);

bool tm_sensor_set_refresh(uint8_t refresh_code);
float tm_sensor_ta();
float tm_sensor_vdd();

#endif // TM_SENSOR_H
