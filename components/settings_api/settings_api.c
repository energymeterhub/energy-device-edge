#include "settings_api.h"
#include "config.h"
#include "wifi_mgr.h" // for scanning
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_app_desc.h"
#include "cJSON.h"
#include <stdbool.h>
#include <sys/param.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include "lwip/netdb.h"
#include "lwip/sockets.h"

#include "esp_wifi.h"

static const char *TAG = "settings_api";
static meter_client_handle_t s_meter_client = NULL;
static settings_api_source_apply_fn s_source_apply_callback = NULL;
static bool s_source_apply_in_progress = false;
typedef struct {
    bool include_wifi;
    bool include_source;
    bool validate_meter;
    bool validate_destination;
    const char *success_payload;
} config_save_options_t;

typedef struct {
    device_config_t cfg;
} source_apply_task_args_t;

static esp_err_t validate_meter_connection(
    const char *type,
    const char *host,
    uint16_t port,
    bool skip_connectivity_probe,
    char *error_message,
    size_t error_message_size);

static void set_api_cors_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET,POST,OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
}

static bool ui_should_hide_monitor_sections(void)
{
    return wifi_mgr_is_ap_mode();
}

static bool meter_source_is_configured(const device_config_t *cfg)
{
    if (!cfg) {
        return false;
    }

    return cfg->meter.host[0] != '\0' || cfg->meter.type[0] != '\0';
}

static const char *protocol_label(meter_protocol_t protocol)
{
    switch (protocol) {
        case METER_PROTOCOL_MODBUS_TCP:
            return "Modbus TCP";
        case METER_PROTOCOL_HTTP:
            return "HTTP";
        case METER_PROTOCOL_UNKNOWN:
        default:
            return "Unknown";
    }
}

static esp_err_t validate_destination_config(const device_config_t *cfg,
                                             char *error_message,
                                             size_t error_message_size)
{
    if (!cfg || !error_message || error_message_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    error_message[0] = '\0';

    if (strcmp(cfg->destination.type, DESTINATION_TYPE_NONE) == 0) {
        return ESP_OK;
    }

    if (strcmp(cfg->destination.type, DESTINATION_TYPE_IAMMETER_CLOUD) == 0) {
        return ESP_OK;
    }

    if (strcmp(cfg->destination.type, DESTINATION_TYPE_IAMMETER_LOCAL) == 0) {
        if (cfg->destination.address[0] == '\0') {
            strlcpy(error_message, "Destination Address is required for IAMMETER Local", error_message_size);
            return ESP_ERR_INVALID_ARG;
        }
        return ESP_OK;
    }

    snprintf(error_message, error_message_size, "Unsupported destination type: %s", cfg->destination.type);
    return ESP_ERR_NOT_SUPPORTED;
}

static void delayed_reboot_task(void *param)
{
    ESP_LOGW(TAG, "Rebooting in 2 seconds...");
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
}

void settings_api_set_meter_client(meter_client_handle_t client)
{
    s_meter_client = client;
    ESP_LOGI(TAG, "Meter client set for API access");
}

void settings_api_set_source_apply_callback(settings_api_source_apply_fn callback)
{
    s_source_apply_callback = callback;
}

static void source_apply_task(void *param)
{
    source_apply_task_args_t *args = (source_apply_task_args_t *)param;
    char error_message[160] = {0};

    ESP_LOGI(TAG, "Background source apply started");

    if (args && s_source_apply_callback) {
        esp_err_t err = s_source_apply_callback(&args->cfg, error_message, sizeof(error_message));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Background source apply failed: %s",
                     error_message[0] ? error_message : esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "Background source apply completed");
        }
    }

    if (args) {
        free(args);
    }
    s_source_apply_in_progress = false;
    vTaskDelete(NULL);
}

