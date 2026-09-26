#ifndef TM_SETTINGS_H
#define TM_SETTINGS_H

#include <stdint.h>
#include <stddef.h>
#include "tm_config.h"
#include "tm_packet.h"
#include "tm_detector.h"
#include "tm_cloud_proto.h"

typedef struct {
    char ssid[33];
    char password[65];
    char edges[TM_MAX_EDGES][16];   // dotted quads; empty = unused
    char key[TM_KEY_MAX_LEN + 1];   // ASCII; its bytes are the HMAC key
    // A label people use for the node (on the enclosure, in the install
    // manifest). Identity on the wire stays the factory MAC; 0 = unset.
    uint16_t node_id;
    uint8_t mode;                   // TM_MODE_*
    char lora_gw[16];               // TMLAccess address, dotted quad; empty = unset
    // How a Wi-Fi node reaches TMedge. Separate from `mode` (the radio):
    // udp = the local gateway/edge in `edges`; wss = straight to the cloud
    // at `cloud_url`. A node flashed with new firmware keeps udp until it is
    // told otherwise -- flashing alone never migrates an installed node.
    uint8_t transport;              // TM_TRANSPORT_*
    char cloud_url[TM_CLOUD_URL_MAX + 1];
    int32_t params[TM_PARAM_COUNT];
    uint32_t last_cmd;              // highest command sequence applied
    uint16_t boot;                  // boot counter, incremented by tm_settings_begin
} TmSettings;

#define TM_MODE_WIFI 0
#define TM_MODE_LORA 1

#define TM_TRANSPORT_UDP 0
#define TM_TRANSPORT_WSS 1

/** What this firmware can do, for `show` (TMflash reads it before using new commands). */
#define TM_CAPABILITIES "wss1,ota-https1"

/** A test build (env:tmsense_testcloud) also accepts ws:// and other ports, for a local fixture. */
#if defined(TM_CLOUD_TEST_BUILD)
#define TM_CLOUD_ALLOW_TEST_URL true
#else
#define TM_CLOUD_ALLOW_TEST_URL false
#endif

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

/** Filled in by main: one line each for `show` about the uplink's live state. */
typedef void (*TmShowTransport)(void);
void tm_console_set_transport_reporter(TmShowTransport fn);

#endif // TM_SETTINGS_H
