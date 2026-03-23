idf_component_register(SRCS "main.c"
    PRIV_REQUIRES wifi config nvs_flash webserver settings_api meter_client cloud
    INCLUDE_DIRS ".")
