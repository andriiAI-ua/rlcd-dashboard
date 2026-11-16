idf_component_register(
    SRCS "srvimg.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_http_client esp_timer mbedtls net texts
)
