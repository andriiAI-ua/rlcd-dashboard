idf_component_register(
    SRCS "tlscert.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES mbedtls nvs_flash texts
)
