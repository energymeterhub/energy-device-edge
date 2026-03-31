# Meter Client Design

This document describes the current `meter_client` scope in `energy-device-edge`.

## Goal

Provide one small source abstraction that can read local energy data from:

- `IAMMETER WEM3080T` over Modbus TCP
- `Fronius SunSpec Inverter` over SunSpec Modbus TCP
- `Shelly Pro 3EM` over Shelly RPC HTTP

and normalize all of them into the same `meter_data_t` structure.

## Architecture

```text
app_main
  -> meter_client
       -> iammeter_driver
            -> modbus_reader
       -> fronius_sunspec_driver
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
- `FRONIUS_SUNSPEC`
- `SHELLY_3EM`

Legacy aliases accepted during parsing:

- `IAMMETER` -> `IAMMETER_WEM3080T`
- `FRONIUS` -> `FRONIUS_SUNSPEC`
- `FRONIUS_GEN24` -> `FRONIUS_SUNSPEC`
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

### Fronius SunSpec Inverter

- transport: SunSpec Modbus TCP
- default port: `502`
- discovery start: holding registers `40000-40001`
- read blocks:
  - Common model at `40002-40069`
  - inverter model `103` at `40070-40121`
- expected output:
  - scaled three-phase current and voltage
  - total AC power
  - lifetime energy
  - line frequency

## Normalized Contract

All drivers fill the same high-level fields:

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
- The UI keeps source port editable so simulator ports such as `1503` or `18080` can be used during development.

## Out Of Scope

Additional sources should continue to arrive as dedicated drivers behind the same `meter_client` interface rather than as special cases in the uploader or UI.
