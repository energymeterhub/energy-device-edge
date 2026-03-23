#include "meter_client.h"
#include "meter_client_internal.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <strings.h>

static const char *TAG = "meter_client";

// Driver registry, statically declared at build time.
static const meter_driver_registry_t driver_registry[] = {
    {METER_TYPE_IAMMETER_WEM3080T, "IAMMETER_WEM3080T", &iammeter_driver_ops},
    {METER_TYPE_SHELLY_3EM,        "SHELLY_3EM",        &shelly_3em_driver_ops},
};

static const size_t driver_registry_size = sizeof(driver_registry) / sizeof(driver_registry[0]);

static const char *firmware_version_string(void)
{
    const esp_app_desc_t *app_desc = esp_app_get_description();
    return (app_desc && app_desc->version[0] != '\0') ? app_desc->version : "0.0.0";
}

meter_type_t meter_type_from_string(const char *type_str)
{
    if (!type_str) {
        return METER_TYPE_UNKNOWN;
    }

    if (strcasecmp(type_str, "IAMMETER") == 0 ||
        strcasecmp(type_str, "IAMMETER_WEM3080T") == 0) {
        return METER_TYPE_IAMMETER_WEM3080T;
    }

    if (strcasecmp(type_str, "SHELLY") == 0 ||
        strcasecmp(type_str, "SHELLY_3EM") == 0 ||
        strcasecmp(type_str, "SHELLY_PRO_3EM") == 0) {
        return METER_TYPE_SHELLY_3EM;
    }
    
    for (size_t i = 0; i < driver_registry_size; i++) {
        if (strcasecmp(type_str, driver_registry[i].name) == 0) {
            return driver_registry[i].type;
        }
    }
    
    ESP_LOGW(TAG, "Unknown meter type string: %s", type_str);
    return METER_TYPE_UNKNOWN;
}

const char* meter_type_to_string(meter_type_t type)
{
    for (size_t i = 0; i < driver_registry_size; i++) {
        if (driver_registry[i].type == type) {
            return driver_registry[i].name;
        }
    }
    return "UNKNOWN";
}

meter_protocol_t meter_type_get_protocol(meter_type_t type)
{
    switch (type) {
        case METER_TYPE_IAMMETER_WEM3080T:
            return METER_PROTOCOL_MODBUS_TCP;
        case METER_TYPE_SHELLY_3EM:
            return METER_PROTOCOL_HTTP;
        case METER_TYPE_UNKNOWN:
        case METER_TYPE_MAX:
        default:
            return METER_PROTOCOL_UNKNOWN;
    }
}

uint16_t meter_type_default_port(meter_type_t type)
{
    switch (type) {
        case METER_TYPE_IAMMETER_WEM3080T:
            return 502;
        case METER_TYPE_SHELLY_3EM:
            return 80;
        case METER_TYPE_UNKNOWN:
        case METER_TYPE_MAX:
        default:
            return 0;
    }
}

static const meter_driver_ops_t* get_driver_ops(meter_type_t type)
{
    for (size_t i = 0; i < driver_registry_size; i++) {
        if (driver_registry[i].type == type) {
            return driver_registry[i].ops;
        }
    }
    return NULL;
}

meter_client_handle_t meter_client_create(const char *type_str, 
                                           const meter_client_config_t *config)
{
    if (!type_str || !config) {
        ESP_LOGE(TAG, "Invalid parameters");
        return NULL;
    }
    
    meter_type_t type = meter_type_from_string(type_str);
    if (type == METER_TYPE_UNKNOWN) {
        ESP_LOGE(TAG, "Unknown meter type: %s", type_str);
        return NULL;
    }
    
    const meter_driver_ops_t *ops = get_driver_ops(type);
    if (!ops) {
        ESP_LOGE(TAG, "No driver found for type: %s", type_str);
        return NULL;
    }
    
    meter_client_t *client = calloc(1, sizeof(meter_client_t));
    if (!client) {
        ESP_LOGE(TAG, "Failed to allocate client");
        return NULL;
    }
    
    client->type = type;
    client->ops = ops;
    
    // Call driver initialization.
    esp_err_t err = ops->init(config, &client->driver_ctx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Driver init failed: %s", esp_err_to_name(err));
        free(client);
        return NULL;
    }
    
    ESP_LOGI(TAG, "Created meter client: %s (type=%d)", type_str, type);
    return client;
}

