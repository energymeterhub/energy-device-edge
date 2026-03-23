#pragma once

#include "esp_err.h"
#include "meter_client.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Cloud uploader configuration
 */
typedef struct {
    const char *server_url;     // Cloud server URL (e.g., "https://www.iammeter.com")
    const char *sn;             // Device SN (or NULL to use MAC)
    uint32_t upload_interval_ms; // Upload interval in milliseconds
} cloud_uploader_config_t;

/**
 * @brief Initialize cloud uploader
 * 
 * @param config Cloud uploader configuration
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t cloud_uploader_init(const cloud_uploader_config_t *config);

/**
 * @brief Start cloud uploader task
 * 
 * This will start a background task that periodically uploads meter data
 * 
 * @param meter_client Meter client handle to read data from
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t cloud_uploader_start(meter_client_handle_t meter_client);

/**
 * @brief Stop cloud uploader task
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t cloud_uploader_stop(void);

/**
 * @brief Deinitialize cloud uploader
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t cloud_uploader_deinit(void);

/**
 * @brief Upload meter data immediately (one-time)
 * 
 * @param meter_client Meter client handle
 * @param server_url Server URL
 * @param sn Device SN (or NULL to use MAC)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t cloud_uploader_upload_once(meter_client_handle_t meter_client,
                                      const char *server_url,
                                      const char *sn);

#ifdef __cplusplus
}
#endif
