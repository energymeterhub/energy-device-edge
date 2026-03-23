#pragma once
#include <esp_err.h>
#include <esp_http_server.h>

#ifdef __cplusplus
extern "C" {
#endif

// Start the web server.
// Returns the HTTP server handle, or NULL on failure.
httpd_handle_t webserver_start(void);

// Stop the web server.
void webserver_stop(httpd_handle_t server);

// Register an API handler.
void webserver_register_handler(httpd_handle_t server, const httpd_uri_t *uri_handler);

#ifdef __cplusplus
}
#endif
