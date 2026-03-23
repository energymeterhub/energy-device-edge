#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CONFIG_NAMESPACE "cfg"
#define CONFIG_KEY_ACTIVE "active"
#define CONFIG_KEY_SLOT0 "slot0"
#define CONFIG_KEY_SLOT1 "slot1"

/* Current config version */
#define DEVICE_CONFIG_VERSION 4

#define DESTINATION_TYPE_NONE "NONE"
#define DESTINATION_TYPE_IAMMETER_CLOUD "IAMMETER_CLOUD"
#define DESTINATION_TYPE_IAMMETER_LOCAL "IAMMETER_LOCAL"
#define IAMMETER_CLOUD_BASE_URL "https://www.iammeter.com"

/* Example low-frequency configuration structure. Add fields as needed. */
typedef struct {
    uint32_t version; /* set to DEVICE_CONFIG_VERSION */
    uint32_t crc;     /* CRC over bytes after this field */

    /* wifi settings (sensitive: don't log) */
    struct {
        char ssid[32];
        char password[64];
    } wifi;

    /* meter settings */
    struct {
        char type[32];      /* meter type: "IAMMETER_WEM3080T" | "SHELLY_3EM" */
        char host[128];     /* meter IP address or domain */
    } meter;

    /* legacy upload settings kept for migration compatibility */
    struct {
        char server[128];
        char sn[64];
    } cloud;

    /* device settings */
    struct {
        char device_name[32];
    } device;

    /* Source device port */
    uint16_t meter_port;       /* meter port, usually 502 for IAMMETER or 80 for Shelly Pro 3EM */

    /* upload destination settings */
    struct {
        char type[32];         /* NONE | IAMMETER_CLOUD | IAMMETER_LOCAL */
        char address[128];     /* base URL for local destination */
        char sn[64];           /* virtual meter SN for upload */
    } destination;

    /* future fields can be appended here */
} device_config_t;

/* API */
esp_err_t config_init(void);
esp_err_t config_load(device_config_t *cfg);
esp_err_t config_save(const device_config_t *cfg);
esp_err_t config_factory_reset(void);
void      config_set_defaults(device_config_t *cfg);
bool      config_destination_is_enabled(const device_config_t *cfg);
esp_err_t config_get_destination_url(const device_config_t *cfg, char *buffer, size_t buffer_size);

#ifdef __cplusplus
}
#endif
