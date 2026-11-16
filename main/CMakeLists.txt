idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES board st7305 sensors gfx net ui settings hass hadata srvimg texts web audio nvs_flash esp_app_format esp_pm
                  esp_timer app_update
)
