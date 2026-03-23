#include "meter_client_internal.h"
#include "modbus_reader.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "iammeter_drv";

// IAMMETER private driver context.
typedef struct {
    char host[128];
    uint16_t port;
    uint8_t unit_id;
    bool initialized;
} iammeter_ctx_t;

// Helper functions for register conversions.
static int32_t regs_to_int32(uint16_t high, uint16_t low)
{
    return (int32_t)(((uint32_t)high << 16) | low);
}

static uint32_t regs_to_uint32(uint16_t high, uint16_t low)
{
    return ((uint32_t)high << 16) | low;
}

// Parse one phase payload.
static void parse_phase_data(meter_phase_data_t *phase, const uint16_t *regs, uint16_t offset)
{
    phase->voltage = regs[offset] / 100.0f;
    phase->current = regs[offset + 1] / 100.0f;
    phase->active_power = regs_to_int32(regs[offset + 2], regs[offset + 3]);
    phase->forward_energy = regs_to_uint32(regs[offset + 4], regs[offset + 5]) / 800.0f;
    phase->reverse_energy = regs_to_uint32(regs[offset + 6], regs[offset + 7]) / 800.0f;
    phase->power_factor = regs[offset + 8] / 1000.0f;
}

// Initialization function.
static esp_err_t iammeter_init(const meter_client_config_t *config, void **driver_ctx)
{
    if (!config || !driver_ctx) {
        ESP_LOGE(TAG, "Invalid parameters");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!config->host[0]) {
        ESP_LOGE(TAG, "Host address is empty");
        return ESP_ERR_INVALID_ARG;
    }
    
    iammeter_ctx_t *ctx = calloc(1, sizeof(iammeter_ctx_t));
    if (!ctx) {
        ESP_LOGE(TAG, "Failed to allocate context");
        return ESP_ERR_NO_MEM;
    }
    
    strncpy(ctx->host, config->host, sizeof(ctx->host) - 1);
    ctx->port = config->port ? config->port : 502;
    ctx->unit_id = config->unit_id ? config->unit_id : 1;
    ctx->initialized = false;
    
    // Initialize the Modbus reader.
    modbus_reader_cfg_t mb_cfg = {
        .host = ctx->host,
        .port = ctx->port,
        .unit_id = ctx->unit_id,
        .reg_start = 0,
        .reg_count = 38  // IAMMETER requires 38 registers
    };
    
    esp_err_t err = modbus_reader_init(&mb_cfg, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "modbus_reader_init failed: %s", esp_err_to_name(err));
        free(ctx);
        return err;
    }
    
    ctx->initialized = true;
    *driver_ctx = ctx;
    
    ESP_LOGI(TAG, "iammeter driver initialized: %s:%u (unit=%u)", 
             ctx->host, ctx->port, ctx->unit_id);
    return ESP_OK;
}

// Read function.
static esp_err_t iammeter_read(void *driver_ctx, meter_data_t *data)
{
    if (!driver_ctx || !data) {
        ESP_LOGE(TAG, "Invalid parameters");
        return ESP_ERR_INVALID_ARG;
    }
    
    iammeter_ctx_t *ctx = (iammeter_ctx_t *)driver_ctx;
    if (!ctx->initialized) {
        ESP_LOGE(TAG, "Driver not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    // Read all registers (0-37, total 38 registers).
    uint16_t regs[38] = {0};
    esp_err_t err = modbus_reader_read_holding(regs, 38);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read modbus registers: %s", esp_err_to_name(err));
        return err;
    }
    
    // Parse three-phase data.
    // Phase A: registers 0-8
    parse_phase_data(&data->phase_a, regs, 0);
    
    // Phase B: registers 10-18
    parse_phase_data(&data->phase_b, regs, 10);
    
    // Phase C: registers 20-28
    parse_phase_data(&data->phase_c, regs, 20);
    
    // Parse model and frequency.
    const uint16_t model_num = regs[9];
    if (model_num == 2) {
        strlcpy(data->model, "WEM3080T", sizeof(data->model));
    } else {
        snprintf(data->model, sizeof(data->model), "IAMMETER-%u", model_num);
    }
    data->frequency = regs[30] / 100.0f;
    
    // Parse aggregate values (registers 32-37).
    data->total_power = regs_to_int32(regs[32], regs[33]);
    data->total_forward_energy = regs_to_uint32(regs[34], regs[35]) / 800.0f;
    data->total_reverse_energy = regs_to_uint32(regs[36], regs[37]) / 800.0f;
    
    // Mark all three phases as valid.
    data->valid_phases = 0x07;  // bit0=A, bit1=B, bit2=C
    
    // Optional timestamp support can be added with time.h.
    // data->timestamp = time(NULL);
    
    ESP_LOGD(TAG, "Read successful: Total Power=%d W, Model=%s", 
             data->total_power, data->model);
    
    return ESP_OK;
}

// Deinitialization function.
static esp_err_t iammeter_deinit(void *driver_ctx)
{
    if (!driver_ctx) {
        return ESP_ERR_INVALID_ARG;
    }
    
    iammeter_ctx_t *ctx = (iammeter_ctx_t *)driver_ctx;
    
    if (ctx->initialized) {
        modbus_reader_deinit();
        ctx->initialized = false;
    }
    
    free(ctx);
    ESP_LOGI(TAG, "iammeter driver deinitialized");
    
    return ESP_OK;
}

// Exported operation table.
const meter_driver_ops_t iammeter_driver_ops = {
    .init = iammeter_init,
    .read = iammeter_read,
    .deinit = iammeter_deinit,
};
