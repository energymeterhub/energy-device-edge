#include "modbus_reader.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "mbcontroller.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "modbus_reader";

#define MODBUS_RESPONSE_TIMEOUT_MS 5000
#define MODBUS_RESTART_DELAY_MS 1000
#define MODBUS_RESTART_COOLDOWN_MS 3000

static modbus_reader_cfg_t s_cfg;
static bool s_started = false;
static void *s_master_ctx = NULL;
static esp_netif_t *s_netif = NULL;
static SemaphoreHandle_t s_modbus_lock = NULL;
static TickType_t s_last_restart_attempt = 0;

enum {
    CID_HOLD_READ = 0,
    CID_HOLD_READ_DYNAMIC,
    CID_COUNT
};

static mb_parameter_descriptor_t s_desc_mut[CID_COUNT];

static const mb_parameter_descriptor_t s_param_desc[] = {
    {
        .cid = CID_HOLD_READ,
        .param_key = "HOLD_READ",
        .param_units = "raw",
        .mb_slave_addr = 1,
        .mb_param_type = MB_PARAM_HOLDING,
        .mb_reg_start  = 0,
        .mb_size       = 1,
        .param_offset  = 0,
        .param_type    = PARAM_TYPE_U16,
        .param_size    = 2,
        .param_opts    = { .min = 0, .max = 0, .step = 0 },
        .access        = PAR_PERMS_READ
    },
    {
        .cid = CID_HOLD_READ_DYNAMIC,
        .param_key = "HOLD_READ_DYNAMIC",
        .param_units = "raw",
        .mb_slave_addr = 1,
        .mb_param_type = MB_PARAM_HOLDING,
        .mb_reg_start  = 0,
        .mb_size       = 1,
        .param_offset  = 0,
        .param_type    = PARAM_TYPE_U16,
        .param_size    = 2,
        .param_opts    = { .min = 0, .max = 0, .step = 0 },
        .access        = PAR_PERMS_READ
    },
};

static esp_err_t s_master_start(const modbus_reader_cfg_t *cfg, esp_netif_t *netif)
{
    mb_communication_info_t comm = {0};

    static char slave_addr_entry[160] = {0};
    static char *slave_ip_table[2] = {0};
    int written = snprintf(
        slave_addr_entry,
        sizeof(slave_addr_entry),
        "%u;%s;%u",
        cfg->unit_id,
        cfg->host,
        cfg->port
    );
    if (written < 0 || written >= (int)sizeof(slave_addr_entry)) {
        ESP_LOGE(TAG, "slave descriptor is too long for host=%s port=%u", cfg->host, cfg->port);
        return ESP_ERR_INVALID_ARG;
    }

    slave_ip_table[0] = slave_addr_entry;
    slave_ip_table[1] = NULL;

    comm.tcp_opts.mode = MB_TCP;
    comm.tcp_opts.port = cfg->port;
    comm.tcp_opts.addr_type = MB_IPV4;
    comm.tcp_opts.ip_addr_table = (void*)slave_ip_table;
    comm.tcp_opts.ip_netif_ptr = (void*)netif;
    comm.tcp_opts.uid = cfg->unit_id;
    comm.tcp_opts.response_tout_ms = MODBUS_RESPONSE_TIMEOUT_MS;
    comm.tcp_opts.start_disconnected = true;

    ESP_LOGI(TAG, "Using Modbus slave descriptor: %s", slave_addr_entry);

    esp_err_t err = mbc_master_create_tcp(&comm, &s_master_ctx);
    if (err != ESP_OK || s_master_ctx == NULL) {
        ESP_LOGE(TAG, "mbc_master_create_tcp failed: %s", esp_err_to_name(err));
        return err != ESP_OK ? err : ESP_ERR_INVALID_STATE;
    }

    memcpy(s_desc_mut, s_param_desc, sizeof(s_param_desc));
    s_desc_mut[CID_HOLD_READ].mb_slave_addr = cfg->unit_id;
    s_desc_mut[CID_HOLD_READ].mb_reg_start  = cfg->reg_start;
    s_desc_mut[CID_HOLD_READ].mb_size       = cfg->reg_count;
    s_desc_mut[CID_HOLD_READ].param_size    = cfg->reg_count * sizeof(uint16_t);
    s_desc_mut[CID_HOLD_READ_DYNAMIC].mb_slave_addr = cfg->unit_id;

    err = mbc_master_set_descriptor(s_master_ctx, &s_desc_mut[0], CID_COUNT);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mbc_master_set_descriptor failed: %s", esp_err_to_name(err));
        mbc_master_delete(s_master_ctx);
        s_master_ctx = NULL;
        return err;
    }

    err = mbc_master_start(s_master_ctx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mbc_master_start failed: %s", esp_err_to_name(err));
        mbc_master_delete(s_master_ctx);
        s_master_ctx = NULL;
        return err;
    }

    ESP_LOGI(TAG, "Modbus TCP master started: %s:%u unit=%u reg=%u count=%u",
             cfg->host, cfg->port, cfg->unit_id, cfg->reg_start, cfg->reg_count);

    s_started = true;
    return ESP_OK;
}

