#include "webserver.h"
#include <esp_log.h>
#include <string.h>
#include <fcntl.h>
#include <esp_http_server.h>

static const char *TAG = "webserver";

static void set_cors_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET,POST,OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
}

// References to the embedded HTML asset.
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

// Root page handler.
static esp_err_t root_get_handler(httpd_req_t *req)
{
    set_cors_headers(req);
    httpd_resp_set_type(req, "text/html");
    const size_t index_html_len = (index_html_end - index_html_start);
    httpd_resp_send(req, (const char *)index_html_start, index_html_len);
    return ESP_OK;
}

static esp_err_t options_handler(httpd_req_t *req)
{
    set_cors_headers(req);
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static const httpd_uri_t root_uri = {
    .uri       = "/",
    .method    = HTTP_GET,
    .handler   = root_get_handler,
    .user_ctx  = NULL
};

// Additional URI for /index.html requests.
static const httpd_uri_t index_uri = {
    .uri       = "/index.html",
    .method    = HTTP_GET,
    .handler   = root_get_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t options_uri = {
    .uri       = "/*",
    .method    = HTTP_OPTIONS,
    .handler   = options_handler,
    .user_ctx  = NULL
};

httpd_handle_t webserver_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_open_sockets = 7; // tuned for a resource-constrained device
    config.lru_purge_enable = true;
    config.stack_size = 8192; // extra stack for JSON handling
    config.max_uri_handlers = 12; // allow more API endpoints

    // Enable wildcard URI matching.
    config.uri_match_fn = httpd_uri_match_wildcard;

    httpd_handle_t server = NULL;

    ESP_LOGI(TAG, "Starting webserver on port: '%d'", config.server_port);
    if (httpd_start(&server, &config) == ESP_OK) {
        // Register URI handlers.
        ESP_LOGI(TAG, "Registering URI handlers");
        httpd_register_uri_handler(server, &root_uri);
        httpd_register_uri_handler(server, &index_uri);
        httpd_register_uri_handler(server, &options_uri);
        return server;
    }

    ESP_LOGI(TAG, "Error starting server!");
    return NULL;
}

void webserver_stop(httpd_handle_t server)
{
    if (server) {
        httpd_stop(server);
    }
}

void webserver_register_handler(httpd_handle_t server, const httpd_uri_t *uri_handler)
{
    if (server && uri_handler) {
        httpd_register_uri_handler(server, uri_handler);
    }
}
