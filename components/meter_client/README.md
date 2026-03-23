# Meter Client

`meter_client` is the source abstraction used by `energy-device-edge`.

## Supported Sources

| Device | Type String | Protocol |
| --- | --- | --- |
| IAMMETER WEM3080T | `IAMMETER_WEM3080T` | Modbus TCP |
| Shelly Pro 3EM | `SHELLY_3EM` | Shelly RPC HTTP |

Both drivers populate the same `meter_data_t` structure, so the rest of the firmware can read live data without branching on device-specific payload formats.

## Create A Client

IAMMETER example:

```c
#include "meter_client.h"

meter_client_config_t config = {
    .port = 502,
    .unit_id = 1,
    .timeout_ms = 5000,
};
strncpy(config.host, "192.168.1.163", sizeof(config.host) - 1);

meter_client_handle_t client = meter_client_create("IAMMETER_WEM3080T", &config);
```

Shelly example:

```c
meter_client_config_t config = {
    .port = 80,
    .timeout_ms = 5000,
};
strncpy(config.host, "192.168.1.120", sizeof(config.host) - 1);

meter_client_handle_t client = meter_client_create("SHELLY_3EM", &config);
```

`unit_id` is only relevant for the Modbus source.

## Simulator Ports

If you are pairing the firmware with the local simulator instead of hardware:

- `IAMMETER WEM3080T` commonly stays on `502`
- `Shelly Pro 3EM` commonly uses `18080`

On real hardware, keep the normal default ports:

- `IAMMETER WEM3080T`: `502`
- `Shelly Pro 3EM`: `80`

## Type Aliases

The parser still accepts a couple of legacy aliases during config normalization:

- `IAMMETER` -> `IAMMETER_WEM3080T`
- `SHELLY` -> `SHELLY_3EM`
- `SHELLY_PRO_3EM` -> `SHELLY_3EM`

## Shelly RPC Mapping

The Shelly driver reads:

- `GET /rpc/EM.GetStatus?id=0` for real-time three-phase values
- `GET /rpc/EMData.GetStatus?id=0` for cumulative forward and reverse energy

Phase C active power prefers `c_active_power` and falls back to `c_act_power` for compatibility with different payload variants.
