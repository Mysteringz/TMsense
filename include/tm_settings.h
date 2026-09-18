#ifndef TM_SETTINGS_H
#define TM_SETTINGS_H

#include <stdint.h>
#include <stddef.h>
#include "tm_config.h"
#include "tm_packet.h"
#include "tm_detector.h"

typedef struct {
    char ssid[33];
    char password[65];
    char edges[TM_MAX_EDGES][16];   // dotted quads; empty = unused
    char key[TM_KEY_MAX_LEN + 1];   // ASCII; its bytes are the HMAC key
    int32_t params[TM_PARAM_COUNT];
    uint32_t last_cmd;              // highest command sequence applied
    uint16_t boot;                  // boot counter, incremented by tm_settings_begin
} TmSettings;

extern TmSettings g_settings;

/** Load defaults, overlay what flash holds, bump and persist the boot counter. */
void tm_settings_begin();
bool tm_settings_save();
void tm_settings_factory_reset();
void tm_settings_persist_last_cmd();

/** Validate and apply one parameter; false if out of range. */
bool tm_settings_set_param(uint8_t id, int32_t value);
void tm_settings_to_detector(TmDetectorParams* out);
const char* tm_param_name(uint8_t id);

/**
 * Serial console. Feed it bytes; it acts on whole lines. Returns a bitmask of
 * TM_CONSOLE_* actions for the caller (which owns the detector and the radio).
 */
#define TM_CONSOLE_PARAMS_CHANGED 0x01
#define TM_CONSOLE_RESET_BG 0x02
#define TM_CONSOLE_NETWORK_CHANGED 0x04
#define TM_CONSOLE_REBOOT 0x08
uint8_t tm_console_poll();

#endif // TM_SETTINGS_H
