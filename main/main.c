#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config.h"
#include "wifi_mgr.h"
#include "webserver.h"
#include "settings_api.h"
#include "meter_client.h"
#include "cloud_uploader.h"

static const char *TAG = "main";

static device_config_t s_cfg;
static meter_client_handle_t s_meter_client = NULL;

#define METER_INIT_RETRY_COUNT 3
#define METER_INIT_RETRY_DELAY_MS 2000

static void print_config_masked(const device_config_t *cfg)
{
    if (!cfg) return;

    char destination_url[128] = {0};
    esp_err_t destination_url_err = config_get_destination_url(cfg, destination_url, sizeof(destination_url));

    ESP_LOGI(TAG, "Device: %s", cfg->device.device_name);
    ESP_LOGI(TAG, "Wi-Fi: %s (password hidden)", cfg->wifi.ssid);
    ESP_LOGI(TAG, "Meter: %s @ %s:%u", cfg->meter.type, cfg->meter.host, cfg->meter_port);
    ESP_LOGI(TAG, "Destination: %s @ %s (SN: %s)",
             cfg->destination.type,
             destination_url_err == ESP_OK ? destination_url : "<disabled>",
             strlen(cfg->destination.sn) > 0 ? cfg->destination.sn : "<use MAC>");
}

static void init_configuration(void)
{
    esp_err_t err = config_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "config_init failed: %s", esp_err_to_name(err));
        return;
    }

    if (config_load(&s_cfg) != ESP_OK) {
        ESP_LOGW(TAG, "No valid config found, resetting to defaults...");
        config_set_defaults(&s_cfg);
        config_save(&s_cfg);
    }

    print_config_masked(&s_cfg);
}

static bool init_wifi(void)
{
    esp_err_t wifi_err = wifi_mgr_start_sta_if_configured(&s_cfg);
    if (wifi_err == ESP_OK) {
        ESP_LOGI(TAG, "STA connected successfully");
        return true;
    }

    ESP_LOGW(TAG, "STA not available, starting SoftAP provisioning mode...");
    if (wifi_mgr_start_ap_force() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start SoftAP");
    }
    return false;
}

static void init_webserver(void)
{
    httpd_handle_t server = webserver_start();
    if (!server) {
        ESP_LOGE(TAG, "Failed to start webserver");
        return;
    }

    settings_api_register(server);
    ESP_LOGI(TAG, "Webserver and API are running");
}

static esp_err_t start_cloud_runtime(
    const device_config_t *cfg,
    meter_client_handle_t meter_client,
    char *error_message,
    size_t error_message_size)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!config_destination_is_enabled(cfg)) {
        return ESP_OK;
    }

    if (!meter_client) {
        if (error_message && error_message_size > 0) {
            strlcpy(error_message, "Source must be active before upload can start", error_message_size);
        }
        return ESP_ERR_INVALID_STATE;
    }

    char destination_url[128] = {0};
    if (config_get_destination_url(cfg, destination_url, sizeof(destination_url)) != ESP_OK) {
        if (error_message && error_message_size > 0) {
            strlcpy(error_message, "Destination URL is not ready", error_message_size);
        }
        return ESP_ERR_INVALID_STATE;
    }

    cloud_uploader_config_t cloud_cfg = {
        .server_url = destination_url,
        .sn = cfg->destination.sn,
        .upload_interval_ms = 60000,
    };

    esp_err_t err = cloud_uploader_init(&cloud_cfg);
    if (err != ESP_OK) {
        if (error_message && error_message_size > 0) {
            snprintf(error_message, error_message_size, "Failed to initialize uploader");
        }
        return err;
    }

    err = cloud_uploader_start(meter_client);
    if (err != ESP_OK) {
        cloud_uploader_deinit();
        if (error_message && error_message_size > 0) {
            snprintf(error_message, error_message_size, "Failed to start uploader");
        }
        return err;
    }

    return ESP_OK;
}