static esp_err_t schedule_source_apply(const device_config_t *cfg)
{
    if (!cfg || !s_source_apply_callback) {
        return ESP_OK;
    }

    if (s_source_apply_in_progress) {
        return ESP_ERR_INVALID_STATE;
    }
    s_source_apply_in_progress = true;

    source_apply_task_args_t *args = calloc(1, sizeof(source_apply_task_args_t));
    if (!args) {
        s_source_apply_in_progress = false;
        return ESP_ERR_NO_MEM;
    }

    args->cfg = *cfg;

    BaseType_t result = xTaskCreate(source_apply_task, "source_apply", 6144, args, 5, NULL);
    if (result != pdPASS) {
        s_source_apply_in_progress = false;
        free(args);
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t send_json_response(httpd_req_t *req, const char *status, const char *payload)
{
    httpd_resp_set_type(req, "application/json");
    if (status && status[0] != '\0') {
        httpd_resp_set_status(req, status);
    }
    httpd_resp_send(req, payload ? payload : "{}", -1);
    return ESP_OK;
}

static esp_err_t send_json_error(httpd_req_t *req, const char *status, const char *message, const char *fallback)
{
    cJSON *root = cJSON_CreateObject();
    const char *json = NULL;

    if (root) {
        cJSON_AddStringToObject(root, "error", message && message[0] != '\0' ? message : fallback);
        json = cJSON_PrintUnformatted(root);
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, status ? status : "400 Bad Request");
    httpd_resp_send(req, json ? json : fallback, -1);

    if (json) {
        free((void *)json);
    }
    if (root) {
        cJSON_Delete(root);
    }
    return ESP_FAIL;
}

static esp_err_t send_json_status_message(httpd_req_t *req,
                                          const char *http_status,
                                          const char *status_value,
                                          const char *message,
                                          const char *apply_state)
{
    cJSON *root = cJSON_CreateObject();
    char *json = NULL;

    if (!root) {
        return send_json_response(req,
                                  http_status,
                                  "{\"status\":\"ok\",\"message\":\"Request completed\"}");
    }

    cJSON_AddStringToObject(root, "status", status_value && status_value[0] ? status_value : "ok");
    if (message && message[0] != '\0') {
        cJSON_AddStringToObject(root, "message", message);
    }
    if (apply_state && apply_state[0] != '\0') {
        cJSON_AddStringToObject(root, "apply", apply_state);
    }

    json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!json) {
        return send_json_response(req,
                                  http_status,
                                  "{\"status\":\"ok\",\"message\":\"Request completed\"}");
    }

    esp_err_t result = send_json_response(req, http_status, json);
    free(json);
    return result;
}

static esp_err_t read_json_request(httpd_req_t *req, cJSON **out_root)
{
    if (!req || !out_root) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_root = NULL;

    if (req->content_len <= 0) {
        *out_root = cJSON_CreateObject();
        return *out_root ? ESP_OK : ESP_ERR_NO_MEM;
    }

    char buf[1024];
    if (req->content_len >= sizeof(buf)) {
        return ESP_ERR_INVALID_SIZE;
    }

    const int remaining = req->content_len;
    int received = httpd_req_recv(req, buf, remaining);
    if (received <= 0) {
        return received == HTTPD_SOCK_ERR_TIMEOUT ? ESP_ERR_TIMEOUT : ESP_FAIL;
    }

    buf[remaining] = '\0';
    *out_root = cJSON_Parse(buf);
    return *out_root ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

static void load_config_or_defaults(device_config_t *cfg)
{
    if (!cfg) {
        return;
    }

    if (config_load(cfg) != ESP_OK) {
        config_set_defaults(cfg);
    }
}

static void apply_wifi_payload(device_config_t *cfg, cJSON *root)
{
    if (!cfg || !root) {
        return;
    }

    cJSON *wifi = cJSON_GetObjectItem(root, "wifi");
    if (!wifi) {
        return;
    }

    cJSON *ssid = cJSON_GetObjectItem(wifi, "ssid");
    if (ssid && cJSON_IsString(ssid)) {
        strlcpy(cfg->wifi.ssid, ssid->valuestring, sizeof(cfg->wifi.ssid));
    }

    cJSON *pwd = cJSON_GetObjectItem(wifi, "password");
    if (pwd && cJSON_IsString(pwd) && strlen(pwd->valuestring) > 0) {
        strlcpy(cfg->wifi.password, pwd->valuestring, sizeof(cfg->wifi.password));
    }
}

static esp_err_t apply_source_payload(
    device_config_t *cfg,
    cJSON *root,
    char *error_message,
    size_t error_message_size)
{
    if (!cfg || !root || !error_message || error_message_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    error_message[0] = '\0';

    cJSON *meter = cJSON_GetObjectItem(root, "meter");
    if (meter) {
        cJSON *type = cJSON_GetObjectItem(meter, "type");
        if (type && cJSON_IsString(type)) {
            strlcpy(cfg->meter.type, type->valuestring, sizeof(cfg->meter.type));
        }

        cJSON *host = cJSON_GetObjectItem(meter, "host");
        if (host && cJSON_IsString(host)) {
            strlcpy(cfg->meter.host, host->valuestring, sizeof(cfg->meter.host));
        }

        cJSON *port = cJSON_GetObjectItem(meter, "port");
        if (port && cJSON_IsNumber(port)) {
            if (port->valuedouble < 1 || port->valuedouble > 65535) {
                strlcpy(error_message, "Meter Port must be between 1 and 65535", error_message_size);
                return ESP_ERR_INVALID_ARG;
            }
            cfg->meter_port = (uint16_t)port->valueint;
        }
    }

    cJSON *cloud = cJSON_GetObjectItem(root, "cloud");
    if (cloud) {
        cJSON *server = cJSON_GetObjectItem(cloud, "server");
        if (server && cJSON_IsString(server)) {
            strlcpy(cfg->cloud.server, server->valuestring, sizeof(cfg->cloud.server));
            ESP_LOGI(TAG, "Saving cloud.server: %s", cfg->cloud.server);
        }

        cJSON *sn = cJSON_GetObjectItem(cloud, "sn");
        if (sn && cJSON_IsString(sn)) {
            strlcpy(cfg->cloud.sn, sn->valuestring, sizeof(cfg->cloud.sn));
            ESP_LOGI(TAG, "Saving cloud.sn: %s", cfg->cloud.sn);
        }
    }

    cJSON *destination = cJSON_GetObjectItem(root, "destination");
    if (destination) {
        cJSON *type = cJSON_GetObjectItem(destination, "type");
        if (type && cJSON_IsString(type)) {
            strlcpy(cfg->destination.type, type->valuestring, sizeof(cfg->destination.type));
        }

        cJSON *address = cJSON_GetObjectItem(destination, "address");
        if (address && cJSON_IsString(address)) {
            strlcpy(cfg->destination.address, address->valuestring, sizeof(cfg->destination.address));
        }

        cJSON *sn = cJSON_GetObjectItem(destination, "sn");
        if (sn && cJSON_IsString(sn)) {
            strlcpy(cfg->destination.sn, sn->valuestring, sizeof(cfg->destination.sn));
        }
    }

    cJSON *device = cJSON_GetObjectItem(root, "device");
    if (device) {
        cJSON *dname = cJSON_GetObjectItem(device, "device_name");
        if (dname && cJSON_IsString(dname)) {
            strlcpy(cfg->device.device_name, dname->valuestring, sizeof(cfg->device.device_name));
        }
    }

    meter_type_t meter_type = meter_type_from_string(cfg->meter.type);
    if (cfg->meter_port == 0) {
        cfg->meter_port = meter_type_default_port(meter_type);
    }

    return ESP_OK;
}

static esp_err_t handle_config_save(httpd_req_t *req, const config_save_options_t *options)
{
    if (!req || !options) {
        return ESP_ERR_INVALID_ARG;
    }

    set_api_cors_headers(req);

    cJSON *root = NULL;
    esp_err_t err = read_json_request(req, &root);
    if (err == ESP_ERR_INVALID_SIZE) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    if (err == ESP_ERR_TIMEOUT) {
        httpd_resp_send_408(req);
        return ESP_FAIL;
    }
    if (err != ESP_OK || !root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    device_config_t cfg;
    load_config_or_defaults(&cfg);

    if (options->include_wifi) {
        apply_wifi_payload(&cfg, root);
    }

    char validation_error[160];
    if (options->include_source) {
        err = apply_source_payload(&cfg, root, validation_error, sizeof(validation_error));
        if (err != ESP_OK) {
            cJSON_Delete(root);
            return send_json_error(req, "400 Bad Request", validation_error, "{\"error\":\"Invalid source settings\"}");
        }
    }

    cJSON_Delete(root);

    if (options->validate_meter) {
        meter_type_t meter_type = meter_type_from_string(cfg.meter.type);
        const bool skip_meter_probe = false;

        err = validate_meter_connection(
            cfg.meter.type,
            cfg.meter.host,
            cfg.meter_port ? cfg.meter_port : meter_type_default_port(meter_type),
            skip_meter_probe,
            validation_error,
            sizeof(validation_error)
        );
        if (err != ESP_OK) {
            return send_json_error(req, "400 Bad Request", validation_error, "{\"error\":\"Meter validation failed\"}");
        }
    }

    if (options->validate_destination) {
        err = validate_destination_config(&cfg, validation_error, sizeof(validation_error));
        if (err != ESP_OK) {
            return send_json_error(req, "400 Bad Request", validation_error, "{\"error\":\"Destination validation failed\"}");
        }
    }

    err = config_save(&cfg);
    if (err != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    if (options->include_source) {
        return send_json_status_message(
            req,
            NULL,
            "saved",
            "Source settings validated and saved. The device must restart to apply them.",
            "restart_required"
        );
    }

    return send_json_response(req, NULL, options->success_payload ? options->success_payload : "{\"status\":\"ok\"}");
}

static esp_err_t validate_meter_connection(
    const char *type,
    const char *host,
    uint16_t port,
    bool skip_connectivity_probe,
    char *error_message,
    size_t error_message_size)
{
    if (!error_message || error_message_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    error_message[0] = '\0';

    const bool has_type = type && strlen(type) > 0;
    const bool has_host = host && strlen(host) > 0;

    if (!has_type && !has_host) {
        return ESP_OK;
    }

    if (!has_type) {
        strlcpy(error_message, "Meter type is required", error_message_size);
        return ESP_ERR_INVALID_ARG;
    }

    if (!has_host) {
        strlcpy(error_message, "Meter Host is required", error_message_size);
        return ESP_ERR_INVALID_ARG;
    }

    if (strstr(host, "://") || strchr(host, '/')) {
        strlcpy(error_message, "Meter host must not include a scheme or path", error_message_size);
        return ESP_ERR_INVALID_ARG;
    }

    if (strchr(host, ':')) {
        strlcpy(error_message, "Meter host must not include a port; use Meter Port", error_message_size);
        return ESP_ERR_INVALID_ARG;
    }

    if (port == 0) {
        strlcpy(error_message, "Meter Port must be between 1 and 65535", error_message_size);
        return ESP_ERR_INVALID_ARG;
    }

    meter_type_t meter_type = meter_type_from_string(type);
    if (meter_type == METER_TYPE_UNKNOWN) {
        snprintf(error_message, error_message_size, "Unsupported meter type: %s", type);
        return ESP_ERR_NOT_SUPPORTED;
    }

    meter_protocol_t protocol = meter_type_get_protocol(meter_type);
    if (protocol == METER_PROTOCOL_UNKNOWN) {
        snprintf(error_message, error_message_size, "Unsupported protocol for meter type: %s", type);
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (skip_connectivity_probe) {
        return ESP_OK;
    }

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);

    struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM
    };
    struct addrinfo *result = NULL;
    int addr_err = getaddrinfo(host, port_str, &hints, &result);
    if (addr_err != 0 || !result) {
        snprintf(error_message, error_message_size, "Failed to resolve %s device address %s:%u",
                 protocol_label(protocol), host, port);
        if (result) {
            freeaddrinfo(result);
        }
        return ESP_ERR_INVALID_ARG;
    }

    int sock = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (sock < 0) {
        snprintf(error_message, error_message_size, "Failed to create %s socket for %s:%u",
                 protocol_label(protocol), host, port);
        freeaddrinfo(result);
        return ESP_FAIL;
    }

    int flags = fcntl(sock, F_GETFL, 0);
    if (flags >= 0) {
        (void)fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    }

    int connect_err = connect(sock, result->ai_addr, result->ai_addrlen);
    if (connect_err != 0 && errno != EINPROGRESS) {
        snprintf(error_message, error_message_size, "Cannot connect to %s device at %s:%u",
                 protocol_label(protocol), host, port);
        close(sock);
        freeaddrinfo(result);
        return ESP_ERR_TIMEOUT;
    }

    fd_set write_set;
    FD_ZERO(&write_set);
    FD_SET(sock, &write_set);

    struct timeval timeout = {
        .tv_sec = 3,
        .tv_usec = 0
    };

    int select_result = select(sock + 1, NULL, &write_set, NULL, &timeout);
    if (select_result <= 0) {
        snprintf(error_message, error_message_size, "Timed out connecting to %s device at %s:%u",
                 protocol_label(protocol), host, port);
        close(sock);
        freeaddrinfo(result);
        return ESP_ERR_TIMEOUT;
    }

    int socket_error = 0;
    socklen_t socket_error_len = sizeof(socket_error);
    if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &socket_error, &socket_error_len) != 0 || socket_error != 0) {
        snprintf(error_message, error_message_size, "Cannot connect to %s device at %s:%u",
                 protocol_label(protocol), host, port);
        close(sock);
        freeaddrinfo(result);
        return ESP_ERR_TIMEOUT;
    }

    close(sock);
    freeaddrinfo(result);
    return ESP_OK;
}

/* GET /api/meter/data */
static esp_err_t api_meter_data_get_handler(httpd_req_t *req)
{
    set_api_cors_headers(req);

    if (!s_meter_client) {
        device_config_t cfg;
        load_config_or_defaults(&cfg);

        const char *resp = meter_source_is_configured(&cfg)
            ? "{\"error\":\"Failed to read meter data\"}"
            : "{\"error\":\"Meter not configured\"}";
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, resp, strlen(resp));
        return ESP_OK;
    }

    meter_data_t data;
    esp_err_t err = meter_client_read(s_meter_client, &data);
    
    if (err != ESP_OK) {
        const char *resp = "{\"error\":\"Failed to read meter data\"}";
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, resp, strlen(resp));
        return ESP_OK;
    }

    char *json_str = meter_data_format_json(&data, NULL, NULL);
    if (!json_str) {
        const char *resp = "{\"error\":\"Failed to format data\"}";
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, resp, strlen(resp));
        return ESP_OK;
    }
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    
    cJSON_free(json_str);
    return ESP_OK;
}