esp_err_t meter_client_read(meter_client_handle_t handle, meter_data_t *data)
{
    if (!handle || !data) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!handle->ops || !handle->ops->read) {
        ESP_LOGE(TAG, "Driver read function not available");
        return ESP_ERR_NOT_SUPPORTED;
    }
    
    memset(data, 0, sizeof(meter_data_t));
    data->type = handle->type;
    
    esp_err_t err = handle->ops->read(handle->driver_ctx, data);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "Driver read failed: %s", esp_err_to_name(err));
    }
    
    return err;
}

void meter_client_destroy(meter_client_handle_t handle)
{
    if (!handle) {
        return;
    }
    
    if (handle->ops && handle->ops->deinit) {
        esp_err_t err = handle->ops->deinit(handle->driver_ctx);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Driver deinit failed: %s", esp_err_to_name(err));
        }
    }
    
    ESP_LOGI(TAG, "Destroyed meter client (type=%s)", meter_type_to_string(handle->type));
    free(handle);
}

// Add phase data to JSON array (helper function)
static void add_phase_data_to_json(cJSON *array, const meter_phase_data_t *phase, float frequency)
{
    cJSON_AddItemToArray(array, cJSON_CreateNumber(round(phase->voltage * 10) / 10));
    cJSON_AddItemToArray(array, cJSON_CreateNumber(round(phase->current * 100) / 100));
    cJSON_AddItemToArray(array, cJSON_CreateNumber(round((double)phase->active_power)));
    cJSON_AddItemToArray(array, cJSON_CreateNumber(round(phase->forward_energy * 1000) / 1000));
    cJSON_AddItemToArray(array, cJSON_CreateNumber(round(phase->reverse_energy * 1000) / 1000));
    cJSON_AddItemToArray(array, cJSON_CreateNumber(round(frequency * 100) / 100));
    cJSON_AddItemToArray(array, cJSON_CreateNumber(round(phase->power_factor * 100) / 100));
}

char* meter_data_format_json(const meter_data_t *data, const char *mac, const char *sn)
{
    if (!data) {
        return NULL;
    }

    // Get MAC address if not provided
    char mac_str[13];
    if (mac && strlen(mac) == 12) {
        strncpy(mac_str, mac, sizeof(mac_str));
    } else {
        uint8_t mac_bytes[6];
        esp_read_mac(mac_bytes, ESP_MAC_WIFI_STA);
        snprintf(mac_str, sizeof(mac_str), "%02X%02X%02X%02X%02X%02X", 
                 mac_bytes[0], mac_bytes[1], mac_bytes[2], 
                 mac_bytes[3], mac_bytes[4], mac_bytes[5]);
    }

    // Use configured SN or fall back to MAC
    const char *upload_sn = (sn && strlen(sn) > 0) ? sn : mac_str;

    // Create JSON structure
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }

    cJSON_AddStringToObject(root, "method", "uploadsn");
    cJSON_AddStringToObject(root, "mac", mac_str);
    cJSON_AddStringToObject(root, "version", firmware_version_string());
    cJSON_AddStringToObject(root, "server", "em");
    cJSON_AddStringToObject(root, "SN", upload_sn);

    // Create Datas array with three phases
    cJSON *datas = cJSON_CreateArray();
    if (!datas) {
        cJSON_Delete(root);
        return NULL;
    }

    cJSON *phase_a = cJSON_CreateArray();
    add_phase_data_to_json(phase_a, &data->phase_a, data->frequency);
    cJSON_AddItemToArray(datas, phase_a);
    
    cJSON *phase_b = cJSON_CreateArray();
    add_phase_data_to_json(phase_b, &data->phase_b, data->frequency);
    cJSON_AddItemToArray(datas, phase_b);
    
    cJSON *phase_c = cJSON_CreateArray();
    add_phase_data_to_json(phase_c, &data->phase_c, data->frequency);
    cJSON_AddItemToArray(datas, phase_c);
    
    cJSON_AddItemToObject(root, "Datas", datas);

    // Convert to string
    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    return json_str;
}
