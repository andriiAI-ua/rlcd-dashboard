idf_component_register(
    SRCS "fwupdate.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES app_update esp_app_format esp_http_client mbedtls texts
)