static meter_client_handle_t create_meter_client_from_config(
    const device_config_t *cfg,
    char *error_message,
    size_t error_message_size)
{
    if (!cfg) {
        return NULL;
    }

    if (wifi_mgr_is_ap_mode()) {
        return NULL;
    }

    if (strlen(cfg->meter.host) == 0 || strlen(cfg->meter.type) == 0) {
        return NULL;
    }

    meter_type_t meter_type = meter_type_from_string(cfg->meter.type);
    uint16_t default_meter_port = meter_type_default_port(meter_type);

    meter_client_config_t meter_cfg = {
        .port = cfg->meter_port ? cfg->meter_port : default_meter_port,
        .unit_id = 1,
        .timeout_ms = 5000,
    };
    strncpy(meter_cfg.host, cfg->meter.host, sizeof(meter_cfg.host) - 1);

    meter_client_handle_t client = meter_client_create(cfg->meter.type, &meter_cfg);
    if (!client && error_message && error_message_size > 0) {
        snprintf(error_message, error_message_size, "Failed to connect to source device");
    }

    return client;
}

static esp_err_t reconfigure_runtime_source(
    const device_config_t *cfg,
    char *error_message,
    size_t error_message_size)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }

    if (error_message && error_message_size > 0) {
        error_message[0] = '\0';
    }

    ESP_LOGI(TAG, "Hot-applying source settings: type=%s host=%s port=%u destination=%s",
             cfg->meter.type,
             cfg->meter.host,
             cfg->meter_port,
             cfg->destination.type);

    meter_client_handle_t previous_client = s_meter_client;
    meter_client_handle_t next_client = create_meter_client_from_config(cfg, error_message, error_message_size);

    if ((strlen(cfg->meter.host) > 0 && strlen(cfg->meter.type) > 0) && !wifi_mgr_is_ap_mode() && !next_client) {
        ESP_LOGW(TAG, "Failed to create new source client during hot-apply");
        return ESP_FAIL;
    }

    esp_err_t stop_err = cloud_uploader_stop();
    if (stop_err != ESP_OK && stop_err != ESP_ERR_INVALID_STATE) {
        if (error_message && error_message_size > 0) {
            strlcpy(error_message, "Timed out stopping uploader", error_message_size);
        }
        if (next_client) {
            meter_client_destroy(next_client);
        }
        ESP_LOGW(TAG, "Hot-apply aborted because uploader did not stop cleanly");
        return stop_err;
    }

    esp_err_t deinit_err = cloud_uploader_deinit();
    if (deinit_err != ESP_OK) {
        if (error_message && error_message_size > 0) {
            strlcpy(error_message, "Failed to reset uploader runtime", error_message_size);
        }
        if (next_client) {
            meter_client_destroy(next_client);
        }
        ESP_LOGW(TAG, "Hot-apply aborted because uploader deinit failed");
        return deinit_err;
    }

    s_meter_client = next_client;
    settings_api_set_meter_client(next_client);

    esp_err_t err = start_cloud_runtime(cfg, next_client, error_message, error_message_size);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Hot-apply failed while starting uploader/runtime: %s",
                 error_message && error_message[0] ? error_message : esp_err_to_name(err));
        s_meter_client = previous_client;
        settings_api_set_meter_client(previous_client);

        if (next_client) {
            meter_client_destroy(next_client);
        }

        char rollback_error[160] = {0};
        esp_err_t rollback_err = start_cloud_runtime(&s_cfg, previous_client, rollback_error, sizeof(rollback_error));
        if (rollback_err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to restart previous uploader after rollback: %s",
                     rollback_error[0] ? rollback_error : esp_err_to_name(rollback_err));
        }

        return err;
    }

    if (previous_client) {
        meter_client_destroy(previous_client);
    }

    s_cfg = *cfg;
    ESP_LOGI(TAG, "Source settings hot-applied successfully");
    print_config_masked(&s_cfg);
    return ESP_OK;
}

