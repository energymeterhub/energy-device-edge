#include "cloud_uploader.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include <string.h>

static const char *TAG = "cloud_uploader";

#define UPLOAD_PATH "/api/v1/sensor/uploadsensor"
#define MAX_URL_LEN 256
#define MAX_JSON_LEN 2048
#define MAX_RESPONSE_LEN 512

// HTTP response buffer
typedef struct {
    char buffer[MAX_RESPONSE_LEN];
    int len;
} http_response_t;

// Cloud uploader context
typedef struct {
    char server_url[MAX_URL_LEN];
    char sn[64];
    uint32_t upload_interval_ms;
    meter_client_handle_t meter_client;
    TaskHandle_t task_handle;
    bool running;
} cloud_uploader_ctx_t;

static cloud_uploader_ctx_t *s_ctx = NULL;

// HTTP event handler
static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_response_t *response = (http_response_t *)evt->user_data;
    
    switch (evt->event_id) {
        case HTTP_EVENT_ERROR:
            ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
            break;
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
            break;
        case HTTP_EVENT_HEADER_SENT:
            ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
            break;
        case HTTP_EVENT_ON_HEADER:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
            break;
        case HTTP_EVENT_ON_DATA:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
            if (response && evt->data_len > 0) {
                // Append to response buffer
                int copy_len = evt->data_len;
                if (response->len + copy_len >= MAX_RESPONSE_LEN) {
                    copy_len = MAX_RESPONSE_LEN - response->len - 1;
                }
                if (copy_len > 0) {
                    memcpy(response->buffer + response->len, evt->data, copy_len);
                    response->len += copy_len;
                    response->buffer[response->len] = '\0';
                }
            }
            break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
            break;
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGD(TAG, "HTTP_EVENT_DISCONNECTED");
            break;
        case HTTP_EVENT_REDIRECT:
            ESP_LOGD(TAG, "HTTP_EVENT_REDIRECT");
            break;
    }
    return ESP_OK;
}

// Upload meter data to cloud
static esp_err_t upload_meter_data(meter_client_handle_t meter_client, 
                                    const char *server_url, 
                                    const char *sn)
{
    if (!meter_client || !server_url || strlen(server_url) == 0) {
        ESP_LOGE(TAG, "Invalid parameters");
        return ESP_ERR_INVALID_ARG;
    }

    // Read meter data
    meter_data_t data;
    esp_err_t err = meter_client_read(meter_client, &data);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read meter data: %s", esp_err_to_name(err));
        return err;
    }

    // Format JSON using shared function
    char *json_str = meter_data_format_json(&data, NULL, sn);
    if (!json_str) {
        ESP_LOGE(TAG, "Failed to format JSON");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Uploading data: %s", json_str);

    // Construct full URL
    char full_url[MAX_URL_LEN];
    snprintf(full_url, sizeof(full_url), "%s%s", server_url, UPLOAD_PATH);

    // Prepare response buffer
    http_response_t response = {0};

    // Configure HTTP client
    esp_http_client_config_t config = {
        .url = full_url,
        .method = HTTP_METHOD_POST,
        .event_handler = http_event_handler,
        .user_data = &response,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,  // Use built-in certificate bundle
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        cJSON_free(json_str);
        return ESP_FAIL;
    }

    // Set headers
    esp_http_client_set_header(client, "Content-Type", "application/json");

    // Set POST data
    esp_http_client_set_post_field(client, json_str, strlen(json_str));

    // Perform HTTP request
    err = esp_http_client_perform(client);
    
    if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "Upload complete, status=%d", status_code);
        
        // Print server response
        if (response.len > 0) {
            ESP_LOGI(TAG, "Server response: %s", response.buffer);
            
            // Parse JSON response to check success
            cJSON *resp_json = cJSON_Parse(response.buffer);
            if (resp_json) {
                cJSON *successful = cJSON_GetObjectItem(resp_json, "successful");
                cJSON *message = cJSON_GetObjectItem(resp_json, "message");
                
                if (cJSON_IsBool(successful) && cJSON_IsTrue(successful)) {
                    ESP_LOGI(TAG, "Server confirmed upload success");
                    err = ESP_OK;
                } else {
                    const char *msg = (message && cJSON_IsString(message)) ? message->valuestring : "Unknown error";
                    ESP_LOGW(TAG, "Server rejected upload: %s", msg);
                    err = ESP_FAIL;
                }
                cJSON_Delete(resp_json);
            } else {
                ESP_LOGW(TAG, "Failed to parse server response");
                // Still treat 2xx as success if we can't parse JSON
                err = (status_code >= 200 && status_code < 300) ? ESP_OK : ESP_FAIL;
            }
        } else {
            ESP_LOGW(TAG, "No response body received");
            err = (status_code >= 200 && status_code < 300) ? ESP_OK : ESP_FAIL;
        }
    } else {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
    }

    // Cleanup
    esp_http_client_cleanup(client);
    cJSON_free(json_str);

    return err;
}

