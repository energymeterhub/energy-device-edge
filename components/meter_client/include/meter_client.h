#pragma once

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Meter type enumeration.
 */
typedef enum {
    METER_TYPE_UNKNOWN = 0,
    METER_TYPE_IAMMETER_WEM3080T, // IAMMETER WEM3080T three-phase meter
    METER_TYPE_SHELLY_3EM,        // Shelly Pro 3EM over Shelly RPC HTTP
    METER_TYPE_MAX
} meter_type_t;

/**
 * @brief Meter transport / protocol type.
 */
typedef enum {
    METER_PROTOCOL_UNKNOWN = 0,
    METER_PROTOCOL_MODBUS_TCP,
    METER_PROTOCOL_HTTP
} meter_protocol_t;

/**
 * @brief Generic phase data.
 */
typedef struct {
    float voltage;             // voltage (V)
    float current;             // current (A)
    int32_t active_power;      // active power (W)
    int32_t reactive_power;    // reactive power (VAR), optional
    float forward_energy;      // forward energy (kWh)
    float reverse_energy;      // reverse energy (kWh)
    float power_factor;        // power factor (0-1)
} meter_phase_data_t;

/**
 * @brief Generic meter data, supports single-phase and three-phase devices.
 */
typedef struct {
    meter_type_t type;         // device type
    char model[32];            // model string
    uint32_t timestamp;        // data timestamp (Unix time)
    
    // Phase data. Only phase_a is used for single-phase devices.
    meter_phase_data_t phase_a;
    meter_phase_data_t phase_b;
    meter_phase_data_t phase_c;
    
    // Aggregate values
    float frequency;           // frequency (Hz)
    int32_t total_power;       // total active power (W)
    float total_forward_energy; // total forward energy (kWh)
    float total_reverse_energy; // total reverse energy (kWh)

    uint8_t valid_phases;      // valid phase bitmask (bit0=A, bit1=B, bit2=C)
} meter_data_t;

/**
 * @brief Client configuration.
 */
typedef struct {
    char host[128];            // IP address or hostname
    uint16_t port;             // TCP port number
    uint8_t unit_id;           // Modbus slave address
    uint32_t timeout_ms;       // timeout in milliseconds
    void *extra_config;        // optional device-specific configuration
} meter_client_config_t;

/**
 * @brief Opaque client handle.
 */
typedef struct meter_client_t* meter_client_handle_t;

/**
 * @brief Create a client from a type string.
 * 
 * @param type_str Type string, such as "IAMMETER_WEM3080T" or "SHELLY_3EM".
 * @param config Client configuration.
 * @return Client handle, or NULL on failure.
 */
meter_client_handle_t meter_client_create(const char *type_str, 
                                           const meter_client_config_t *config);

/**
 * @brief Read data from the device.
 * 
 * @param handle Client handle.
 * @param data Output structure.
 * @return ESP_OK on success, otherwise an error code.
 */
esp_err_t meter_client_read(meter_client_handle_t handle, meter_data_t *data);

/**
 * @brief Destroy a client instance.
 * 
 * @param handle Client handle.
 */
void meter_client_destroy(meter_client_handle_t handle);

/**
 * @brief Convert a type string to an enum value.
 * 
 * @param type_str Type string.
 * @return Meter type enum.
 */
meter_type_t meter_type_from_string(const char *type_str);

/**
 * @brief Get the type name string.
 * 
 * @param type Meter type enum.
 * @return Type name string.
 */
const char* meter_type_to_string(meter_type_t type);

/**
 * @brief Get the underlying protocol used by a meter type.
 *
 * @param type Meter type enum.
 * @return Meter protocol enum.
 */
meter_protocol_t meter_type_get_protocol(meter_type_t type);

/**
 * @brief Get the default port for a meter type.
 *
 * @param type Meter type enum.
 * @return Default port number.
 */
uint16_t meter_type_default_port(meter_type_t type);

/**
 * @brief Format meter data as standard JSON for upload and API responses.
 * 
 * @param data Meter data.
 * @param mac MAC string, 12 hex chars. If NULL, it is read automatically.
 * @param sn Serial number. If NULL or empty, the MAC is used.
 * @return JSON string that must be freed with cJSON_free, or NULL on failure.
 */
char* meter_data_format_json(const meter_data_t *data, const char *mac, const char *sn);

#ifdef __cplusplus
}
#endif
