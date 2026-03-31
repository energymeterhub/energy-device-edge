#include "config.h"
#include <string.h>
#include <stdio.h>
#include <strings.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

static void normalize_destination(device_config_t *cfg)
{
    if (!cfg) {
        return;
    }

    if (cfg->destination.type[0] == '\0') {
        if (cfg->cloud.server[0] == '\0') {
            strlcpy(cfg->destination.type, DESTINATION_TYPE_NONE, sizeof(cfg->destination.type));
        } else if (strcmp(cfg->cloud.server, IAMMETER_CLOUD_BASE_URL) == 0) {
            strlcpy(cfg->destination.type, DESTINATION_TYPE_IAMMETER_CLOUD, sizeof(cfg->destination.type));
        } else {
            strlcpy(cfg->destination.type, DESTINATION_TYPE_IAMMETER_LOCAL, sizeof(cfg->destination.type));
            strlcpy(cfg->destination.address, cfg->cloud.server, sizeof(cfg->destination.address));
        }
    }

    if (cfg->destination.sn[0] == '\0' && cfg->cloud.sn[0] != '\0') {
        strlcpy(cfg->destination.sn, cfg->cloud.sn, sizeof(cfg->destination.sn));
    }

    if (strcmp(cfg->destination.type, DESTINATION_TYPE_IAMMETER_CLOUD) == 0) {
        cfg->destination.address[0] = '\0';
        strlcpy(cfg->cloud.server, IAMMETER_CLOUD_BASE_URL, sizeof(cfg->cloud.server));
    } else if (strcmp(cfg->destination.type, DESTINATION_TYPE_IAMMETER_LOCAL) == 0) {
        strlcpy(cfg->cloud.server, cfg->destination.address, sizeof(cfg->cloud.server));
    } else {
        strlcpy(cfg->destination.type, DESTINATION_TYPE_NONE, sizeof(cfg->destination.type));
        cfg->destination.address[0] = '\0';
        cfg->cloud.server[0] = '\0';
    }

    strlcpy(cfg->cloud.sn, cfg->destination.sn, sizeof(cfg->cloud.sn));
}

static void normalize_meter_type(device_config_t *cfg)
{
    if (!cfg) {
        return;
    }

    if (cfg->meter.type[0] == '\0') {
        return;
    }

    if (strcasecmp(cfg->meter.type, "IAMMETER") == 0 ||
        strcasecmp(cfg->meter.type, "IAMMETER_WEM3080T") == 0) {
        strlcpy(cfg->meter.type, "IAMMETER_WEM3080T", sizeof(cfg->meter.type));
        return;
    }

    if (strcasecmp(cfg->meter.type, "SHELLY") == 0 ||
        strcasecmp(cfg->meter.type, "SHELLY_3EM") == 0 ||
        strcasecmp(cfg->meter.type, "SHELLY_PRO_3EM") == 0) {
        strlcpy(cfg->meter.type, "SHELLY_3EM", sizeof(cfg->meter.type));
        return;
    }

    if (strcasecmp(cfg->meter.type, "FRONIUS") == 0 ||
        strcasecmp(cfg->meter.type, "FRONIUS_SUNSPEC") == 0 ||
        strcasecmp(cfg->meter.type, "FRONIUS_GEN24") == 0) {
        strlcpy(cfg->meter.type, "FRONIUS_SUNSPEC", sizeof(cfg->meter.type));
        return;
    }

    cfg->meter.type[0] = '\0';
}

//#define CONFIG_TEST_MODE   1
static const char *TAG = "config";

/* CRC32 implementation (polynomial 0xEDB88320) */
static uint32_t crc32_compute(const uint8_t *data, size_t len)
{
    static uint32_t table[256];
    static int init = 0;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t crc = i;
            for (int j = 0; j < 8; ++j)
                crc = (crc >> 1) ^ (0xEDB88320u & (-(int)(crc & 1)));
            table[i] = crc;
        }
        init = 1;
    }
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i)
        crc = (crc >> 8) ^ table[(crc ^ data[i]) & 0xFFu];
    return crc ^ 0xFFFFFFFFu;
}

/* Helpers to read/write NVS blobs */
static esp_err_t read_slot_blob(const char *key, void *out, size_t *inout_size)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(CONFIG_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    err = nvs_get_blob(h, key, out, inout_size);
    nvs_close(h);
    return err;
}

static esp_err_t write_slot_blob(const char *key, const void *data, size_t size)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(h, key, data, size);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static esp_err_t write_active(uint8_t v)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, CONFIG_KEY_ACTIVE, v);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static esp_err_t read_active(uint8_t *v)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(CONFIG_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    err = nvs_get_u8(h, CONFIG_KEY_ACTIVE, v);
    nvs_close(h);
    return err;
}

