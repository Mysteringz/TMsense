#ifndef TM_PROTOCOL_H
#define TM_PROTOCOL_H

/**
 * TMnode wire protocol, version 1 — SOURCE OF TRUTH.
 *
 * Mirrored by TMedge/src/shared/protocol.ts. Change both in the same commit and
 * run `npm run crosscheck` in TMedge, which parses bytes produced by this
 * firmware's own serializer compiled on the host.
 *
 * Every packet, uplink or downlink, is one datagram:
 *
 *   0   'T' 'M'            magic
 *   2   version (1)
 *   3   type               TM_TYPE_*
 *   4   uid[6]             node identity = the ESP32's factory MAC. Unique with
 *                          no provisioning, so a campus of nodes can run one
 *                          firmware image with nothing per-node compiled in.
 *   10  boot   uint16 LE   boot counter, kept in flash, +1 every boot
 *   12  seq    uint32 LE   per-boot packet counter
 *   16  uptime uint32 LE   ms since boot
 *   20  len    uint16 LE   payload length
 *   22  payload[len]
 *   ..  tag[8]             HMAC-SHA256(key, bytes[0, 22+len)) truncated
 *
 * (boot, seq) only ever increases, across reboots too, which is what lets the
 * edge reject replays without the reboot exception the old gateway needed.
 *
 * All multi-byte fields are little-endian. Temperatures are centi-degrees C
 * (int16) unless stated. Sensor coordinates are in pixels of the 32x24 grid,
 * x to the right, y down, as the sensor reports them.
 *
 * Budget: a REPORT with TM_MAX_DETECTIONS detections is 22 + 14 + 7*24 + 8 =
 * 212 bytes, inside a LoRa frame (222 B at AS923 DR5). Keep it that way: the
 * REPORT is the packet a campus-scale radio has to carry. RAW is Wi-Fi only.
 */

#include <stdint.h>

#define TM_MAGIC_0 0x54  // 'T'
#define TM_MAGIC_1 0x4D  // 'M'
#define TM_PROTOCOL_VERSION 1

#define TM_HEADER_SIZE 22
#define TM_TAG_SIZE 8
#define TM_UID_SIZE 6

#define TM_GRID_W 32
#define TM_GRID_H 24
#define TM_GRID_SIZE (TM_GRID_W * TM_GRID_H)

#define TM_MAX_DETECTIONS 24

// --- Uplink (node -> edge) ---------------------------------------------------

/**
 * REPORT, every frame. What occupancy is computed from.
 *
 *   0   frame     uint32   sensor frame number (matches the RAW of the same frame)
 *   4   ta        int16    sensor die temperature
 *   6   scene_min int16
 *   8   scene_max int16
 *   10  bg_mean   int16    mean of the background model
 *   12  flags     uint8    TM_REPORT_*
 *   13  count     uint8    detections that follow
 *   14  detection[count], 7 bytes each:
 *         x8       uint8   centroid x in 1/8 pixel (0..255 = 0..31.875)
 *         y8       uint8   centroid y in 1/8 pixel
 *         area     uint8   pixels in the blob
 *         contrast uint8   peak (temperature - background), 0.05 C units
 *         peak     uint8   peak absolute temperature, 0.25 C units
 *         heat     uint16  sum over the blob of (temperature - background),
 *                          0.1 C*px units. Grows with how much of a person the
 *                          blob holds, so two people merged into one blob read
 *                          as about twice the heat of one. The edge uses it
 *                          where resolution cannot separate them.
 */
#define TM_TYPE_REPORT 0x01
#define TM_REPORT_FIXED_SIZE 14
#define TM_DETECTION_SIZE 7
#define TM_REPORT_BACKGROUND_READY 0x01
#define TM_REPORT_GLOBAL_SHIFT 0x02   // scene changed as a whole; counts unreliable
#define TM_REPORT_TRUNCATED 0x04      // more blobs than TM_MAX_DETECTIONS

/**
 * RAW, every `raw_every` frames (0 = never). The full frame for the debug
 * console and for calibration. Quantised to 8 bits across this frame's own
 * range, which at 110 deg / 3-5 m (a 5-15 C scene) is 0.02-0.06 C per level,
 * finer than the sensor's own noise.
 *
 *   0   frame  uint32
 *   4   t_min  int16      temperature of level 0, centi-C
 *   6   step   uint16     C per level, in 1/10000 C
 *   8   pixels uint8[768] row-major, y then x
 */
#define TM_TYPE_RAW 0x02
#define TM_RAW_SIZE (8 + TM_GRID_SIZE)

/**
 * STATUS, every 10 s and immediately after a command. Health and settings.
 *
 *   0   fw_version  char[12]  NUL-padded
 *   12  ip          uint8[4]
 *   16  rssi        int8      dBm
 *   17  channel     uint8
 *   18  free_heap   uint32
 *   22  min_heap    uint32
 *   26  stack_free  uint16    loop task stack high-water mark, bytes
 *   28  wifi_drops  uint16    disconnects since boot
 *   30  sensor_err  uint16    failed sensor reads since boot
 *   32  frames      uint32    frames processed since boot
 *   36  fps_x100    uint16
 *   38  vdd_x100    uint16    sensor supply, 0.01 V
 *   40  ta          int16
 *   42  last_cmd    uint32    sequence of the last command applied
 *   46  flags       uint8     TM_STATUS_*
 *   47  param_count uint8
 *   48  params      int32[param_count], in TM_PARAM_* order
 */
