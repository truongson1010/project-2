idf_component_register(
    SRCS "main.c" "ssd1306.c" "esp32-dht11.c"
    INCLUDE_DIRS "."
    REQUIRES driver json esp_wifi esp_event nvs_flash
    PRIV_REQUIRES esp_timer
)