/* compute CRC over bytes after crc field for provided length */
static uint32_t compute_crc_for_blob(const uint8_t *blob, size_t blob_len)
{
    /* find offset of crc field in struct */
    size_t crc_offset = offsetof(device_config_t, crc) + sizeof(((device_config_t*)0)->crc);
    if (blob_len <= crc_offset) return 0;
    const uint8_t *start = blob + crc_offset;
    size_t len = blob_len - crc_offset;
    return crc32_compute(start, len);
}

esp_err_t config_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

void config_set_defaults(device_config_t *cfg)
{
    if (!cfg) return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->version = DEVICE_CONFIG_VERSION;
    /* defaults */
    strncpy(cfg->wifi.ssid, "", sizeof(cfg->wifi.ssid)-1);
    strncpy(cfg->wifi.password, "", sizeof(cfg->wifi.password)-1);
    strncpy(cfg->meter.type, "IAMMETER_WEM3080T", sizeof(cfg->meter.type)-1);
    strncpy(cfg->meter.host, "", sizeof(cfg->meter.host)-1);
    strncpy(cfg->cloud.server, "https://www.iammeter.com", sizeof(cfg->cloud.server)-1);
    strncpy(cfg->cloud.sn, "", sizeof(cfg->cloud.sn)-1);
    strncpy(cfg->device.device_name, "energy_device_edge", sizeof(cfg->device.device_name)-1);
    cfg->meter_port = 502;
    strncpy(cfg->destination.type, DESTINATION_TYPE_IAMMETER_CLOUD, sizeof(cfg->destination.type)-1);
    strncpy(cfg->destination.address, "", sizeof(cfg->destination.address)-1);
    strncpy(cfg->destination.sn, "", sizeof(cfg->destination.sn)-1);
    /* crc will be filled on save */
}