static void init_meter_and_cloud(void)
{
    settings_api_set_meter_client(NULL);

    if (wifi_mgr_is_ap_mode()) {
        ESP_LOGI(TAG, "SoftAP is active, skip source polling and cloud startup");
        return;
    }

    if (strlen(s_cfg.meter.host) == 0 || strlen(s_cfg.meter.type) == 0) {
        ESP_LOGW(TAG, "Meter is not configured, skipping meter startup");
        return;
    }

    ESP_LOGI(TAG, "Initializing meter client: %s @ %s:%u",
             s_cfg.meter.type, s_cfg.meter.host, s_cfg.meter_port);

    ESP_LOGI(TAG, "Waiting %d ms for network and meter readiness...", METER_INIT_RETRY_DELAY_MS);
    vTaskDelay(pdMS_TO_TICKS(METER_INIT_RETRY_DELAY_MS));

    meter_type_t meter_type = meter_type_from_string(s_cfg.meter.type);
    uint16_t default_meter_port = meter_type_default_port(meter_type);

    meter_client_config_t meter_cfg = {
        .port = s_cfg.meter_port ? s_cfg.meter_port : default_meter_port,
        .unit_id = 1,
        .timeout_ms = 5000,
    };
    strncpy(meter_cfg.host, s_cfg.meter.host, sizeof(meter_cfg.host) - 1);

    for (int attempt = 1; attempt <= METER_INIT_RETRY_COUNT; attempt++) {
        s_meter_client = meter_client_create(s_cfg.meter.type, &meter_cfg);
        if (s_meter_client) {
            break;
        }

        ESP_LOGW(TAG, "Meter client init attempt %d/%d failed", attempt, METER_INIT_RETRY_COUNT);
        if (attempt < METER_INIT_RETRY_COUNT) {
            vTaskDelay(pdMS_TO_TICKS(METER_INIT_RETRY_DELAY_MS));
        }
    }

    if (!s_meter_client) {
        ESP_LOGE(TAG, "Failed to create meter client for type: %s", s_cfg.meter.type);
        ESP_LOGW(TAG, "Continuing without meter data");
        return;
    }

    settings_api_set_meter_client(s_meter_client);
    ESP_LOGI(TAG, "Meter client created successfully");

    char upload_error[160] = {0};
    esp_err_t upload_err = start_cloud_runtime(&s_cfg, s_meter_client, upload_error, sizeof(upload_error));
    if (upload_err == ESP_OK) {
        ESP_LOGI(TAG, "Cloud uploader started successfully");
        return;
    }

    if (config_destination_is_enabled(&s_cfg)) {
        ESP_LOGE(TAG, "Failed to start cloud uploader: %s",
                 upload_error[0] ? upload_error : esp_err_to_name(upload_err));
    } else {
        ESP_LOGW(TAG, "Upload destination is not configured, skipping upload startup");
    }
}

static void handle_ap_recovery(void)
{
    static TickType_t last_attempt = 0;
    const TickType_t retry_interval = pdMS_TO_TICKS(30000);

    if (!wifi_mgr_is_ap_mode()) {
        return;
    }

    if (s_cfg.wifi.ssid[0] == '\0') {
        return;
    }

    TickType_t now = xTaskGetTickCount();
    if (last_attempt != 0 && (now - last_attempt) < retry_interval) {
        return;
    }
    last_attempt = now;

    ESP_LOGI(TAG, "SoftAP recovery mode active, checking configured Wi-Fi...");
    if (wifi_mgr_try_sta_recovery(&s_cfg) == ESP_OK) {
        ESP_LOGI(TAG, "Configured Wi-Fi is reachable again, rebooting device");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "booting energy device edge...");

    init_configuration();
    bool sta_connected = init_wifi();
    init_webserver();
    settings_api_set_source_apply_callback(reconfigure_runtime_source);

    if (sta_connected) {
        init_meter_and_cloud();
    } else {
        settings_api_set_meter_client(NULL);
    }

    ESP_LOGI(TAG, "System initialized. Background tasks running...");

    while (1) {
        handle_ap_recovery();
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
