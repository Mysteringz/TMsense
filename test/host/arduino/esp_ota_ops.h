#pragma once
#include <cstdint>
#define ESP_OK 0
#define ESP_PARTITION_SUBTYPE_APP_OTA_0 16
#define ESP_PARTITION_SUBTYPE_APP_OTA_1 17
struct esp_partition_t { uint8_t subtype; };
inline esp_partition_t host_slots[2] = {{16}, {17}};
inline int host_running_slot = 0, host_boot_slot = 0;
inline bool host_boot_write_fail = false;
inline int host_valid_calls = 0;
inline bool host_valid_fail = false;
inline const esp_partition_t* esp_ota_get_running_partition() { return &host_slots[host_running_slot]; }
inline const esp_partition_t* esp_ota_get_next_update_partition(void*) { return &host_slots[1 - host_running_slot]; }
inline int esp_ota_set_boot_partition(const esp_partition_t* p) {
    if (host_boot_write_fail) return -1;
    host_boot_slot = p->subtype - 16; return ESP_OK;
}
inline int esp_ota_mark_app_valid_cancel_rollback() { ++host_valid_calls; return host_valid_fail ? -1 : ESP_OK; }
