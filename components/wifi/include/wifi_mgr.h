#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "esp_wifi_types.h"
#include "config.h"

#ifdef __cplusplus
extern "C" {
#endif

// If SSID is empty, start an open SoftAP. Otherwise do nothing.
esp_err_t wifi_mgr_start_ap_if_needed(const device_config_t *cfg);

// Scan nearby access points.
// out_list: allocated wifi_ap_record_t array, caller must free it.
// count: number of returned access points.
esp_err_t wifi_mgr_scan(wifi_ap_record_t **out_list, uint16_t *count);

// Return true when the AP interface is currently active.
bool wifi_mgr_is_ap_mode(void);

// Try to connect STA while keeping AP mode available.
esp_err_t wifi_mgr_try_sta_recovery(const device_config_t *cfg);

#ifdef __cplusplus
}
#endif

esp_err_t wifi_mgr_start_sta_if_configured(const device_config_t *cfg);
esp_err_t wifi_mgr_start_ap_force(void);