static void s_master_stop_locked(void)
{
    if (!s_master_ctx) {
        s_started = false;
        return;
    }

    esp_err_t stop_err = mbc_master_stop(s_master_ctx);
    if (stop_err != ESP_OK && stop_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "mbc_master_stop failed during cleanup: %s", esp_err_to_name(stop_err));
    }

    esp_err_t delete_err = mbc_master_delete(s_master_ctx);
    if (delete_err != ESP_OK) {
        ESP_LOGW(TAG, "mbc_master_delete failed during cleanup: %s", esp_err_to_name(delete_err));
    }

    s_master_ctx = NULL;
    s_started = false;
}

static esp_err_t s_master_restart_locked(void)
{
    if (!s_netif) {
        ESP_LOGE(TAG, "Cannot restart Modbus master without a valid netif");
        return ESP_ERR_INVALID_STATE;
    }

    TickType_t now = xTaskGetTickCount();
    TickType_t cooldown = pdMS_TO_TICKS(MODBUS_RESTART_COOLDOWN_MS);
    if (s_last_restart_attempt != 0 && (now - s_last_restart_attempt) < cooldown) {
        ESP_LOGW(TAG, "Skipping Modbus restart during cooldown window");
        return ESP_ERR_INVALID_STATE;
    }

    s_last_restart_attempt = now;
    ESP_LOGW(TAG, "Restarting Modbus TCP session...");
    s_master_stop_locked();
    vTaskDelay(pdMS_TO_TICKS(MODBUS_RESTART_DELAY_MS));
    return s_master_start(&s_cfg, s_netif);
}

