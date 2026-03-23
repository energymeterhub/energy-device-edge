idf_component_register(
    SRCS "cloud_uploader.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_client json meter_client esp-tls
)
