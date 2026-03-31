#include "meter_client_internal.h"
#include "modbus_reader.h"
#include "esp_log.h"
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "fronius_drv";

#define SUNSPEC_SIGNATURE_START 40000
#define SUNSPEC_COMMON_START 40002
#define SUNSPEC_COMMON_SIZE 68
#define SUNSPEC_INV103_START 40070
#define SUNSPEC_INV103_SIZE 52

typedef struct {
    char host[128];
    uint16_t port;
    uint8_t unit_id;
    bool initialized;
} fronius_ctx_t;

static int16_t reg_to_int16(uint16_t value)
{
    return (int16_t)value;
}

static uint32_t regs_to_uint32(uint16_t high, uint16_t low)
{
    return ((uint32_t)high << 16) | low;
}

static float apply_sf_u16(uint16_t raw, int16_t sf)
{
    return (float)raw * powf(10.0f, (float)sf);
}

static float apply_sf_u32(uint32_t raw, int16_t sf)
{
    return (float)raw * powf(10.0f, (float)sf);
}

static void read_ascii_words(char *out, size_t out_size, const uint16_t *regs, size_t word_count)
{
    if (!out || out_size == 0) {
        return;
    }

    size_t write_index = 0;
    for (size_t i = 0; i < word_count && write_index + 1 < out_size; ++i) {
        char left = (char)((regs[i] >> 8) & 0xFF);
        char right = (char)(regs[i] & 0xFF);
        if (left != '\0' && write_index + 1 < out_size) {
            out[write_index++] = left;
        }
        if (right != '\0' && write_index + 1 < out_size) {
            out[write_index++] = right;
        }
    }

    while (write_index > 0 && out[write_index - 1] == ' ') {
        write_index--;
    }
    out[write_index] = '\0';
}

static esp_err_t fronius_sunspec_init(const meter_client_config_t *config, void **driver_ctx)
{
    if (!config || !driver_ctx) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!config->host[0]) {
        ESP_LOGE(TAG, "Host address is empty");
        return ESP_ERR_INVALID_ARG;
    }

    fronius_ctx_t *ctx = calloc(1, sizeof(fronius_ctx_t));
    if (!ctx) {
        return ESP_ERR_NO_MEM;
    }

    strlcpy(ctx->host, config->host, sizeof(ctx->host));
    ctx->port = config->port ? config->port : 502;
    ctx->unit_id = config->unit_id ? config->unit_id : 1;

    modbus_reader_cfg_t mb_cfg = {
        .host = ctx->host,
        .port = ctx->port,
        .unit_id = ctx->unit_id,
        .reg_start = SUNSPEC_SIGNATURE_START,
        .reg_count = 2
    };

    esp_err_t err = modbus_reader_init(&mb_cfg, NULL);
    if (err != ESP_OK) {
        free(ctx);
        return err;
    }

    uint16_t sig[2] = {0};
    err = modbus_reader_read_holding(sig, 2);
    if (err != ESP_OK || sig[0] != 0x5375 || sig[1] != 0x6E53) {
        ESP_LOGE(TAG, "SunSpec signature not found at 40000");
        modbus_reader_deinit();
        free(ctx);
        return err != ESP_OK ? err : ESP_FAIL;
    }

    ctx->initialized = true;
    *driver_ctx = ctx;
    ESP_LOGI(TAG, "fronius SunSpec driver initialized: %s:%u unit=%u", ctx->host, ctx->port, ctx->unit_id);
    return ESP_OK;
}