esp_err_t modbus_reader_init(const modbus_reader_cfg_t *cfg, esp_netif_t *netif)
{
    if (!cfg || !cfg->host || cfg->port == 0 || cfg->reg_count == 0) return ESP_ERR_INVALID_ARG;
    if (s_started) return ESP_OK;

    s_cfg = *cfg;

    if (!s_modbus_lock) {
        s_modbus_lock = xSemaphoreCreateMutex();
        if (!s_modbus_lock) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (!netif) {
        netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    }
    if (!netif) {
        ESP_LOGE(TAG, "No netif (WiFi STA) found. Ensure WiFi connected and netif created.");
        return ESP_ERR_INVALID_STATE;
    }

    s_netif = netif;
    ESP_LOGI(TAG, "init: host=%s port=%u unit_id=%u", cfg->host, cfg->port, cfg->unit_id);
    return s_master_start(cfg, netif);
}

esp_err_t modbus_reader_read_holding(uint16_t *out_regs, uint16_t reg_count)
{
    if (!s_modbus_lock) return ESP_ERR_INVALID_STATE;
    if (!s_started || !s_master_ctx) return ESP_ERR_INVALID_STATE;
    if (!out_regs || reg_count == 0) return ESP_ERR_INVALID_ARG;
    if (reg_count > s_cfg.reg_count) return ESP_ERR_INVALID_ARG;

    if (xSemaphoreTake(s_modbus_lock, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGW(TAG, "Timed out waiting for Modbus lock");
        return ESP_ERR_TIMEOUT;
    }

    uint8_t type = 0;
    memset(out_regs, 0, reg_count * sizeof(uint16_t));

    esp_err_t err = mbc_master_get_parameter(
        s_master_ctx,
        CID_HOLD_READ,
        (uint8_t*)out_regs,
        &type
    );

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "read ok: type=%u first_reg=%u", type, out_regs[0]);
    } else {
        ESP_LOGE(TAG, "read failed: %s", esp_err_to_name(err));
        if (err == ESP_ERR_INVALID_STATE || err == ESP_FAIL) {
            esp_err_t rc = s_master_restart_locked();
            if (rc != ESP_OK) {
                ESP_LOGE(TAG, "Reconnect failed: %s", esp_err_to_name(rc));
            } else {
                memset(out_regs, 0, reg_count * sizeof(uint16_t));
                err = mbc_master_get_parameter(
                    s_master_ctx,
                    CID_HOLD_READ,
                    (uint8_t*)out_regs,
                    &type
                );
                if (err == ESP_OK) {
                    ESP_LOGI(TAG, "Read recovered after reconnect");
                } else {
                    ESP_LOGE(TAG, "Read still failing after reconnect: %s", esp_err_to_name(err));
                }
            }
        } else if (err == ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "Read timed out; keep current Modbus session and retry later");
        }
    }

    xSemaphoreGive(s_modbus_lock);
    return err;
}

esp_err_t modbus_reader_read_holding_range(uint16_t reg_start, uint16_t *out_regs, uint16_t reg_count)
{
    if (!s_modbus_lock) return ESP_ERR_INVALID_STATE;
    if (!s_started || !s_master_ctx) return ESP_ERR_INVALID_STATE;
    if (!out_regs || reg_count == 0) return ESP_ERR_INVALID_ARG;

    if (xSemaphoreTake(s_modbus_lock, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGW(TAG, "Timed out waiting for Modbus lock");
        return ESP_ERR_TIMEOUT;
    }

    s_desc_mut[CID_HOLD_READ_DYNAMIC].mb_slave_addr = s_cfg.unit_id;
    s_desc_mut[CID_HOLD_READ_DYNAMIC].mb_reg_start = reg_start;
    s_desc_mut[CID_HOLD_READ_DYNAMIC].mb_size = reg_count;
    s_desc_mut[CID_HOLD_READ_DYNAMIC].param_size = reg_count * sizeof(uint16_t);

    esp_err_t err = mbc_master_set_descriptor(s_master_ctx, &s_desc_mut[0], CID_COUNT);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to set dynamic descriptor: %s", esp_err_to_name(err));
        xSemaphoreGive(s_modbus_lock);
        return err;
    }

    uint8_t type = 0;
    memset(out_regs, 0, reg_count * sizeof(uint16_t));
    err = mbc_master_get_parameter(
        s_master_ctx,
        CID_HOLD_READ_DYNAMIC,
        (uint8_t*)out_regs,
        &type
    );

    if (err != ESP_OK && (err == ESP_ERR_INVALID_STATE || err == ESP_FAIL)) {
        esp_err_t rc = s_master_restart_locked();
        if (rc == ESP_OK) {
            err = mbc_master_set_descriptor(s_master_ctx, &s_desc_mut[0], CID_COUNT);
            if (err == ESP_OK) {
                memset(out_regs, 0, reg_count * sizeof(uint16_t));
                err = mbc_master_get_parameter(
                    s_master_ctx,
                    CID_HOLD_READ_DYNAMIC,
                    (uint8_t*)out_regs,
                    &type
                );
            }
        }
    }

    xSemaphoreGive(s_modbus_lock);
    return err;
}

esp_err_t modbus_reader_deinit(void)
{
    if (!s_modbus_lock) return ESP_OK;

    if (xSemaphoreTake(s_modbus_lock, pdMS_TO_TICKS(5000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    s_master_stop_locked();
    xSemaphoreGive(s_modbus_lock);
    return ESP_OK;
}
