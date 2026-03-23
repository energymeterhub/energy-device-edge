#include "meter_client_internal.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "cJSON.h"
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "shelly_3em_drv";

#define SHELLY_BASE_URL_LEN 192
#define SHELLY_RESPONSE_MAX_LEN 4096

typedef struct {
    char buffer[SHELLY_RESPONSE_MAX_LEN];
    int len;
} shelly_http_response_t;

typedef struct {
    char host[128];
    uint16_t port;
    uint32_t timeout_ms;
    char base_url[SHELLY_BASE_URL_LEN];
    bool initialized;
} shelly_ctx_t;

static esp_err_t shelly_http_event_handler(esp_http_client_event_t *evt)
{
    shelly_http_response_t *response = (shelly_http_response_t *)evt->user_data;

    if (evt->event_id == HTTP_EVENT_ON_DATA && response && evt->data_len > 0) {
        int copy_len = evt->data_len;
        if (response->len + copy_len >= SHELLY_RESPONSE_MAX_LEN) {
            copy_len = SHELLY_RESPONSE_MAX_LEN - response->len - 1;
        }

        if (copy_len > 0) {
            memcpy(response->buffer + response->len, evt->data, copy_len);
            response->len += copy_len;
            response->buffer[response->len] = '\0';
        }
    }

    return ESP_OK;
}

static esp_err_t shelly_http_get_json(const shelly_ctx_t *ctx, const char *path, cJSON **out_json)
{
    if (!ctx || !path || !out_json) {
        return ESP_ERR_INVALID_ARG;
    }

    char url[SHELLY_BASE_URL_LEN];
    if (snprintf(url, sizeof(url), "%s%s", ctx->base_url, path) >= (int)sizeof(url)) {
        return ESP_ERR_INVALID_SIZE;
    }

    shelly_http_response_t response = {0};
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .event_handler = shelly_http_event_handler,
        .user_data = &response,
        .timeout_ms = ctx->timeout_ms ? (int)ctx->timeout_ms : 5000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_perform(client);
    const int status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP GET %s failed: %s", path, esp_err_to_name(err));
        return err;
    }

    if (status_code < 200 || status_code >= 300) {
        ESP_LOGE(TAG, "HTTP GET %s returned status %d", path, status_code);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(response.buffer);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse JSON from %s", path);
        return ESP_FAIL;
    }

    *out_json = root;
    return ESP_OK;
}

static float json_number_value(cJSON *object, const char *key, float fallback)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsNumber(item) ? (float)item->valuedouble : fallback;
}

static float json_number_value_any(cJSON *object, const char *const *keys, size_t key_count, float fallback)
{
    for (size_t i = 0; i < key_count; ++i) {
        cJSON *item = cJSON_GetObjectItemCaseSensitive(object, keys[i]);
        if (cJSON_IsNumber(item)) {
            return (float)item->valuedouble;
        }
    }

    return fallback;
}

static float derive_power_factor(float voltage, float current, float active_power)
{
    if (voltage <= 0.0f || current <= 0.0f) {
        return 0.0f;
    }

    const float apparent_power = voltage * current;
    if (apparent_power <= 0.0f) {
        return 0.0f;
    }

    float power_factor = active_power / apparent_power;
    if (power_factor > 1.0f) {
        power_factor = 1.0f;
    } else if (power_factor < -1.0f) {
        power_factor = -1.0f;
    }

    return power_factor;
}

static esp_err_t parse_shelly_phase(
    cJSON *em_status,
    cJSON *em_data_status,
    const char *voltage_key,
    const char *current_key,
    const char *const *power_keys,
    size_t power_key_count,
    const char *forward_energy_key,
    const char *reverse_energy_key,
    meter_phase_data_t *phase)
{
    if (!cJSON_IsObject(em_status) || !cJSON_IsObject(em_data_status) || !phase) {
        return ESP_ERR_INVALID_ARG;
    }

    phase->voltage = json_number_value(em_status, voltage_key, 0.0f);
    phase->current = json_number_value(em_status, current_key, 0.0f);
    phase->active_power = (int32_t)lroundf(json_number_value_any(em_status, power_keys, power_key_count, 0.0f));
    phase->reactive_power = 0;
    phase->forward_energy = json_number_value(em_data_status, forward_energy_key, 0.0f) / 1000.0f;
    phase->reverse_energy = json_number_value(em_data_status, reverse_energy_key, 0.0f) / 1000.0f;
    phase->power_factor = derive_power_factor(phase->voltage, phase->current, (float)phase->active_power);

    return ESP_OK;
}