static esp_err_t fronius_sunspec_read(void *driver_ctx, meter_data_t *data)
{
    if (!driver_ctx || !data) {
        return ESP_ERR_INVALID_ARG;
    }

    fronius_ctx_t *ctx = (fronius_ctx_t *)driver_ctx;
    if (!ctx->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    uint16_t common[SUNSPEC_COMMON_SIZE] = {0};
    uint16_t inv[SUNSPEC_INV103_SIZE] = {0};

    esp_err_t err = modbus_reader_read_holding_range(SUNSPEC_COMMON_START, common, SUNSPEC_COMMON_SIZE);
    if (err != ESP_OK) {
        return err;
    }

    err = modbus_reader_read_holding_range(SUNSPEC_INV103_START, inv, SUNSPEC_INV103_SIZE);
    if (err != ESP_OK) {
        return err;
    }

    if (common[0] != 1 || inv[0] != 103) {
        ESP_LOGE(TAG, "Unexpected SunSpec model chain: common=%u inverter=%u", common[0], inv[0]);
        return ESP_FAIL;
    }

    int16_t a_sf = reg_to_int16(inv[6]);
    int16_t v_sf = reg_to_int16(inv[13]);
    int16_t w_sf = reg_to_int16(inv[15]);
    int16_t hz_sf = reg_to_int16(inv[17]);
    int16_t wh_sf = reg_to_int16(inv[27]);

    data->phase_a.current = apply_sf_u16(inv[3], a_sf);
    data->phase_b.current = apply_sf_u16(inv[4], a_sf);
    data->phase_c.current = apply_sf_u16(inv[5], a_sf);
    data->phase_a.voltage = apply_sf_u16(inv[10], v_sf);
    data->phase_b.voltage = apply_sf_u16(inv[11], v_sf);
    data->phase_c.voltage = apply_sf_u16(inv[12], v_sf);
    data->total_power = (int32_t)lroundf(apply_sf_u16(inv[14], w_sf));
    data->frequency = apply_sf_u16(inv[16], hz_sf);
    data->total_forward_energy = apply_sf_u32(regs_to_uint32(inv[24], inv[25]), wh_sf) / 1000.0f;
    data->total_reverse_energy = 0.0f;
    data->valid_phases = 0x07;

    data->phase_a.active_power = data->total_power / 3;
    data->phase_b.active_power = data->total_power / 3;
    data->phase_c.active_power = data->total_power - data->phase_a.active_power - data->phase_b.active_power;
    data->phase_a.forward_energy = data->total_forward_energy / 3.0f;
    data->phase_b.forward_energy = data->total_forward_energy / 3.0f;
    data->phase_c.forward_energy = data->total_forward_energy / 3.0f;
    data->phase_a.reverse_energy = 0.0f;
    data->phase_b.reverse_energy = 0.0f;
    data->phase_c.reverse_energy = 0.0f;
    data->phase_a.power_factor = 0.0f;
    data->phase_b.power_factor = 0.0f;
    data->phase_c.power_factor = 0.0f;

    char manufacturer[17] = {0};
    char model[17] = {0};
    read_ascii_words(manufacturer, sizeof(manufacturer), &common[2], 8);
    read_ascii_words(model, sizeof(model), &common[10], 8);
    if (manufacturer[0] != '\0' && model[0] != '\0') {
        strlcpy(data->model, manufacturer, sizeof(data->model));
        strlcat(data->model, " ", sizeof(data->model));
        strlcat(data->model, model, sizeof(data->model));
    } else if (model[0] != '\0') {
        strlcpy(data->model, model, sizeof(data->model));
    } else {
        strlcpy(data->model, "Fronius SunSpec", sizeof(data->model));
    }

    return ESP_OK;
}

static esp_err_t fronius_sunspec_deinit(void *driver_ctx)
{
    if (!driver_ctx) {
        return ESP_ERR_INVALID_ARG;
    }

    fronius_ctx_t *ctx = (fronius_ctx_t *)driver_ctx;
    if (ctx->initialized) {
        modbus_reader_deinit();
        ctx->initialized = false;
    }
    free(ctx);
    return ESP_OK;
}

const meter_driver_ops_t fronius_sunspec_driver_ops = {
    .init = fronius_sunspec_init,
    .read = fronius_sunspec_read,
    .deinit = fronius_sunspec_deinit,
};
