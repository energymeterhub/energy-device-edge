idf_component_register(SRCS "settings_api.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_http_server config json wifi app_update meter_client esp_app_format)