#define TM_TYPE_STATUS 0x03
#define TM_STATUS_FIXED_SIZE 48
#define TM_FW_VERSION_LEN 12
#define TM_STATUS_SENSOR_OK 0x01
#define TM_STATUS_BACKGROUND_READY 0x02
#define TM_STATUS_SIGNED 0x04

/**
 * OTA_STATUS, node -> edge. Sent on every state change, and about every two
 * seconds while downloading, so a rollout can be watched rather than guessed
 * at.
 *
 *   0   state    uint8     TM_OTA_*
 *   1   percent  uint8     0..100 while downloading
 *   2   error    uint8     TM_OTA_ERR_*
 *   3   reserved uint8
 *   4   image    uint32    first four bytes of the image's SHA-256: which
 *                          image this is about, and after a reboot, which one
 *                          the node is actually running
 */
#define TM_TYPE_OTA_STATUS 0x04
#define TM_OTA_STATUS_SIZE 8

#define TM_OTA_IDLE 0
#define TM_OTA_DOWNLOADING 1
#define TM_OTA_VERIFYING 2
#define TM_OTA_APPLYING 3
#define TM_OTA_REBOOTING 4
#define TM_OTA_CONFIRMED 5   // booted the new image and proved itself healthy
#define TM_OTA_FAILED 6
#define TM_OTA_REVERTED 7    // the new image did not come up; back on the old one

#define TM_OTA_ERR_NONE 0
#define TM_OTA_ERR_HTTP 1     // gateway unreachable, or not 200
#define TM_OTA_ERR_SIZE 2     // fewer or more bytes than promised
#define TM_OTA_ERR_SHA 3      // the image is not the one that was signed for
#define TM_OTA_ERR_FLASH 4    // no OTA partition, or the write failed
#define TM_OTA_ERR_NO_WIFI 5
#define TM_OTA_ERR_BUSY 6     // an update is already running

// --- Downlink (edge -> node) -------------------------------------------------

/**
 * COMMAND. Header uid is the target node; boot/seq/uptime are ignored and sent
 * as zero. Signed with the same key. A node applies a command only if cmd_seq
 * is greater than the last one it applied, which it keeps in flash, so an old
 * command replayed later -- even after a reboot -- does nothing.
 *
 *   0   cmd_seq  uint32    the edge uses unix seconds
 *   4   opcode   uint8     TM_CMD_*
 *   5   arg0     uint8     parameter id for SET_PARAM
 *   6   value    int32
 */
#define TM_TYPE_COMMAND 0x10
#define TM_COMMAND_SIZE 10

#define TM_CMD_SET_PARAM 1        // arg0 = TM_PARAM_*, value
#define TM_CMD_RESET_BACKGROUND 2
#define TM_CMD_IDENTIFY 3         // blink the LED for `value` seconds
#define TM_CMD_REBOOT 4
#define TM_CMD_SAVE_PARAMS 5      // persist current params to flash

/**
 * OTA, edge -> node. "Fetch this image from your gateway and flash it."
 *
 * The node downloads from the address the packet arrived from -- its gateway,
 * which is the only machine on its network it already trusts to reach -- so
 * no URL, host name or DNS is involved. The SHA-256 is what makes the image
 * safe: the packet carrying it is signed with the shared key, and the node
 * refuses anything whose bytes do not hash to this.
 *
 *   0   cmd_seq  uint32    replay rule as COMMAND: must beat the last applied
 *   4   port     uint16    HTTP port on the gateway
 *   6   size     uint32    image bytes
 *   10  sha256   uint8[32]
 *   42  path     char[TM_OTA_PATH_LEN]  NUL-padded, e.g. "/fw/9f3a12....bin"
 */
#define TM_TYPE_OTA 0x11
#define TM_OTA_PATH_LEN 48
#define TM_OTA_SIZE (42 + TM_OTA_PATH_LEN)

/**
 * Tunable parameters, by id. Values are int32 in the unit given. The order is
 * the order STATUS reports them in; append, never reorder.
 */
#define TM_PARAM_MIN_CONTRAST   0  // centi-C: a pixel this much above background is foreground
#define TM_PARAM_MIN_PEAK       1  // centi-C: a blob's peak must reach this
#define TM_PARAM_NOISE_K        2  // x10: foreground also needs > k * pixel noise sigma
#define TM_PARAM_MIN_AREA       3  // px
#define TM_PARAM_MAX_AREA       4  // px
#define TM_PARAM_BG_TAU         5  // frames: background time constant
#define TM_PARAM_BG_FRAMES      6  // frames learned before detecting
#define TM_PARAM_RAW_EVERY      7  // send RAW every N frames, 0 = never
#define TM_PARAM_REFRESH        8  // MLX90640 refresh code: 2 = 1 fps, 3 = 2 fps, 4 = 4 fps
#define TM_PARAM_SPLIT_SEP_X10  9  // px x10: two peaks closer than this are one person
#define TM_PARAM_COUNT         10

#endif // TM_PROTOCOL_H