esp_err_t config_load(device_config_t *cfg)
{
    #ifdef CONFIG_TEST_MODE
    ESP_LOGW(TAG, "CONFIG_TEST_MODE enabled");
    // Load defaults first.
    config_set_defaults(cfg);
    // Test scenario 1: configuration exists but Wi-Fi cannot connect.
    // strlcpy(cfg->wifi.ssid, "CMCC-bHsu", sizeof(cfg->wifi.ssid));
    // strlcpy(cfg->wifi.password, "739nf6xq", sizeof(cfg->wifi.password));
    
    strlcpy(cfg->wifi.ssid, "gwwn", sizeof(cfg->wifi.ssid));
    strlcpy(cfg->wifi.password, "honest123", sizeof(cfg->wifi.password));

    // strlcpy(cfg->wifi.ssid, "3212", sizeof(cfg->wifi.ssid));
    // strlcpy(cfg->wifi.password, "42065881", sizeof(cfg->wifi.password));

    // Test scenario 2: device has no Wi-Fi configuration at all.
    // cfg->wifi.ssid[0] = '\0';

    return ESP_OK;
    #endif


    if (!cfg) return ESP_ERR_INVALID_ARG;
    uint8_t active = 0xFF;
    esp_err_t err = read_active(&active);
    const char *keys[2] = {CONFIG_KEY_SLOT0, CONFIG_KEY_SLOT1};

    int try_order[2] = {0,1};
    if (err == ESP_OK && (active == 0 || active == 1)) {
        try_order[0] = active;
        try_order[1] = 1 - active;
    }

    for (int i = 0; i < 2; ++i) {
        size_t required = 0;
        esp_err_t r = read_slot_blob(keys[try_order[i]], NULL, &required);
        if (r == ESP_ERR_NVS_NOT_FOUND) continue;
        if (r != ESP_OK && r != ESP_ERR_NVS_INVALID_NAME) continue;
        /* allocate temporary buffer */
        uint8_t *buf = malloc(required);
        if (!buf) return ESP_ERR_NO_MEM;
        size_t got = required;
        r = read_slot_blob(keys[try_order[i]], buf, &got);
        if (r == ESP_OK) {
            if (got < sizeof(uint32_t)*2) {
                /* too small to contain version+crc */
                free(buf);
                continue;
            }
            /* extract stored crc (at offset of crc field)
               stored blob layout matches beginning of device_config_t */
            uint32_t stored_crc = 0;
            size_t crc_field_offset = offsetof(device_config_t, crc);
            if (got >= crc_field_offset + sizeof(uint32_t)) {
                memcpy(&stored_crc, buf + crc_field_offset, sizeof(uint32_t));
            } else {
                free(buf);
                continue;
            }
            uint32_t calc = compute_crc_for_blob(buf, got);
            if (calc == stored_crc) {
                /* good: copy into cfg, allow partial older blobs */
                size_t copy_len = got < sizeof(device_config_t) ? got : sizeof(device_config_t);
                memset(cfg, 0, sizeof(*cfg));
                memcpy(cfg, buf, copy_len);
                /* if older version (smaller blob), fill defaults for new fields */
                if (cfg->version < DEVICE_CONFIG_VERSION) {
                    device_config_t defaults;
                    config_set_defaults(&defaults);
                    /* copy missing tail bytes from defaults */
                    if (copy_len < sizeof(device_config_t)) {
                        memcpy(((uint8_t*)cfg) + copy_len,
                               ((uint8_t*)&defaults) + copy_len,
                               sizeof(device_config_t) - copy_len);
                    }
                    cfg->version = DEVICE_CONFIG_VERSION;
                    if (cfg->meter_port == 0) {
                        cfg->meter_port = defaults.meter_port;
                    }
                }
                normalize_destination(cfg);
                normalize_meter_type(cfg);
                free(buf);
                ESP_LOGI(TAG, "config loaded from slot %d", try_order[i]);
                ESP_LOGI(TAG, "Loaded config - destination.type: %s, destination.address: %s, destination.sn: %s, meter.host: %s",
                         cfg->destination.type,
                         cfg->destination.address,
                         cfg->destination.sn,
                         cfg->meter.host);
                return ESP_OK;
            } else {
                ESP_LOGW(TAG, "crc mismatch on slot %d", try_order[i]);
            }
        }
        free(buf);
    }

    /* both failed -> defaults */
    config_set_defaults(cfg);
    normalize_meter_type(cfg);
    ESP_LOGI(TAG, "no valid config found, using defaults");
    ESP_LOGI(TAG, "Default config - destination.type: %s, destination.address: %s, destination.sn: %s, meter.host: %s",
             cfg->destination.type,
             cfg->destination.address,
             cfg->destination.sn,
             cfg->meter.host);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t config_save(const device_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    /* basic validation: device_name present */
    if (strlen(cfg->device.device_name) == 0) return ESP_ERR_INVALID_ARG;

    /* Prepare a full-sized buffer to write */
    device_config_t tmp;
    memcpy(&tmp, cfg, sizeof(device_config_t));
    tmp.version = DEVICE_CONFIG_VERSION;
    normalize_destination(&tmp);
    normalize_meter_type(&tmp);
    /* compute crc over bytes after crc field */
    size_t crc_offset = offsetof(device_config_t, crc) + sizeof(tmp.crc);
    tmp.crc = crc32_compute(((uint8_t*)&tmp) + crc_offset, sizeof(device_config_t) - crc_offset);

    ESP_LOGI(TAG, "Saving config - destination.type: %s, destination.address: %s, destination.sn: %s, meter.host: %s",
             tmp.destination.type,
             tmp.destination.address,
             tmp.destination.sn,
             tmp.meter.host);

    /* determine active and write to non-active slot */
    uint8_t active = 0xFF;
    esp_err_t err = read_active(&active);
    int target = 0;
    if (err == ESP_OK && (active == 0 || active == 1)) target = 1 - active;
    else target = 0; /* default write slot0 */

    const char *target_key = (target == 0) ? CONFIG_KEY_SLOT0 : CONFIG_KEY_SLOT1;
    err = write_slot_blob(target_key, &tmp, sizeof(tmp));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed write slot %d: %d", target, err);
        return err;
    }

    /* switch active */
    err = write_active((uint8_t)target);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed set active=%d: %d", target, err);
        return err;
    }

    ESP_LOGI(TAG, "config saved to slot %d", target);
    return ESP_OK;
}

esp_err_t config_factory_reset(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_erase_all(h);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "factory reset completed for namespace: %s", CONFIG_NAMESPACE);
    }

    return err;
}

bool config_destination_is_enabled(const device_config_t *cfg)
{
    return cfg && strcmp(cfg->destination.type, DESTINATION_TYPE_NONE) != 0;
}

esp_err_t config_get_destination_url(const device_config_t *cfg, char *buffer, size_t buffer_size)
{
    if (!cfg || !buffer || buffer_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    buffer[0] = '\0';

    if (strcmp(cfg->destination.type, DESTINATION_TYPE_IAMMETER_CLOUD) == 0) {
        strlcpy(buffer, IAMMETER_CLOUD_BASE_URL, buffer_size);
        return ESP_OK;
    }

    if (strcmp(cfg->destination.type, DESTINATION_TYPE_IAMMETER_LOCAL) == 0) {
        if (cfg->destination.address[0] == '\0') {
            return ESP_ERR_INVALID_STATE;
        }
        strlcpy(buffer, cfg->destination.address, buffer_size);
        return ESP_OK;
    }

    return ESP_ERR_NOT_FOUND;
}
