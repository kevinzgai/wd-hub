idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        esp_driver_i2s
        esp_driver_gpio
        esp_timer
)
