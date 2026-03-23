# Cloud Uploader

`cloud_uploader` periodically reads normalized meter data from `meter_client` and posts it to an IAMMETER-compatible HTTP endpoint.

## What It Does

- reads the current source device through `meter_client`
- formats the payload with `meter_data_format_json()`
- sends an HTTP `POST` request to the configured destination
- optionally runs as a background FreeRTOS task on a fixed interval

## Endpoint Shape

The uploader appends this path to the configured base URL:

```text
/api/v1/sensor/uploadsensor
```

Examples:

- IAMMETER Cloud: `https://www.iammeter.com/api/v1/sensor/uploadsensor`
- IAMMETER Local: `http://192.168.1.50/api/v1/sensor/uploadsensor`

## Configuration

```c
#include "cloud_uploader.h"

cloud_uploader_config_t config = {
    .server_url = "https://www.iammeter.com",
    .sn = "EDGE_DEVICE_SN",
    .upload_interval_ms = 60000,
};
```

Fields:

| Field | Meaning |
| --- | --- |
| `server_url` | Base URL of the upload target |
| `sn` | Virtual device serial number; if empty, the MAC is used |
| `upload_interval_ms` | Upload interval in milliseconds; defaults to `60000` when omitted |

## Usage

```c
meter_client_handle_t meter = meter_client_create("IAMMETER_WEM3080T", &meter_cfg);

cloud_uploader_config_t uploader_cfg = {
    .server_url = "https://www.iammeter.com",
    .sn = "EDGE_DEVICE_SN",
    .upload_interval_ms = 60000,
};

ESP_ERROR_CHECK(cloud_uploader_init(&uploader_cfg));
ESP_ERROR_CHECK(cloud_uploader_start(meter));
```

One-shot upload without starting the background task:

```c
ESP_ERROR_CHECK(cloud_uploader_upload_once(
    meter,
    "https://www.iammeter.com",
    "EDGE_DEVICE_SN"
));
```

Shutdown:

```c
cloud_uploader_stop();
cloud_uploader_deinit();
```

## Payload Shape

The component uploads the standard IAMMETER-compatible JSON produced by `meter_data_format_json()`, including:

- `method`
- `mac`
- `version`
- `server`
- `SN`
- `Datas`

`Datas` always contains three phase arrays, regardless of whether the original source was Modbus or HTTP.

## Operational Notes

- The uploader requires a valid `meter_client` handle.
- HTTPS uses the ESP-IDF certificate bundle through `esp_crt_bundle_attach`.
- Upload failures are logged and retried on the next interval; they do not stop the task automatically.
