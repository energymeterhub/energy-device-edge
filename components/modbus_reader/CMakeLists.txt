idf_component_register(
    SRCS "modbus_reader.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_netif espressif__esp-modbus

)
