idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    EMBED_FILES "image.ppm"
    REQUIRES
        esphome__esp-hub75
        esp_eth
        esp_event
        esp_netif
        esp_http_client
        json
        mbedtls
        esp_driver_spi
        esp_driver_gpio
        esp_hw_support
)

target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)
