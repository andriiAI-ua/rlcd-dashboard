idf_component_register(
    SRCS "web.c"
    INCLUDE_DIRS "include"
    REQUIRES settings
    PRIV_REQUIRES esp_app_format esp_http_server esp_https_server esp_netif esp_timer lwip net fwupdate audio tlscert hadata texts
)
