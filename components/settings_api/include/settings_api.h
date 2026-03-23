#pragma once
#include <esp_err.h>
#include <esp_http_server.h>
#include "config.h"
#include "meter_client.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef esp_err_t (*settings_api_source_apply_fn)(
    const device_config_t *cfg,
    char *error_message,
    size_t error_message_size);

// Register configuration-related API handlers on the web server.
void settings_api_register(httpd_handle_t server);

// Set the meter client instance used by the API layer.
void settings_api_set_meter_client(meter_client_handle_t client);

// Set a callback used to hot-apply source configuration after saving.
void settings_api_set_source_apply_callback(settings_api_source_apply_fn callback);

#ifdef __cplusplus
}
#endif