static esp_err_t shelly_3em_init(const meter_client_config_t *config, void **driver_ctx)
{
    if (!config || !driver_ctx) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!config->host[0]) {
        ESP_LOGE(TAG, "Host address is empty");
        return ESP_ERR_INVALID_ARG;
    }

    shelly_ctx_t *ctx = calloc(1, sizeof(shelly_ctx_t));
    if (!ctx) {
        return ESP_ERR_NO_MEM;
    }

    strlcpy(ctx->host, config->host, sizeof(ctx->host));
    ctx->port = config->port ? config->port : 80;
    ctx->timeout_ms = config->timeout_ms ? config->timeout_ms : 5000;

    if (snprintf(ctx->base_url, sizeof(ctx->base_url), "http://%s:%u", ctx->host, ctx->port) >=
        (int)sizeof(ctx->base_url)) {
        free(ctx);
        return ESP_ERR_INVALID_SIZE;
    }

    cJSON *identity = NULL;
    esp_err_t err = shelly_http_get_json(ctx, "/rpc/EM.GetStatus?id=0", &identity);
    if (err != ESP_OK) {
        free(ctx);
        return err;
    }

    cJSON *id = cJSON_GetObjectItemCaseSensitive(identity, "id");
    if (!cJSON_IsNumber(id) || id->valueint != 0) {
        ESP_LOGW(TAG, "Unexpected Shelly Pro 3EM EM status payload");
    }

    cJSON_Delete(identity);
    ctx->initialized = true;
    *driver_ctx = ctx;

    ESP_LOGI(TAG, "shelly 3em driver initialized: %s:%u", ctx->host, ctx->port);
    return ESP_OK;
}

static esp_err_t shelly_3em_read(void *driver_ctx, meter_data_t *data)
{
    if (!driver_ctx || !data) {
        return ESP_ERR_INVALID_ARG;
    }

    shelly_ctx_t *ctx = (shelly_ctx_t *)driver_ctx;
    if (!ctx->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    cJSON *em_status = NULL;
    cJSON *em_data_status = NULL;
    esp_err_t err = shelly_http_get_json(ctx, "/rpc/EM.GetStatus?id=0", &em_status);
    if (err != ESP_OK) {
        return err;
    }

    err = shelly_http_get_json(ctx, "/rpc/EMData.GetStatus?id=0", &em_data_status);
    if (err != ESP_OK) {
        cJSON_Delete(em_status);
        return err;
    }

    static const char *phase_a_power_keys[] = {"a_act_power"};
    static const char *phase_b_power_keys[] = {"b_act_power"};
    static const char *phase_c_power_keys[] = {"c_active_power", "c_act_power"};

    err = parse_shelly_phase(
        em_status,
        em_data_status,
        "a_voltage",
        "a_current",
        phase_a_power_keys,
        sizeof(phase_a_power_keys) / sizeof(phase_a_power_keys[0]),
        "a_total_act_energy",
        "a_total_act_ret_energy",
        &data->phase_a
    );
    if (err == ESP_OK) {
        err = parse_shelly_phase(
            em_status,
            em_data_status,
            "b_voltage",
            "b_current",
            phase_b_power_keys,
            sizeof(phase_b_power_keys) / sizeof(phase_b_power_keys[0]),
            "b_total_act_energy",
            "b_total_act_ret_energy",
            &data->phase_b
        );
    }
    if (err == ESP_OK) {
        err = parse_shelly_phase(
            em_status,
            em_data_status,
            "c_voltage",
            "c_current",
            phase_c_power_keys,
            sizeof(phase_c_power_keys) / sizeof(phase_c_power_keys[0]),
            "c_total_act_energy",
            "c_total_act_ret_energy",
            &data->phase_c
        );
    }
    if (err != ESP_OK) {
        cJSON_Delete(em_status);
        cJSON_Delete(em_data_status);
        return ESP_FAIL;
    }

    data->frequency = json_number_value_any(em_status, (const char *const[]){"freq", "frequency"}, 2, 0.0f);
    data->total_power = (int32_t)lroundf(json_number_value(em_status, "total_act_power",
        (float)(data->phase_a.active_power + data->phase_b.active_power + data->phase_c.active_power)));
    data->total_forward_energy = json_number_value(em_data_status, "total_act",
        data->phase_a.forward_energy * 1000.0f +
        data->phase_b.forward_energy * 1000.0f +
        data->phase_c.forward_energy * 1000.0f) / 1000.0f;
    data->total_reverse_energy = json_number_value(em_data_status, "total_act_ret",
        data->phase_a.reverse_energy * 1000.0f +
        data->phase_b.reverse_energy * 1000.0f +
        data->phase_c.reverse_energy * 1000.0f) / 1000.0f;
    data->valid_phases = 0x07;
    strlcpy(data->model, "Shelly Pro 3EM", sizeof(data->model));

    cJSON_Delete(em_status);
    cJSON_Delete(em_data_status);
    return ESP_OK;
}

static esp_err_t shelly_3em_deinit(void *driver_ctx)
{
    if (!driver_ctx) {
        return ESP_ERR_INVALID_ARG;
    }

    shelly_ctx_t *ctx = (shelly_ctx_t *)driver_ctx;
    ctx->initialized = false;
    free(ctx);
    return ESP_OK;
}

const meter_driver_ops_t shelly_3em_driver_ops = {
    .init = shelly_3em_init,
    .read = shelly_3em_read,
    .deinit = shelly_3em_deinit,
};
