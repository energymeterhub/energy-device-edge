# Meter Client Design

This document describes the current `meter_client` scope in `energy-device-edge`.

## Goal

Provide one small source abstraction that can read local energy data from:

- `IAMMETER WEM3080T` over Modbus TCP
- `Shelly Pro 3EM` over Shelly RPC HTTP

and normalize both into the same `meter_data_t` structure.

## Architecture

```text
app_main
  -> meter_client
       -> iammeter_driver
            -> modbus_reader
       -> shelly_3em_driver
            -> esp_http_client
  -> cloud_uploader
  -> settings_api
  -> webserver
```

## Type Strings

Canonical type strings:

- `IAMMETER_WEM3080T`
- `SHELLY_3EM`

Legacy aliases accepted during parsing:

- `IAMMETER` -> `IAMMETER_WEM3080T`
- `SHELLY` -> `SHELLY_3EM`

## Protocol Rules

### IAMMETER WEM3080T

- transport: Modbus TCP
- default port: `502`
- data source: holding registers `0-37`
- expected output: three phases plus aggregate power and energy

### Shelly Pro 3EM

- transport: Shelly RPC HTTP
- default port: `80`
- validation endpoint: `/rpc/EM.GetStatus?id=0`
- read endpoints:
  - `/rpc/EM.GetStatus?id=0`
  - `/rpc/EMData.GetStatus?id=0`
- expected output:
  - instantaneous three-phase voltage, current, and power from `EM.GetStatus`
  - cumulative forward and reverse energy from `EMData.GetStatus`

## Normalized Contract

Both drivers fill the same high-level fields:

- `model`
- `phase_a`
- `phase_b`
- `phase_c`
- `total_power`
- `total_forward_energy`
- `total_reverse_energy`
- `frequency`
- `valid_phases`

The rest of the firmware only depends on this normalized contract. The upload path, monitor UI, and API do not need to know whether the source was Modbus or HTTP.

## Known Differences

- `Shelly Pro 3EM` does not provide line frequency in the payload used here, so `frequency` stays `0`.
- IAMMETER uses `unit_id = 1` in the current firmware startup path.
- The UI keeps source port editable so a simulator port such as `18080` can be used during development.

## Out Of Scope

The project intentionally no longer carries inverter-specific drivers or extra meter families. If more sources are added later, they should come back as dedicated drivers behind the same `meter_client` interface rather than as special cases in the uploader or UI.