// Uploader task
static void cloud_uploader_task(void *arg)
{
    cloud_uploader_ctx_t *ctx = (cloud_uploader_ctx_t *)arg;
    
    ESP_LOGI(TAG, "Cloud uploader task started, interval=%lu ms", ctx->upload_interval_ms);

    while (ctx->running) {
        // Upload data
        esp_err_t err = upload_meter_data(ctx->meter_client, ctx->server_url, ctx->sn);
        
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Data uploaded successfully");
        } else {
            ESP_LOGW(TAG, "Upload failed: %s", esp_err_to_name(err));
        }

        if (!ctx->running) {
            break;
        }

        // Wait for next upload interval, but allow stop requests to wake the task immediately.
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(ctx->upload_interval_ms));
    }

    ESP_LOGI(TAG, "Cloud uploader task stopped");
    ctx->task_handle = NULL;
    vTaskDelete(NULL);
}

esp_err_t cloud_uploader_init(const cloud_uploader_config_t *config)
{
    if (!config || !config->server_url) {
        ESP_LOGE(TAG, "Invalid config");
        return ESP_ERR_INVALID_ARG;
    }

    if (s_ctx) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    s_ctx = calloc(1, sizeof(cloud_uploader_ctx_t));
    if (!s_ctx) {
        ESP_LOGE(TAG, "Failed to allocate context");
        return ESP_ERR_NO_MEM;
    }

    strncpy(s_ctx->server_url, config->server_url, sizeof(s_ctx->server_url) - 1);
    if (config->sn) {
        strncpy(s_ctx->sn, config->sn, sizeof(s_ctx->sn) - 1);
    }
    s_ctx->upload_interval_ms = config->upload_interval_ms > 0 ? 
                                 config->upload_interval_ms : 60000; // Default 60s

    ESP_LOGI(TAG, "Cloud uploader initialized: %s (interval=%lu ms)", 
             s_ctx->server_url, s_ctx->upload_interval_ms);

    return ESP_OK;
}

esp_err_t cloud_uploader_start(meter_client_handle_t meter_client)
{
    if (!s_ctx) {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (!meter_client) {
        ESP_LOGE(TAG, "Invalid meter client");
        return ESP_ERR_INVALID_ARG;
    }

    if (s_ctx->running) {
        ESP_LOGW(TAG, "Already running");
        return ESP_OK;
    }

    s_ctx->meter_client = meter_client;
    s_ctx->running = true;

    BaseType_t ret = xTaskCreate(
        cloud_uploader_task,
        "cloud_upload",
        8192,  // Increased from 4096 to prevent stack overflow with JSON formatting
        s_ctx,
        5,
        &s_ctx->task_handle
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create task");
        s_ctx->running = false;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Cloud uploader started");
    return ESP_OK;
}

esp_err_t cloud_uploader_stop(void)
{
    if (!s_ctx) {
        return ESP_OK;
    }

    if (!s_ctx->running) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Stopping cloud uploader...");
    s_ctx->running = false;

    if (s_ctx->task_handle) {
        xTaskNotifyGive(s_ctx->task_handle);
    }

    // Wait for task to finish
    int timeout = 150; // 15 seconds
    while (s_ctx->task_handle && timeout > 0) {
        vTaskDelay(pdMS_TO_TICKS(100));
        timeout--;
    }

    if (s_ctx->task_handle) {
        ESP_LOGW(TAG, "Task did not stop gracefully");
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(TAG, "Cloud uploader stopped");
    return ESP_OK;
}

esp_err_t cloud_uploader_deinit(void)
{
    if (!s_ctx) {
        return ESP_OK;
    }

    esp_err_t err = cloud_uploader_stop();
    if (err != ESP_OK) {
        return err;
    }

    free(s_ctx);
    s_ctx = NULL;

    ESP_LOGI(TAG, "Cloud uploader deinitialized");
    return ESP_OK;
}

esp_err_t cloud_uploader_upload_once(meter_client_handle_t meter_client,
                                      const char *server_url,
                                      const char *sn)
{
    if (!meter_client || !server_url) {
        ESP_LOGE(TAG, "Invalid parameters");
        return ESP_ERR_INVALID_ARG;
    }

    return upload_meter_data(meter_client, server_url, sn);
}
