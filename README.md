# Energy Device Edge

`energy-device-edge` is an ESP-IDF firmware project that reads a local energy source on the LAN, normalizes the data into the IAMMETER upload shape, and optionally forwards it to an IAMMETER-compatible destination.

## Current Scope

Supported source devices:

| Device | Type String | Protocol | Default Port |
| --- | --- | --- | --- |
| IAMMETER WEM3080T | `IAMMETER_WEM3080T` | Modbus TCP | `502` |
| Shelly Pro 3EM | `SHELLY_3EM` | Shelly RPC HTTP | `80` |

Supported destinations:

| Destination | Type String | Notes |
| --- | --- | --- |
| Disabled | `NONE` | Read locally only |
| IAMMETER Cloud | `IAMMETER_CLOUD` | Uses the public IAMMETER cloud base URL |
| IAMMETER Local | `IAMMETER_LOCAL` | Uses a user-provided base URL |

The project no longer includes inverter drivers or other source types. Both supported sources map into the same `meter_data_t` structure, so the monitor UI and uploader stay device-agnostic.

## Runtime Behavior

The firmware has two operating modes:

1. `STA connected`
   The device joins the configured Wi-Fi network, starts the web UI, polls the configured source device, and starts the uploader when a destination is enabled.
2. `SoftAP recovery`
   If Wi-Fi is missing or cannot connect, the device starts an open provisioning hotspot named `energy_device_edge_xxxx`, serves the UI/API locally, and skips source polling and cloud upload until normal Wi-Fi becomes available again.

When the device is in SoftAP mode, it periodically checks whether the configured Wi-Fi is reachable and reboots back into normal mode once connectivity returns.

## Web UI And API

The embedded UI is a small control surface for:

- live monitor data
- source and destination configuration
- Wi-Fi scan and provisioning
- OTA firmware upload
- system restart
- factory reset

Main HTTP endpoints:

| Method | Path | Purpose |
| --- | --- | --- |
| `GET` | `/` | Embedded UI |
| `GET` | `/api/config` | Load saved configuration and UI state |
| `POST` | `/api/config` | Save the full configuration in one request |
| `POST` | `/api/source` | Save source, destination, and device settings, then hot-apply them |
| `POST` | `/api/network` | Save Wi-Fi settings only |
| `GET` | `/api/meter/data` | Read normalized live meter JSON |
| `GET` | `/api/wifi/scan` | Scan nearby Wi-Fi networks |
| `POST` | `/api/ota` | Upload a new firmware image |
| `POST` | `/api/restart` | Reboot the device |
| `POST` | `/api/factory-reset` | Clear saved configuration and reboot |

`POST /api/source` validates the selected source, saves the configuration, and immediately hot-applies the new meter address, port, and uploader target without requiring a reboot. `POST /api/config` remains available for full-payload compatibility. `POST /api/network` only updates Wi-Fi credentials, so network setup is not blocked by meter validation.

## Simulator Pairing

This firmware is designed to pair with the local `energy-device-simulator` project during development.

Typical simulator ports:

- `IAMMETER WEM3080T`: `502`
- `Shelly Pro 3EM`: `18080`

Typical real-device ports:

- `IAMMETER WEM3080T`: `502`
- `Shelly Pro 3EM`: `80`

The UI lets you override the source port so you can point the firmware at either the simulator or real hardware.

## Project Layout

- `main/`
  Application boot sequence and runtime orchestration.
- `components/config`
  Persistent device configuration and normalization logic.
- `components/meter_client`
  Source abstraction plus the IAMMETER and Shelly drivers.
- `components/modbus_reader`
  Shared Modbus TCP register reader used by IAMMETER.
- `components/cloud`
  Periodic uploader for IAMMETER-compatible JSON payloads.
- `components/settings_api`
  REST endpoints for config, meter reads, Wi-Fi scan, OTA, and restart.
- `components/webserver`
  Embedded HTML UI.
- `components/wifi`
  STA/AP startup, recovery, and scan support.
- `docs/`
  Design notes for the current source abstraction.

## Build

Typical ESP-IDF workflow:

```bash
idf.py build
idf.py -p PORT flash monitor
```

The repo keeps `sdkconfig.defaults` as the checked-in baseline. Local `sdkconfig`, build output, and managed components are treated as generated files and should not be committed.

## Notes

- The uploader posts to `/api/v1/sensor/uploadsensor` on the selected destination base URL.
- `Shelly Pro 3EM` uses `/rpc/EM.GetStatus?id=0` for real-time values and `/rpc/EMData.GetStatus?id=0` for cumulative forward and reverse energy.
- `Shelly Pro 3EM` does not expose frequency in the payload used here, so frequency stays `0` and the UI reports that as not available.
- If old NVS data still contains removed source types, config normalization clears them and the UI expects one of the current supported source types to be selected again.
