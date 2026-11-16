idf_component_register(
    SRCS "hass.c"
    INCLUDE_DIRS "include"
    REQUIRES settings
    PRIV_REQUIRES esp_hw_support hadata net texts
)
