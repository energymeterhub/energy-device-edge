idf_component_register(
    SRCS 
        "meter_client.c"
        "drivers/iammeter_driver.c"
        "drivers/shelly_3em_driver.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES modbus_reader json esp_http_client esp_app_format
)
