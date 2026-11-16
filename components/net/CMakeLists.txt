idf_component_register(
    SRCS "net.c"
    INCLUDE_DIRS "include"
    REQUIRES settings
    PRIV_REQUIRES esp_wifi esp_netif esp_event esp_timer esp_hw_support dns_server
)
