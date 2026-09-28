#ifndef TM_DISPLAY_H
#define TM_DISPLAY_H

/**
 * The Heltec V3's 0.96" SSD1306 OLED (128x64) on its own I2C bus (SDA 17,
 * SCL 18, reset 21, powered through Vext on 36), so it never shares a bus
 * with the MLX90640 on 41/42.
 *
 * Used only while a node starts up or when someone asks: an installer on a
 * ladder sees at a glance which service is missing. Once everything works
 * the panel is put to sleep (a few microamps), not driven all day.
 *
 * A board without the display (no answer at 0x3C) turns every call into a
 * no-op, so the same image runs on any ESP32-S3 node.
 */

#include <stdbool.h>
#include <stdint.h>
#include "tm_status_text.h"

bool tm_display_begin();
bool tm_display_present();

/** The HKUMySeat splash, wiped in from the left over ~0.5 s. */
void tm_display_logo();

/** Eight lines of status. Skips the bus entirely if nothing changed. */
void tm_display_status(const char lines[TM_STATUS_LINES][TM_STATUS_COLS + 1]);

/** Panel on / asleep (charge pump off). Vext is left on: the sensor may share it. */
void tm_display_power(bool on);
bool tm_display_is_on();

#endif // TM_DISPLAY_H
