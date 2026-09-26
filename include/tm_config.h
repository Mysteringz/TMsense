#ifndef TM_CONFIG_H
#define TM_CONFIG_H

/**
 * Build-time constants and the defaults a fresh node boots with.
 *
 * A node is meant to be provisioned, not compiled: every setting below can be
 * changed at runtime over USB serial (`set ssid ...`, `save`) and is then kept
 * in flash. So a campus runs one firmware image, and the node's identity is
 * its factory MAC, not a number someone typed.
 *
 * For bench work it is still convenient to bake in defaults: copy
 * node_config.example.h to node_config.h (git-ignored) and fill it in. Values
 * saved in flash win over these.
 */

#if defined(TM_HOST_TEST)
  // the host harness defines what it needs
#elif defined(TM_NO_NODE_CONFIG)
  // Release image (TMflash): no bench defaults baked in, so a binary handed
  // around never carries a Wi-Fi password or key; TMflash provisions them.
#elif __has_include("node_config.h")
  #include "node_config.h"
#endif

#define TM_FW_VERSION "tmsense-1.4"   // at most 12 chars: STATUS fw_version

#ifndef TM_DEFAULT_SSID
  #define TM_DEFAULT_SSID ""
#endif
#ifndef TM_DEFAULT_PASSWORD
  #define TM_DEFAULT_PASSWORD ""
#endif
// Up to two edges, comma-separated. REPORT and STATUS go to both, so one edge
// can be down, rebooting or being replaced without a gap; RAW goes only to
// the first, because it is the large one and only a console needs it.
#ifndef TM_DEFAULT_EDGES
  #define TM_DEFAULT_EDGES ""
#endif
#ifndef TM_DEFAULT_KEY
  #define TM_DEFAULT_KEY ""
#endif

#define TM_UPLINK_PORT 5200     // edge listens here
#define TM_DOWNLINK_PORT 5201   // node listens here for commands
#define TM_MAX_EDGES 2

#define TM_SERIAL_BAUD 115200
#define TM_STATUS_INTERVAL_MS 10000
#define TM_WIFI_RETRY_MS 5000

// Heltec WiFi LoRa 32 V3
#define TM_PIN_SDA 41
#define TM_PIN_SCL 42
#define TM_PIN_LED 35
#define TM_I2C_HZ 1000000

#endif // TM_CONFIG_H
