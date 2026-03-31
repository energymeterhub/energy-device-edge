#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *host;     // e.g. "192.168.1.163"
    uint16_t    port;     // default Modbus TCP port: 502
    uint8_t     unit_id;  // usually 1, some devices ignore it
    uint16_t    reg_start;// starting holding register address
    uint16_t    reg_count;// register count, 1 register = 16 bits
} modbus_reader_cfg_t;

// Initialize and start the Modbus TCP master.
// Requires STA mode to already have an IP address.
esp_err_t modbus_reader_init(const modbus_reader_cfg_t *cfg, esp_netif_t *netif);

// Read holding registers into out_regs. Buffer length must be at least reg_count.
esp_err_t modbus_reader_read_holding(uint16_t *out_regs, uint16_t reg_count);

// Read an arbitrary holding-register range without changing the configured default block.
esp_err_t modbus_reader_read_holding_range(uint16_t reg_start, uint16_t *out_regs, uint16_t reg_count);

// Release all resources.
esp_err_t modbus_reader_deinit(void);

#ifdef __cplusplus
}
#endif
