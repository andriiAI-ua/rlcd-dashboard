idf_component_register(
    SRCS "hadata.c"
    INCLUDE_DIRS "include"
    REQUIRES settings
    PRIV_REQUIRES esp_http_client mbedtls net texts util lwip
)
