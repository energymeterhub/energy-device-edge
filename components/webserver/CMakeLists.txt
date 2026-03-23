idf_component_register(SRCS "webserver.c"
                       INCLUDE_DIRS "include"
                       EMBED_FILES "html/index.html"
                       REQUIRES esp_http_server)