/* POST /api/restart */
static esp_err_t api_restart_post_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Restart requested via API");
    set_api_cors_headers(req);
    httpd_resp_send(req, "{\"status\":\"restarting\"}", -1);
    
    xTaskCreate(delayed_reboot_task, "reboot_task", 2048, NULL, 5, NULL);
    return ESP_OK;
}

/* POST /api/factory-reset */
static esp_err_t api_factory_reset_post_handler(httpd_req_t *req)
{
    ESP_LOGW(TAG, "Factory reset requested via API");
    set_api_cors_headers(req);

    esp_err_t err = config_factory_reset();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Factory reset failed: %s", esp_err_to_name(err));
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_send(req, "{\"status\":\"factory_resetting\"}", -1);
    xTaskCreate(delayed_reboot_task, "factory_reset_task", 2048, NULL, 5, NULL);
    return ESP_OK;
}

/* GET /api/config */
static esp_err_t api_config_get_handler(httpd_req_t *req)
{
    set_api_cors_headers(req);

    device_config_t cfg;
    config_load(&cfg);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "version", cfg.version);

    const esp_app_desc_t *app_desc = esp_app_get_description();

    cJSON *wifi = cJSON_CreateObject();
    cJSON_AddStringToObject(wifi, "ssid", cfg.wifi.ssid);
    cJSON_AddStringToObject(wifi, "password", "");
    cJSON_AddItemToObject(root, "wifi", wifi);

    cJSON_AddBoolToObject(root, "ap_mode", wifi_mgr_is_ap_mode());
    cJSON_AddBoolToObject(root, "hide_monitor_ui", ui_should_hide_monitor_sections());

    cJSON *meter = cJSON_CreateObject();
    cJSON_AddStringToObject(meter, "type", cfg.meter.type);
    cJSON_AddStringToObject(meter, "host", cfg.meter.host);
    cJSON_AddNumberToObject(
        meter,
        "port",
        cfg.meter_port ? cfg.meter_port : meter_type_default_port(meter_type_from_string(cfg.meter.type))
    );
    cJSON_AddItemToObject(root, "meter", meter);

    cJSON *cloud = cJSON_CreateObject();
    cJSON_AddStringToObject(cloud, "server", cfg.cloud.server);
    cJSON_AddStringToObject(cloud, "sn", cfg.cloud.sn);
    cJSON_AddItemToObject(root, "cloud", cloud);

    cJSON *destination = cJSON_CreateObject();
    cJSON_AddStringToObject(destination, "type", cfg.destination.type);
    cJSON_AddStringToObject(destination, "address", cfg.destination.address);
    cJSON_AddStringToObject(destination, "sn", cfg.destination.sn);
    cJSON_AddItemToObject(root, "destination", destination);
    ESP_LOGI(TAG, "Loading destination.type: %s, destination.address: %s, destination.sn: %s",
             cfg.destination.type,
             cfg.destination.address,
             cfg.destination.sn);

    cJSON *device = cJSON_CreateObject();
    cJSON_AddStringToObject(device, "device_name", cfg.device.device_name);
    cJSON_AddItemToObject(root, "device", device);

    cJSON *firmware = cJSON_CreateObject();
    cJSON_AddStringToObject(firmware, "project", app_desc->project_name);
    cJSON_AddStringToObject(firmware, "version", app_desc->version);
    cJSON_AddStringToObject(firmware, "idf", app_desc->idf_ver);
    cJSON_AddStringToObject(firmware, "build_date", app_desc->date);
    cJSON_AddStringToObject(firmware, "build_time", app_desc->time);
    cJSON_AddItemToObject(root, "firmware", firmware);

    const char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));

    free((void *)json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

/* POST /api/config */
static esp_err_t api_config_post_handler(httpd_req_t *req)
{
    const config_save_options_t options = {
        .include_wifi = true,
        .include_source = true,
        .validate_meter = true,
        .validate_destination = true,
        .success_payload = "{\"status\":\"ok\"}"
    };
    return handle_config_save(req, &options);
}

/* POST /api/network */
static esp_err_t api_network_post_handler(httpd_req_t *req)
{
    const config_save_options_t options = {
        .include_wifi = true,
        .include_source = false,
        .validate_meter = false,
        .validate_destination = false,
        .success_payload = "{\"status\":\"ok\"}"
    };
    return handle_config_save(req, &options);
}

/* POST /api/source */
static esp_err_t api_source_post_handler(httpd_req_t *req)
{
    const config_save_options_t options = {
        .include_wifi = false,
        .include_source = true,
        .validate_meter = true,
        .validate_destination = true,
        .success_payload = "{\"status\":\"ok\"}"
    };
    return handle_config_save(req, &options);
}

/* POST /api/ota */
static esp_err_t api_ota_post_handler(httpd_req_t *req)
{
    set_api_cors_headers(req);

    esp_ota_handle_t update_handle = 0 ;
    const esp_partition_t *update_partition = NULL;
    esp_err_t err;
    char buf[1024];
    int received;
    int remaining = req->content_len;

    ESP_LOGI(TAG, "Starting OTA update, size: %d", remaining);

    update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        ESP_LOGE(TAG, "No OTA partition found");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &update_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed (%s)", esp_err_to_name(err));
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    while (remaining > 0) {
        received = httpd_req_recv(req, buf, MIN(remaining, sizeof(buf)));
        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            ESP_LOGE(TAG, "OTA Download failed");
            esp_ota_end(update_handle);
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }

        err = esp_ota_write(update_handle, buf, received);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed (%s)", esp_err_to_name(err));
            esp_ota_end(update_handle);
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        remaining -= received;
    }

    err = esp_ota_end(update_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed (%s)", esp_err_to_name(err));
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed (%s)", esp_err_to_name(err));
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA Success, rebooting...");
    httpd_resp_send(req, "{\"status\":\"success\"}", -1);
    
    xTaskCreate(delayed_reboot_task, "reboot_task", 2048, NULL, 5, NULL);
    return ESP_OK;
}

/* GET /api/wifi/scan */
static esp_err_t api_wifi_scan_handler(httpd_req_t *req)
{
    set_api_cors_headers(req);

    wifi_ap_record_t *ap_list = NULL;
    uint16_t count = 0;

    esp_err_t err = wifi_mgr_scan(&ap_list, &count);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Scan failed: %d", err);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_CreateArray();
    for (int i = 0; i < count; i++) {
        if (strlen((char *)ap_list[i].ssid) == 0) continue;

        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "ssid", (char *)ap_list[i].ssid);
        cJSON_AddNumberToObject(item, "rssi", ap_list[i].rssi);
        cJSON_AddNumberToObject(item, "auth", ap_list[i].authmode);
        cJSON_AddItemToArray(root, item);
    }
    
    if (ap_list) free(ap_list);

    const char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    
    free((void *)json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

static const httpd_uri_t config_get_uri = {
    .uri       = "/api/config",
    .method    = HTTP_GET,
    .handler   = api_config_get_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t config_post_uri = {
    .uri       = "/api/config",
    .method    = HTTP_POST,
    .handler   = api_config_post_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t network_post_uri = {
    .uri       = "/api/network",
    .method    = HTTP_POST,
    .handler   = api_network_post_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t source_post_uri = {
    .uri       = "/api/source",
    .method    = HTTP_POST,
    .handler   = api_source_post_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t wifi_scan_uri = {
    .uri       = "/api/wifi/scan",
    .method    = HTTP_GET,
    .handler   = api_wifi_scan_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t ota_post_uri = {
    .uri       = "/api/ota",
    .method    = HTTP_POST,
    .handler   = api_ota_post_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t meter_data_get_uri = {
    .uri       = "/api/meter/data",
    .method    = HTTP_GET,
    .handler   = api_meter_data_get_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t restart_post_uri = {
    .uri       = "/api/restart",
    .method    = HTTP_POST,
    .handler   = api_restart_post_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t factory_reset_post_uri = {
    .uri       = "/api/factory-reset",
    .method    = HTTP_POST,
    .handler   = api_factory_reset_post_handler,
    .user_ctx  = NULL
};

void settings_api_register(httpd_handle_t server)
{
    if (!server) return;
    httpd_register_uri_handler(server, &config_get_uri);
    httpd_register_uri_handler(server, &config_post_uri);
    httpd_register_uri_handler(server, &network_post_uri);
    httpd_register_uri_handler(server, &source_post_uri);
    httpd_register_uri_handler(server, &wifi_scan_uri);
    httpd_register_uri_handler(server, &ota_post_uri);
    httpd_register_uri_handler(server, &meter_data_get_uri);
    httpd_register_uri_handler(server, &restart_post_uri);
    httpd_register_uri_handler(server, &factory_reset_post_uri);
}
