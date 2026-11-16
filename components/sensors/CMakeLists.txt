idf_component_register(
    SRCS "shtc3.c" "rtc_clock.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_i2c
    PRIV_REQUIRES util
)
