#pragma once

#include "meter_client.h"

/**
 * @brief Device driver operation table.
 */
typedef struct {
    esp_err_t (*init)(const meter_client_config_t *config, void **driver_ctx);
    esp_err_t (*read)(void *driver_ctx, meter_data_t *data);
    esp_err_t (*deinit)(void *driver_ctx);
} meter_driver_ops_t;

/**
 * @brief Driver registry entry.
 */
typedef struct {
    meter_type_t type;
    const char *name;
    const meter_driver_ops_t *ops;
} meter_driver_registry_t;

/**
 * @brief Meter client instance.
 */
typedef struct meter_client_t {
    meter_type_t type;
    const meter_driver_ops_t *ops;
    void *driver_ctx;  // driver-specific private context
} meter_client_t;

// Driver operation tables exposed by each implementation.
extern const meter_driver_ops_t iammeter_driver_ops;
extern const meter_driver_ops_t fronius_sunspec_driver_ops;
extern const meter_driver_ops_t shelly_3em_driver_ops;
