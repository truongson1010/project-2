idf_component_register(
  SRCS "main.c"
  REQUIRES        esp_wifi esp_netif esp_event json esp_timer   
  PRIV_REQUIRES   nvs_flash                                     
)
